#include "game/StandInPeace.h"

#include "DevLine.h"
#include "game/PeaceRules.h"
#include "game/RemotePlayers.h"
#include "game/VtableHook.h"
#include "net/FormsCommand.h"

#include <optional>

namespace StandInPeace
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Ask = PeaceRules::Ask;
		constexpr std::size_t ASKS = static_cast<std::size_t>(Ask::kCount);

		// Character's functions the engine calls to ask an actor's AI about another (Actor's virtuals, Skyrim SE/AE
		// slots as CommonLibSSE-NG lists them).
		constexpr std::size_t CHECK_VALID_TARGET_INDEX = 0xD6;  // bool CheckValidTarget(TESObjectREFR& target)
		constexpr std::size_t INITIATE_DIALOGUE_INDEX = 0xD8;   // void InitiateDialogue(Actor* target, PackageLocation*, PackageLocation*)
		constexpr std::size_t INITIATE_SPECTATOR_INDEX = 0xDC;  // void InitiateSpectator(Actor* target)
		constexpr std::size_t INITIATE_FLEE_INDEX = 0xDD;       // void InitiateFlee(TESObjectREFR* fleeRef, ...)

		using CheckTargetFn = bool (*)(RE::Actor*, RE::TESObjectREFR&);
		using DialogueFn = void (*)(RE::Actor*, RE::Actor*, RE::PackageLocation*, RE::PackageLocation*);
		using SpectatorFn = void (*)(RE::Actor*, RE::Actor*);
		using FleeFn = void (*)(RE::Actor*, RE::TESObjectREFR*, bool, bool, bool, RE::TESObjectCELL*, RE::TESObjectREFR*, float, float);
		CheckTargetFn originalCheckTarget = nullptr;
		DialogueFn    originalDialogue = nullptr;
		SpectatorFn   originalSpectator = nullptr;
		FleeFn        originalFlee = nullptr;

		// Per kind of asking (PeaceRules::Ask): hooked, asked at all, turned away (any thread).
		std::array<bool, ASKS>                       hooked{};
		std::array<std::atomic<std::uint64_t>, ASKS> asked{};
		std::array<std::atomic<std::uint64_t>, ASKS> refused{};
		std::array<std::atomic<bool>, ASKS>          refusalTold{};
		std::atomic<RE::FormID>                      lastAsker{ 0 };
		std::atomic<RE::FormID>                      lastStandIn{ 0 };
		std::atomic<std::uint8_t>                    lastAsk{ static_cast<std::uint8_t>(Ask::kCount) };

		// The ghost flag (AfterUpdate): set again after an update had it off, and stand-ins with no process to hold it.
		std::atomic<std::uint64_t> ghostSet{ 0 };
		std::atomic<std::uint64_t> noProcess{ 0 };
		std::atomic<bool>          ghostTold{ false };

		// The sweep (main thread).
		PeaceRules::CalmBook book;
		Clock::time_point    nextSweep{};
		std::uint64_t        sweeps = 0;
		std::uint64_t        calmed = 0;
		std::uint64_t        givenUp = 0;
		std::size_t          targeting = 0;  // at the last sweep
		std::size_t          standInsFighting = 0;  // stand-ins in combat at the last sweep (none expected)
		std::uint64_t        standInsStopped = 0;   // stand-ins found in combat and taken out of it
		RE::FormID           lastStandInStopped = 0;
		RE::FormID           lastCalmed = 0;
		RE::FormID           lastCalmedFrom = 0;

		bool IsStandIn(const RE::TESObjectREFR* a_ref)
		{
			return a_ref && RemotePlayers::IsStandInId(a_ref->GetFormID());
		}

		// Whether the engine's asking a_asker about a_other is turned away (PeaceRules::Refused); counted. Any thread.
		bool Turned(Ask a_ask, const RE::Actor* a_asker, const RE::TESObjectREFR* a_other)
		{
			const auto i = static_cast<std::size_t>(a_ask);
			asked[i].fetch_add(1, std::memory_order_relaxed);
			if (!RemotePlayers::AnyStandIn()) {
				return false;
			}
			const bool askerIs = IsStandIn(a_asker);
			const bool otherIs = IsStandIn(a_other);
			if (!PeaceRules::Refused(askerIs, otherIs)) {
				return false;
			}
			refused[i].fetch_add(1, std::memory_order_relaxed);
			const auto asker = a_asker ? a_asker->GetFormID() : 0u;
			const auto standIn = askerIs ? asker : (a_other ? a_other->GetFormID() : 0u);
			lastAsker.store(asker, std::memory_order_relaxed);
			lastStandIn.store(standIn, std::memory_order_relaxed);
			lastAsk.store(static_cast<std::uint8_t>(a_ask), std::memory_order_relaxed);
			if (!refusalTold[i].exchange(true)) {
				logs::info("Peace: {:08X} asked to {} stand-in {:08X}{}: turned away (the first of its kind; counted in `peace`)", asker, PeaceRules::Name(a_ask),
					standIn, askerIs ? " (the stand-in itself asking)" : "");
			}
			return true;
		}

		bool CheckTarget(RE::Actor* a_this, RE::TESObjectREFR& a_target)
		{
			if (Turned(Ask::kTarget, a_this, &a_target)) {
				return false;
			}
			return originalCheckTarget(a_this, a_target);
		}

		void Dialogue(RE::Actor* a_this, RE::Actor* a_target, RE::PackageLocation* a_first, RE::PackageLocation* a_second)
		{
			if (Turned(Ask::kTalk, a_this, a_target)) {
				return;
			}
			originalDialogue(a_this, a_target, a_first, a_second);
		}

		void Spectator(RE::Actor* a_this, RE::Actor* a_target)
		{
			if (Turned(Ask::kWatch, a_this, a_target)) {
				return;
			}
			originalSpectator(a_this, a_target);
		}

		void Flee(RE::Actor* a_this, RE::TESObjectREFR* a_fleeRef, bool a_runOnce, bool a_knows, bool a_combatMode, RE::TESObjectCELL* a_cell, RE::TESObjectREFR* a_ref,
			float a_fleeFromDist, float a_fleeToDist)
		{
			if (Turned(Ask::kFlee, a_this, a_fleeRef)) {
				return;
			}
			originalFlee(a_this, a_fleeRef, a_runOnce, a_knows, a_combatMode, a_cell, a_ref, a_fleeFromDist, a_fleeToDist);
		}

		template <class Fn>
		bool HookSlot(std::uintptr_t a_vtable, std::size_t a_index, Fn a_hook, Fn& a_original, std::string_view a_what)
		{
			return VtableHook::Slot(a_vtable, a_index, a_hook, a_original, "Peace", a_what);
		}

		// Our player's bounty, every crime faction's together (the rig's: a stand-in hit or witnessed makes none).
		std::int64_t Bounty()
		{
			const auto data = RE::TESDataHandler::GetSingleton();
			if (!data || !RE::PlayerCharacter::GetSingleton()) {
				return -1;
			}
			std::int64_t total = 0;
			for (const auto faction : data->GetFormArray<RE::TESFaction>()) {
				if (faction && faction->TracksCrimes()) {
					total += faction->GetCrimeGold();
				}
			}
			return total;
		}

		struct Fighter
		{
			RE::NiPointer<RE::Actor> actor;
			RE::FormID               standIn = 0;
		};

		// Every actor in high and middle-high process whose combat target is a stand-in, and the stand-ins in combat.
		std::vector<Fighter> FightersOfStandIns(std::vector<RE::NiPointer<RE::Actor>>* a_standInsFighting)
		{
			std::vector<Fighter>                  out;
			std::vector<RE::NiPointer<RE::Actor>> fighting;
			const auto                            lists = RE::ProcessLists::GetSingleton();
			const auto           player = RE::PlayerCharacter::GetSingleton();
			if (lists) {
				for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
					for (const auto& handle : *handles) {
						const auto actor = handle.get();
						if (!actor || actor.get() == player || actor->IsDeleted() || actor->IsDisabled()) {
							continue;
						}
						if (IsStandIn(actor.get())) {
							if (actor->IsInCombat()) {
								fighting.push_back(actor);
							}
							continue;
						}
						const auto target = actor->GetActorRuntimeData().currentCombatTarget.get();
						if (target && IsStandIn(target.get())) {
							out.push_back({ actor, target->GetFormID() });
						}
					}
				}
			}
			if (a_standInsFighting) {
				*a_standInsFighting = std::move(fighting);
			}
			return out;
		}

		// The actors on the cast line from our player through a stand-in (PeaceRules::OnLine), alive, never our player or a
		// stand-in: whom a spell passing through it would hit.
		struct Bystander
		{
			RE::FormID id = 0;
			float      along = 0.0f;  // how far from our player
		};

		std::vector<Bystander> OnTheLine(const RE::NiPoint3& a_from, const RE::NiPoint3& a_through, float a_beyond, float a_width)
		{
			std::vector<Bystander> out;
			const auto             player = RE::PlayerCharacter::GetSingleton();
			const auto             lists = RE::ProcessLists::GetSingleton();
			if (!player || !lists) {
				return out;
			}
			const PeaceRules::Flat from{ a_from.x, a_from.y };
			const PeaceRules::Flat through{ a_through.x, a_through.y };
			for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (const auto& handle : *handles) {
					const auto actor = handle.get();
					if (!actor || actor.get() == player || actor->IsDeleted() || actor->IsDisabled() || actor->IsDead() || IsStandIn(actor.get())) {
						continue;
					}
					const auto at = actor->GetPosition();
					if (PeaceRules::OnLine(from, through, { at.x, at.y }, a_beyond, a_width)) {
						out.push_back({ actor->GetFormID(), at.GetDistance(a_from) });
					}
				}
			}
			return out;
		}
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_Character[0] };
		const auto                      address = vtable.address();
		hooked[static_cast<std::size_t>(Ask::kTarget)] =
			HookSlot<CheckTargetFn>(address, CHECK_VALID_TARGET_INDEX, &CheckTarget, originalCheckTarget, "Character combat target check");
		hooked[static_cast<std::size_t>(Ask::kTalk)] = HookSlot<DialogueFn>(address, INITIATE_DIALOGUE_INDEX, &Dialogue, originalDialogue, "Character dialogue start");
		hooked[static_cast<std::size_t>(Ask::kWatch)] = HookSlot<SpectatorFn>(address, INITIATE_SPECTATOR_INDEX, &Spectator, originalSpectator, "Character spectator start");
		hooked[static_cast<std::size_t>(Ask::kFlee)] = HookSlot<FleeFn>(address, INITIATE_FLEE_INDEX, &Flee, originalFlee, "Character flee start");
		if (!hooked[static_cast<std::size_t>(Ask::kTarget)]) {
			logs::warn("Peace: NPCs here may pick a friend's stand-in as a combat target; the sweep tells them to stop");
		}
	}

	void AfterUpdate(RE::Actor* a_standIn)
	{
		if (!a_standIn) {
			return;
		}
		const auto process = a_standIn->GetActorRuntimeData().currentProcess;
		const auto cached = process ? process->cachedValues : nullptr;
		if (!cached) {
			noProcess.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if (!cached->flags.all(RE::CachedValues::Flags::kActorIsGhost)) {
			cached->flags.set(RE::CachedValues::Flags::kActorIsGhost);
			if (ghostSet.fetch_add(1, std::memory_order_relaxed) == 0 && !ghostTold.exchange(true)) {
				logs::info("Peace: stand-in {:08X} made a ghost here (weapons and spells pass through it)", a_standIn->GetFormID());
			}
		}
	}

	void Frame()
	{
		const auto now = Clock::now();
		if (now < nextSweep) {
			return;
		}
		nextSweep = now + PeaceRules::SWEEP_EVERY;
		if (!RemotePlayers::AnyStandIn()) {
			targeting = 0;
			standInsFighting = 0;
			if (book.Size() != 0) {
				book.Clear();
			}
			return;
		}
		++sweeps;
		book.Age(now);
		std::vector<RE::NiPointer<RE::Actor>> standInsInCombat;
		const auto                            fighters = FightersOfStandIns(&standInsInCombat);
		targeting = fighters.size();
		standInsFighting = standInsInCombat.size();
		// A stand-in in combat itself (its engine-run furniture update put it there, or a console's startcombat named it):
		// its AI never runs, so it would never leave that state on its own. Taken out of it, by the same book.
		for (const auto& standIn : standInsInCombat) {
			switch (book.Seen(standIn->GetFormID(), now)) {
			case PeaceRules::Calm::kStop:
				standIn->StopCombat();
				++standInsStopped;
				lastStandInStopped = standIn->GetFormID();
				if (standInsStopped == 1 || standInsStopped % 50 == 0) {
					logs::info("Peace: stand-in {:08X} was in a fight here: taken out of it ({} so far)", lastStandInStopped, standInsStopped);
				}
				break;
			case PeaceRules::Calm::kGiveUp:
				++givenUp;
				logs::warn("Peace: stand-in {:08X} is in a fight again each time it is taken out ({} times in {} s): left for {} s", standIn->GetFormID(), PeaceRules::CALM_TRIES,
					std::chrono::duration_cast<std::chrono::seconds>(PeaceRules::CALM_WINDOW).count(),
					std::chrono::duration_cast<std::chrono::seconds>(PeaceRules::CALM_REST).count());
				break;
			case PeaceRules::Calm::kWait:
				break;
			}
		}
		for (const auto& fighter : fighters) {
			switch (book.Seen(fighter.actor->GetFormID(), now)) {
			case PeaceRules::Calm::kStop:
				fighter.actor->StopCombat();
				++calmed;
				lastCalmed = fighter.actor->GetFormID();
				lastCalmedFrom = fighter.standIn;
				if (calmed == 1 || calmed % 50 == 0) {
					logs::info("Peace: {:08X} was fighting stand-in {:08X}: told to stop ({} told so far)", lastCalmed, fighter.standIn, calmed);
				}
				break;
			case PeaceRules::Calm::kGiveUp:
				++givenUp;
				logs::warn("Peace: {:08X} fights stand-in {:08X} again each time it is told to stop ({} times in {} s): left alone for {} s", fighter.actor->GetFormID(),
					fighter.standIn, PeaceRules::CALM_TRIES, std::chrono::duration_cast<std::chrono::seconds>(PeaceRules::CALM_WINDOW).count(),
					std::chrono::duration_cast<std::chrono::seconds>(PeaceRules::CALM_REST).count());
				break;
			case PeaceRules::Calm::kWait:
				break;
			}
		}
	}

	void Clear()
	{
		book.Clear();
		targeting = 0;
		standInsFighting = 0;
	}

	std::string Command(std::string_view a_args)
	{
		if (a_args == "targeting") {
			std::vector<RE::NiPointer<RE::Actor>> fighting;
			const auto                            fighters = FightersOfStandIns(&fighting);
			std::string                           out = std::format("targeting={} standInsFighting={}", fighters.size(), fighting.size());
			for (const auto& fighter : fighters) {
				const auto base = fighter.actor->GetActorBase();
				out += std::format(" | {:08X} base={:08X} standIn={:08X} dead={}", fighter.actor->GetFormID(), base ? base->GetFormID() : 0u, fighter.standIn,
					fighter.actor->IsDead() ? 1 : 0);
			}
			return out;
		}
		// peace find <base> [radius]: the nearest actor of that base to our player (the rig's creature, placed with the
		// console), alive or dead, never a stand-in: its id, how far, whether it is dead or in combat, and its target.
		const auto words = DevLine::Words(a_args);
		// peace line <stand-in> [beyond] [width]: the actors on the line from our player through that stand-in (whom a
		// spell passing through it would hit; the rig casts only on a clear line).
		if (!words.empty() && words[0] == "line") {
			const auto id = words.size() > 1 ? FormsCommand::ParseHex(words[1]) : std::nullopt;
			const auto beyond = words.size() > 2 ? DevLine::Number(words[2]) : std::optional<std::uint64_t>{ static_cast<std::uint64_t>(PeaceRules::LINE_BEYOND) };
			const auto width = words.size() > 3 ? DevLine::Number(words[3]) : std::optional<std::uint64_t>{ static_cast<std::uint64_t>(PeaceRules::LINE_WIDTH) };
			if (!id || !beyond || !width || words.size() > 4) {
				return "usage: peace line <stand-in hex> [beyond] [width]";
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto standIn = RE::TESForm::LookupByID<RE::Actor>(*id);
			if (!player || !standIn || !IsStandIn(standIn)) {
				return std::format("line none ({:08X} is no stand-in here)", *id);
			}
			const auto bystanders = OnTheLine(player->GetPosition(), standIn->GetPosition(), static_cast<float>(*beyond), static_cast<float>(*width));
			std::string out = std::format("line clear={} n={} standIn={:08X} beyond={} width={}", bystanders.empty() ? 1 : 0, bystanders.size(), *id, *beyond, *width);
			for (const auto& bystander : bystanders) {
				out += std::format(" | {:08X} along={:.0f}", bystander.id, bystander.along);
			}
			return out;
		}
		if (!words.empty() && words[0] == "find") {
			const auto base = words.size() > 1 ? FormsCommand::ParseHex(words[1]) : std::nullopt;
			const auto radius = words.size() > 2 ? DevLine::Number(words[2]) : std::optional<std::uint64_t>{ 2000 };
			if (!base || !radius || words.size() > 3) {
				return "usage: peace find <base hex> [radius]";
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto lists = RE::ProcessLists::GetSingleton();
			if (!player || !lists) {
				return "actor none (not in the world)";
			}
			const auto               from = player->GetPosition();
			RE::NiPointer<RE::Actor> best;
			float                    bestDistance = static_cast<float>(*radius);
			for (const auto* handles : { &lists->highActorHandles, &lists->middleHighActorHandles }) {
				for (const auto& handle : *handles) {
					const auto actor = handle.get();
					if (!actor || actor.get() == player || actor->IsDeleted() || actor->IsDisabled() || IsStandIn(actor.get())) {
						continue;
					}
					const auto actorBase = actor->GetActorBase();
					if (!actorBase || actorBase->GetFormID() != *base) {
						continue;
					}
					const auto distance = actor->GetPosition().GetDistance(from);
					if (distance <= bestDistance) {
						best = actor;
						bestDistance = distance;
					}
				}
			}
			if (!best) {
				return std::format("actor none (no {:08X} within {})", *base, *radius);
			}
			const auto target = best->GetActorRuntimeData().currentCombatTarget.get();
			return std::format("actor id={:08X} base={:08X} dist={:.0f} dead={} combat={} target={:08X} targetStandIn={}", best->GetFormID(), *base, bestDistance,
				best->IsDead() ? 1 : 0, best->IsInCombat() ? 1 : 0, target ? target->GetFormID() : 0u, target && IsStandIn(target.get()) ? 1 : 0);
		}
		if (!a_args.empty()) {
			return "usage: peace [targeting | find <base hex> [radius] | line <stand-in hex> [beyond] [width]]";
		}
		std::string hooks;
		std::string asks;
		for (std::size_t i = 0; i < ASKS; ++i) {
			const auto name = PeaceRules::Name(static_cast<Ask>(i));
			hooks += std::format("{}{}:{}", i == 0 ? "" : ",", name, hooked[i] ? 1 : 0);
			asks += std::format(" {}Asked={} {}Refused={}", name, asked[i].load(std::memory_order_relaxed), name, refused[i].load(std::memory_order_relaxed));
		}
		const auto last = lastAsk.load(std::memory_order_relaxed);
		return std::format("peace hooks={}{} sweeps={} calmed={} givenUp={} targeting={} standInsFighting={} standInsStopped={} lastStandInStopped={:08X} book={} bookGivenUp={} "
						   "lastAsker={:08X} lastAsk={} lastStandIn={:08X} lastCalmed={:08X} lastCalmedFrom={:08X} ghostSet={} noProcess={} bounty={}{}",
			hooks, asks, sweeps, calmed, givenUp, targeting, standInsFighting, standInsStopped, lastStandInStopped, book.Size(), book.GivenUp(),
			lastAsker.load(std::memory_order_relaxed),
			last < ASKS ? PeaceRules::Name(static_cast<Ask>(last)) : std::string_view{ "none" }, lastStandIn.load(std::memory_order_relaxed), lastCalmed, lastCalmedFrom,
			ghostSet.load(std::memory_order_relaxed), noProcess.load(std::memory_order_relaxed), Bounty(), RemotePlayers::Peace());
	}
}
