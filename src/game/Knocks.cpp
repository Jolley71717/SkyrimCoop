#include "game/Knocks.h"

#include "DevLine.h"
#include "game/Furniture.h"
#include "game/KnockRules.h"
#include "game/NpcSync.h"
#include "game/Pairs.h"
#include "game/PeaceRules.h"
#include "game/RemotePlayers.h"
#include "game/Riding.h"
#include "game/StateBuffer.h"
#include "net/FormsCommand.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Knocks
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;
		using Side = Protocol::PairSide;
		using Phase = KnockRules::Phase;
		using Shown = KnockRules::Shown;
		using Verdict = KnockRules::Verdict;
		using Vec = KnockRules::Vec;
		using Kind = Protocol::KnockKind;

		std::atomic<bool>          active{ false };
		std::atomic<std::uint32_t> ourId{ 0 };

		// ---- actors held by knocks here: written by the tick, read by the update hooks with no lock ----
		PeaceRules::IdSet<KnockRules::MAX_HELD> heldIds;
		std::atomic<bool>                       anyHeld{ false };
		std::atomic<std::uint64_t>              eventsKept{ 0 };

		enum class From : std::uint8_t
		{
			kPlayed,  // a friend's down, played here
			kNative,  // this game's world knocked it (an explosion, our player's push, a fall here)
			kJoined,  // knocked here already when the friend's down came
		};

		constexpr std::string_view NameOf(From a_from)
		{
			return a_from == From::kPlayed ? "played" : a_from == From::kNative ? "native" : "joined";
		}

		struct Hold
		{
			RE::ActorHandle                  handle;
			RE::FormID                       id = 0;
			Side                             side;        // what the actor shows: a friend's player, an NPC (none: unknown)
			std::uint32_t                    sender = 0;  // whose down it is (0: none heard: a native knock)
			From                             from = From::kPlayed;
			Kind                             kind = Kind::kPush;
			Clock::time_point                since;
			bool                             heardDown = false;
			std::optional<Clock::time_point> upAt;
			Vec                              rest;  // where the friend's actor rests (its up)
			bool                             everKnocked = false;
			std::optional<Clock::time_point> standingSince;
			bool                             kept = false;  // its Paralysis value raised by this game (KEPT)
			std::optional<Clock::time_point> paralyzeCallAt;  // a paralysis by value: the engine's knock if not down by then
			// A push played here: where it was, how hard, how fast its friend's was thrown; measured once (Scale).
			Vec                              playedAt;
			float                            magnitude = 0.0f;
			float                            wanted = 0.0f;
			bool                             inPlace = false;
			bool                             measured = false;
			bool                             asked = false;  // a native knock of a copy, asked of its runner
		};
		std::vector<Hold> holds;

		constexpr float KEPT = 1.0f;  // the Paralysis value a kept actor is raised by

		KnockRules::Watch         ours;     // our player's and our NPCs' knocks
		KnockRules::Watch         natives;  // this world's knocks of copies and stand-ins
		KnockRules::Inbox         inbox;
		KnockRules::Scale         scale;
		KnockRules::VerdictCounts verdicts;
		KnockRules::AskCounts     askVerdicts;
		std::vector<Protocol::Knock> outgoing;

		struct Ask
		{
			std::uint32_t   sender = 0;
			Protocol::Knock knock;
		};
		std::vector<Ask> asks;  // heard, applied by the next tick

		std::unordered_map<RE::FormID, float> devKept;  // `knocks paralyze`: what it raised, per actor

		struct Counts
		{
			std::uint64_t saidDown = 0, saidUp = 0, saidPush = 0, saidParalysis = 0;
			float         launchMax = 0.0f;
			std::uint64_t heard = 0, upHeard = 0, upUnknown = 0, playedPush = 0, playedParalysis = 0, inPlace = 0, failed = 0, joined = 0, took = 0;
			std::uint64_t keptDown = 0, byValue = 0, byCall = 0, letUp = 0, ended = 0, noTake = 0, gaveUp = 0, released = 0, died = 0, handedOver = 0, holdFull = 0;
			std::uint64_t nativeCopies = 0, nativeStandIns = 0, asked = 0, askHeard = 0, askApplied = 0, savedBack = 0;
			float         restGapLast = -1.0f, restGapMax = -1.0f, speedLast = -1.0f, wantedLast = -1.0f;
		};
		Counts counts;

		std::string lastSaid = "none";
		std::string lastPlayed = "none";
		std::string lastAsked = "none";
		std::string lastHold = "none";

		std::string SideText(const Side& a_side)
		{
			if (a_side.player != 0) {
				return std::format("player:{}", a_side.player);
			}
			if (a_side.npc.id == 0) {
				return "none";
			}
			return a_side.npc.IsSpawn() ? std::format("npc:{:08X}@{}", a_side.npc.id, a_side.npc.spawner) : std::format("npc:{:08X}", a_side.npc.id);
		}

		std::string KnockText(const Protocol::Knock& a_knock)
		{
			const auto phase = a_knock.phase == Protocol::KnockPhase::kDown ? "down" : a_knock.phase == Protocol::KnockPhase::kUp ? "up" : "ask";
			return std::format("actor={} phase={} kind={} at=({:.0f},{:.0f},{:.0f}) speed={:.0f}", SideText(a_knock.actor), phase, KnockRules::NameOf(a_knock.kind), a_knock.x,
				a_knock.y, a_knock.z, KnockRules::Length({ a_knock.vx, a_knock.vy, a_knock.vz }));
		}

		Vec PosOf(const RE::Actor* a_actor)
		{
			const auto at = a_actor->GetPosition();
			return { at.x, at.y, at.z };
		}

		std::uint32_t KnockStateOf(RE::Actor* a_actor)
		{
			const auto state = a_actor->AsActorState();
			return state ? static_cast<std::uint32_t>(state->GetKnockState()) : 0;
		}

		float ParalysisOf(RE::Actor* a_actor)
		{
			const auto values = a_actor->AsActorValueOwner();
			return values ? values->GetActorValue(RE::ActorValue::kParalysis) : 0.0f;
		}

		void RaiseParalysis(RE::Actor* a_actor, float a_by)
		{
			if (const auto values = a_actor->AsActorValueOwner()) {
				values->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kParalysis, a_by);
			}
		}

		bool Busy(RE::Actor* a_actor)
		{
			return a_actor->IsInKillMove() || Pairs::Held(a_actor) || Furniture::Held(a_actor) || Riding::Seated(a_actor) || a_actor->IsOnMount();
		}

		// The engine's knock: a_actor thrown away from a_push.from as hard as a_push.magnitude. False with no process.
		bool KnockBy(RE::Actor* a_actor, const KnockRules::Push& a_push)
		{
			const auto process = a_actor->GetActorRuntimeData().currentProcess;
			if (!process) {
				return false;
			}
			process->KnockExplosion(a_actor, RE::NiPoint3{ a_push.from.x, a_push.from.y, a_push.from.z }, a_push.magnitude);
			return true;
		}

		bool KnockParalyze(RE::Actor* a_actor)
		{
			const auto process = a_actor->GetActorRuntimeData().currentProcess;
			if (!process) {
				return false;
			}
			process->KnockParalyze(a_actor);
			return true;
		}

		void Publish()
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(holds.size());
			for (const auto& hold : holds) {
				ids.push_back(hold.id);
			}
			if (!heldIds.Assign(ids)) {
				logs::warn("Knocks: more actors held by knocks than {}: the rest are placed by their states", KnockRules::MAX_HELD);
			}
			anyHeld.store(!holds.empty(), std::memory_order_release);
		}

		Hold* HoldOf(RE::FormID a_id)
		{
			const auto it = std::ranges::find(holds, a_id, &Hold::id);
			return it != holds.end() ? &*it : nullptr;
		}

		// Its Paralysis value given back, when this game raised it (the actor still the one held).
		void TakeBack(Hold& a_hold)
		{
			if (!a_hold.kept) {
				return;
			}
			a_hold.kept = false;
			if (const auto actor = a_hold.handle.get(); actor && actor->GetFormID() == a_hold.id) {
				RaiseParalysis(actor.get(), -KEPT);
				++counts.letUp;
			}
		}

		void Keep(Hold& a_hold, RE::Actor* a_actor)
		{
			RaiseParalysis(a_actor, KEPT);
			a_hold.kept = true;
		}

		void Summarise(const Hold& a_hold, RE::Actor* a_actor, std::string_view a_end, Clock::time_point a_now)
		{
			float gap = -1.0f;
			if (a_actor && a_hold.upAt) {
				gap = KnockRules::Distance(PosOf(a_actor), a_hold.rest);
				counts.restGapLast = gap;
				counts.restGapMax = (std::max)(counts.restGapMax, gap);
			}
			lastHold = std::format("actor={:08X} side={} from={} kind={} heard={} up={} everKnocked={} restGap={} ms={} end={}", a_hold.id, SideText(a_hold.side),
				NameOf(a_hold.from), KnockRules::NameOf(a_hold.kind), a_hold.heardDown ? 1 : 0, a_hold.upAt ? 1 : 0, a_hold.everKnocked ? 1 : 0,
				gap < 0.0f ? std::string{ "none" } : std::format("{:.0f}", gap), std::chrono::duration_cast<std::chrono::milliseconds>(a_now - a_hold.since).count(), a_end);
		}

		void LetGoAll(bool a_giveBack)
		{
			for (auto& hold : holds) {
				if (a_giveBack) {
					TakeBack(hold);
				}
			}
			counts.released += holds.size();
			holds.clear();
			Publish();
		}

		// ---- ours ----

		void SayOurs(Clock::time_point a_now)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			std::vector<std::pair<Side, RE::NiPointer<RE::Actor>>> actors;
			if (player) {
				actors.emplace_back(Side{ .player = ourId.load() }, RE::NiPointer<RE::Actor>{ player });
			}
			for (auto& [key, actor] : NpcSync::RunActors()) {
				if (actor) {
					actors.emplace_back(Side{ .npc = key }, std::move(actor));
				}
			}
			std::unordered_set<std::uint32_t> seen;
			for (const auto& [side, actor] : actors) {
				const auto id = actor->GetFormID();
				if (actor->IsDead() || !actor->Is3DLoaded()) {
					continue;  // a death is its own word (NpcSync); an unloaded actor is no one's knock
				}
				seen.insert(id);
				const auto state = KnockStateOf(actor.get());
				for (const auto& said : ours.Step(id, KnockRules::PhaseOf(state), PosOf(actor.get()), ParalysisOf(actor.get()) > 0.0f, a_now)) {
					auto knock = KnockRules::ToWire(side, said, Protocol::SentMs(said.when));
					if (!Protocol::KnockHolds(knock)) {
						continue;
					}
					if (said.phase == Protocol::KnockPhase::kDown) {
						++counts.saidDown;
						++(said.kind == Kind::kParalysis ? counts.saidParalysis : counts.saidPush);
						counts.launchMax = (std::max)(counts.launchMax, KnockRules::Length(said.launch));
					} else {
						++counts.saidUp;
					}
					lastSaid = KnockText(knock);
					logs::info("Knocks: said {} ({:08X})", lastSaid, id);
					outgoing.push_back(knock);
				}
			}
			ours.ForgetIf([&](std::uint32_t a_id) { return !seen.contains(a_id); });
		}

		// ---- this world's knocks of copies and stand-ins ----

		void WatchNatives(Clock::time_point a_now)
		{
			struct Seen
			{
				RE::NiPointer<RE::Actor> actor;
				Side                     side;
				bool                     copy = false;
			};
			std::vector<Seen> shown;
			for (auto& [key, actor] : NpcSync::CopyActors()) {
				shown.push_back({ std::move(actor), Side{ .npc = key }, true });
			}
			for (auto& [player, actor] : RemotePlayers::StandIns()) {
				shown.push_back({ std::move(actor), Side{ .player = player }, false });
			}
			std::unordered_set<std::uint32_t> seen;
			bool                              changed = false;
			for (const auto& one : shown) {
				const auto id = one.actor->GetFormID();
				auto*      hold = HoldOf(id);
				if ((hold && hold->from != From::kNative) || one.actor->IsDead() || !one.actor->Is3DLoaded()) {
					continue;  // a friend's knock played here (or joined): not this world's
				}
				seen.insert(id);
				const auto phase = KnockRules::PhaseOf(KnockStateOf(one.actor.get()));
				const bool paralysed = ParalysisOf(one.actor.get()) > 0.0f;
				// Held from the moment it is knocked: its states would pull it out of the engine's ragdoll.
				if (phase != Phase::kStanding && !hold) {
					if (holds.size() >= KnockRules::MAX_HELD) {
						++counts.holdFull;
					} else {
						holds.push_back({ .handle = one.actor->GetHandle(),
							.id = id,
							.side = one.side,
							.from = From::kNative,
							.kind = paralysed ? Kind::kParalysis : Kind::kPush,
							.since = a_now,
							.everKnocked = true });
						hold = &holds.back();
						changed = true;
						++(one.copy ? counts.nativeCopies : counts.nativeStandIns);
						logs::info("Knocks: {} {:08X} knocked by this game's world ({}): held", one.copy ? "the copy" : "the stand-in", id, SideText(one.side));
					}
				}
				for (const auto& said : natives.Step(id, phase, PosOf(one.actor.get()), paralysed, a_now)) {
					// A copy's push is asked of its runner (a stand-in's player's game decides its own; a paralysis is its
					// effect's, which reaches the runner on its own).
					if (said.phase != Protocol::KnockPhase::kDown || !one.copy || said.kind != Kind::kPush || !hold || hold->asked) {
						continue;
					}
					auto ask = KnockRules::ToWire(one.side, said, Protocol::SentMs(said.when), true);
					if (!Protocol::KnockHolds(ask)) {
						continue;
					}
					hold->asked = true;
					++counts.asked;
					lastAsked = KnockText(ask);
					logs::info("Knocks: asked the runner of {}: {}", SideText(one.side), lastAsked);
					outgoing.push_back(ask);
				}
			}
			natives.ForgetIf([&](std::uint32_t a_id) { return !seen.contains(a_id); });
			if (changed) {
				Publish();
			}
		}

		// ---- a friend's ----

		struct Here
		{
			Shown                    shown = Shown::kNone;
			RE::NiPointer<RE::Actor> actor;
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
			} else {
				here.actor = NpcSync::ActorOf(a_side.npc);
				here.shown = !here.actor ? Shown::kNone : NpcSync::RunnerOf(a_side.npc) == ourId.load() ? Shown::kRunHere : Shown::kCopy;
			}
			if (here.actor && (here.actor->IsDisabled() || !here.actor->Is3DLoaded())) {
				here = {};
			}
			return here;
		}

		void PlayDown(std::uint32_t a_sender, const Protocol::Knock& a_knock, Clock::time_point a_now)
		{
			const auto           here = HereOf(a_knock.actor);
			KnockRules::Facts    facts{ .shown = here.shown, .full = holds.size() >= KnockRules::MAX_HELD };
			if (here.actor) {
				facts.alive = !here.actor->IsDead();
				facts.busy = Busy(here.actor.get());
				facts.knockedHere = KnockRules::PhaseOf(KnockStateOf(here.actor.get())) != Phase::kStanding || HoldOf(here.actor->GetFormID()) != nullptr;
				facts.drift = KnockRules::Distance(PosOf(here.actor.get()), { a_knock.x, a_knock.y, a_knock.z });
			}
			const auto verdict = KnockRules::Judge(facts);
			verdicts.Add(verdict);
			if (verdict == Verdict::kAlready) {
				// Joined: held as the friend's from now (its up awaited, kept down meanwhile).
				const auto id = here.actor->GetFormID();
				auto*      hold = HoldOf(id);
				if (!hold) {
					if (facts.full) {
						++counts.holdFull;
						return;
					}
					holds.push_back({ .handle = here.actor->GetHandle(), .id = id, .side = a_knock.actor, .from = From::kJoined, .since = a_now, .everKnocked = true });
					hold = &holds.back();
					Publish();
				} else if (hold->from == From::kNative) {
					hold->from = From::kJoined;
				}
				hold->side = a_knock.actor;
				hold->sender = a_sender;
				hold->kind = a_knock.kind;
				hold->heardDown = true;
				hold->upAt.reset();
				++counts.joined;
				logs::info("Knocks: {} from player {} joined: {:08X} was knocked here already", KnockText(a_knock), a_sender, id);
				return;
			}
			if (verdict != Verdict::kPlay) {
				const auto why = KnockRules::NameOf(verdict);  // (outside the log call: its guard reads "NameOf(" as a player's name)
				logs::info("Knocks: not played ({}): {} from player {}", why, KnockText(a_knock), a_sender);
				return;
			}
			const auto actor = here.actor.get();
			Hold       hold{ .handle = actor->GetHandle(), .id = actor->GetFormID(), .side = a_knock.actor, .sender = a_sender, .from = From::kPlayed, .kind = a_knock.kind, .since = a_now, .heardDown = true };
			if (a_knock.kind == Kind::kParalysis) {
				// By its Paralysis value (what a paralysis spell or poison raises); the engine's paralysis knock if that alone
				// has not put it down by PARALYZE_CALL.
				Keep(hold, actor);
				hold.paralyzeCallAt = a_now + KnockRules::PARALYZE_CALL;
				++counts.playedParalysis;
				++counts.byValue;
			} else {
				const auto push = KnockRules::PushFor(PosOf(actor), { a_knock.vx, a_knock.vy, a_knock.vz }, scale.SpeedPerMagnitude());
				if (!KnockBy(actor, push)) {
					++counts.failed;
					logs::info("Knocks: {:08X} has no process: {} from player {} not played", actor->GetFormID(), KnockText(a_knock), a_sender);
					return;
				}
				hold.playedAt = PosOf(actor);
				hold.magnitude = push.magnitude;
				hold.wanted = KnockRules::Length({ a_knock.vx, a_knock.vy, a_knock.vz });
				hold.inPlace = push.inPlace;
				++counts.playedPush;
				counts.inPlace += push.inPlace ? 1 : 0;
			}
			holds.push_back(hold);
			Publish();
			lastPlayed = std::format("{} on {:08X}", KnockText(a_knock), actor->GetFormID());
			logs::info("Knocks: played {} from player {}", lastPlayed, a_sender);
		}

		void PlayUp(std::uint32_t a_sender, const Protocol::Knock& a_knock, Clock::time_point a_now)
		{
			const auto here = HereOf(a_knock.actor);
			auto*      hold = here.actor ? HoldOf(here.actor->GetFormID()) : nullptr;
			if (!hold) {
				++counts.upUnknown;  // its down was not played here (refused, late) or it was let go already
				return;
			}
			++counts.upHeard;
			hold->upAt = a_now;
			hold->rest = { a_knock.x, a_knock.y, a_knock.z };
			hold->sender = a_sender;
			TakeBack(*hold);  // the engine gets it up now
		}

		void ApplyAsks(Clock::time_point a_now)
		{
			for (const auto& ask : asks) {
				const auto         actor = NpcSync::ActorOf(ask.knock.actor.npc);
				KnockRules::AskFacts facts{ .ours = actor && NpcSync::RunnerOf(ask.knock.actor.npc) == ourId.load() && actor->Is3DLoaded(), .kind = ask.knock.kind };
				if (facts.ours) {
					facts.alive = !actor->IsDead();
					facts.down = KnockRules::PhaseOf(KnockStateOf(actor.get())) != Phase::kStanding;
					facts.busy = Busy(actor.get());
					facts.drift = KnockRules::Distance(PosOf(actor.get()), { ask.knock.x, ask.knock.y, ask.knock.z });
				}
				const auto verdict = KnockRules::JudgeAsk(facts);
				askVerdicts.Add(verdict);
				if (verdict != KnockRules::AskVerdict::kApply) {
					const auto why = KnockRules::NameOf(verdict);
					logs::info("Knocks: ask not applied ({}): {} from player {}", why, KnockText(ask.knock), ask.sender);
					continue;
				}
				const auto push = KnockRules::PushFor(PosOf(actor.get()), { ask.knock.vx, ask.knock.vy, ask.knock.vz }, scale.SpeedPerMagnitude());
				if (KnockBy(actor.get(), push)) {
					++counts.askApplied;
					logs::info("Knocks: applied {} from player {} to {:08X} (magnitude {:.1f})", KnockText(ask.knock), ask.sender, actor->GetFormID(), push.magnitude);
				}
			}
			asks.clear();
			(void)a_now;
		}

		// The actors held here, each let go when its knock is over.
		void Tend(Clock::time_point a_now)
		{
			const auto before = holds.size();
			std::erase_if(holds, [&](Hold& a_hold) {
				const auto actor = a_hold.handle.get();
				if (!actor || actor->GetFormID() != a_hold.id) {
					++counts.released;
					return true;  // gone (its value went with it)
				}
				if (actor->IsDead()) {
					TakeBack(a_hold);
					++counts.died;
					Summarise(a_hold, actor.get(), "died", a_now);
					return true;  // a body is its death's (NpcSync, the ragdolls' change)
				}
				if (a_hold.side.player == 0 && a_hold.side.npc.id != 0 && NpcSync::RunnerOf(a_hold.side.npc) == ourId.load()) {
					TakeBack(a_hold);
					++counts.handedOver;
					Summarise(a_hold, actor.get(), "handedOver", a_now);
					return true;  // ours now: its knock is this game's to say
				}
				const auto state = KnockStateOf(actor.get());
				const auto phase = KnockRules::PhaseOf(state);
				if (phase != Phase::kStanding && !a_hold.everKnocked) {
					a_hold.everKnocked = true;
					++counts.took;
				}
				if (phase == Phase::kStanding) {
					if (!a_hold.standingSince) {
						a_hold.standingSince = a_now;
					}
				} else {
					a_hold.standingSince.reset();
				}
				// A paralysis by value that has not put it down: the engine's paralysis knock.
				if (a_hold.paralyzeCallAt && a_now >= *a_hold.paralyzeCallAt) {
					a_hold.paralyzeCallAt.reset();
					if (phase == Phase::kStanding && !a_hold.upAt && KnockParalyze(actor.get())) {
						++counts.byCall;
					}
				}
				// A push played here: how fast it was thrown, once (what a magnitude does in this game).
				if (a_hold.from == From::kPlayed && a_hold.kind == Kind::kPush && !a_hold.inPlace && !a_hold.measured && a_now - a_hold.since >= KnockRules::SAMPLE) {
					a_hold.measured = true;
					const float seconds = std::chrono::duration<float>(a_now - a_hold.since).count();
					const float speed = KnockRules::Distance(PosOf(actor.get()), a_hold.playedAt) / seconds;
					scale.Note(a_hold.magnitude, speed);
					counts.speedLast = speed;
					counts.wantedLast = a_hold.wanted;
				}
				// Lying while its friend's actor is still down: kept down by its Paralysis value.
				if (KnockRules::WantKeptDown(a_hold.heardDown, a_hold.upAt.has_value(), a_hold.kept,
						KnockRules::Resting(state) || a_hold.kind == Kind::kParalysis, true)) {
					Keep(a_hold, actor.get());
					++counts.keptDown;
				}
				const auto step = KnockRules::HoldStep(a_hold.since, a_now, phase, a_hold.everKnocked, a_hold.upAt, a_hold.standingSince);
				switch (step) {
				case KnockRules::Hold::kHold:
					return false;
				case KnockRules::Hold::kEnded:
					++counts.ended;
					break;
				case KnockRules::Hold::kNoTake:
					++counts.noTake;
					logs::info("Knocks: {:08X} ({}) never went down; let go", a_hold.id, SideText(a_hold.side));
					break;
				case KnockRules::Hold::kGaveUp:
					++counts.gaveUp;
					logs::info("Knocks: {:08X} ({}) still down {}; let go", a_hold.id, SideText(a_hold.side), a_hold.upAt ? "after its friend's up" : "with no up");
					break;
				}
				TakeBack(a_hold);
				Summarise(a_hold, actor.get(), KnockRules::NameOf(step), a_now);
				return true;
			});
			if (holds.size() != before) {
				Publish();
			}
		}

		std::string Report()
		{
			return std::format(
				"knocks ours watched={} saidDown={} saidUp={} saidPush={} saidParalysis={} launchMax={:.0f} | theirs heard={} played={} playedPush={} playedParalysis={} "
				"inPlace={} refused={} failed={} joined={} took={} keptDown={} byValue={} byCall={} letUp={} ended={} noTake={} gaveUp={} released={} died={} handedOver={} "
				"holdFull={} upHeard={} upUnknown={} restGapLast={} restGapMax={} heldNow={} waiting={} late={} dropped={} | native copies={} standIns={} asked={} "
				"| asks askHeard={} applied={} askRefused={} | scale perMagnitude={:.0f} learnt={} ignored={} speedLast={} wantedLast={} | savedBack={} eventsKept={}",
				ours.Size(), counts.saidDown, counts.saidUp, counts.saidPush, counts.saidParalysis, counts.launchMax, counts.heard,
				counts.playedPush + counts.playedParalysis, counts.playedPush, counts.playedParalysis, counts.inPlace, verdicts.Others(), counts.failed, counts.joined,
				counts.took, counts.keptDown, counts.byValue, counts.byCall, counts.letUp, counts.ended, counts.noTake, counts.gaveUp, counts.released, counts.died,
				counts.handedOver, counts.holdFull, counts.upHeard, counts.upUnknown,
				counts.restGapLast < 0.0f ? std::string{ "none" } : std::format("{:.0f}", counts.restGapLast),
				counts.restGapMax < 0.0f ? std::string{ "none" } : std::format("{:.0f}", counts.restGapMax), holds.size(), inbox.Size(), inbox.late, inbox.dropped,
				counts.nativeCopies, counts.nativeStandIns, counts.asked, counts.askHeard, counts.askApplied, askVerdicts.Others(), scale.SpeedPerMagnitude(), scale.learnt,
				scale.ignored, counts.speedLast < 0.0f ? std::string{ "none" } : std::format("{:.0f}", counts.speedLast),
				counts.wantedLast < 0.0f ? std::string{ "none" } : std::format("{:.0f}", counts.wantedLast), counts.savedBack, eventsKept.load());
		}

		std::string List()
		{
			const auto  now = Clock::now();
			std::string out = std::format("held={}", holds.size());
			for (const auto& hold : holds) {
				const auto actor = hold.handle.get();
				const auto state = actor && actor->GetFormID() == hold.id ? KnockStateOf(actor.get()) : 0;
				out += std::format(" | actor={:08X} side={} from={} kind={} ageMs={} heard={} up={} everKnocked={} kept={} phase={} knock={}", hold.id, SideText(hold.side),
					NameOf(hold.from), KnockRules::NameOf(hold.kind), std::chrono::duration_cast<std::chrono::milliseconds>(now - hold.since).count(), hold.heardDown ? 1 : 0,
					hold.upAt ? 1 : 0, hold.everKnocked ? 1 : 0, hold.kept ? 1 : 0, KnockRules::NameOf(KnockRules::PhaseOf(state)), state);
			}
			return out;
		}

		RE::Actor* ActorById(std::string_view a_word)
		{
			const auto id = FormsCommand::ParseHex(a_word);
			return id ? RE::TESForm::LookupByID<RE::Actor>(*id) : nullptr;
		}

		std::string State(std::string_view a_word)
		{
			const auto actor = ActorById(a_word);
			if (!actor) {
				return "usage: knocks state <local id> (hex, an actor)";
			}
			const auto state = KnockStateOf(actor);
			const auto at = PosOf(actor);
			const auto hold = HoldOf(actor->GetFormID());
			return std::format("state id={:08X} knock={} phase={} resting={} paralysis={:.1f} dead={} held={} kept={} at=({:.0f},{:.0f},{:.0f})", actor->GetFormID(), state,
				KnockRules::NameOf(KnockRules::PhaseOf(state)), KnockRules::Resting(state) ? 1 : 0, ParalysisOf(actor), actor->IsDead() ? 1 : 0, hold ? 1 : 0,
				hold && hold->kept ? 1 : 0, at.x, at.y, at.z);
		}

		// `knocks push <local> <magnitude> [<from local>]`: the rig's set-up (an injected push).
		std::string PushCommand(const std::vector<std::string_view>& a_words)
		{
			if (a_words.size() < 3 || a_words.size() > 4) {
				return "usage: knocks push <local id> <magnitude> [<from local id>]";
			}
			const auto actor = ActorById(a_words[1]);
			float      magnitude = 0.0f;
			if (std::from_chars(a_words[2].data(), a_words[2].data() + a_words[2].size(), magnitude).ec != std::errc{} || !(magnitude > 0.0f) || magnitude > KnockRules::MAX_MAGNITUDE) {
				return std::format("refused: a magnitude above 0, at most {:.0f}", KnockRules::MAX_MAGNITUDE);
			}
			if (!actor || actor->IsDead() || !actor->Is3DLoaded()) {
				return "refused: no such actor loaded and alive";
			}
			KnockRules::Push push{ .magnitude = magnitude };
			if (a_words.size() == 4) {
				const auto from = ActorById(a_words[3]);
				if (!from || from == actor) {
					return "refused: no such actor to push from";
				}
				push.from = PosOf(from);
			} else {
				const float heading = actor->GetAngleZ();  // from in front of it: thrown backwards
				const auto  at = PosOf(actor);
				push.from = { at.x + std::sin(heading) * KnockRules::BEHIND, at.y + std::cos(heading) * KnockRules::BEHIND, at.z };
			}
			if (!KnockBy(actor, push)) {
				return "refused: the actor has no process";
			}
			logs::info("Knocks: the developer channel pushed {:08X} (magnitude {:.1f})", actor->GetFormID(), magnitude);
			return std::format("pushed id={:08X} magnitude={:.1f} from=({:.0f},{:.0f},{:.0f})", actor->GetFormID(), magnitude, push.from.x, push.from.y, push.from.z);
		}

		// `knocks paralyze <local> <on|off>`: the rig's set-up (an injected paralysis); off takes back only what on raised.
		std::string ParalyzeCommand(const std::vector<std::string_view>& a_words)
		{
			if (a_words.size() != 3 || (a_words[2] != "on" && a_words[2] != "off")) {
				return "usage: knocks paralyze <local id> <on|off>";
			}
			const auto actor = ActorById(a_words[1]);
			if (!actor) {
				return "refused: no such actor";
			}
			const auto id = actor->GetFormID();
			if (a_words[2] == "off") {
				const auto it = devKept.find(id);
				if (it == devKept.end()) {
					return std::format("off id={:08X} raised=0", id);
				}
				RaiseParalysis(actor, -it->second);
				devKept.erase(it);
				logs::info("Knocks: the developer channel took back {:08X}'s paralysis", id);
				return std::format("off id={:08X} raised=1 paralysis={:.1f}", id, ParalysisOf(actor));
			}
			if (actor->IsDead() || !actor->Is3DLoaded()) {
				return "refused: the actor is dead or not loaded";
			}
			if (!devKept.contains(id)) {
				RaiseParalysis(actor, KEPT);
				devKept[id] = KEPT;
			}
			const bool called = KnockParalyze(actor);
			logs::info("Knocks: the developer channel paralysed {:08X}", id);
			return std::format("on id={:08X} paralysis={:.1f} knock={}", id, ParalysisOf(actor), called ? 1 : 0);
		}
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
		for (const auto& due : inbox.Take(now - StateBuffer::Buffer::Delay())) {
			if (due.knock.phase == Protocol::KnockPhase::kUp) {
				PlayUp(due.sender, due.knock, now);
			} else {
				PlayDown(due.sender, due.knock, now);
			}
		}
		ApplyAsks(now);
		Tend(now);
		SayOurs(now);
		WatchNatives(now);
		if (!outgoing.empty()) {
			for (auto& msg : KnockRules::Batch(std::move(outgoing))) {
				a_send(Protocol::Encode(msg), true);
			}
			outgoing.clear();
		}
	}

	void Heard(const Protocol::Knocks& a_msg)
	{
		if (!active) {
			return;
		}
		const auto now = Clock::now();
		for (const auto& knock : a_msg.knocks) {
			if (knock.phase == Protocol::KnockPhase::kAsk) {
				++counts.askHeard;
				if (asks.size() < KnockRules::Inbox::MAX) {
					asks.push_back({ a_msg.sender, knock });
				}
				continue;
			}
			++counts.heard;
			const auto at = knock.actor.player != 0 ? RemotePlayers::SenderTime(knock.actor.player, knock.sentMs) : NpcSync::SenderTime(knock.actor.npc, knock.sentMs);
			inbox.Push(knock, a_msg.sender, at, now);
		}
	}

	void Left(std::uint32_t a_playerId)
	{
		inbox.Forget(a_playerId);
		std::erase_if(asks, [&](const Ask& a_ask) { return a_ask.sender == a_playerId; });
		const auto now = Clock::now();
		const auto before = holds.size();
		std::erase_if(holds, [&](Hold& a_hold) {
			if (a_hold.side.player == a_playerId) {
				TakeBack(a_hold);
				++counts.released;
				return true;  // its stand-in goes
			}
			if (a_hold.sender == a_playerId && !a_hold.upAt) {
				a_hold.upAt = now;  // no up will come: let up, and go once it stands
				TakeBack(a_hold);
			}
			return false;
		});
		if (holds.size() != before) {
			Publish();
		}
	}

	void Clear()
	{
		LetGoAll(true);
		ours.Clear();
		natives.Clear();
		inbox.Clear();
		asks.clear();
		outgoing.clear();
		active = false;
		ourId = 0;
	}

	void ForgetWorld()
	{
		LetGoAll(true);
		ours.Clear();
		natives.Clear();
		inbox.Clear();
		asks.clear();
		devKept.clear();
	}

	void BeforeSave()
	{
		for (auto& hold : holds) {
			if (hold.kept) {
				TakeBack(hold);
				++counts.savedBack;  // kept down again by the next tick (WantKeptDown)
			}
		}
		for (const auto& [id, raised] : devKept) {
			if (const auto actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
				RaiseParalysis(actor, -raised);
				logs::info("Knocks: {:08X}'s paralysis from the developer channel taken back before the save", id);
			}
		}
		devKept.clear();
	}

	void Release(RE::Actor* a_actor)
	{
		if (!a_actor || holds.empty()) {
			return;
		}
		const auto id = a_actor->GetFormID();
		const auto before = holds.size();
		std::erase_if(holds, [&](Hold& a_hold) {
			if (a_hold.id != id) {
				return false;
			}
			TakeBack(a_hold);
			return true;
		});
		if (holds.size() != before) {
			counts.released += before - holds.size();
			Publish();
		}
	}

	bool Held(const RE::Actor* a_actor)
	{
		return a_actor && anyHeld.load(std::memory_order_acquire) && heldIds.Contains(a_actor->GetFormID());
	}

	bool EventsStay(RE::Actor* a_actor)
	{
		if (!a_actor || !active.load(std::memory_order_relaxed)) {
			return false;
		}
		if (KnockRules::PhaseOf(KnockStateOf(a_actor)) == Phase::kStanding) {
			return false;
		}
		eventsKept.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (words.empty()) {
			return Report();
		}
		if (words[0] == "last") {
			return std::format("said {} | played {} | asked {}", lastSaid, lastPlayed, lastAsked);
		}
		if (words[0] == "list") {
			return List();
		}
		if (words[0] == "holds") {
			return std::format("holds {}", lastHold);
		}
		if (words[0] == "state" && words.size() == 2) {
			return State(words[1]);
		}
		if (words[0] == "push") {
			return PushCommand(words);
		}
		if (words[0] == "paralyze") {
			return ParalyzeCommand(words);
		}
		return "usage: knocks [last | list | holds | state <local id> | push <local id> <magnitude> [<from local id>] | paralyze <local id> <on|off>]";
	}
}
