#include "game/RemotePlayers.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/Gait.h"
#include "game/GaitDrive.h"
#include "game/GameThread.h"
#include "game/Gear.h"
#include "game/LocalPlayer.h"
#include "game/NpcSync.h"
#include "game/Retire.h"
#include "game/StandInRules.h"
#include "game/StateBuffer.h"
#include "game/VtableHook.h"

#include <Windows.h>

namespace RemotePlayers
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// A stand-in is placed once its friend is this near (game units, about two exterior cells), and taken away past
		// the second: a stand-in in a cell this game has not loaded would stand on nothing.
		constexpr float SHOW_WITHIN = 8000.0f;
		constexpr float HIDE_BEYOND = 10000.0f;
		// After a stand-in is taken away for a save or by its looks changing, the next is placed no sooner than this.
		constexpr auto RESPAWN_AFTER = 250ms;
		// A friend whose race this game lacks (a race mod only they have: their looks name race 0, FormTranslate.h) is
		// shown as a Nord, which every game has (Skyrim.esm's NordRace).
		constexpr RE::FormID NORD_RACE = 0x00013746;
		// What a stand-in holds is read back this often (Gear::Check), and dressed again when it is not what its friend
		// holds, no sooner than REDRESS_COOL after the last time (a hand the game will not fill is not asked every second).
		constexpr auto CHECK_EVERY = 1s;
		constexpr auto REDRESS_COOL = 5s;

		// The kinds of magic effect StandInRules names, the game's own numbers.
		using ArchetypeID = RE::EffectArchetypes::ArchetypeID;
		static_assert(StandInRules::kArchNone == std::to_underlying(ArchetypeID::kNone) && StandInRules::kValueModifier == std::to_underlying(ArchetypeID::kValueModifier) &&
					  StandInRules::kScript == std::to_underlying(ArchetypeID::kScript) && StandInRules::kDualValueModifier == std::to_underlying(ArchetypeID::kDualValueModifier) &&
					  StandInRules::kCommandSummoned == std::to_underlying(ArchetypeID::kCommandSummoned) &&
					  StandInRules::kInvisibility == std::to_underlying(ArchetypeID::kInvisibility) && StandInRules::kLight == std::to_underlying(ArchetypeID::kLight) &&
					  StandInRules::kDarkness == std::to_underlying(ArchetypeID::kDarkness) && StandInRules::kNightEye == std::to_underlying(ArchetypeID::kNightEye) &&
					  StandInRules::kBoundWeapon == std::to_underlying(ArchetypeID::kBoundWeapon) &&
					  StandInRules::kSummonCreature == std::to_underlying(ArchetypeID::kSummonCreature) &&
					  StandInRules::kDetectLife == std::to_underlying(ArchetypeID::kDetectLife) && StandInRules::kReanimate == std::to_underlying(ArchetypeID::kReanimate) &&
					  StandInRules::kValueAndParts == std::to_underlying(ArchetypeID::kValueAndParts) &&
					  StandInRules::kAccumulateMagnitude == std::to_underlying(ArchetypeID::kAccumulateMagnitude) &&
					  StandInRules::kPeakValueModifier == std::to_underlying(ArchetypeID::kPeakValueModifier) && StandInRules::kCloak == std::to_underlying(ArchetypeID::kCloak) &&
					  StandInRules::kSpawnHazard == std::to_underlying(ArchetypeID::kSpawnHazard) && StandInRules::kEtherealize == std::to_underlying(ArchetypeID::kEtherealize) &&
					  StandInRules::kSpawnScriptedRef == std::to_underlying(ArchetypeID::kSpawnScriptedRef),
			"StandInRules' magic effect kinds are the game's");

		struct Remote
		{
			std::string                          name;
			std::optional<Protocol::PlayerLooks> looks;
			StateBuffer::Buffer                  buffer;  // under lock: the hook reads it

			// The stand-in (main thread), and what the hook keeps of it (under lock).
			RE::ActorHandle   standIn;
			RE::FormID        standInId = 0;
			RE::TESNPC*       base = nullptr;
			bool              dressed = false;
			std::size_t       wornShown = 0;  // of looks->worn, the items the stand-in was dressed in
			Clock::time_point placeAfter{};
			Clock::time_point placedAt{};
			std::uint32_t     placed = 0;

			Gait::Shown   shown;  // under lock
			std::uint64_t drawn = 0;
			std::uint64_t takenEvents = 0;
			std::uint64_t refusedEvents = 0;
			std::string   lastRefused;
			Protocol::PlayerState lastDrawn;
			bool          hasDrawn = false;

			// What its friend does (Acted): played on its graph at the moment the stand-in is drawn at (under lock).
			NpcRules::ActionQueue   actions;
			StandInRules::ActCounts played;
			StandInRules::ActCounts refused;  // its graph turned them away
			std::uint64_t           actionsIn = 0;
			std::uint64_t           actionsUnplaced = 0;  // came while it had no stand-in here: not played
			std::string             lastActionRefused;
			float                   aim = 0.0f;  // the pitch it aims at (StandInRules::AimPitch)
			// What it did here, its friend's game being the one that deals it all (the hooks and event sinks, under lock):
			// effects of its spells refused and let take hold (on itself), its blows and casts, health its blows took
			// here (none expected: its damage is nothing) and gave back.
			std::uint64_t magicRefused = 0;
			std::uint64_t magicKept = 0;
			std::uint64_t blows = 0;
			std::uint64_t casts = 0;
			std::uint64_t damaged = 0;
			float         dealt = 0.0f;
			float         undone = 0.0f;

			// What it holds (main thread): the gear it was dressed for, read back now and then.
			Protocol::NpcGear       gear;
			std::size_t             gearMissing = 0;
			std::size_t             refusedForms = 0;  // named forms no player wears or holds there: left out
			NpcRules::GearShown     held;
			bool                    checked = false;
			std::uint32_t           voiceShown = 0;
			std::uint32_t           voiceWanted = 0;     // its friend's voice, as the looks name it (0: none ready)
			bool                    voiceJudged = false;  // named (not unnamed): the stand-in's is checked against it
			std::uint32_t           baseShouts = 0;       // the shouts its base kept when it was placed (Gear::BaseShouts: never changed)
			std::uint64_t           redressed = 0;
			std::uint64_t           ammoGiven = 0;
			std::vector<std::uint32_t> wornLogged;  // the worn list last said in the log
			Clock::time_point       nextCheck{};
			Clock::time_point       redressAfter{};
			StandInRules::DrawFix   drawFix;  // its weapon drawn or put away as its friend's is (the game's own way)
			// Frames (not asks) that told it again, as the game's own update would: one slow draw counts dozens (under lock).
			std::uint64_t           drawsFinished = 0;
		};

		// The game's weapon states, as StandInRules reads them.
		static_assert(std::to_underlying(RE::WEAPON_STATE::kSheathed) == std::to_underlying(StandInRules::Weapon::kSheathed) &&
						  std::to_underlying(RE::WEAPON_STATE::kWantToDraw) == std::to_underlying(StandInRules::Weapon::kWantToDraw) &&
						  std::to_underlying(RE::WEAPON_STATE::kDrawing) == std::to_underlying(StandInRules::Weapon::kDrawing) &&
						  std::to_underlying(RE::WEAPON_STATE::kDrawn) == std::to_underlying(StandInRules::Weapon::kDrawn) &&
						  std::to_underlying(RE::WEAPON_STATE::kWantToSheathe) == std::to_underlying(StandInRules::Weapon::kWantToSheathe) &&
						  std::to_underlying(RE::WEAPON_STATE::kSheathing) == std::to_underlying(StandInRules::Weapon::kSheathing),
			"StandInRules::Weapon follows RE::WEAPON_STATE");

		std::optional<StandInRules::Weapon> WeaponOf(RE::Actor* a_actor)
		{
			const auto state = a_actor ? a_actor->AsActorState() : nullptr;
			return state ? std::optional{ static_cast<StandInRules::Weapon>(std::to_underlying(state->GetWeaponState())) } : std::nullopt;
		}

		std::mutex                                lock;  // players' buffers and graph state, byActor and retiring
		std::unordered_map<std::uint32_t, Remote> players;
		std::unordered_map<RE::Actor*, std::uint32_t> byActor;   // stand-in -> player id
		std::unordered_map<RE::Actor*, RE::FormID>    retiring;  // stand-ins on their way out -> their form id
		std::atomic<std::size_t>                  standIns{ 0 };
		std::atomic<std::size_t>                  watched{ 0 };  // byActor and retiring: the hook looks only when some
		// Copies of other games' NPCs this game shows (NpcSync): while there are some, the hook asks NpcSync about every
		// other Character, and the leg channels look them up.
		std::atomic<std::size_t> copies{ 0 };

		bool Watching()
		{
			return watched.load(std::memory_order_relaxed) != 0 || copies.load(std::memory_order_relaxed) != 0;
		}

		void CountWatched()  // under lock
		{
			standIns = byActor.size();
			watched = byActor.size() + retiring.size();
		}

		// The player whose stand-in a_ref is, if it is one now (under lock).
		Remote* RemoteOf(const RE::TESObjectREFR* a_ref)
		{
			if (!a_ref || !a_ref->IsActor()) {
				return nullptr;
			}
			const auto actor = const_cast<RE::Actor*>(static_cast<const RE::Actor*>(a_ref));
			const auto it = byActor.find(actor);
			if (it == byActor.end()) {
				return nullptr;
			}
			const auto player = players.find(it->second);
			return player != players.end() && player->second.standInId == actor->GetFormID() ? &player->second : nullptr;
		}

		// The stand-ins on their way out of the world (main thread): disabled, then deleted once that is done
		// (src/game/Retire.h), then held until the game lets go of them. The hook keeps their AI off all the while.
		struct Grave
		{
			RE::ActorHandle                  actor;
			RE::FormID                       id = 0;
			RE::FormID                       base = 0;    // what it was placed from: a load only removes an actor of that base
			Clock::time_point                placedAt{};  // long ago for one this game did not place
			std::optional<Clock::time_point> disabledAt;
			std::uint64_t                    disabledTick = 0;
			bool                             deleted = false;
			bool                             toldStuck = false;
		};
		std::vector<Grave> graveyard;

		// Actors this game placed (stand-ins, and copies of other games' spawns) taken out of the world for the save being
		// written, for the co-save: each one's form id and its base's; and those the co-save of a loaded save named,
		// removed once the load is done if the id still names an actor of that base.
		struct Taken
		{
			RE::FormID id = 0;
			RE::FormID base = 0;  // 0: a record of version 1, which named stand-ins only (any preset base)
		};
		std::vector<Taken>      takenForSave;
		std::vector<Taken>      namedByLoad;
		bool                    hooked = false;         // the Character update is ours: without it, no stand-in is placed
		bool                    placedThisRun = false;  // a stand-in was placed since the game started
		bool                    coSaveRecord = false;   // the loaded save's co-save has a stand-ins record

		// Whether the save and load sweeps of preset actors have a reason to run: this game placed a stand-in, the
		// loaded save was written with some taken out, or friends are in the session. Otherwise they could only find
		// another mod's actors, and walk every form for nothing.
		bool SweepWanted()
		{
			return placedThisRun || coSaveRecord || !players.empty();
		}

		// The preset base for a race and sex, once looked for (nullptr: there is none).
		std::map<std::pair<RE::FormID, bool>, RE::TESNPC*> bases;

		// ---- the hook ----
		using UpdateFn = void (*)(RE::Actor*, float);
		constexpr std::size_t UPDATE_INDEX = 0xAD;  // Actor::Update
		UpdateFn              originalUpdate = nullptr;
		std::uintptr_t        characterVtable = 0;

		const RE::BSFixedString& Name(std::string_view a_text)
		{
			// The graph's names, made once each (a BSFixedString takes the game's string cache's lock).
			static std::mutex                                       namesLock;
			static std::map<std::string, RE::BSFixedString, std::less<>> names;
			std::scoped_lock                                        l{ namesLock };
			auto                                                    it = names.find(a_text);
			if (it == names.end()) {
				it = names.emplace(std::string{ a_text }, RE::BSFixedString{ a_text }).first;
			}
			return it->second;
		}

		void SetMoveFlags(RE::Actor* a_actor, const Gait::Step& a_step, const Protocol::PlayerState& a_state)
		{
			const auto state = a_actor->AsActorState();
			if (!state) {
				return;
			}
			auto& flags = state->actorState1;
			flags.sneaking = a_step.shown.sneaking ? 1 : 0;
			flags.sprinting = a_step.shown.sprinting ? 1 : 0;
			flags.running = a_step.shown.moving && !a_step.shown.sprinting && a_state.speed > 200.0f ? 1 : 0;
			flags.walking = a_step.shown.moving && a_state.speed <= 200.0f ? 1 : 0;
			flags.movingForward = a_step.shown.moving ? 1 : 0;
		}

		// Where the graph's events are sent from: the stand-in's own update (the hook, by default) or the tick (dev
		// `graph from tick`, a probe for why the graph turns moveStart away).
		std::atomic<bool> eventsFromTick{ false };

		// ---- the graph's Speed and Direction (GaitDrive.h) ----
		// The engine gives an actor's graph its Speed and Direction itself, each time the graph updates, through one
		// channel object per variable: BSTAnimationGraphDataChannel<Actor, float, ActorSpeedChannel>, whose poll (slot 1
		// of its vtable) reads the movement speed in the actor's process (AIProcess middle-high +2A8) over the actor's
		// scale, and <..., ActorDirectionChannel>, the movement direction (+B8) as a fraction of a turn. Every
		// high-process actor's movement job (the process lists' movement jobs, then Actor's movement update) sets both
		// each frame from the actor's movement controller's desired movement, which is none for an actor whose AI never
		// runs: whatever was set on a stand-in's graph read 0 again at its next update, and its legs stood still while it
		// slid along. So each poll is hooked: after the engine's, a stand-in's channel holds its friend's value, which
		// the graph then reads. Nothing else of the actor changes: its movement controller still moves it by nothing.
		//
		// A channel: its vtable at 0, the variable's name at 10, the value the graph is given at 18, the channel's own
		// data at 20 (ActorSpeedChannel: the last raw speed and the scale), the actor at 28 (Skyrim SE 1.7.104).
		constexpr std::size_t CHANNEL_POLL_INDEX = 1;
		constexpr std::size_t CHANNEL_VALUE = 0x18;
		constexpr std::size_t CHANNEL_ACTOR = 0x28;
		using PollFn = void (*)(void*, bool);
		PollFn originalSpeedPoll = nullptr;
		PollFn originalDirectionPoll = nullptr;

		GaitDrive::Table<const RE::Actor*> drive;
		// The channel values given to stand-ins' graphs (dev `standins`: driven=).
		std::atomic<std::uint64_t> speedsDriven{ 0 };
		std::atomic<std::uint64_t> directionsDriven{ 0 };

		// The values to give a channel's actor, if it is a stand-in or an NPC copy this game moves. The polls run as graphs
		// update, on the game's job threads: only the table's own lock is taken (GaitDrive.h), never the players' one.
		std::optional<GaitDrive::Values> DrivenFor(void* a_channel)
		{
			if (!Watching()) {
				return std::nullopt;
			}
			const auto actor = *reinterpret_cast<RE::Actor* const*>(static_cast<std::byte*>(a_channel) + CHANNEL_ACTOR);
			return actor ? drive.Get(actor, actor->GetFormID(), Clock::now()) : std::nullopt;
		}

		float& ChannelValue(void* a_channel)
		{
			return *reinterpret_cast<float*>(static_cast<std::byte*>(a_channel) + CHANNEL_VALUE);
		}

		void SpeedPoll(void* a_channel, bool a_flag)
		{
			originalSpeedPoll(a_channel, a_flag);
			if (const auto values = DrivenFor(a_channel)) {
				ChannelValue(a_channel) = values->speed;
				speedsDriven.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void DirectionPoll(void* a_channel, bool a_flag)
		{
			originalDirectionPoll(a_channel, a_flag);
			if (const auto values = DrivenFor(a_channel)) {
				ChannelValue(a_channel) = values->direction;
				directionsDriven.fetch_add(1, std::memory_order_relaxed);
			}
		}

		template <class Fn>
		bool HookSlot(std::uintptr_t a_vtable, std::size_t a_index, Fn a_hook, Fn& a_original, std::string_view a_what)
		{
			return VtableHook::Slot(a_vtable, a_index, a_hook, a_original, "Players", a_what);
		}

		// Sends a step's events to a stand-in's graph and keeps the count of those taken and turned away. One turned away
		// is sent again next time (what the graph shows stays what it was).
		void TellGraph(RE::Actor* a_actor, std::uint32_t a_player, const Gait::Step& a_step, const Gait::Shown& a_was)
		{
			if (a_step.events.empty()) {
				return;
			}
			std::string   refused;
			std::uint64_t taken = 0;
			for (const auto event : a_step.events) {
				if (a_actor->NotifyAnimationGraph(Name(event))) {
					++taken;
				} else {
					refused = event;
				}
			}
			std::scoped_lock l{ lock };
			const auto       player = players.find(a_player);
			if (player == players.end() || player->second.standInId != a_actor->GetFormID()) {
				return;
			}
			auto& remote = player->second;
			if (remote.takenEvents == 0 && taken > 0) {
				logs::info("Players: player {}'s stand-in {:08X}: its graph took its first event ({} of {}{})", a_player, remote.standInId, taken,
					a_step.events.size(), refused.empty() ? "" : "; turned away " + refused);
			}
			remote.takenEvents += taken;
			if (!refused.empty()) {
				remote.shown = a_was;
				++remote.refusedEvents;
				if (remote.lastRefused != refused || remote.refusedEvents == 1) {
					logs::info("Players: player {}'s stand-in {:08X}: its graph turned {} away ({} times so far; sent from the {})", a_player, remote.standInId,
						refused, remote.refusedEvents, eventsFromTick ? "tick" : "update");
				}
				remote.lastRefused = refused;
			} else {
				remote.shown = a_step.shown;
			}
		}

		// Plays a friend's actions that are due on their stand-in's graph, and counts each by kind, played or turned away.
		void PlayActions(RE::Actor* a_actor, std::uint32_t a_player, const std::vector<std::string>& a_events)
		{
			if (a_events.empty()) {
				return;
			}
			std::vector<std::pair<std::string_view, bool>> results;
			results.reserve(a_events.size());
			for (const auto& event : a_events) {
				results.emplace_back(event, a_actor->NotifyAnimationGraph(Name(event)));
			}
			std::scoped_lock l{ lock };
			const auto       player = players.find(a_player);
			if (player == players.end() || player->second.standInId != a_actor->GetFormID()) {
				return;
			}
			auto& remote = player->second;
			for (const auto& [event, taken] : results) {
				if (taken) {
					if (remote.played.Total() == 0) {
						logs::info("Players: player {}'s stand-in {:08X} played its friend's first action ({})", a_player, remote.standInId, event);
					}
					remote.played.Add(event);
				} else {
					if (remote.refused.Total() == 0 || remote.lastActionRefused != event) {
						logs::info("Players: player {}'s stand-in {:08X}: its graph turned its friend's {} away ({} turned away so far)", a_player, remote.standInId, event,
							remote.refused.Total() + 1);
					}
					remote.refused.Add(event);
					remote.lastActionRefused = std::string{ event };
				}
			}
		}

		// ---- a stand-in deals nothing here (StandInRules::MagicKept; docs/DESIGN.md, "Players") ----
		// Its friend's game deals every blow, shot and spell of theirs; one dealt here too would land twice. Its weapons do
		// no damage (kAttackDamageMult 0, Neutralise), and the effects of its spells, enchantments, poisons and shouts are
		// refused as they are put on an actor: MagicTarget::AddTarget (slot 1 of an actor's magic target vtable), hooked
		// for Character (read from a stand-in, a Character, when the first is placed) and for our player (read from the
		// player). Any health its blows still take (an arrow, say) is given back and counted (HandleHealthDamage).
		using AddTargetFn = bool (*)(RE::MagicTarget*, RE::MagicTarget::AddTargetData&);
		constexpr std::size_t ADD_TARGET_INDEX = 1;
		AddTargetFn           originalCharacterAddTarget = nullptr;
		AddTargetFn           originalPlayerAddTarget = nullptr;
		bool                  characterMagicTried = false;
		std::atomic<bool>     characterMagicHooked{ false };
		std::atomic<bool>     playerMagicHooked{ false };

		// Whether an effect may take hold on a_this: any caster's but a stand-in's; a stand-in's on itself, of a kind
		// MagicKept keeps. Counted on its stand-in. On whatever thread the game adds effects.
		bool LetMagic(RE::MagicTarget* a_this, const RE::MagicTarget::AddTargetData& a_data)
		{
			if (!Watching() || !a_data.caster || !a_data.caster->IsActor()) {
				return true;
			}
			const auto caster = static_cast<RE::Actor*>(a_data.caster);
			const bool onItself = caster->AsMagicTarget() == a_this;
			const auto base = a_data.effect ? a_data.effect->baseEffect : nullptr;
			const auto archetype = base ? static_cast<std::int32_t>(std::to_underlying(base->GetArchetype())) : StandInRules::kArchNone;
			std::scoped_lock l{ lock };
			if (const auto gone = retiring.find(caster); gone != retiring.end() && gone->second == caster->GetFormID()) {
				return false;  // on its way out: nothing of it takes hold anywhere
			}
			const auto remote = RemoteOf(caster);
			if (!remote) {
				return true;
			}
			const bool kept = StandInRules::MagicKept(onItself, archetype);
			++(kept ? remote->magicKept : remote->magicRefused);
			return kept;
		}

		bool CharacterAddTarget(RE::MagicTarget* a_this, RE::MagicTarget::AddTargetData& a_data)
		{
			return LetMagic(a_this, a_data) && originalCharacterAddTarget(a_this, a_data);
		}

		bool PlayerAddTarget(RE::MagicTarget* a_this, RE::MagicTarget::AddTargetData& a_data)
		{
			return LetMagic(a_this, a_data) && originalPlayerAddTarget(a_this, a_data);
		}

		// Character's magic target vtable, read from a placed stand-in (a Character: Place checks), so no index into the
		// class's list of vtables is assumed. Main thread, once a run.
		void HookCharacterMagic(RE::Actor* a_standIn)
		{
			if (characterMagicTried) {
				return;
			}
			characterMagicTried = true;
			const auto target = a_standIn->AsMagicTarget();
			if (!target) {
				logs::warn("Players: a stand-in has no magic target: its spells' effects are not held back");
				return;
			}
			const auto vtable = *reinterpret_cast<const std::uintptr_t*>(target);
			REL::Relocation<std::uintptr_t> listed{ RE::VTABLE_Character[4] };
			if (vtable != listed.address()) {
				logs::info("Players: Character's magic target vtable is not its fifth listed one (game+{:X} vs game+{:X}); the object's is hooked",
					vtable - REL::Module::get().base(), listed.address() - REL::Module::get().base());
			}
			characterMagicHooked = HookSlot<AddTargetFn>(vtable, ADD_TARGET_INDEX, &CharacterAddTarget, originalCharacterAddTarget, "Character magic target's add");
			if (!characterMagicHooked) {
				logs::warn("Players: stand-ins' spells, enchantments and shouts take hold on NPCs here as well as in their friend's game");
			}
		}

		void HookPlayerMagic()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto target = player ? player->AsMagicTarget() : nullptr;
			if (!target) {
				logs::warn("Players: no player magic target yet: stand-ins' spells may take hold on our player");
				return;
			}
			const auto vtable = *reinterpret_cast<const std::uintptr_t*>(target);
			playerMagicHooked = HookSlot<AddTargetFn>(vtable, ADD_TARGET_INDEX, &PlayerAddTarget, originalPlayerAddTarget, "player's magic target's add");
			if (!playerMagicHooked) {
				logs::warn("Players: stand-ins' spells, enchantments and shouts take hold on our player");
			}
		}

		using HealthDamageFn = void (*)(RE::Actor*, RE::Actor*, float);
		constexpr std::size_t HEALTH_DAMAGE_INDEX = 0x104;  // Actor::HandleHealthDamage
		HealthDamageFn        originalCharacterHealthDamage = nullptr;
		HealthDamageFn        originalPlayerHealthDamage = nullptr;

		// Health a stand-in's blow took here: counted, and given back (its friend's game deals that blow).
		void AfterHealthDamage(RE::Actor* a_target, RE::Actor* a_attacker, float a_damage)
		{
			if (!a_attacker || a_attacker == a_target || a_damage == 0.0f || !Watching()) {
				return;
			}
			const float amount = std::abs(a_damage);
			{
				std::scoped_lock l{ lock };
				const auto       remote = RemoteOf(a_attacker);
				if (!remote) {
					return;
				}
				++remote->damaged;
				remote->dealt += amount;
				if (remote->damaged == 1) {
					logs::warn("Players: stand-in {:08X} took {:.1f} health from {:08X} here; given back (its friend's game deals its blows)", remote->standInId, amount,
						a_target->GetFormID());
				}
			}
			const auto values = a_target->AsActorValueOwner();
			if (values && !a_target->IsDead()) {
				values->RestoreActorValue(RE::ActorValue::kHealth, amount);
				std::scoped_lock l{ lock };
				if (const auto remote = RemoteOf(a_attacker)) {
					remote->undone += amount;
				}
			}
		}

		void CharacterHealthDamage(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
		{
			originalCharacterHealthDamage(a_this, a_attacker, a_damage);
			AfterHealthDamage(a_this, a_attacker, a_damage);
		}

		void PlayerHealthDamage(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
		{
			originalPlayerHealthDamage(a_this, a_attacker, a_damage);
			AfterHealthDamage(a_this, a_attacker, a_damage);
		}

		// A stand-in's blows and casts as the game reports them (dev `actions`: blows=, casts=): what the rig compares with
		// what its friend did.
		struct DeedSink final : RE::BSTEventSink<RE::TESHitEvent>, RE::BSTEventSink<RE::TESSpellCastEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (a_event && Watching()) {
					std::scoped_lock l{ lock };
					if (const auto remote = RemoteOf(a_event->cause.get())) {
						++remote->blows;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (a_event && Watching()) {
					std::scoped_lock l{ lock };
					if (const auto remote = RemoteOf(a_event->object.get())) {
						++remote->casts;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		DeedSink deedSink;

		void UpdateHook(RE::Actor* a_this, float a_delta)
		{
			if (!Watching()) {
				originalUpdate(a_this, a_delta);
				return;
			}
			std::optional<StateBuffer::Sampled> sample;
			Gait::Step                          step;
			Gait::Shown                         was;
			bool                                ours = false;
			std::uint32_t                       playerId = 0;
			std::vector<std::string>            actions;
			float                               aim = 0.0f;
			const bool                          eventsHere = !eventsFromTick.load(std::memory_order_relaxed);
			{
				std::scoped_lock l{ lock };
				if (const auto gone = retiring.find(a_this); gone != retiring.end() && gone->second == a_this->GetFormID()) {
					ours = true;  // on its way out: still no AI of its own, and no longer moved
				} else if (const auto it = byActor.find(a_this); it != byActor.end()) {
					if (const auto player = players.find(it->second); player != players.end() && player->second.standInId == a_this->GetFormID()) {
						ours = true;
						playerId = it->second;
						auto& remote = player->second;
						sample = remote.buffer.Sample(Clock::now());
						if (sample) {
							was = remote.shown;
							step = Gait::Next(remote.shown, sample->state, sample->held);
							if (eventsHere) {
								remote.shown = step.shown;  // else the tick tells the graph (TellGraph from Frame)
							}
							remote.lastDrawn = sample->state;
							remote.hasDrawn = true;
							++remote.drawn;
							// Its friend's actions, each at the moment it was done, as the stand-in is drawn (a delay behind),
							// told its graph where its legs' events are (the tick, in tick mode: Frame).
							if (eventsHere) {
								actions = remote.actions.Due(Clock::now() - StateBuffer::Buffer::Delay());
							}
							aim = remote.aim = StandInRules::AimPitch(sample->state);
						}
					}
				}
			}
			if (!ours) {
				// A copy of an NPC another game runs, moved by its runner's states (NpcSync); any other actor, or a copy
				// left to its own AI, gets the game's own update.
				if (copies.load(std::memory_order_relaxed) == 0 || !NpcSync::UpdateCopy(a_this, a_delta)) {
					originalUpdate(a_this, a_delta);
				}
				return;
			}
			// Its own AI never runs: no packages, no combat, no greetings. Where its friend is, the engine's no-AI update
			// places its 3D.
			if (sample) {
				a_this->SetPosition({ sample->state.x, sample->state.y, sample->state.z }, true);
				a_this->SetHeading(sample->state.heading);
				// Aimed where its friend aims while a weapon or a spell is out (the way its arrows and spells leave), level
				// otherwise.
				a_this->data.angle.x = aim;
				SetMoveFlags(a_this, step, sample->state);
				// What its graph's Speed and Direction channels give it from now on (SpeedPoll, DirectionPoll).
				drive.Set(a_this, a_this->GetFormID(), { step.speed, step.direction }, Clock::now());
			}
			a_this->UpdateNoAI(a_delta);
			if (sample && eventsHere) {
				TellGraph(a_this, playerId, step, was);
			}
			if (sample && eventsHere) {
				PlayActions(a_this, playerId, actions);
			}
			// A draw or a sheathe its graph turned away is asked again, as the game's own update would (FinishDraw).
			if (sample && RemotePlayers::FinishDraw(a_this)) {
				std::scoped_lock l{ lock };
				if (const auto player = players.find(playerId); player != players.end() && player->second.standInId == a_this->GetFormID()) {
					++player->second.drawsFinished;
				}
			}
		}

		// ---- placing and taking away ----
		RE::TESNPC* BaseFor(RE::FormID a_race, bool a_female)
		{
			const auto key = std::pair{ a_race, a_female };
			if (const auto it = bases.find(key); it != bases.end()) {
				return it->second;
			}
			RE::TESNPC* best = nullptr;
			if (const auto data = RE::TESDataHandler::GetSingleton()) {
				for (const auto npc : data->GetFormArray<RE::TESNPC>()) {
					if (!npc || !npc->IsPreset() || !npc->race || npc->race->GetFormID() != a_race || npc->IsFemale() != a_female) {
						continue;
					}
					if (!best || npc->GetFormID() < best->GetFormID()) {
						best = npc;  // the lowest id: every game picks the same one
					}
				}
			}
			bases.emplace(key, best);
			if (best) {
				logs::info("Players: stand-ins of race {:08X} ({}) are made from preset {:08X}", a_race, a_female ? "female" : "male", best->GetFormID());
			} else {
				logs::warn("Players: no character-creation preset of race {:08X} ({}): no stand-in can be made for it", a_race, a_female ? "female" : "male");
			}
			return best;
		}

		bool IsStandInBase(const RE::TESBoundObject* a_base)
		{
			const auto npc = a_base ? a_base->As<RE::TESNPC>() : nullptr;
			return npc && npc->IsPreset();
		}

		bool InGraveyard(RE::FormID a_id)
		{
			return std::ranges::any_of(graveyard, [&](const Grave& a_grave) { return a_grave.id == a_id; });
		}

		// Sent on its way out of the world (Retire.h): disabled at the next tend, deleted once that is done. a_placedAt:
		// when it was placed (one whose 3D is still loading waits for it); long ago for one this game did not place.
		void SendOut(RE::Actor* a_actor, Clock::time_point a_placedAt)
		{
			const auto id = a_actor->GetFormID();
			if (InGraveyard(id)) {
				return;
			}
			const auto base = a_actor->GetObjectReference();
			graveyard.push_back({ .actor = a_actor->GetHandle(), .id = id, .base = base ? base->GetFormID() : 0, .placedAt = a_placedAt });
			std::scoped_lock l{ lock };
			retiring[a_actor] = id;
			CountWatched();
		}

		void LetGo(const Grave& a_grave)
		{
			std::scoped_lock l{ lock };
			std::erase_if(retiring, [&](const auto& a_entry) { return a_entry.second == a_grave.id; });
			CountWatched();
		}

		// The graveyard's step for this tick: each stand-in on its way out is disabled, deleted or let go of, as
		// Retire::Next says. a_save: before a save; every one still there is named in the co-save.
		void Tend(bool a_save)
		{
			if (graveyard.empty()) {
				return;
			}
			const auto now = Clock::now();
			const auto tick = GameThread::Ticks();
			std::erase_if(graveyard, [&](Grave& a_grave) {
				const auto actor = a_grave.actor.get();
				if (!actor || actor->GetFormID() != a_grave.id) {
					logs::info("Players: stand-in {:08X} is gone{} (the game let go of it)", a_grave.id, a_grave.deleted ? "" : " before it was deleted");
					LetGo(a_grave);
					return true;
				}
				if (a_save) {
					takenForSave.push_back({ a_grave.id, a_grave.base });
				}
				if (a_grave.deleted) {
					return false;  // held until the game frees it, so the hook never runs its AI meanwhile
				}
				const Retire::Facts facts{
					.has3D = actor->Is3DLoaded(),
					.sincePlaced = now - a_grave.placedAt,
					.sinceDisabled = a_grave.disabledAt ? std::optional{ now - *a_grave.disabledAt } : std::nullopt,
					.ticksSinceDisabled = a_grave.disabledAt ? tick - a_grave.disabledTick : 0,
				};
				switch (Retire::Next(facts)) {
				case Retire::Step::kDisable:
					actor->Disable();
					a_grave.disabledAt = now;
					a_grave.disabledTick = tick;
					logs::info("Players: stand-in {:08X} disabled{}; deleted once the game has taken it out", a_grave.id, a_save ? " before the save" : "");
					break;
				case Retire::Step::kDelete:
					actor->SetDelete(true);
					a_grave.deleted = true;
					logs::info("Players: stand-in {:08X} marked for deletion, {} ms and {} ticks after it was disabled", a_grave.id,
						std::chrono::duration_cast<std::chrono::milliseconds>(*facts.sinceDisabled).count(), facts.ticksSinceDisabled);
					break;
				case Retire::Step::kWait:
					if (Retire::Stuck(facts) && !a_grave.toldStuck) {
						a_grave.toldStuck = true;
						logs::warn("Players: stand-in {:08X} was disabled {} s ago and still has its 3D; left disabled, not deleted", a_grave.id,
							std::chrono::duration_cast<std::chrono::seconds>(*facts.sinceDisabled).count());
					} else if (a_save && !a_grave.disabledAt) {
						logs::info("Players: stand-in {:08X} is still loading its 3D: left as it is for this save (the co-save names it)", a_grave.id);
					}
					break;
				}
				return false;
			});
		}

		// Out of the world, never both disabled and deleted in one frame (Retire.h): its AI stays off meanwhile.
		void TakeAway(std::uint32_t a_id, Remote& a_remote, std::string_view a_why)
		{
			const auto actor = a_remote.standIn.get();
			const bool same = actor && actor->GetFormID() == a_remote.standInId;
			if (same) {
				drive.Erase(actor.get());
				SendOut(actor.get(), a_remote.placedAt);
				logs::info("Players: player {}'s stand-in {:08X} taken away ({})", a_id, a_remote.standInId, a_why);
			}
			{
				std::scoped_lock l{ lock };
				for (auto it = byActor.begin(); it != byActor.end();) {
					it = it->second == a_id ? byActor.erase(it) : std::next(it);
				}
				CountWatched();
				a_remote.shown = {};
				a_remote.standInId = 0;
				a_remote.actions.Clear();  // no stand-in plays them now
			}
			a_remote.standIn = {};
			a_remote.dressed = false;
			a_remote.checked = false;
			a_remote.wornShown = 0;
			a_remote.voiceShown = 0;
			a_remote.voiceWanted = 0;
			a_remote.voiceJudged = false;
			a_remote.drawFix.Reset();
			a_remote.placeAfter = Clock::now() + RESPAWN_AFTER;
		}

		bool SameSpace(const Protocol::PlayerState& a_a, const Protocol::PlayerState& a_b)
		{
			return a_a.cell == a_b.cell && a_a.worldspace == a_b.worldspace;
		}

		float Distance(const Protocol::PlayerState& a_a, const Protocol::PlayerState& a_b)
		{
			return std::hypot(a_a.x - a_b.x, a_a.y - a_b.y, a_a.z - a_b.z);
		}

		// What makes a stand-in deal nothing and never fall: its weapons' damage nothing (kAttackDamageMult; its spells'
		// effects are refused by the magic target hooks), essential, its health, magicka and stamina kept full (KeepFull:
		// its friend's game is the one that is hurt, tires and runs out of magicka; a stand-in that did would stop
		// casting or power attacking while its friend goes on).
		void Neutralise(RE::Actor* a_actor)
		{
			if (const auto values = a_actor->AsActorValueOwner()) {
				values->SetActorValue(RE::ActorValue::kAttackDamageMult, 0.0f);
			}
			a_actor->GetActorRuntimeData().boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential);
		}

		void KeepFull(RE::Actor* a_actor)
		{
			const auto values = a_actor->AsActorValueOwner();
			if (!values) {
				return;
			}
			for (const auto value : { RE::ActorValue::kHealth, RE::ActorValue::kMagicka, RE::ActorValue::kStamina }) {
				if (const float lost = -a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, value); lost > 0.0f) {
					values->RestoreActorValue(value, lost);
				}
			}
		}

		void Place(std::uint32_t a_id, Remote& a_remote, const Protocol::PlayerState& a_at)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto race = a_remote.looks->race;  // none, or a runtime form (another game's ID): a Nord
			const auto base = BaseFor(race != 0 && !FormIds::IsRuntime(race) ? race : NORD_RACE, a_remote.looks->female);
			if (!base || !player) {
				a_remote.placeAfter = Clock::now() + 10s;  // said once in the log; looked at again now and then
				return;
			}
			const auto placed = player->PlaceObjectAtMe(base, false);
			const auto actor = placed ? placed->As<RE::Actor>() : nullptr;
			if (!actor) {
				logs::warn("Players: the game placed no actor for player {}", a_id);
				a_remote.placeAfter = Clock::now() + 5s;
				return;
			}
			if (*reinterpret_cast<std::uintptr_t*>(actor) != characterVtable) {
				logs::warn("Players: the stand-in {:08X} is not a Character; taken away", actor->GetFormID());
				SendOut(actor, Clock::now());
				a_remote.placeAfter = Clock::now() + 30s;
				return;
			}
			actor->SetPosition({ a_at.x, a_at.y, a_at.z }, true);
			actor->SetHeading(a_at.heading);
			// Nothing a player does to a stand-in reaches the preset NPC under it: talking, pickpocketing, trading,
			// follower commands and any other activation are all one activation, blocked here (the prompt is hidden
			// too where the game knows that flag). It is a friend, not an NPC with dialogue of its own.
			actor->extraList.SetExtraFlags(static_cast<RE::ExtraFlags::Flag>(
				std::to_underlying(RE::ExtraFlags::Flag::kBlockActivate) | std::to_underlying(RE::ExtraFlags::Flag::kBlockPlayerActivate) |
				std::to_underlying(RE::ExtraFlags::Flag::kBlockActivateText)), true);
			if (!a_remote.name.empty()) {
				actor->SetDisplayName(a_remote.name, true);
			}
			Neutralise(actor);
			a_remote.baseShouts = Gear::BaseShouts(actor);
			HookCharacterMagic(actor);
			placedThisRun = true;
			a_remote.standIn = actor->GetHandle();
			a_remote.base = base;
			a_remote.dressed = false;
			a_remote.checked = false;
			a_remote.wornShown = 0;
			a_remote.voiceShown = 0;
			a_remote.voiceWanted = 0;
			a_remote.voiceJudged = false;
			a_remote.drawFix.Reset();
			a_remote.placedAt = Clock::now();
			++a_remote.placed;
			{
				std::scoped_lock l{ lock };
				a_remote.standInId = actor->GetFormID();
				byActor[actor] = a_id;
				CountWatched();
				a_remote.shown = {};
				a_remote.actions.Clear();
				a_remote.aim = 0.0f;
			}
			logs::info("Players: player {}'s stand-in {:08X} placed (base {:08X}) at ({:.0f},{:.0f},{:.0f}) in {:08X}", a_id, a_remote.standInId, base->GetFormID(),
				a_at.x, a_at.y, a_at.z, a_at.cell ? a_at.cell : a_at.worldspace);
		}

		// ---- what a stand-in wears and holds ----
		// Another player's game named it: kept only where a player could have it (StandInRules::KeptInHand, KeptInQuiver,
		// KeptAsVoice; worn: armor a player can wear, no creature skins, no non-playable pieces). Never a runtime form
		// (FF): that ID is another game's, and here names something else or nothing (FormTranslate drops them; this holds
		// for any that get through). What is not is left out (worn) or read as nothing held, and counted.
		bool WearableArmor(const RE::TESForm* a_form)
		{
			const auto armor = a_form ? a_form->As<RE::TESObjectARMO>() : nullptr;
			return armor && armor->GetPlayable();
		}

		// What a form is, as StandInRules' rules for hands, the quiver and the voice take it.
		StandInRules::Holdable HoldableOf(const RE::TESForm* a_form)
		{
			using K = StandInRules::Holdable::Kind;
			StandInRules::Holdable out;
			if (!a_form) {
				return out;
			}
			out.kind = K::kOther;
			if (a_form->GetFormType() == RE::FormType::Scroll) {
				out.kind = K::kScroll;  // a scroll is a SpellItem too: said first
			} else if (const auto weapon = a_form->As<RE::TESObjectWEAP>()) {
				out.kind = K::kWeapon;
				out.playable = weapon->GetPlayable();
				out.bound = weapon->IsBound();
			} else if (const auto spell = a_form->GetFormType() == RE::FormType::Spell ? a_form->As<RE::SpellItem>() : nullptr) {
				const auto type = spell->GetSpellType();
				using ST = RE::MagicSystem::SpellType;
				if (type == ST::kSpell) {
					out.kind = K::kSpell;
				} else if (type == ST::kPower || type == ST::kLesserPower || type == ST::kVoicePower) {
					out.kind = K::kPower;
				}
			} else if (a_form->Is(RE::FormType::Shout)) {
				out.kind = K::kShout;
			} else if (const auto light = a_form->As<RE::TESObjectLIGH>()) {
				out.kind = light->CanBeCarried() ? K::kTorch : K::kOther;
			} else if (const auto ammo = a_form->As<RE::TESAmmo>()) {
				out.kind = K::kAmmo;
				out.playable = ammo->GetPlayable();
			} else if (const auto armor = a_form->As<RE::TESObjectARMO>()) {
				out.kind = K::kShield;
				out.playable = armor->GetPlayable();
			}
			return out;
		}

		Protocol::PlayerLooks Allowed(const Protocol::PlayerLooks& a_looks, std::size_t& a_refused)
		{
			const auto lookup = [](std::uint32_t a_id) { return a_id != 0 && !FormIds::IsRuntime(a_id) ? RE::TESForm::LookupByID(a_id) : nullptr; };
			auto       out = a_looks;
			const auto before = out.worn.size();
			std::erase_if(out.worn, [&](std::uint32_t a_id) { return !WearableArmor(lookup(a_id)); });
			a_refused += before - out.worn.size();
			const auto keep = [&](std::uint32_t& a_id, bool a_ok) {
				if (a_id != 0 && !FormIds::IsRuntime(a_id) && !a_ok) {
					a_id = 0;
					++a_refused;
				}
			};
			const auto right = HoldableOf(lookup(out.right));
			const auto left = HoldableOf(lookup(out.left));
			keep(out.right, StandInRules::KeptInHand(right));
			keep(out.left, StandInRules::KeptInHand(left));
			// A conjured weapon kept in a hand lets its own (non-playable) ammunition into the quiver.
			const bool boundInHand = (out.right != 0 && right.bound) || (out.left != 0 && left.bound);
			keep(out.ammo, StandInRules::KeptInQuiver(HoldableOf(lookup(out.ammo)), boundInHand));
			keep(out.voice, StandInRules::KeptAsVoice(HoldableOf(lookup(out.voice))));
			return out;
		}

		// A stand-in whose friend shoots all evening is never out of arrows (StandInRules::AmmoToAdd).
		void TopUpAmmo(Remote& a_remote, RE::Actor* a_actor)
		{
			const auto ammo = a_remote.gear.ammo != 0 && !FormIds::IsRuntime(a_remote.gear.ammo) ? RE::TESForm::LookupByID<RE::TESAmmo>(a_remote.gear.ammo) : nullptr;
			if (!ammo) {
				return;
			}
			if (const auto add = StandInRules::AmmoToAdd(Gear::CountOf(a_actor, ammo->GetFormID())); add > 0) {
				a_actor->AddObjectToContainer(ammo, nullptr, add, nullptr);
				a_remote.ammoGiven += static_cast<std::uint64_t>(add);
			}
		}

		// Dressed in what its friend wears and holds (Gear::Dress, given what it lacks: it is this game's own and never
		// saved), its friend's voice ready, its quiver full.
		void Dress(std::uint32_t a_id, Remote& a_remote, RE::Actor* a_actor)
		{
			if (!a_actor->Is3DLoaded()) {
				return;
			}
			std::size_t refused = 0;
			const auto  looks = Allowed(*a_remote.looks, refused);
			a_remote.refusedForms = refused;
			a_remote.gear = StandInRules::GearFor(looks);
			const auto dressed = Gear::Dress(a_remote.gear, true, a_actor, Gear::Spells::kHands);
			a_remote.gearMissing = dressed.missing;
			TopUpAmmo(a_remote, a_actor);
			// Its friend's voice, or none when they have none ready; one their game could not name is left as it is.
			a_remote.voiceWanted = looks.voice;
			a_remote.voiceJudged = (looks.unnamed & Protocol::kVoiceUnnamed) == 0;
			if (a_remote.voiceJudged && Gear::SetVoice(a_actor, looks.voice, true)) {
				a_remote.voiceShown = looks.voice;
			}
			a_remote.dressed = true;
			a_remote.checked = false;
			a_remote.nextCheck = Clock::now() + 250ms;  // read back once the equips are done
			// Said when what it wears changed (a hand's change, many a fight, is counted only: dev `actions`).
			if (a_remote.wornLogged != a_remote.looks->worn || refused + dressed.skipped > 0) {
				a_remote.wornLogged = a_remote.looks->worn;
				logs::info("Players: player {}'s stand-in {:08X} dressed: {} put on, {} taken off, {} worn items named; {} unknown here, {} no player could have there", a_id,
					a_remote.standInId, dressed.equipped, dressed.unequipped, a_remote.looks->worn.size(), dressed.skipped, refused);
			}
		}

		// What it holds, read back: dressed again (no sooner than REDRESS_COOL) when it is not what its friend's looks say
		// (the game put the preset's outfit back, a hand it could not fill, an equip that did not take), its damage kept
		// nothing, its quiver full.
		void Check(std::uint32_t a_id, Remote& a_remote, RE::Actor* a_actor, Clock::time_point a_now)
		{
			a_remote.nextCheck = a_now + CHECK_EVERY;
			a_remote.held = Gear::Check(a_remote.gear, a_actor, Gear::Spells::kHands);
			a_remote.wornShown = a_remote.held.matched;
			a_remote.checked = true;
			const auto voice = Gear::VoiceOf(a_actor);
			a_remote.voiceShown = voice ? voice->GetFormID() : 0;
			Neutralise(a_actor);
			TopUpAmmo(a_remote, a_actor);
			// A voice that did not take, or that the game cleared, is drift as a hand is.
			const bool voiceDrifted = StandInRules::VoiceDrifted(a_remote.voiceWanted, !a_remote.voiceJudged, a_remote.voiceShown);
			if ((!a_remote.held.Complete(a_remote.gearMissing) || voiceDrifted) && a_now >= a_remote.redressAfter) {
				a_remote.redressAfter = a_now + REDRESS_COOL;
				a_remote.dressed = false;
				if (a_remote.redressed++ == 0) {
					logs::info("Players: player {}'s stand-in {:08X} does not hold what its friend does ({} short, {} extra, voice {:08X} for {:08X}): dressed again", a_id,
						a_remote.standInId, a_remote.held.Short(), a_remote.held.extra, a_remote.voiceShown, a_remote.voiceWanted);
				}
			}
		}

		// Its weapon, spells or fists drawn or put away as its friend's are, the game's own way (StandInRules::DrawFix):
		// told the moment they part, once it holds what its friend holds, and again if the state went back.
		void FixDraw(std::uint32_t a_id, Remote& a_remote, RE::Actor* a_actor, const Protocol::PlayerState& a_state, Clock::time_point a_now)
		{
			const auto weapon = WeaponOf(a_actor);
			if (!weapon) {
				return;
			}
			const bool wanted = (a_state.flags & Protocol::kWeaponDrawn) != 0;
			const bool ready = a_remote.dressed && a_remote.checked && a_actor->Is3DLoaded();
			using A = StandInRules::DrawFix::Ask;
			const auto ask = a_remote.drawFix.Due(wanted, *weapon, ready, a_now);
			if (ask == A::kNone) {
				return;
			}
			a_actor->DrawWeaponMagicHands(wanted);
			const auto now = WeaponOf(a_actor);
			if (ask == A::kFirst ? a_remote.drawFix.asks <= 2 : a_remote.drawFix.fixes <= 3) {
				logs::info("Players: player {}'s stand-in {:08X} told to {} ({}; its weapon state {} -> {}{})", a_id, a_remote.standInId,
					wanted ? "draw" : "put away", ask == A::kFirst ? "its friend did" : "the last time did not hold", std::to_underlying(*weapon),
					now ? static_cast<int>(std::to_underlying(*now)) : -1,
					ask == A::kAgain ? std::format("; {} times so far", a_remote.drawFix.fixes) : std::string{});
			}
		}

		// The placed actors of a preset base in the loaded game: stand-ins, or ones a save brought back.
		std::vector<RE::Actor*> PresetActors()
		{
			std::vector<RE::Actor*> out;
			const auto [map, mapLock] = RE::TESForm::GetAllForms();
			if (!map) {
				return out;
			}
			RE::BSReadLockGuard guard{ mapLock };
			for (const auto& [id, form] : *map) {
				if (!form || !form->IsActor() || id < 0xFF000000 || form->IsDeleted()) {
					continue;
				}
				const auto actor = static_cast<RE::Actor*>(form);
				if (actor != RE::PlayerCharacter::GetSingleton() && IsStandInBase(actor->GetObjectReference())) {
					out.push_back(actor);
				}
			}
			return out;
		}

		bool IsCurrentStandIn(const RE::Actor* a_actor)
		{
			for (const auto& [id, remote] : players) {
				if (remote.standInId != 0 && remote.standInId == a_actor->GetFormID()) {
					return true;
				}
			}
			return false;
		}
	}

	bool Install()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_Character[0] };
		characterVtable = vtable.address();
		if (!HookSlot<UpdateFn>(characterVtable, UPDATE_INDEX, &UpdateHook, originalUpdate, "Character update")) {
			logs::error("Players: without the Character update hook no stand-in is placed");
			return false;
		}
		hooked = true;
		// Without these, stand-ins are placed and moved as before; their legs stand still.
		REL::Relocation<std::uintptr_t> speedChannel{ RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorSpeedChannel_[0] };
		REL::Relocation<std::uintptr_t> directionChannel{ RE::VTABLE_BSTAnimationGraphDataChannel_Actor_float_ActorDirectionChannel_[0] };
		const bool speed = HookSlot<PollFn>(speedChannel.address(), CHANNEL_POLL_INDEX, &SpeedPoll, originalSpeedPoll, "graph's Speed channel");
		const bool direction = speed && HookSlot<PollFn>(directionChannel.address(), CHANNEL_POLL_INDEX, &DirectionPoll, originalDirectionPoll, "graph's Direction channel");
		if (!direction) {
			logs::warn("Players: the graph's {} channel is not ours: stand-ins' legs won't follow their friends'", speed ? "Direction" : "Speed");
		}
		// What a stand-in does to others here: nothing (its spells' effects on Character are hooked once one is placed).
		HookPlayerMagic();
		if (!HookSlot<HealthDamageFn>(characterVtable, HEALTH_DAMAGE_INDEX, &CharacterHealthDamage, originalCharacterHealthDamage, "Character health damage")) {
			logs::warn("Players: health a stand-in's blow takes from an NPC here is not given back");
		}
		if (const auto player = RE::PlayerCharacter::GetSingleton()) {
			const auto playerVtable = *reinterpret_cast<const std::uintptr_t*>(player);
			if (playerVtable == characterVtable ||
				!HookSlot<HealthDamageFn>(playerVtable, HEALTH_DAMAGE_INDEX, &PlayerHealthDamage, originalPlayerHealthDamage, "player's health damage")) {
				logs::warn("Players: health a stand-in's blow takes from our player is not given back");
			}
		}
		if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
			events->AddEventSink<RE::TESHitEvent>(&deedSink);
			events->AddEventSink<RE::TESSpellCastEvent>(&deedSink);
		}
		return true;
	}

	void Joined(std::uint32_t a_playerId, std::string_view a_name)
	{
		std::scoped_lock l{ lock };
		auto& remote = players[a_playerId];
		remote.name = std::string{ a_name };
		// A player who joins again starts their state count over: what the buffer held would turn every new one away.
		remote.buffer = {};
		logs::info("Players: player {} joined ({} here now)", a_playerId, players.size());
	}

	void Left(std::uint32_t a_playerId)
	{
		const auto it = players.find(a_playerId);
		if (it == players.end()) {
			return;
		}
		TakeAway(a_playerId, it->second, "left");
		std::scoped_lock l{ lock };
		players.erase(a_playerId);
		logs::info("Players: player {} left ({} here now)", a_playerId, players.size());
	}

	void Moved(std::uint32_t a_playerId, const Protocol::PlayerState& a_state)
	{
		std::scoped_lock l{ lock };
		if (const auto it = players.find(a_playerId); it != players.end()) {
			it->second.buffer.Push(a_state, Clock::now());
		}
	}

	void Looked(std::uint32_t a_playerId, const Protocol::PlayerLooks& a_looks)
	{
		const auto it = players.find(a_playerId);
		if (it == players.end()) {
			return;
		}
		auto&      remote = it->second;
		const auto change = remote.looks ? StandInRules::Compare(*remote.looks, a_looks) : StandInRules::Change::kRemake;
		const bool body = !remote.looks || change == StandInRules::Change::kRemake || remote.looks->worn != a_looks.worn;
		remote.looks = a_looks;
		if (body) {
			// Said when the body or the armor changed; a hand's change (many a fight) only re-dresses.
			logs::info("Players: player {}'s looks: race {:08X}, {}, {} worn items", a_playerId, a_looks.race, a_looks.female ? "female" : "male", a_looks.worn.size());
		}
		if (remote.standInId == 0 || change == StandInRules::Change::kSame) {
			return;
		}
		if (change == StandInRules::Change::kRemake) {
			TakeAway(a_playerId, remote, "their body changed");  // another race, sex or weight: another preset, another body
		} else {
			remote.dressed = false;  // the same stand-in, dressed again in the next frame
		}
	}

	void Acted(std::uint32_t a_playerId, const std::vector<Protocol::PlayerAction>& a_actions)
	{
		const auto     now = Clock::now();
		std::scoped_lock l{ lock };
		const auto     it = players.find(a_playerId);
		if (it == players.end()) {
			return;
		}
		auto& remote = it->second;
		remote.actionsIn += a_actions.size();
		if (remote.standInId == 0) {
			remote.actionsUnplaced += a_actions.size();  // no stand-in here to play them (elsewhere, or far)
			return;
		}
		for (const auto& action : a_actions) {
			remote.actions.Push(action.event, action.sentMs ? remote.buffer.sender.At(action.sentMs) : std::nullopt, now);
		}
	}

	void Frame()
	{
		if (players.empty()) {
			return;
		}
		const auto now = Clock::now();
		const auto here = LocalPlayer::InWorld() ? LocalPlayer::Read() : std::nullopt;
		for (auto& [id, remote] : players) {
			std::optional<StateBuffer::Sampled> sample;
			{
				std::scoped_lock l{ lock };
				remote.buffer.Frame(now);
				sample = remote.buffer.Sample(now);
			}
			const auto actor = remote.standIn.get();
			if (remote.standInId != 0 && (!actor || actor->GetFormID() != remote.standInId || actor->IsDeleted() || actor->IsDisabled())) {
				// The game let it go (its cell unloaded, a load): placed again when it may be.
				logs::info("Players: player {}'s stand-in {:08X} is gone from the world", id, remote.standInId);
				TakeAway(id, remote, "gone");
				continue;
			}
			const bool wanted = here && sample && remote.looks && SameSpace(*here, sample->state) && Distance(*here, sample->state) < (actor ? HIDE_BEYOND : SHOW_WITHIN);
			if (!wanted) {
				if (actor) {
					TakeAway(id, remote, !here ? "this game left the world" : !sample ? "no recent state" : "elsewhere");
				}
				continue;
			}
			if (!actor) {
				// Without the hook a stand-in would run its own AI: none is placed.
				if (hooked && now >= remote.placeAfter) {
					Place(id, remote, sample->state);
				}
				continue;
			}
			if (!remote.dressed) {
				Dress(id, remote, actor.get());
			} else if (now >= remote.nextCheck && actor->Is3DLoaded()) {
				Check(id, remote, actor.get(), now);
			}
			KeepFull(actor.get());
			FixDraw(id, remote, actor.get(), sample->state, now);
			if (eventsFromTick && actor->Is3DLoaded()) {
				Gait::Step  step;
				Gait::Shown was;
				std::vector<std::string> actions;
				{
					std::scoped_lock l{ lock };
					was = remote.shown;
					step = Gait::Next(was, sample->state, sample->held);
					remote.shown = step.shown;
					actions = remote.actions.Due(now - StateBuffer::Buffer::Delay());
				}
				TellGraph(actor.get(), id, step, was);
				PlayActions(actor.get(), id, actions);  // its friend's actions from the tick too, as its legs' events
			}
		}
	}

	void Clear()
	{
		for (auto& [id, remote] : players) {
			TakeAway(id, remote, "the session ended");
		}
		// The graveyard goes on (TendGraveyard, each tick, session or not).
		drive.Clear();
		std::scoped_lock l{ lock };
		players.clear();
		byActor.clear();
		CountWatched();
	}

	void TendGraveyard()
	{
		Tend(false);
	}

	void ForgetStandIns()
	{
		drive.Clear();
		std::scoped_lock l{ lock };
		for (auto& [id, remote] : players) {
			remote.standIn = {};
			remote.standInId = 0;
			remote.dressed = false;
			remote.shown = {};
			remote.buffer.Clear();  // the states before the load are of another moment
		}
		byActor.clear();
		// Those on their way out go with the world too: the load lets go of every reference this game placed.
		retiring.clear();
		graveyard.clear();
		CountWatched();
	}

	void BeforeSave()
	{
		takenForSave.clear();
		std::size_t taken = 0;
		for (auto& [id, remote] : players) {
			if (remote.standInId != 0) {
				TakeAway(id, remote, "a save is written");
				++taken;
			}
		}
		// And any other placed preset actor (one an earlier save brought back that a sweep missed): none is saved.
		for (const auto actor : SweepWanted() ? PresetActors() : std::vector<RE::Actor*>{}) {
			if (!actor->IsDisabled() && !InGraveyard(actor->GetFormID())) {
				SendOut(actor, {});
				++taken;
			}
		}
		// Each is disabled now, so none stands in the world the save is made of; none is deleted in the same frame
		// (Retire.h: that crashed both games). Those disabled a while ago are deleted now; the others a second after the
		// save, and the co-save names them all, so a load of this save removes any it kept.
		Tend(true);
		if (taken > 0 || !takenForSave.empty() || !players.empty()) {
			logs::info("Save: {} stand-in(s) taken out of the world before the save, {} named in the co-save; placed again after", taken, takenForSave.size());
		}
	}

	void WriteCoSave(SKSE::SerializationInterface* a_intf)
	{
		if (takenForSave.empty()) {
			return;
		}
		if (!a_intf->OpenRecord(COSAVE_STANDINS, COSAVE_STANDINS_VERSION)) {
			logs::warn("Co-save: could not open the stand-ins record");
			return;
		}
		// Version 2: the count, then each actor's form id and its base's.
		const auto count = static_cast<std::uint32_t>(takenForSave.size());
		a_intf->WriteRecordData(&count, sizeof(count));
		for (const auto& taken : takenForSave) {
			a_intf->WriteRecordData(&taken.id, sizeof(taken.id));
			a_intf->WriteRecordData(&taken.base, sizeof(taken.base));
		}
		logs::info("Co-save: {} placed actor id(s) written (stand-ins, copies of spawns), to be removed should a load bring any back", count);
		takenForSave.clear();
	}

	void ReadCoSave(SKSE::SerializationInterface* a_intf, std::uint32_t a_version, std::uint32_t a_length)
	{
		namedByLoad.clear();
		coSaveRecord = true;
		// Version 1 (before NPC copies): the ids alone, all stand-ins. Version 2: id and base, each.
		const std::uint32_t each = a_version == 1 ? sizeof(RE::FormID) : 2 * sizeof(RE::FormID);
		std::uint32_t       count = 0;
		if ((a_version != 1 && a_version != COSAVE_STANDINS_VERSION) || a_length < sizeof(count) || a_intf->ReadRecordData(&count, sizeof(count)) != sizeof(count) ||
			count > 1024 || a_length != sizeof(count) + count * each) {
			logs::warn("Co-save: a stand-ins record that can't be read (version {}, {} bytes); the load's sweep still runs", a_version, a_length);
			return;
		}
		for (std::uint32_t i = 0; i < count; ++i) {
			Taken taken;
			if (a_intf->ReadRecordData(&taken.id, sizeof(taken.id)) != sizeof(taken.id) ||
				(a_version != 1 && a_intf->ReadRecordData(&taken.base, sizeof(taken.base)) != sizeof(taken.base))) {
				logs::warn("Co-save: the stand-ins record ended after {} of {} actors; the load's sweep still runs", i, count);
				break;
			}
			namedByLoad.push_back(taken);
		}
		logs::info("Co-save: this save was written with {} placed actor(s) taken out", count);
	}

	void AfterLoad()
	{
		std::size_t named = 0;
		for (const auto& taken : namedByLoad) {
			// Only an actor of the base it was placed from (a record of version 1: any preset base): the save's form ids
			// are its own, and one this game placed holds the id it was saved with.
			const auto actor = RE::TESForm::LookupByID<RE::Actor>(taken.id);
			const auto base = actor ? actor->GetObjectReference() : nullptr;
			const bool ours = base && (taken.base != 0 ? base->GetFormID() == taken.base : IsStandInBase(base));
			if (ours && !actor->IsDeleted()) {
				SendOut(actor, {});
				++named;
			}
		}
		std::size_t swept = 0;
		for (const auto actor : SweepWanted() ? PresetActors() : std::vector<RE::Actor*>{}) {
			if (!IsCurrentStandIn(actor) && !actor->IsDisabled() && !InGraveyard(actor->GetFormID())) {
				SendOut(actor, {});
				++swept;
			}
		}
		if (named + swept > 0) {
			logs::warn("Load: removed {} stand-in(s) the save brought back ({} named by the co-save, {} found by the sweep)", named + swept, named, swept);
		} else {
			logs::info("Load: no stand-in in this save (the co-save named {}{})", namedByLoad.size(), SweepWanted() ? "" : "; no sweep: no session or stand-in in this game");
		}
		namedByLoad.clear();
		coSaveRecord = false;
	}

	std::size_t Count()
	{
		return players.size();
	}

	bool IsOurs(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		std::scoped_lock l{ lock };
		const auto       actor = const_cast<RE::Actor*>(a_actor);
		return byActor.contains(actor) || retiring.contains(actor);
	}

	RE::NiPointer<RE::Actor> StandInOf(std::uint32_t a_playerId)
	{
		RE::ActorHandle handle;
		RE::FormID      id = 0;
		{
			std::scoped_lock l{ lock };
			const auto       it = players.find(a_playerId);
			if (it == players.end()) {
				return {};
			}
			handle = it->second.standIn;
			id = it->second.standInId;
		}
		auto actor = handle.get();
		if (!actor || id == 0 || actor->GetFormID() != id) {
			return {};
		}
		return actor;
	}

	std::uint32_t PlayerOf(const RE::Actor* a_actor)
	{
		if (!a_actor) {
			return 0;
		}
		std::scoped_lock l{ lock };
		const auto       it = byActor.find(const_cast<RE::Actor*>(a_actor));
		return it == byActor.end() ? 0 : it->second;
	}

	std::vector<Protocol::PlayerState> FriendStates()
	{
		std::vector<Protocol::PlayerState> out;
		std::scoped_lock                   l{ lock };
		for (const auto& [id, remote] : players) {
			if (!remote.buffer.snapshots.empty()) {
				out.push_back(remote.buffer.snapshots.back().state);
			}
		}
		return out;
	}

	void SetCopies(std::size_t a_count)
	{
		copies.store(a_count, std::memory_order_relaxed);
	}

	void DriveLegs(RE::Actor* a_actor, GaitDrive::Values a_values)
	{
		drive.Set(a_actor, a_actor->GetFormID(), a_values, Clock::now());
	}

	void StopLegs(RE::Actor* a_actor)
	{
		drive.Erase(a_actor);
	}

	// Actor::Update ends so (Skyrim SE 1.7.104): while the weapon state is kWantToDraw or kWantToSheathe it calls
	// DrawWeaponMagicHands again with that wish. UpdateNoAI, all a stand-in or a copy gets, does not.
	bool FinishDraw(RE::Actor* a_actor)
	{
		const auto weapon = WeaponOf(a_actor);
		const auto again = weapon ? StandInRules::AskAgain(*weapon) : std::nullopt;
		if (!again) {
			return false;
		}
		a_actor->DrawWeaponMagicHands(*again);
		return true;
	}

	void TakeOut(RE::Actor* a_actor, std::chrono::steady_clock::time_point a_placedAt)
	{
		drive.Erase(a_actor);
		SendOut(a_actor, a_placedAt);
	}

	std::string Describe()
	{
		std::size_t disabled = 0;
		std::size_t deleted = 0;
		for (const auto& grave : graveyard) {
			disabled += grave.disabledAt && !grave.deleted ? 1 : 0;
			deleted += grave.deleted ? 1 : 0;
		}
		std::string out = std::format("players={} standins={} leaving={} (disabled={} deleted={}) driven speed={} direction={} channels={} from={}", players.size(),
			standIns.load(), graveyard.size(), disabled, deleted, speedsDriven.load(), directionsDriven.load(), drive.Size(), eventsFromTick ? "tick" : "hook");
		const auto  here = LocalPlayer::Read();
		// What each stand-in really has on: the armor its inventory holds as worn, not what its friend's looks named (the
		// rig's `forms` step checks an item is on the actor). Read before the lock: the game's inventory has its own.
		std::unordered_map<std::uint32_t, std::string> equipped;
		for (const auto& [id, remote] : players) {
			const auto actor = remote.standIn.get();
			if (!actor) {
				continue;
			}
			std::vector<RE::FormID> ids;
			for (const auto& [item, data] : actor->GetInventory([](RE::TESBoundObject& a_object) { return a_object.IsArmor(); })) {
				if (data.first > 0 && data.second && data.second->IsWorn()) {
					ids.push_back(item->GetFormID());
				}
			}
			std::ranges::sort(ids);
			auto& list = equipped[id];
			for (const auto item : ids) {
				list += std::format("{}{:08X}", list.empty() ? "" : ",", item);
			}
		}
		std::scoped_lock l{ lock };
		for (const auto& [id, remote] : players) {
			const auto actor = remote.standIn.get();
			std::string at = "none";
			if (actor) {
				const auto p = actor->GetPosition();
				at = std::format("({:.0f},{:.0f},{:.0f}) heading={:.3f}", p.x, p.y, p.z, actor->GetAngleZ());
			}
			// What its graph reads now: Speed and Direction (the channels' values, its friend's while it is driven) and the
			// SpeedSampled its graph made of them (what its legs cycle at).
			const auto graphFloat = [&](std::string_view a_name, std::string_view a_format) -> std::string {
				float v = 0.0f;
				return actor && actor->GetGraphVariableFloat(Name(a_name), v) ? std::vformat(a_format, std::make_format_args(v)) : std::string{ "none" };
			};
			const auto& newest = remote.buffer.snapshots.empty() ? std::optional<Protocol::PlayerState>{} : std::optional{ remote.buffer.snapshots.back().state };
			out += std::format(" | player {} standin={:08X} placed={} dressed={} at={} target={} graphSpeed={} graphDir={} graphSampled={} shown={}{}{}{} drawn={} refused={}{} taken={} states={} newest={} dist={}",
				id, remote.standInId, remote.placed, remote.dressed ? 1 : 0, at,
				remote.hasDrawn ? std::format("({:.0f},{:.0f},{:.0f}) speed={:.0f} dir={:.3f} flags={:#04x}", remote.lastDrawn.x, remote.lastDrawn.y, remote.lastDrawn.z, remote.lastDrawn.speed,
					Protocol::UnpackDirection(remote.lastDrawn.direction), remote.lastDrawn.flags) : "none",
				graphFloat("Speed", "{:.0f}"), graphFloat("Direction", "{:.3f}"), graphFloat("SpeedSampled", "{:.0f}"), remote.shown.moving ? "m" : "-", remote.shown.sneaking ? "s" : "-", remote.shown.sprinting ? "r" : "-",
				remote.shown.inAir ? "j" : "-", remote.drawn, remote.refusedEvents, remote.lastRefused.empty() ? "" : " last=" + remote.lastRefused, remote.takenEvents,
				remote.buffer.snapshots.size(),
				newest ? std::format("({:.0f},{:.0f},{:.0f}) in {:08X}", newest->x, newest->y, newest->z, newest->cell ? newest->cell : newest->worldspace) : "none",
				here && newest ? std::format("{:.0f}", SameSpace(*here, *newest) ? Distance(*here, *newest) : -1.0f) : "?");
			// Whose name the stand-in shows (never the name itself: friend = its player's, base = its preset's own),
			// and whether this game's crosshair is on it (the rig's activation check).
			std::string_view named = "none";
			bool             aimed = false;
			if (actor) {
				const auto shown = actor->GetDisplayFullName();
				const auto baseName = remote.base ? remote.base->GetName() : nullptr;
				const std::string_view text = shown ? shown : "";
				named = text.empty() ? "empty" : (!remote.name.empty() && text == remote.name) ? "friend" : (baseName && text == baseName) ? "base" : "other";
				if (const auto pick = RE::CrosshairPickData::GetSingleton()) {
					aimed = pick->GetActiveTarget().get().get() == actor.get();
				}
			}
			// What it wears: how many of the items its friend's game named it was dressed in, their form IDs in this game as
			// the looks named them (wornIds), and the armor the stand-in really has on (equippedIds: what the rig checks).
			std::string wornIds;
			if (remote.looks) {
				for (const auto item : remote.looks->worn) {
					wornIds += std::format("{}{:08X}", wornIds.empty() ? "" : ",", item);
				}
			}
			const auto on = equipped.find(id);
			out += std::format(" named={} aimed={} race={:08X} worn={}/{} wornIds={} equippedIds={}", named, aimed ? 1 : 0, remote.looks ? remote.looks->race : 0u, remote.wornShown,
				remote.looks ? remote.looks->worn.size() : 0u, wornIds.empty() ? "none" : wornIds, on == equipped.end() || on->second.empty() ? "none" : on->second);
		}
		return out;
	}

	std::string Actions()
	{
		std::string out = std::format("magicHooks character={} player={}", characterMagicHooked ? 1 : 0, playerMagicHooked ? 1 : 0);
		// What each stand-in's game objects say, read before the lock (the game's own locks).
		struct Read
		{
			int         drawnNow = -1;  // its weapon drawn (1), put away (0), no stand-in (-1)
			float       angleX = 0.0f;
			std::uint32_t baseShouts = 0;  // the shouts its base keeps now (Gear::BaseShouts)
			int         weaponState = -1;  // the game's weapon state (StandInRules::Weapon), -1: none
			std::string process = "none";   // its process: high, low (no middle-high data), none
			int         responses = -1;     // its process has the animation event handlers (1) or not (0)
		};
		std::unordered_map<std::uint32_t, Read> reads;
		for (const auto& [id, remote] : players) {
			if (const auto actor = remote.standIn.get(); actor && actor->GetFormID() == remote.standInId) {
				const auto weapon = WeaponOf(actor.get());
				Read read{ weapon ? (*weapon == StandInRules::Weapon::kDrawn ? 1 : 0) : -1, actor->GetAngleX(), Gear::BaseShouts(actor.get()) };
				read.weaponState = weapon ? static_cast<int>(std::to_underlying(*weapon)) : -1;
				if (const auto process = actor->GetActorRuntimeData().currentProcess) {
					read.process = process->InHighProcess() ? "high" : process->middleHigh ? "middle" : "low";
					read.responses = process->middleHigh ? (process->middleHigh->animResponse ? 1 : 0) : 0;
				}
				reads[id] = read;
			}
		}
		std::scoped_lock l{ lock };
		for (const auto& [id, remote] : players) {
			const auto read = reads.contains(id) ? reads[id] : Read{};
			const auto wanted = remote.hasDrawn ? ((remote.lastDrawn.flags & Protocol::kWeaponDrawn) != 0 ? 1 : 0) : -1;
			const auto handOk = [&](bool a_ok) { return remote.checked ? (a_ok ? "1" : "0") : "?"; };
			out += std::format(" | player {} standin={:08X} in={} unplaced={} waiting={} lost={} played={} byKind={} refused={} refusedKinds={}{} drawn={} wanted={} drawFixes={} weaponState={} drawAsks={} drawsFinished={} process={} responses={}"
							   " aim={:.2f} angleX={:.2f} right={:08X}:{} left={:08X}:{} ammo={:08X}:{} voice={:08X}/{:08X} gearOk={} missing={} refusedForms={} redressed={} baseShouts={}/{}"
							   " ammoGiven={} magicRefused={} magicKept={} blows={} casts={} damaged={} dealt={:.1f} undone={:.1f}",
				id, remote.standInId, remote.actionsIn, remote.actionsUnplaced, remote.actions.Size(), remote.actions.Dropped(), remote.played.Total(),
				remote.played.Line(), remote.refused.Total(), remote.refused.Line(), remote.lastActionRefused.empty() ? "" : " lastRefused=" + remote.lastActionRefused,
				read.drawnNow, wanted, remote.drawFix.fixes, read.weaponState, remote.drawFix.asks, remote.drawsFinished, read.process, read.responses, remote.aim, read.angleX, remote.gear.right, handOk(remote.held.right), remote.gear.left,
				handOk(remote.held.left), remote.gear.ammo, handOk(remote.held.ammo), remote.looks ? remote.looks->voice : 0u, remote.voiceShown,
				remote.checked ? (remote.held.Complete(remote.gearMissing) && !StandInRules::VoiceDrifted(remote.voiceWanted, !remote.voiceJudged, remote.voiceShown) ? "1" : "0") : "?",
				remote.gearMissing, remote.refusedForms, remote.redressed, read.baseShouts, remote.baseShouts,
				remote.ammoGiven, remote.magicRefused, remote.magicKept, remote.blows, remote.casts, remote.damaged, remote.dealt, remote.undone);
		}
		return out;
	}

	std::string Strays()
	{
		const auto  actors = PresetActors();
		std::size_t live = 0;
		std::string list;
		for (const auto actor : actors) {
			if (actor->IsDisabled()) {
				continue;
			}
			++live;
			if (list.size() < 400) {
				list += std::format(" {:08X}{}", actor->GetFormID(), IsCurrentStandIn(actor) ? "(ours)" : "");
			}
		}
		std::size_t ours = 0;
		for (const auto& [id, remote] : players) {
			ours += remote.standInId != 0 ? 1 : 0;
		}
		return std::format("strays={} standins={} presetActors={}{}", live - (std::min)(live, ours), ours, live, list);
	}

	std::string Graph(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (words.size() == 2 && words[0] == "from" && (words[1] == "hook" || words[1] == "tick")) {
			eventsFromTick = words[1] == "tick";
			logs::info("Players: the stand-ins' graph events are sent from the {} from now on", eventsFromTick ? "tick" : "update hook");
			return std::format("graph events from the {}", words[1]);
		}
		const auto id = words.empty() ? std::nullopt : DevLine::Number(words[0]);
		if (!id) {
			return "error: usage: graph <player id> [vars | weapon | active | event <name> | float <name> | bool <name> | int <name>] | graph from hook|tick";
		}
		const auto it = players.find(static_cast<std::uint32_t>(*id));
		const auto actor = it != players.end() ? it->second.standIn.get() : RE::NiPointer<RE::Actor>{};
		if (!actor) {
			return "error: that player has no stand-in";
		}
		const auto what = words.size() > 1 ? words[1] : std::string_view{ "vars" };
		if (what == "event" && words.size() == 3) {
			const bool taken = actor->NotifyAnimationGraph(Name(words[2]));
			logs::info("Players: graph event {} sent to {:08X}: {}", words[2], actor->GetFormID(), taken ? "taken" : "refused");
			return std::format("event {} {}", words[2], taken ? "taken" : "refused");
		}
		if ((what == "float" || what == "bool" || what == "int") && words.size() == 3) {
			if (what == "float") {
				float v = 0;
				return actor->GetGraphVariableFloat(Name(words[2]), v) ? std::format("{}={}", words[2], v) : std::format("{}: none", words[2]);
			}
			if (what == "bool") {
				bool v = false;
				return actor->GetGraphVariableBool(Name(words[2]), v) ? std::format("{}={}", words[2], v ? 1 : 0) : std::format("{}: none", words[2]);
			}
			std::int32_t v = 0;
			return actor->GetGraphVariableInt(Name(words[2]), v) ? std::format("{}={}", words[2], v) : std::format("{}: none", words[2]);
		}
		if (what == "weapon") {
			// What a draw, an attack, a block and a cast are known by: the game's weapon state and the graph's own.
			const auto weapon = WeaponOf(actor.get());
			std::string out = std::format("{:08X} weaponState={}", actor->GetFormID(), weapon ? static_cast<int>(std::to_underlying(*weapon)) : -1);
			for (const auto name : { "iRightHandType", "iLeftHandType", "iEquippedItemState", "iWantBlock", "iState", "iLeftHandEquipped" }) {
				std::int32_t v = 0;
				out += actor->GetGraphVariableInt(Name(name), v) ? std::format(" {}={}", name, v) : std::format(" {}=none", name);
			}
			for (const auto name : { "bEquipOk", "IsEquipping", "IsUnequipping", "IsAttackReady", "IsAttacking", "IsBlocking", "IsCastingRight", "IsCastingLeft",
					 "bWantCastLeft", "bWantCastRight", "bAllowRotation", "bAnimationDriven", "IsStaggering", "IsRecoiling", "bInJumpState" }) {
				bool v = false;
				out += actor->GetGraphVariableBool(Name(name), v) ? std::format(" {}={}", name, v ? 1 : 0) : std::format(" {}=none", name);
			}
			return out;
		}
		if (what == "active") {
			// The graph's active nodes by name (its states among them): where it stands when it turns events away.
			RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
			if (!actor->GetAnimationGraphManager(manager) || !manager) {
				return std::format("{:08X} active: no graph", actor->GetFormID());
			}
			RE::BSSpinLockGuard guard{ manager->GetRuntimeData().updateLock };  // never while the graph updates
			const auto          index = manager->GetRuntimeData().activeGraph;
			if (index >= manager->graphs.size() || !manager->graphs[index]) {
				return std::format("{:08X} active: no graph", actor->GetFormID());
			}
			const auto behavior = manager->graphs[index]->behaviorGraph;
			if (!behavior || !behavior->activeNodes) {
				return std::format("{:08X} active: no behavior", actor->GetFormID());
			}
			const auto& nodes = *behavior->activeNodes;
			std::string out = std::format("{:08X} active={}:", actor->GetFormID(), nodes.size());
			for (std::int32_t i = 0; i < nodes.size(); ++i) {
				const auto node = nodes[i].nodeTemplate;
				const char* name = node ? node->name.data() : nullptr;
				out += std::format(" {}", name && *name ? name : "?");
				if (out.size() > 3000) {
					out += " ...";
					break;
				}
			}
			return out;
		}
		// The variables a locomotion graph is known by, each as it reads or "none".
		std::string out = std::format("{:08X}", actor->GetFormID());
		for (const auto name : { "Speed", "Direction", "SpeedSampled", "TurnDelta", "SpeedWalk", "SpeedRun" }) {
			float v = 0;
			out += actor->GetGraphVariableFloat(Name(name), v) ? std::format(" {}={:.2f}", name, v) : std::format(" {}=none", name);
		}
		for (const auto name : { "IsSprinting", "IsSneaking", "bInJumpState", "IsRunning", "bAnimationDriven", "bMotionDriven", "IsEquipping" }) {
			bool v = false;
			out += actor->GetGraphVariableBool(Name(name), v) ? std::format(" {}={}", name, v ? 1 : 0) : std::format(" {}=none", name);
		}
		for (const auto name : { "iState", "iSyncIdleLocomotion", "iState_NPCSneaking", "iState_NPCDefault" }) {
			std::int32_t v = 0;
			out += actor->GetGraphVariableInt(Name(name), v) ? std::format(" {}={}", name, v) : std::format(" {}=none", name);
		}
		return out;
	}
}
