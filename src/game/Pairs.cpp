#include "game/Pairs.h"

#include "DevLine.h"
#include "game/Furniture.h"
#include "game/NpcSync.h"
#include "game/PairRules.h"
#include "game/PeaceRules.h"
#include "game/RemotePlayers.h"
#include "game/Riding.h"
#include "game/StateBuffer.h"
#include "net/FormsCommand.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pairs
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;
		using Side = Protocol::PairSide;
		using Shown = PairRules::Shown;
		using Verdict = PairRules::Verdict;

		// Pair events the hooks heard, for the tick to say (each by the next frame).
		constexpr std::size_t MAX_NOTED = 64;

		std::atomic<bool>          active{ false };
		std::atomic<std::uint32_t> ourId{ 0 };

		// ---- what the graph notify hooks (any thread) and the tick share ----
		struct NotedEvent
		{
			RE::FormID        id = 0;  // looked up by the tick (no handle made on a job thread)
			std::string       event;
			Clock::time_point at;
		};
		std::mutex                                           lock;
		std::vector<NotedEvent>                              noted;           // attackers' events to say
		std::unordered_map<RE::FormID, Clock::time_point>    lastPairEvent;   // each actor's last pair event
		std::atomic<bool>                                    anyPairEvent{ false };
		std::atomic<std::uint64_t>                           eventsKept{ 0 };  // graph events not sent: an actor in a pair
		std::atomic<std::uint64_t>                           notedLost{ 0 };
		std::array<std::atomic<std::uint64_t>, 4>            heardByRole{};    // none, attacker, victim, end

		// ---- actors held by pairs played here: written by the tick, read by the update hooks with no lock ----
		PeaceRules::IdSet<PairRules::MAX_HELD> heldIds;
		std::atomic<bool>                      anyHeld{ false };

		// ---- the tick's own ----
		struct Played
		{
			std::uint32_t                  sender = 0;
			RE::ActorHandle                attacker;
			RE::ActorHandle                victim;
			RE::FormID                     attackerId = 0;
			RE::FormID                     victimId = 0;
			std::uint32_t                  attackerPlayer = 0;  // a stand-in's player (0: a copy or an NPC we run)
			std::uint32_t                  victimPlayer = 0;
			bool                           kill = false;
			bool                           took = false;
			std::optional<bool>            inNow;  // Tend: either actor in it now (none: an actor is gone)
			Clock::time_point              since;
			std::string                    event;
		};
		std::vector<Played> played;

		PairRules::Outbox     outbox;
		PairRules::Inbox      inbox;
		PairRules::EventIndex index;

		// The load order's paired idles (an attacker's event), for `pairs find`.
		struct Idle
		{
			RE::FormID  id = 0;
			std::string event;
			std::string editor;
		};
		std::vector<Idle> idles;

		std::unordered_map<RE::FormID, Clock::time_point> flagSince;  // ours in a kill move, since
		std::unordered_set<RE::FormID>                    flagTried;  // ... and judged by its flag once

		struct Counts
		{
			std::uint64_t byEvent = 0, byFlag = 0, ambiguous = 0, notPaired = 0, noPartner = 0, victimStandIn = 0, unshared = 0, gone = 0;
			std::uint64_t heard = 0, failed = 0, second = 0, took = 0, ended = 0, noTake = 0, gaveUp = 0, released = 0, playedOnOurs = 0;
			PairRules::KindCounts    said;
			PairRules::KindCounts    playedKinds;
			PairRules::VerdictCounts verdicts;
		};
		Counts counts;

		std::string lastSaid = "none";
		std::string lastPlayed = "none";

		std::string SideText(const Side& a_side)
		{
			if (a_side.player != 0) {
				return std::format("player:{}", a_side.player);
			}
			return a_side.npc.IsSpawn() ? std::format("npc:{:08X}@{}", a_side.npc.id, a_side.npc.spawner) : std::format("npc:{:08X}", a_side.npc.id);
		}

		std::string PairText(const Protocol::Pair& a_pair)
		{
			return std::format("attacker={} victim={} idle={:08X} event={} kind={}", SideText(a_pair.attacker), SideText(a_pair.victim), a_pair.idle, a_pair.event,
				PairRules::NameOf(PairRules::KindOf(a_pair.flags)));
		}

		void Publish()
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(played.size() * 2);
			for (const auto& pair : played) {
				ids.push_back(pair.attackerId);
				ids.push_back(pair.victimId);
			}
			if (!heldIds.Assign(ids)) {
				logs::warn("Pairs: more actors held by pairs than {}: the rest are placed by their states", PairRules::MAX_HELD);
			}
			anyHeld.store(!played.empty(), std::memory_order_release);
		}

		void LetGoAll()
		{
			counts.released += played.size();
			played.clear();
			Publish();
		}

		// ---- the actors ----

		// The idle a_actor's process played last (none: no process, or none played).
		RE::TESIdleForm* LastIdle(RE::Actor* a_actor)
		{
			const auto process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			return process && process->middleHigh ? process->middleHigh->lastIdlePlayed : nullptr;
		}

		std::string_view EventOf(const RE::TESIdleForm* a_idle)
		{
			return a_idle && a_idle->animEventName.c_str() ? std::string_view{ a_idle->animEventName.c_str() } : std::string_view{};
		}

		// The other actor of a pair a_actor is in: its process's target.
		RE::NiPointer<RE::Actor> PartnerOf(RE::Actor* a_actor)
		{
			const auto process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			if (!process || process->target == 0) {
				return {};
			}
			auto partner = RE::Actor::LookupByHandle(process->target);
			return partner && partner.get() != a_actor ? partner : RE::NiPointer<RE::Actor>{};
		}

		// The side a_actor is as this game says its own: our player, or an NPC this game runs.
		std::optional<Side> OursOf(RE::Actor* a_actor, RE::PlayerCharacter* a_player)
		{
			if (a_actor == a_player) {
				return Side{ .player = ourId.load() };
			}
			if (const auto key = NpcSync::KeyOf(a_actor); key && NpcSync::RunnerOf(*key) == ourId.load()) {
				return Side{ .npc = *key };
			}
			return std::nullopt;
		}

		enum class How : std::uint8_t
		{
			kEvent,
			kFlag,
		};

		// a_actor (ours) began a pair, by a_event: said, with its partner as the session names it.
		void Say(RE::Actor* a_actor, const Side& a_side, std::string_view a_event, How a_how, Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto partner = PartnerOf(a_actor);
			if (!partner) {
				++counts.noPartner;
				return;
			}
			Side victim;
			if (partner.get() == player) {
				victim.player = ourId.load();
			} else if (RemotePlayers::PlayerOf(partner.get()) != 0) {
				// A friend's stand-in: no actor here fights one (StandInPeace), so none begins a pair on one.
				++counts.victimStandIn;
				return;
			} else if (const auto key = NpcSync::KeyOf(partner.get())) {
				victim.npc = *key;
			} else {
				++counts.unshared;  // an actor the session does not share (a quest's, made as it ran)
				return;
			}
			Protocol::Pair pair{ .attacker = a_side, .victim = victim, .sentMs = Protocol::SentMs(a_now), .event = std::string{ a_event } };
			if (pair.event.size() > Protocol::MAX_EVENT_NAME) {
				pair.event.resize(Protocol::MAX_EVENT_NAME);
			}
			// The idle: the one its process played last when it is this event's; else the load order's by the event.
			if (const auto last = LastIdle(a_actor); last && PairRules::Lower(EventOf(last)) == PairRules::Lower(a_event)) {
				pair.idle = last->GetFormID();
			} else {
				pair.idle = index.Find(a_event).value_or(0);
			}
			const bool kill = a_actor->IsInKillMove() || partner->IsInKillMove() || PairRules::Lower(a_event).find("killmove") != std::string::npos;
			pair.flags = static_cast<std::uint8_t>((kill ? Protocol::kPairKill : 0) | (a_actor->IsSneaking() ? Protocol::kPairSneak : 0));
			const auto at = a_actor->GetPosition();
			pair.x = at.x;
			pair.y = at.y;
			pair.z = at.z;
			pair.heading = a_actor->GetAngleZ();
			if (!Protocol::PairHolds(pair)) {
				return;
			}
			if (outbox.Say(pair, a_now)) {
				++(a_how == How::kEvent ? counts.byEvent : counts.byFlag);
				counts.said.Add(PairRules::KindOf(pair.flags));
				lastSaid = PairText(pair);
				logs::info("Pairs: said {}{}", lastSaid, a_how == How::kFlag ? " (by its kill move flag)" : "");
			}
		}

		void SayOurs(const SendFn& a_send, Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			std::vector<NotedEvent> fresh;
			{
				std::scoped_lock l{ lock };
				fresh.swap(noted);
				std::erase_if(lastPairEvent, [&](const auto& a_entry) { return a_now - a_entry.second > PairRules::EVENT_HOLD; });
				anyPairEvent.store(!lastPairEvent.empty(), std::memory_order_release);
			}
			for (const auto& one : fresh) {
				const auto actor = RE::TESForm::LookupByID<RE::Actor>(one.id);
				if (!actor) {
					++counts.gone;
					continue;
				}
				if (const auto side = OursOf(actor, player)) {
					Say(actor, *side, one.event, How::kEvent, a_now);
				}
			}
			// Ours in a kill move whose event was never heard: said by the idle its process played last.
			std::vector<RE::NiPointer<RE::Actor>> ours;
			if (player) {
				ours.emplace_back(player);
			}
			for (auto& [key, actor] : NpcSync::RunActors()) {
				if (actor) {
					ours.push_back(actor);
				}
			}
			std::unordered_set<RE::FormID> inKill;
			for (const auto& actor : ours) {
				const auto id = actor->GetFormID();
				if (!actor->IsInKillMove()) {
					continue;
				}
				inKill.insert(id);
				const auto since = flagSince.try_emplace(id, a_now).first->second;
				const auto side = OursOf(actor.get(), player);
				if (!side || flagTried.contains(id) || a_now - since < PairRules::FLAG_GRACE || outbox.SaidLately(*side, a_now) || outbox.VictimLately(*side, a_now)) {
					continue;
				}
				flagTried.insert(id);
				const auto own = LastIdle(actor.get());
				const auto partner = PartnerOf(actor.get());
				const auto other = partner ? LastIdle(partner.get()) : nullptr;
				switch (PairRules::JudgeByFlag(own ? own->GetFormID() : 0, EventOf(own), other ? other->GetFormID() : 0)) {
				case PairRules::ByFlag::kSay:
					Say(actor.get(), *side, EventOf(own), How::kFlag, a_now);
					break;
				case PairRules::ByFlag::kAmbiguous:
					++counts.ambiguous;
					break;
				case PairRules::ByFlag::kNotPaired:
					++counts.notPaired;
					break;
				}
			}
			std::erase_if(flagSince, [&](const auto& a_entry) { return !inKill.contains(a_entry.first); });
			std::erase_if(flagTried, [&](RE::FormID a_id) { return !inKill.contains(a_id); });
			outbox.Prune(a_now);
			for (auto& msg : outbox.Take()) {
				a_send(Protocol::Encode(msg), true);
			}
		}

		// ---- a friend's ----

		struct Here
		{
			Shown                    shown = Shown::kNone;
			RE::NiPointer<RE::Actor> actor;
			std::uint32_t            player = 0;  // a stand-in's player
		};

		Here HereOf(const Side& a_side)
		{
			Here here;
			if (a_side.player != 0) {
				if (a_side.player == ourId.load()) {
					here.shown = Shown::kOurPlayer;
					here.actor = RE::NiPointer<RE::Actor>{ RE::PlayerCharacter::GetSingleton() };
					return here;
				}
				here.actor = RemotePlayers::StandInOf(a_side.player);
				here.shown = here.actor ? Shown::kStandIn : Shown::kNone;
				here.player = a_side.player;
			} else {
				here.actor = NpcSync::ActorOf(a_side.npc);
				here.shown = !here.actor ? Shown::kNone : NpcSync::RunnerOf(a_side.npc) == ourId.load() ? Shown::kRunHere : Shown::kCopy;
			}
			if (here.actor && (here.actor->IsDisabled() || !here.actor->Is3DLoaded())) {
				here = {};
			}
			return here;
		}

		bool Busy(RE::Actor* a_actor)
		{
			return a_actor->IsInKillMove() || Held(a_actor) || Furniture::Held(a_actor) || Riding::Seated(a_actor) || a_actor->IsOnMount();
		}

		RE::TESIdleForm* IdleFor(const Protocol::Pair& a_pair)
		{
			if (a_pair.idle != 0) {
				if (const auto idle = RE::TESForm::LookupByID<RE::TESIdleForm>(a_pair.idle)) {
					return idle;
				}
			}
			const auto id = index.Find(a_pair.event);
			return id ? RE::TESForm::LookupByID<RE::TESIdleForm>(*id) : nullptr;
		}

		void Play(std::uint32_t a_sender, const Protocol::Pair& a_pair)
		{
			const auto attacker = HereOf(a_pair.attacker);
			const auto victim = HereOf(a_pair.victim);
			const auto idle = IdleFor(a_pair);
			PairRules::Facts facts{ .attacker = attacker.shown, .victim = victim.shown, .kill = (a_pair.flags & Protocol::kPairKill) != 0, .idleFound = idle != nullptr };
			if (attacker.actor && victim.actor) {
				facts.attackerAlive = !attacker.actor->IsDead();
				facts.victimAlive = !victim.actor->IsDead();
				facts.attackerBusy = Busy(attacker.actor.get());
				facts.victimBusy = Busy(victim.actor.get());
				facts.apart = attacker.actor->GetPosition().GetDistance(victim.actor->GetPosition());
				facts.drift = attacker.actor->GetPosition().GetDistance({ a_pair.x, a_pair.y, a_pair.z });
			}
			const auto verdict = PairRules::Judge(facts);
			counts.verdicts.Add(verdict);
			if (verdict != Verdict::kPlay) {
				const auto why = PairRules::NameOf(verdict);  // (outside the log call: its guard reads "NameOf(" as a player's name)
				logs::info("Pairs: not played ({}): {} from player {}", why, PairText(a_pair), a_sender);
				return;
			}
			const auto process = attacker.actor->GetActorRuntimeData().currentProcess;
			bool       ok = process && process->PlayIdle(attacker.actor.get(), idle, victim.actor.get());
			if (!ok && process) {
				ok = process->SetupSpecialIdle(attacker.actor.get(), RE::DEFAULT_OBJECT::kActionIdle, idle, true, true, victim.actor.get());
				counts.second += ok ? 1 : 0;
			}
			if (!ok) {
				++counts.failed;
				logs::info("Pairs: the engine would not play {} from player {}", PairText(a_pair), a_sender);
				return;
			}
			counts.playedKinds.Add(PairRules::KindOf(a_pair.flags));
			counts.playedOnOurs += victim.shown == Shown::kRunHere ? 1 : 0;
			played.push_back({ .sender = a_sender,
				.attacker = attacker.actor->GetHandle(),
				.victim = victim.actor->GetHandle(),
				.attackerId = attacker.actor->GetFormID(),
				.victimId = victim.actor->GetFormID(),
				.attackerPlayer = attacker.player,
				.victimPlayer = victim.player,
				.kill = facts.kill,
				.since = Clock::now(),
				.event = a_pair.event });
			Publish();
			lastPlayed = std::format("{} on {:08X} and {:08X}", PairText(a_pair), attacker.actor->GetFormID(), victim.actor->GetFormID());
			logs::info("Pairs: played {} from player {}", lastPlayed, a_sender);
		}

		// The pairs played here, each let go when it is over.
		void Tend(Clock::time_point a_now)
		{
			const auto before = played.size();
			// Each pair's actors read first (whether either is in it now), then the pairs that are over go.
			for (auto& pair : played) {
				const auto attacker = pair.attacker.get();
				const auto victim = pair.victim.get();
				if (!attacker || !victim || attacker->GetFormID() != pair.attackerId || victim->GetFormID() != pair.victimId) {
					pair.inNow.reset();
					continue;
				}
				const bool in = attacker->IsInKillMove() || victim->IsInKillMove();
				if (in && !pair.took) {
					pair.took = true;
					++counts.took;
				}
				pair.inNow = in;
			}
			std::erase_if(played, [&](const Played& a_pair) {
				const auto in = a_pair.inNow;
				if (!in) {
					++counts.released;
					return true;
				}
				switch (PairRules::HoldStep(a_pair.since, a_now, a_pair.kill, *in, a_pair.took)) {
				case PairRules::Hold::kHold:
					return false;
				case PairRules::Hold::kEnded:
					++counts.ended;
					return true;
				case PairRules::Hold::kNoTake:
					++counts.noTake;
					logs::info("Pairs: {} on {:08X} and {:08X} never took (neither in a kill move); let go", a_pair.event, a_pair.attackerId, a_pair.victimId);
					return true;
				case PairRules::Hold::kGaveUp:
					++counts.gaveUp;
					logs::info("Pairs: {} on {:08X} and {:08X} still playing after {} s; let go", a_pair.event, a_pair.attackerId, a_pair.victimId,
						std::chrono::duration_cast<std::chrono::seconds>(PairRules::MAX_HOLD).count());
					return true;
				}
				return true;
			});
			if (played.size() != before) {
				Publish();
			}
		}

		std::string Report()
		{
			return std::format(
				"pairs ours attackerEvents={} victimEvents={} endEvents={} said={} byEvent={} byFlag={} saidKinds={} coalesced={} outboxDropped={} ambiguous={} notPaired={} "
				"noPartner={} victimStandIn={} unshared={} gone={} notedLost={} eventsKept={} | theirs heard={} played={} playedKinds={} refused={} failed={} second={} "
				"took={} ended={} noTake={} gaveUp={} released={} playedOnOurs={} heldNow={} waiting={} late={} dropped={} | idles={} events={}",
				heardByRole[1].load(), heardByRole[2].load(), heardByRole[3].load(), outbox.said, counts.byEvent, counts.byFlag, counts.said.Line(), outbox.coalesced,
				outbox.dropped, counts.ambiguous, counts.notPaired, counts.noPartner, counts.victimStandIn, counts.unshared, counts.gone, notedLost.load(), eventsKept.load(),
				counts.heard, counts.playedKinds.Total(), counts.playedKinds.Line(), counts.verdicts.Refused(), counts.failed, counts.second, counts.took, counts.ended,
				counts.noTake, counts.gaveUp, counts.released, counts.playedOnOurs, played.size(), inbox.Size(), inbox.late, inbox.dropped, idles.size(), index.Size());
		}

		std::string List()
		{
			const auto now = Clock::now();
			std::string out = std::format("held={}", played.size());
			for (const auto& pair : played) {
				const auto attacker = pair.attacker.get();
				const auto victim = pair.victim.get();
				out += std::format(" | event={} attacker={:08X} victim={:08X} from={} ageMs={} kill={} took={} inA={} inV={} deadV={}", pair.event, pair.attackerId, pair.victimId,
					pair.sender, std::chrono::duration_cast<std::chrono::milliseconds>(now - pair.since).count(), pair.kill ? 1 : 0, pair.took ? 1 : 0,
					attacker && attacker->IsInKillMove() ? 1 : 0, victim && victim->IsInKillMove() ? 1 : 0, victim && victim->IsDead() ? 1 : 0);
			}
			return out;
		}

		std::string Find(std::string_view a_kind)
		{
			const auto wanted = [&](const Idle& a_idle) {
				const auto event = PairRules::Lower(a_idle.event);
				if (a_kind == "kill") {
					return event.find("killmove") != std::string::npos;
				}
				if (a_kind == "feed") {
					return event.find("feed") != std::string::npos;
				}
				return true;
			};
			const Idle* best = nullptr;
			std::size_t of = 0;
			for (const auto& idle : idles) {
				if (wanted(idle)) {
					++of;
					if (!best || idle.id < best->id) {
						best = &idle;
					}
				}
			}
			if (!best) {
				return std::format("none of=0 idles={}", idles.size());
			}
			return std::format("idle id={:08X} event={} editor={} of={}", best->id, best->event, best->editor.empty() ? "-" : best->editor, of);
		}

		// `pairs play <idle> <victim> [<attacker>]`: the rig's set-up (an injected attack).
		std::string PlayCommand(const std::vector<std::string_view>& a_words)
		{
			if (a_words.size() < 3 || a_words.size() > 4) {
				return "usage: pairs play <idle id> <victim local id> [<attacker local id>]";
			}
			const auto idleId = FormsCommand::ParseHex(a_words[1]);
			const auto victimId = FormsCommand::ParseHex(a_words[2]);
			const auto attackerId = a_words.size() == 4 ? FormsCommand::ParseHex(a_words[3]) : std::optional<std::uint32_t>{ 0x14 };
			if (!idleId || !victimId || !attackerId) {
				return "usage: pairs play <idle id> <victim local id> [<attacker local id>] (hex)";
			}
			const auto idle = RE::TESForm::LookupByID<RE::TESIdleForm>(*idleId);
			const auto victim = RE::TESForm::LookupByID<RE::Actor>(*victimId);
			const auto attacker = RE::TESForm::LookupByID<RE::Actor>(*attackerId);
			if (!idle) {
				return std::format("refused: no idle {:08X}", *idleId);
			}
			if (!victim || !attacker || victim == attacker) {
				return "refused: no such actors";
			}
			if (victim->IsDead() || attacker->IsDead() || !victim->Is3DLoaded() || !attacker->Is3DLoaded()) {
				return "refused: an actor is dead or not loaded";
			}
			const auto process = attacker->GetActorRuntimeData().currentProcess;
			bool       ok = process && process->PlayIdle(attacker, idle, victim);
			if (!ok && process) {
				ok = process->SetupSpecialIdle(attacker, RE::DEFAULT_OBJECT::kActionIdle, idle, true, true, victim);
			}
			logs::info("Pairs: the developer channel played {:08X} ({}) on {:08X} with {:08X}: {}", idle->GetFormID(), EventOf(idle), attacker->GetFormID(), victim->GetFormID(),
				ok ? "played" : "refused by the engine");
			return ok ? std::format("played idle={:08X} event={} attacker={:08X} victim={:08X}", idle->GetFormID(), EventOf(idle), attacker->GetFormID(), victim->GetFormID()) :
			            "refused: the engine would not play it";
		}
	}

	bool Install()
	{
		idles.clear();
		index.Clear();
		if (const auto data = RE::TESDataHandler::GetSingleton()) {
			for (const auto idle : data->GetFormArray<RE::TESIdleForm>()) {
				const auto event = EventOf(idle);
				if (!idle || PairRules::RoleOf(event) != PairRules::Role::kAttacker) {
					continue;
				}
				const auto editor = idle->GetFormEditorID();
				idles.push_back({ idle->GetFormID(), std::string{ event }, editor ? std::string{ editor } : std::string{} });
				index.Add(event, idle->GetFormID());
			}
		}
		std::ranges::sort(idles, {}, &Idle::id);
		if (idles.empty()) {
			logs::warn("Pairs: no paired idle in the load order (no kill move's event found): a friend's kill moves are played only by their idle record");
			return false;
		}
		logs::info("Pairs: {} paired idles in the load order, {} events", idles.size(), index.Size());
		return true;
	}

	void Started(std::uint32_t a_ourId)
	{
		ourId = a_ourId;
		active = true;
	}

	void Frame(const SendFn& a_send)
	{
		if (!active) {
			return;
		}
		const auto now = Clock::now();
		SayOurs(a_send, now);
		for (auto& due : inbox.Take(now - StateBuffer::Buffer::Delay())) {
			Play(due.sender, due.pair);
		}
		Tend(now);
	}

	void Heard(const Protocol::Pairs& a_msg)
	{
		if (!active) {
			return;
		}
		const auto now = Clock::now();
		for (const auto& pair : a_msg.pairs) {
			++counts.heard;
			const auto at = pair.attacker.player != 0 ? RemotePlayers::SenderTime(pair.attacker.player, pair.sentMs) : NpcSync::SenderTime(pair.attacker.npc, pair.sentMs);
			inbox.Push(pair, a_msg.sender, at, now);
		}
	}

	void Left(std::uint32_t a_playerId)
	{
		inbox.Forget(a_playerId);
		const auto before = played.size();
		std::erase_if(played, [&](const Played& a_pair) { return a_pair.sender == a_playerId || a_pair.attackerPlayer == a_playerId || a_pair.victimPlayer == a_playerId; });
		if (played.size() != before) {
			counts.released += before - played.size();
			Publish();
		}
	}

	void Clear()
	{
		LetGoAll();
		outbox.Clear();
		inbox.Clear();
		flagSince.clear();
		flagTried.clear();
		{
			std::scoped_lock l{ lock };
			noted.clear();
			lastPairEvent.clear();
			anyPairEvent = false;
		}
		active = false;
		ourId = 0;
	}

	void ForgetWorld()
	{
		LetGoAll();
		inbox.Clear();
		flagSince.clear();
		flagTried.clear();
		std::scoped_lock l{ lock };
		noted.clear();
		lastPairEvent.clear();
		anyPairEvent = false;
	}

	void BeforeSave()
	{
		LetGoAll();
	}

	void Release(RE::Actor* a_actor)
	{
		if (!a_actor || played.empty()) {
			return;
		}
		const auto id = a_actor->GetFormID();
		const auto before = played.size();
		std::erase_if(played, [&](const Played& a_pair) { return a_pair.attackerId == id || a_pair.victimId == id; });
		if (played.size() != before) {
			counts.released += before - played.size();
			Publish();
		}
	}

	void Noted(RE::Actor* a_actor, std::string_view a_event)
	{
		if (!a_actor || !active.load(std::memory_order_relaxed)) {
			return;
		}
		const auto role = PairRules::RoleOf(a_event);
		if (role == PairRules::Role::kNone) {
			return;
		}
		heardByRole[static_cast<std::size_t>(role)].fetch_add(1, std::memory_order_relaxed);
		std::scoped_lock l{ lock };
		const auto now = Clock::now();
		lastPairEvent[a_actor->GetFormID()] = now;
		anyPairEvent.store(true, std::memory_order_release);
		if (role != PairRules::Role::kAttacker) {
			return;
		}
		if (noted.size() >= MAX_NOTED) {
			notedLost.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		noted.push_back({ a_actor->GetFormID(), std::string{ a_event }, now });
	}

	bool EventsStay(RE::Actor* a_actor)
	{
		if (!a_actor || !active.load(std::memory_order_relaxed)) {
			return false;
		}
		std::optional<Clock::time_point> last;
		if (anyPairEvent.load(std::memory_order_acquire)) {
			std::scoped_lock l{ lock };
			if (const auto it = lastPairEvent.find(a_actor->GetFormID()); it != lastPairEvent.end()) {
				last = it->second;
			}
		}
		if (PairRules::EventsStayHere(a_actor->IsInKillMove(), last, Held(a_actor), Clock::now())) {
			eventsKept.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		return false;
	}

	bool Held(const RE::Actor* a_actor)
	{
		return a_actor && anyHeld.load(std::memory_order_acquire) && heldIds.Contains(a_actor->GetFormID());
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (words.empty()) {
			return Report();
		}
		if (words[0] == "last") {
			return std::format("said {} | played {}", lastSaid, lastPlayed);
		}
		if (words[0] == "list") {
			return List();
		}
		if (words[0] == "find") {
			if (words.size() != 2 || (words[1] != "kill" && words[1] != "feed" && words[1] != "any")) {
				return "usage: pairs find <kill|feed|any>";
			}
			return Find(words[1]);
		}
		if (words[0] == "play") {
			return PlayCommand(words);
		}
		return "usage: pairs [last | list | find <kill|feed|any> | play <idle> <victim> [attacker]]";
	}
}
