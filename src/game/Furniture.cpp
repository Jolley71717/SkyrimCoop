#include "game/Furniture.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/Console.h"
#include "game/NpcSync.h"
#include "game/RemotePlayers.h"
#include "game/RideRules.h"
#include "game/SitRules.h"
#include "net/FormsCommand.h"

#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <utility>

namespace Furniture
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Rider = SitRules::Said::Rider;
		using Key = Protocol::NpcKey;
		using SitState = SitRules::SitState;

		bool          active = false;
		std::uint32_t ourId = 0;

		// An actor another game says is in furniture (a friend, an NPC that game runs), and how this game puts it in.
		struct Wanted
		{
			Protocol::Sit                    sit;          // as said (on false: it got up)
			std::uint32_t                    sender = 0;   // the game that said it (0: the server's list for a joiner)
			SitRules::Seat                   seat;
			std::optional<Clock::time_point> readySince;   // actor and furniture both here with their 3D, and near
			RideRules::Handover              handover;     // an NPC's sit while its runner changes
			bool                             held = false;  // kept as it was for its new runner to say (`sit`: handover=1)
			// What was found and seen, last tick (for `sit`).
			RE::FormID                       actorId = 0;
			RE::FormID                       found = 0;     // the furniture here (0: none found)
			bool                             taken = false;  // our player is in it, at that marker
			bool                             nearBy = false;
			float                            dist = -1.0f;  // the actor from the furniture
			std::uint32_t                    state = 0;     // its sit/sleep state
			RE::FormID                       occupies = 0;  // what its process names
			bool                             inWanted = false;
			bool                             inOther = false;
			bool                             holding = false;
			bool                             assisting = false;
			std::uint32_t                    markerHere = Protocol::NO_MARKER;
			bool                             saidLacked = false;  // said once in the log
			bool                             saidTaken = false;   // said once in the log
			std::uint64_t                    heard = 0;
		};
		std::map<Rider, Wanted> wanted;
		SitRules::Said          said;

		// The actors held and assisted this tick (any thread reads them).
		std::mutex                     setLock;
		std::unordered_set<RE::FormID> heldIds;
		std::unordered_set<RE::FormID> assistedIds;

		struct Counts
		{
			std::uint64_t sent = 0, heard = 0, notBelieved = 0, lacked = 0, taken = 0;
			std::uint64_t activates = 0, consoles = 0, stands = 0, quicks = 0, releases = 0, handedToUs = 0;
		};
		Counts                     counts;
		std::atomic<std::uint64_t> eventsKept{ 0 };    // graph events of ours not sent: the engine's furniture play
		std::atomic<std::uint64_t> assistFrames{ 0 };  // updates the engine ran in full for an actor getting in or up

		std::string KeyText(const Key& a_key)
		{
			if (a_key.id == 0) {
				return "none";
			}
			return a_key.IsSpawn() ? std::format("{:08X}@{}", a_key.id, a_key.spawner) : std::format("{:08X}", a_key.id);
		}

		std::string RiderText(const Rider& a_rider)
		{
			return a_rider.player != 0 ? std::format("player={}", a_rider.player) : std::format("npc={}", KeyText(a_rider.npc));
		}

		float Distance(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b)
		{
			const auto d = a_a - a_b;
			return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		}

		void SetIn(std::unordered_set<RE::FormID>& a_set, RE::FormID a_id, bool a_in)
		{
			std::scoped_lock l{ setLock };
			if (a_in) {
				a_set.insert(a_id);
			} else {
				a_set.erase(a_id);
			}
		}

		bool GettingOnOrOffMount(const RE::Actor* a_actor)
		{
			return a_actor->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kGettingOnOffMount);
		}

		// Its sit/sleep state as furniture goes: a rider's (the saddle's) is none.
		SitState StateOf(const RE::Actor* a_actor)
		{
			const auto state = a_actor ? a_actor->AsActorState() : nullptr;
			if (!state || a_actor->IsOnMount() || GettingOnOrOffMount(a_actor)) {
				return SitState::kNormal;
			}
			return SitRules::StateOf(static_cast<std::uint32_t>(std::to_underlying(state->GetSitSleepState())));
		}

		RE::NiPointer<RE::TESObjectREFR> OccupiedBy(RE::Actor* a_actor)
		{
			if (!a_actor || a_actor->IsOnMount()) {
				return {};
			}
			const auto ref = a_actor->GetOccupiedFurniture().get();
			if (!ref || ref.get() == a_actor) {
				return {};
			}
			return ref;
		}

		std::uint32_t MarkerOf(RE::Actor* a_actor)
		{
			const auto process = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
			const auto middleHigh = process ? process->middleHigh : nullptr;
			return middleHigh ? middleHigh->currentFurnitureMarkerID : Protocol::NO_MARKER;
		}

		const RE::TESFurniture* FurnitureBase(const RE::TESObjectREFR* a_ref)
		{
			const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
			return base ? base->As<RE::TESFurniture>() : nullptr;
		}

		std::uint32_t Markers(RE::TESObjectREFR* a_ref)
		{
			const auto root = a_ref ? a_ref->Get3D() : nullptr;
			return root ? RE::BSFurnitureMarkerNode::GetNumFurnitureMarkers(root) : 0;
		}

		// ---- ours ----

		SitRules::Own ReadOwn(RE::Actor* a_actor)
		{
			SitRules::Own own;
			own.state = StateOf(a_actor);
			own.onMount = a_actor->IsOnMount() || GettingOnOrOffMount(a_actor);
			if (const auto ref = OccupiedBy(a_actor)) {
				own.found = true;
				own.ref = ref->GetFormID();
				own.runtimeRef = FormIds::IsRuntime(own.ref);
				if (const auto furniture = FurnitureBase(ref.get())) {
					own.furniture = true;
					own.base = furniture->GetFormID();
					own.flags = furniture->furnFlags.underlying();
					own.benchType = furniture->workBenchData.benchType.underlying();
				}
				const auto at = ref->GetPosition();
				own.x = at.x;
				own.y = at.y;
				own.z = at.z;
				own.marker = MarkerOf(a_actor);
			}
			return own;
		}

		// What our player and each NPC we run is in now (none listed: in nothing).
		std::map<Rider, Protocol::Sit> OursNow()
		{
			std::map<Rider, Protocol::Sit> out;
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				if (const auto sit = SitRules::Tell(ReadOwn(player))) {
					out[Rider{ ourId, {} }] = *sit;
				}
			}
			for (const auto& [npc, actor] : NpcSync::RunActors()) {
				if (const auto sit = SitRules::Tell(ReadOwn(actor.get()))) {
					out[Rider{ 0, npc }] = *sit;
				}
			}
			return out;
		}

		void SendOurs(const SendFn& a_send)
		{
			auto changes = said.Changes(OursNow());
			for (std::size_t first = 0; first < changes.size(); first += Protocol::MAX_SITS) {
				Protocol::Sits msg;
				const auto     end = (std::min)(changes.size(), first + Protocol::MAX_SITS);
				msg.sits.assign(changes.begin() + static_cast<std::ptrdiff_t>(first), changes.begin() + static_cast<std::ptrdiff_t>(end));
				counts.sent += msg.sits.size();
				for (const auto& sit : msg.sits) {
					if (sit.player != 0) {
						if (sit.on) {
							logs::info("Furniture: our player is in {:08X} (base {:08X}, {}, marker {})", sit.furniture, sit.base, SitRules::KindName(sit.kind), sit.marker);
						} else {
							logs::info("Furniture: our player got up");
						}
					}
				}
				a_send(Protocol::Encode(msg), true);
			}
		}

		// ---- theirs ----

		// The actor that shows them here: a friend's stand-in, or the copy of an NPC another game runs (none for one we
		// run: our own AI has it).
		RE::NiPointer<RE::Actor> ActorFor(const Rider& a_rider)
		{
			if (a_rider.player != 0) {
				return RemotePlayers::StandInOf(a_rider.player);
			}
			const auto runner = NpcSync::RunnerOf(a_rider.npc);
			if (runner == 0 || runner == ourId) {
				return {};
			}
			return NpcSync::ActorOf(a_rider.npc);
		}

		bool StillBelieved(const Rider& a_rider, const Wanted& a_wanted)
		{
			if (a_rider.player != 0) {
				return true;
			}
			const auto runner = NpcSync::RunnerOf(a_rider.npc);
			return runner != 0 && runner != ourId && (a_wanted.sender == 0 || a_wanted.sender == runner);
		}

		// The furniture here that the sit names, near a_actor: the reference when this game has it, else one of that base
		// where it stands (SitRules::Pick). Looked for only when the actor is within 2 x NEAR of where it stands.
		RE::NiPointer<RE::TESObjectREFR> Find(const Protocol::Sit& a_sit, RE::Actor* a_actor)
		{
			const RE::NiPoint3 at{ a_sit.x, a_sit.y, a_sit.z };
			if (Distance(a_actor->GetPosition(), at) > 2.0f * SitRules::NEAR) {
				return {};
			}
			std::vector<SitRules::Candidate>               candidates;
			std::map<RE::FormID, RE::NiPointer<RE::TESObjectREFR>> refs;
			const auto add = [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || refs.contains(a_ref->GetFormID())) {
					return;
				}
				const auto furniture = FurnitureBase(a_ref);
				const auto pos = a_ref->GetPosition();
				candidates.push_back({ .id = a_ref->GetFormID(),
					.base = furniture ? furniture->GetFormID() : 0,
					.x = pos.x,
					.y = pos.y,
					.z = pos.z,
					.usable = furniture && !a_ref->IsDisabled() && !a_ref->IsDeleted() });
				refs.emplace(a_ref->GetFormID(), RE::NiPointer<RE::TESObjectREFR>{ a_ref });
			};
			if (a_sit.furniture != 0) {
				add(RE::TESForm::LookupByID<RE::TESObjectREFR>(a_sit.furniture));
			}
			if (a_sit.base != 0) {
				if (const auto tes = RE::TES::GetSingleton()) {
					const float radius = Distance(a_actor->GetPosition(), at) + SitRules::MATCH_RADIUS;
					tes->ForEachReferenceInRange(a_actor, radius, [&](RE::TESObjectREFR* a_ref) {
						if (a_ref && a_ref->GetBaseObject() && a_ref->GetBaseObject()->GetFormID() == a_sit.base) {
							add(a_ref);
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});
				}
			}
			const auto pick = SitRules::Pick(a_sit, candidates);
			return pick ? refs[*pick] : RE::NiPointer<RE::TESObjectREFR>{};
		}

		void Tend(const Rider& a_rider, Wanted& a_wanted, Clock::time_point a_now)
		{
			const auto actor = ActorFor(a_rider);
			const auto believed = StillBelieved(a_rider, a_wanted);
			// An NPC's sit said by a game that runs it no more: kept as it is a moment, for its new runner to say it.
			a_wanted.held = a_rider.player == 0 && a_wanted.sit.on && a_wanted.handover.Holds(!believed, a_now);
			a_wanted.actorId = actor ? actor->GetFormID() : 0;
			if (a_wanted.held || !actor) {
				a_wanted.readySince.reset();
				return;
			}
			const bool saidIn = a_wanted.sit.on && believed;
			auto       furniture = saidIn ? Find(a_wanted.sit, actor.get()) : RE::NiPointer<RE::TESObjectREFR>{};
			a_wanted.found = furniture ? furniture->GetFormID() : 0;
			if (saidIn && !furniture && !a_wanted.saidLacked) {
				a_wanted.saidLacked = true;
				++counts.lacked;
				logs::info("Furniture: {} is in {:08X} (base {:08X}), which this game has not got near it: shown standing by it", RiderText(a_rider), a_wanted.sit.furniture,
					a_wanted.sit.base);
			}
			// Furniture our own player is in, at that marker: theirs waits beside it.
			a_wanted.taken = false;
			if (furniture) {
				if (const auto player = RE::PlayerCharacter::GetSingleton()) {
					const auto ours = OccupiedBy(player);
					a_wanted.taken = SitRules::TakenByUs(ours && ours.get() == furniture.get(), a_wanted.sit.marker, MarkerOf(player), Markers(furniture.get()));
				}
				if (a_wanted.taken) {
					if (!std::exchange(a_wanted.saidTaken, true)) {
						++counts.taken;
						logs::info("Furniture: {} is in {:08X}, where our player is: shown standing by it", RiderText(a_rider), furniture->GetFormID());
					}
					furniture.reset();
				}
			}
			const SitState state = StateOf(actor.get());
			const auto     occupies = OccupiedBy(actor.get());
			const RE::NiPoint3 spot{ a_wanted.sit.x, a_wanted.sit.y, a_wanted.sit.z };
			a_wanted.dist = furniture ? Distance(actor->GetPosition(), furniture->GetPosition()) : Distance(actor->GetPosition(), spot);
			a_wanted.nearBy = a_wanted.dist <= SitRules::NEAR;
			const bool ready = furniture && a_wanted.nearBy && actor->Is3DLoaded() && furniture->Is3DLoaded() && !actor->IsDead();
			if (ready) {
				a_wanted.readySince = a_wanted.readySince.value_or(a_now);
			} else {
				a_wanted.readySince.reset();
			}
			SitRules::SeatFacts facts;
			facts.wanted = furniture != nullptr;
			facts.ready = ready && a_now - *a_wanted.readySince >= SitRules::READY_AFTER;
			facts.state = state;
			facts.inWanted = occupies && furniture && occupies.get() == furniture.get();
			facts.inOther = occupies && !facts.inWanted;
			a_wanted.state = static_cast<std::uint32_t>(state);
			a_wanted.occupies = occupies ? occupies->GetFormID() : 0;
			a_wanted.inWanted = facts.inWanted;
			a_wanted.inOther = facts.inOther;
			const auto act = a_wanted.seat.Step(facts, a_now);
			switch (act) {
			case SitRules::Act::kActivate:
				++counts.activates;
				logs::info("Furniture: {} is put in {:08X} ({}): the furniture activated by it", RiderText(a_rider), furniture->GetFormID(), SitRules::KindName(a_wanted.sit.kind));
				furniture->ActivateRef(actor.get(), 0, nullptr, 1, false);
				break;
			case SitRules::Act::kConsole:
				++counts.consoles;
				logs::info("Furniture: {} not in {:08X} yet: the console's activate", RiderText(a_rider), furniture->GetFormID());
				Console::Run(std::format("{:08X}.activate {:08X}", furniture->GetFormID(), actor->GetFormID()));
				break;
			case SitRules::Act::kStand:
				++counts.stands;
				logs::info("Furniture: {} gets up from {:08X}", RiderText(a_rider), a_wanted.occupies);
				if (const auto actorState = actor->AsActorState()) {
					actorState->actorState1.sitSleepState = SitRules::Sleeping(state) ? RE::SIT_SLEEP_STATE::kWantToWake : RE::SIT_SLEEP_STATE::kWantToStand;
				}
				break;
			case SitRules::Act::kQuick:
				++counts.quicks;
				logs::info("Furniture: {} out of {:08X} at once (state {})", RiderText(a_rider), a_wanted.occupies, a_wanted.state);
				actor->StopInteractingQuick(true);
				break;
			default:
				break;
			}
			a_wanted.markerHere = a_wanted.seat.Seated() ? MarkerOf(actor.get()) : Protocol::NO_MARKER;
			a_wanted.holding = SitRules::Held(saidIn, a_wanted.nearBy, state, occupies != nullptr);
			a_wanted.assisting = a_wanted.seat.Assist();
			SetIn(heldIds, actor->GetFormID(), a_wanted.holding);
			SetIn(assistedIds, actor->GetFormID(), a_wanted.assisting);
		}

		void Forget(const Wanted& a_wanted)
		{
			if (a_wanted.actorId != 0) {
				SetIn(heldIds, a_wanted.actorId, false);
				SetIn(assistedIds, a_wanted.actorId, false);
			}
		}

		// ---- `sit find` and `sit off` ----

		std::optional<Protocol::SitKind> KindNamed(std::string_view a_name)
		{
			for (std::uint8_t i = 0; i < Protocol::SIT_KIND_COUNT; ++i) {
				const auto kind = static_cast<Protocol::SitKind>(i);
				if (SitRules::KindName(kind) == a_name) {
					return kind;
				}
			}
			return std::nullopt;
		}

		// Every furniture reference an actor here is in (ours, NPCs', stand-ins').
		std::unordered_set<RE::FormID> InUse()
		{
			std::unordered_set<RE::FormID> out;
			if (const auto player = RE::PlayerCharacter::GetSingleton()) {
				if (const auto ref = OccupiedBy(player)) {
					out.insert(ref->GetFormID());
				}
			}
			if (const auto lists = RE::ProcessLists::GetSingleton()) {
				for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
					for (const auto& handle : *handles) {
						if (const auto actor = handle.get()) {
							if (const auto ref = OccupiedBy(actor.get())) {
								out.insert(ref->GetFormID());
							}
						}
					}
				}
			}
			return out;
		}

		std::string Find(std::string_view a_kind, float a_radius, bool a_markers)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto tes = RE::TES::GetSingleton();
			const auto kind = KindNamed(a_kind);
			if (!player || !tes || (!kind && a_kind != "any")) {
				return "usage: sit find <sit|sleep|lean|work|perch|any> [radius] [markers]";
			}
			const auto                       inUse = InUse();
			const auto                       at = player->GetPosition();
			RE::NiPointer<RE::TESObjectREFR> best;
			float                            bestDistance = a_radius;
			tes->ForEachReferenceInRange(player, a_radius, [&](RE::TESObjectREFR* a_ref) {
				const auto furniture = FurnitureBase(a_ref);
				if (!furniture || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Is3DLoaded() || inUse.contains(a_ref->GetFormID())) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				// An invisible marker (an idle spot only NPCs use: most lean spots) is not one a player can see or use: found only
				// with `markers` (the rig's NPC legs).
				if ((!a_markers && (furniture->formFlags & RE::TESFurniture::RecordFlags::kIsMarker) != 0) || Markers(a_ref) == 0) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto itsKind = SitRules::KindOf(SitState::kNormal, furniture->furnFlags.underlying(), furniture->workBenchData.benchType.underlying());
				if (kind && itsKind != *kind) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const float d = Distance(a_ref->GetPosition(), at);
				if (d <= bestDistance) {
					bestDistance = d;
					best = RE::NiPointer<RE::TESObjectREFR>{ a_ref };
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			if (!best) {
				return "furniture none";
			}
			const auto furniture = FurnitureBase(best.get());
			const auto pick = RE::CrosshairPickData::GetSingleton();
			const auto target = pick ? pick->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>{};
			const auto pos = best->GetPosition();
			const auto itsKind = SitRules::KindOf(SitState::kNormal, furniture->furnFlags.underlying(), furniture->workBenchData.benchType.underlying());
			return std::format("furniture id={:08X} base={:08X} kind={} bench={} markers={} dist={:.0f} x={:.0f} y={:.0f} z={:.0f} aimed={}", best->GetFormID(), furniture->GetFormID(),
				SitRules::KindName(itsKind), furniture->workBenchData.benchType.underlying(), Markers(best.get()), bestDistance, pos.x, pos.y, pos.z,
				target && target.get() == best.get() ? 1 : 0);
		}

		// `sit off <id>`: an NPC this game runs gets up (set-up for the rig's NPC legs). Refused for any other actor: a
		// stand-in or a copy follows its own game's word.
		std::string Off(std::string_view a_id)
		{
			const auto id = FormsCommand::ParseHex(a_id);
			if (!id) {
				return "usage: sit off <form id>";
			}
			for (const auto& [npc, actor] : NpcSync::RunActors()) {
				if (actor->GetFormID() != *id) {
					continue;
				}
				if (!OccupiedBy(actor.get()) && StateOf(actor.get()) == SitState::kNormal) {
					return std::format("off: {:08X} is in no furniture", *id);
				}
				actor->StopInteractingQuick(true);
				logs::info("Furniture: the developer channel gets {} ({:08X}) up", KeyText(npc), *id);
				return std::format("off: {:08X}", *id);
			}
			return std::format("off: {:08X} is no NPC this game runs", *id);
		}

		std::string Ours()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return "ours none";
			}
			const auto own = ReadOwn(player);
			const auto sit = SitRules::Tell(own);
			return std::format("ours in={} state={} furn={:08X} base={:08X} kind={} marker={} said={} sent={}", sit ? 1 : 0, static_cast<std::uint32_t>(own.state), own.ref, own.base,
				sit ? SitRules::KindName(sit->kind) : std::string_view{ "-" }, own.marker, said.Count(), counts.sent);
		}
	}

	void Started(std::uint32_t a_ourId)
	{
		ourId = a_ourId;
		active = true;
		said.Clear();
		wanted.clear();
	}

	void Frame(const SendFn& a_send)
	{
		if (!active) {
			return;
		}
		// An NPC handed to this game that another game said is in furniture is ours to say now: what it is in here goes
		// out, even nothing (its end), and its copy is let go of (our own AI has it now).
		for (auto it = wanted.begin(); it != wanted.end();) {
			if (it->first.player == 0 && NpcSync::RunnerOf(it->first.npc) == ourId) {
				if (it->second.sit.on) {
					said.HandedOver(it->first);
				}
				Forget(it->second);
				++counts.handedToUs;
				logs::info("Furniture: {} was handed to this game {}: said from here now", RiderText(it->first), it->second.sit.on ? "in furniture" : "up");
				it = wanted.erase(it);
			} else {
				++it;
			}
		}
		SendOurs(a_send);
		const auto now = Clock::now();
		for (auto it = wanted.begin(); it != wanted.end();) {
			Tend(it->first, it->second, now);
			// An end, once its actor is out of furniture (or not here), is forgotten.
			if (!it->second.sit.on && !it->second.holding && !it->second.assisting) {
				Forget(it->second);
				it = wanted.erase(it);
			} else {
				++it;
			}
		}
	}

	void Sits(const Protocol::Sits& a_msg)
	{
		if (!active) {
			return;
		}
		if (a_msg.full) {
			for (auto& [rider, want] : wanted) {
				want.sit.on = false;
			}
		}
		for (const auto& sit : a_msg.sits) {
			++counts.heard;
			if (sit.player == ourId) {
				continue;
			}
			const auto rider = sit.player != 0 ? Rider{ sit.player, {} } : Rider{ 0, sit.rider };
			const auto runner = sit.player != 0 ? 0 : NpcSync::RunnerOf(sit.rider);
			if (sit.player == 0 && runner == ourId) {
				continue;  // ours to say
			}
			// A player's only from that player, an NPC's only from the game that runs it now (as riding's).
			if (sit.player != 0 ? (a_msg.sender != 0 && a_msg.sender != sit.player) : (a_msg.sender != 0 && a_msg.sender != runner)) {
				++counts.notBelieved;
				continue;
			}
			auto&      want = wanted[rider];
			const bool other = want.sit.furniture != sit.furniture || want.sit.base != sit.base || want.sit.x != sit.x || want.sit.y != sit.y || want.sit.z != sit.z;
			if (other) {
				want.seat = {};
				want.saidLacked = false;
				want.saidTaken = false;
			}
			want.sit = sit;
			want.sender = a_msg.sender;
			++want.heard;
		}
	}

	void Left(std::uint32_t a_playerId)
	{
		for (auto& [rider, want] : wanted) {
			if (rider.player == a_playerId || want.sender == a_playerId) {
				want.sit.on = false;
			}
		}
	}

	void Clear()
	{
		for (const auto& [rider, want] : wanted) {
			const auto actor = ActorFor(rider);
			if (actor && (OccupiedBy(actor.get()) || StateOf(actor.get()) != SitState::kNormal)) {
				actor->StopInteractingQuick(true);
			}
		}
		wanted.clear();
		said.Clear();
		{
			std::scoped_lock l{ setLock };
			heldIds.clear();
			assistedIds.clear();
		}
		active = false;
		ourId = 0;
	}

	void ForgetWorld()
	{
		for (auto& [rider, want] : wanted) {
			want.seat = {};
			want.readySince.reset();
			want.actorId = 0;
		}
		std::scoped_lock l{ setLock };
		heldIds.clear();
		assistedIds.clear();
	}

	void Release(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return;
		}
		if (OccupiedBy(a_actor) || StateOf(a_actor) != SitState::kNormal) {
			a_actor->StopInteractingQuick(true);
			++counts.releases;
		}
		std::scoped_lock l{ setLock };
		heldIds.erase(a_actor->GetFormID());
		assistedIds.erase(a_actor->GetFormID());
	}

	bool Held(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::scoped_lock l{ setLock };
		return !heldIds.empty() && heldIds.contains(a_actor->GetFormID());
	}

	bool Assisted(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		bool in = false;
		{
			std::scoped_lock l{ setLock };
			in = !assistedIds.empty() && assistedIds.contains(a_actor->GetFormID());
		}
		if (in) {
			assistFrames.fetch_add(1, std::memory_order_relaxed);
		}
		return in;
	}

	bool EngineOwnsEvents(RE::Actor* a_actor)
	{
		const auto state = a_actor ? a_actor->AsActorState() : nullptr;
		if (!state) {
			return false;
		}
		const auto raw = SitRules::StateOf(static_cast<std::uint32_t>(std::to_underlying(state->GetSitSleepState())));
		if (!SitRules::EngineOwnsEvents(raw, a_actor->IsOnMount(), GettingOnOrOffMount(a_actor))) {
			return false;
		}
		eventsKept.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "find") {
			float radius = 3000.0f;
			if (words.size() > 2) {
				if (const auto number = DevLine::Number(words[2])) {
					radius = static_cast<float>(*number);
				}
			}
			return Find(words.size() > 1 ? std::string_view{ words[1] } : std::string_view{ "any" }, radius, words.size() > 3 && words[3] == "markers");
		}
		if (!words.empty() && words[0] == "off") {
			return words.size() == 2 ? Off(words[1]) : "usage: sit off <form id>";
		}
		std::string out = Ours();
		for (const auto& [rider, sit] : said.Sits()) {
			if (rider.player == 0) {
				out += std::format(" | said npc={} furn={:08X} base={:08X} kind={}", KeyText(rider.npc), sit.furniture, sit.base, SitRules::KindName(sit.kind));
			}
		}
		for (const auto& [rider, want] : wanted) {
			const auto& sit = want.sit;
			out += std::format(
				" | sitter {} on={} kind={} furn={:08X} base={:08X} found={:08X} lacked={} taken={} near={} dist={:.0f} here={:08X} state={} occupies={:08X} inWanted={} inOther={} "
				"seated={} held={} assist={} rounds={} tried={} seatedBy={} givenUp={} stands={} quicks={} stalls={} marker={} markerHere={} sender={} handover={} heard={}",
				RiderText(rider), sit.on ? 1 : 0, SitRules::KindName(sit.kind), sit.furniture, sit.base, want.found, sit.on && want.found == 0 ? 1 : 0, want.taken ? 1 : 0,
				want.nearBy ? 1 : 0, want.dist, want.actorId, want.state, want.occupies, want.inWanted ? 1 : 0, want.inOther ? 1 : 0, want.seat.Seated() ? 1 : 0,
				want.holding ? 1 : 0, want.assisting ? 1 : 0, want.seat.Rounds(), SitRules::ActName(want.seat.LastTried()), SitRules::ActName(want.seat.SeatedBy()),
				want.seat.GivenUp() ? 1 : 0, want.seat.Stands(), want.seat.Quicks(), want.seat.Stalls(), sit.marker, want.markerHere, want.sender, want.held ? 1 : 0, want.heard);
		}
		out += std::format(" | sits sent={} heard={} notBelieved={} lacked={} taken={} activates={} consoles={} stands={} quicks={} releases={} handedToUs={} assistFrames={} eventsKept={}",
			counts.sent, counts.heard, counts.notBelieved, counts.lacked, counts.taken, counts.activates, counts.consoles, counts.stands, counts.quicks, counts.releases,
			counts.handedToUs, assistFrames.load(std::memory_order_relaxed), eventsKept.load(std::memory_order_relaxed));
		return out;
	}
}
