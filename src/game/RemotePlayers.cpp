#include "game/RemotePlayers.h"

#include "DevLine.h"
#include "FormIds.h"
#include "game/Face.h"
#include "game/FaceRules.h"
#include "game/Gait.h"
#include "game/GaitDrive.h"
#include "game/GameThread.h"
#include "game/Gear.h"
#include "game/LocalPlayer.h"
#include "game/NpcSync.h"
#include "game/Furniture.h"
#include "game/Pairs.h"
#include "game/PeaceRules.h"
#include "game/Reactions.h"
#include "game/Retire.h"
#include "game/Riding.h"
#include "game/SpellRules.h"
#include "game/StandInPeace.h"
#include "game/StandInRules.h"
#include "game/StateBuffer.h"
#include "game/VtableHook.h"
#include "game/Water.h"

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

		// The life states StandInRules names (a stand-in that fell here), the game's own numbers.
		using LS = RE::ACTOR_LIFE_STATE;
		static_assert(StandInRules::kLifeAlive == std::to_underlying(LS::kAlive) && StandInRules::kLifeDying == std::to_underlying(LS::kDying) &&
						  StandInRules::kLifeDead == std::to_underlying(LS::kDead) && StandInRules::kLifeUnconscious == std::to_underlying(LS::kUnconcious) &&
						  StandInRules::kLifeReanimate == std::to_underlying(LS::kReanimate) && StandInRules::kLifeRecycle == std::to_underlying(LS::kRecycle) &&
						  StandInRules::kLifeRestrained == std::to_underlying(LS::kRestrained) &&
						  StandInRules::kLifeEssentialDown == std::to_underlying(LS::kEssentialDown) && StandInRules::kLifeBleedout == std::to_underlying(LS::kBleedout),
			"StandInRules' life states are the game's");

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
		static_assert(SpellRules::kArchNone == std::to_underlying(ArchetypeID::kNone) && SpellRules::kArchCommandSummoned == std::to_underlying(ArchetypeID::kCommandSummoned) &&
					  SpellRules::kArchLock == std::to_underlying(ArchetypeID::kLock) && SpellRules::kArchOpen == std::to_underlying(ArchetypeID::kOpen) &&
					  SpellRules::kArchBoundWeapon == std::to_underlying(ArchetypeID::kBoundWeapon) &&
					  SpellRules::kArchSummonCreature == std::to_underlying(ArchetypeID::kSummonCreature) &&
					  SpellRules::kArchTelekinesis == std::to_underlying(ArchetypeID::kTelekinesis) && SpellRules::kArchReanimate == std::to_underlying(ArchetypeID::kReanimate) &&
					  SpellRules::kArchSlowTime == std::to_underlying(ArchetypeID::kSlowTime) &&
					  SpellRules::kArchSpawnScriptedRef == std::to_underlying(ArchetypeID::kSpawnScriptedRef) &&
					  SpellRules::kArchGrabActor == std::to_underlying(ArchetypeID::kGrabActor),
			"SpellRules' magic effect kinds are the game's");

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
			// In water (under lock): the swim events its graph took and turned away, the changes it gave up on
			// (Gait::SWIM_TRIES), and how often the water moved it off the height it is shown at (HoldDepth).
			std::uint64_t swimTaken = 0;
			std::uint64_t swimRefused = 0;
			std::uint64_t swimGivenUp = 0;
			std::uint64_t refloats = 0;
			float         drift = 0.0f;  // the engine's last update's move off its height while swimming (HoldDepth's a_drift)
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
			// What the world did to it here (StandInPeace.h): effects anyone put on it, refused, hits that still landed on
			// it (none expected: weapons and spells pass through it, PeaceRules.h), and the health anyone or anything took
			// from it (a blade, an arrow, a trap, a fall), given back at once in the health damage hook (wounds, how much).
			std::uint64_t magicOnRefused = 0;
			std::uint64_t hitsOn = 0;
			std::uint64_t woundsOn = 0;
			float         healthBack = 0.0f;
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
			StandInRules::DrawFix   drawFix;  // its weapon drawn or put away by hand when its graph disagrees
			// Its stand-ins that fell here (died, bled out, were knocked out: StandInRules::Falls), each taken away and a
			// new one placed; how the last one fell.
			StandInRules::Falls     falls;
			StandInRules::Fall      lastFall = StandInRules::Fall::kStanding;

			// Its face (main thread): what its friend's game sent, whether it goes on (FaceRules::Fits), the copy of the
			// preset it stands on with it (none: the preset itself), the face that copy shows (its hash; 0: the preset's),
			// how its head is watched, and whether its friend's face was given up here for the session.
			std::optional<Protocol::PlayerFace> face;
			std::uint64_t                       facesIn = 0;
			FaceRules::Fit                      fit = FaceRules::Fit::kNoFace;
			RE::TESNPC*                         copy = nullptr;
			std::uint32_t                       faceShown = 0;
			std::uint32_t                       faceTried = 0;  // the face it was placed for (WantedFace; shown or not)
			float                               weightShown = 0.0f;
			FaceRules::Watch                    watch;
			bool                                faceGivenUp = false;
			std::uint64_t                       faceRemakes = 0;
			Clock::time_point                   faceRemakeAfter{};
		};

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

		// The form ids of byActor and retiring, for the hooks that ask with no lock (IsStandInId; PeaceRules::IdSet).
		PeaceRules::IdSet<PeaceRules::MAX_WATCHED> watchedIds;
		bool                                       watchedOverflowTold = false;

		void CountWatched()  // under lock
		{
			standIns = byActor.size();
			watched = byActor.size() + retiring.size();
			std::vector<std::uint32_t> ids;
			ids.reserve(byActor.size() + retiring.size());
			for (const auto& [actor, playerId] : byActor) {
				if (const auto player = players.find(playerId); player != players.end() && player->second.standInId != 0) {
					ids.push_back(player->second.standInId);
				}
			}
			for (const auto& [actor, id] : retiring) {
				ids.push_back(id);
			}
			if (!watchedIds.Assign(ids) && !watchedOverflowTold) {
				watchedOverflowTold = true;
				logs::warn("Players: more than {} stand-ins at once: the world's AI may treat the rest as people of this game", PeaceRules::MAX_WATCHED);
			}
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
			// The copy of a preset it stood on with its friend's face (Face.h), until its 3D is gone and it is deleted: no
			// other face is written into that copy meanwhile (FaceRules::CopyBook, a_inUse).
			const RE::TESNPC* copy = nullptr;
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

		// Faces (Face.h): stand-ins whose friend's face was given up for no head or for ours gone, the stand-ins given
		// their preset back as their base on the way out (so no save ever holds one standing on a copy), and our own
		// head asked back once, a while after a stand-in seemed to take it.
		std::uint64_t                    facesGivenUp = 0;
		std::uint64_t                    oursGivenUp = 0;
		std::uint64_t                    presetsPutBack = 0;
		std::uint64_t                    presetsNamed = 0;   // stand-ins on a copy sent out, their preset named for the co-save
		std::uint64_t                    putBackLoading = 0;  // ... given their preset back before a save while their 3D was still loading
		std::optional<Clock::time_point> repairOursAt;
		bool                             oursRepaired = false;
		constexpr auto                   REPAIR_OURS_AFTER = 2s;
		// A friend's changed face makes a new stand-in no sooner than this after the last (a face editor's last steps).
		constexpr auto FACE_REMAKE_COOL = 2s;

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

		// The water's events, counted apart (dev `swim`).
		bool IsSwimEvent(std::string_view a_event)
		{
			return a_event == Gait::SWIM_START || a_event == Gait::SWIM_STOP;
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

		// Sends a step's events to a stand-in's graph and keeps the count of those taken and turned away. The part of what
		// it shows that an event turned away was to change stays as it was, so that event alone is sent again next time
		// (Gait::Settle; a swim event only SWIM_TRIES times for one change).
		void TellGraph(RE::Actor* a_actor, std::uint32_t a_player, const Gait::Step& a_step)
		{
			if (a_step.events.empty()) {
				return;
			}
			std::vector<std::pair<std::string_view, bool>> results;
			results.reserve(a_step.events.size());
			std::string   refused;
			std::uint64_t taken = 0;
			std::uint64_t refusals = 0;
			std::uint64_t swimTaken = 0;
			std::uint64_t swimRefused = 0;
			for (const auto event : a_step.events) {
				const bool ok = a_actor->NotifyAnimationGraph(Name(event));
				results.emplace_back(event, ok);
				if (ok) {
					++taken;
				} else {
					refused = event;
					++refusals;
				}
				if (IsSwimEvent(event)) {
					++(ok ? swimTaken : swimRefused);
				}
			}
			const auto       settled = Gait::Settle(a_step.shown, results);
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
			remote.swimTaken += swimTaken;
			remote.swimRefused += swimRefused;
			if (swimTaken > 0 && remote.swimTaken == swimTaken) {
				logs::info("Players: player {}'s stand-in {:08X}: its graph took its first swim event", a_player, remote.standInId);
			}
			if (settled.swimGivenUp) {
				++remote.swimGivenUp;
				logs::info("Players: player {}'s stand-in {:08X}: its graph turned {} away {} times in a row; left as it is until its friend next goes in or out of the water ({} so far)",
					a_player, remote.standInId, settled.swimming ? Gait::SWIM_START : Gait::SWIM_STOP, Gait::SWIM_TRIES, remote.swimGivenUp);
			}
			remote.shown = settled;
			if (!refused.empty()) {
				remote.refusedEvents += refusals;
				if (remote.lastRefused != refused || remote.refusedEvents == refusals) {
					logs::info("Players: player {}'s stand-in {:08X}: its graph turned {} away ({} times so far; sent from the {})", a_player, remote.standInId,
						refused, remote.refusedEvents, eventsFromTick ? "tick" : "update");
				}
				remote.lastRefused = refused;
			}
		}

		// Plays a friend's actions that are due on their stand-in's graph, and counts each by kind, played or turned away.
		void PlayActions(RE::Actor* a_actor, std::uint32_t a_player, const std::vector<NpcRules::Event>& a_events)
		{
			if (a_events.empty()) {
				return;
			}
			std::vector<std::pair<std::string_view, bool>> results;
			results.reserve(a_events.size());
			for (const auto& event : a_events) {
				results.emplace_back(event.name, Reactions::Play(a_actor, event));  // a stagger as far and the same way as its friend's
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

		// Whether an effect may take hold on a_this (PeaceRules::JudgeMagic): a stand-in's on anyone else is refused (its
		// friend's game deals it), and so is anyone's on a stand-in (a spell, a shout, a trap's, a hazard's, our player's
		// heal: its friend is not here, and their own game gives them what lands on them); a stand-in's own on itself, of
		// a kind MagicKept keeps, holds. Counted on its stand-in. On whatever thread the game adds effects; the lock is
		// taken only when one of the two is a stand-in or on its way out (watchedIds, read with no lock).
		bool LetMagic(RE::MagicTarget* a_this, const RE::MagicTarget::AddTargetData& a_data)
		{
			if (!Watching() || !a_this) {
				return true;
			}
			// The caster when it is an actor (a trap or a hazard casts too: never a stand-in).
			const auto casterRef = a_data.caster;
			const auto caster = casterRef && casterRef->IsActor() ? static_cast<RE::Actor*>(casterRef) : nullptr;
			const auto targetRef = a_this->MagicTargetIsActor() ? a_this->GetTargetStatsObject() : nullptr;
			const auto target = targetRef && targetRef->IsActor() ? static_cast<RE::Actor*>(targetRef) : nullptr;
			const bool onItself = caster && caster->AsMagicTarget() == a_this;
			const bool casterWatched = caster && watchedIds.Contains(caster->GetFormID());
			const bool targetWatched = !onItself && target && watchedIds.Contains(target->GetFormID());
			const auto base = a_data.effect ? a_data.effect->baseEffect : nullptr;
			const auto archetype = base ? static_cast<std::int32_t>(std::to_underlying(base->GetArchetype())) : StandInRules::kArchNone;
			// A copy of an NPC another game runs makes or moves nothing here (SpellRules::JudgeCopyMagic): its runner's
			// game makes the real summon, the real raised corpse. Asked only of the archetypes that would (NpcSync's
			// lookup of its copies takes no lock: an id set).
			if (caster && !casterWatched && SpellRules::Judged(archetype) && NpcSync::RefusesCopyMagic(caster, archetype)) {
				return false;
			}
			if (!casterWatched && !targetWatched) {
				return true;
			}
			std::scoped_lock l{ lock };
			const auto       leaving = [](RE::Actor* a_actor) {
				const auto gone = a_actor ? retiring.find(a_actor) : retiring.end();
				return gone != retiring.end() && gone->second == a_actor->GetFormID();
			};
			if (leaving(caster) || (!onItself && leaving(target))) {
				return false;  // on its way out: nothing of it takes hold anywhere, and nothing takes hold on it
			}
			const auto casterRemote = caster ? RemoteOf(caster) : nullptr;
			const auto targetRemote = !onItself && target ? RemoteOf(target) : nullptr;
			switch (PeaceRules::JudgeMagic(casterRemote != nullptr, targetRemote != nullptr, onItself, StandInRules::MagicKept(onItself, archetype))) {
			case PeaceRules::Magic::kKept:
				++casterRemote->magicKept;
				return true;
			case PeaceRules::Magic::kRefused:
				++casterRemote->magicRefused;
				return false;
			case PeaceRules::Magic::kRefusedOn:
				if (++targetRemote->magicOnRefused == 1) {
					logs::info("Players: stand-in {:08X}: an effect put on it here was refused, the first (its friend is not here; counted in `peace`)", targetRemote->standInId);
				}
				if (casterRemote) {
					++casterRemote->magicRefused;  // one stand-in's on another: its friend's game deals it, too
				}
				return false;
			case PeaceRules::Magic::kLet:
				break;
			}
			return true;
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

		// Our player's hits that landed on an actor that is no stand-in while stand-ins are here (TESHitEvent; under lock):
		// the rig tells a bystander its spell passed through a stand-in into from a crime of the stand-in's (`peace`).
		std::uint64_t ourHitsElsewhere = 0;
		RE::FormID    lastHitElsewhere = 0;

		// All the health a stand-in has lost here, given back (its damage modifier to 0): how much. Its friend's game is the
		// one that is hurt; here it never weakens, kneels or falls. Any thread (the engine's hit and damage jobs).
		float GiveBackHealth(RE::Actor* a_standIn)
		{
			const auto values = a_standIn->AsActorValueOwner();
			if (!values || a_standIn->IsDead()) {
				return 0.0f;
			}
			const float lost = -a_standIn->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth);
			if (lost <= 0.0f) {
				return 0.0f;
			}
			values->RestoreActorValue(RE::ActorValue::kHealth, lost);
			return lost;
		}

		// A wound on a stand-in (any attacker, none included: a trap, a hazard, a fall), its health given back: counted on it.
		void CountWound(RE::Actor* a_standIn, RE::Actor* a_attacker, float a_damage, float a_back)
		{
			std::scoped_lock l{ lock };
			const auto       remote = RemoteOf(a_standIn);
			if (!remote) {
				return;  // on its way out: given back all the same
			}
			remote->healthBack += a_back;
			if (++remote->woundsOn == 1) {
				logs::warn("Players: stand-in {:08X} lost {:.1f} health here to {:08X}; given back (weapons should pass through it; counted in `peace`)", remote->standInId,
					std::abs(a_damage), a_attacker ? a_attacker->GetFormID() : 0u);
			}
		}

		void CharacterHealthDamage(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
		{
			// A stand-in hurt here by anyone or anything (the ghost flag should let no weapon reach it; this holds when it
			// does not): its health given back before the engine's own handling, which then sees it whole (no kneeling, no
			// fall, no kill move), and again after, for anything that handling took. Its friend's game is the one hurt.
			const bool standIn = a_this && a_damage != 0.0f && watched.load(std::memory_order_relaxed) != 0 && watchedIds.Contains(a_this->GetFormID());
			float      back = standIn ? GiveBackHealth(a_this) : 0.0f;
			originalCharacterHealthDamage(a_this, a_attacker, a_damage);
			if (standIn) {
				back += GiveBackHealth(a_this);
				CountWound(a_this, a_attacker, a_damage, back);
			}
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
					// A hit that landed on a stand-in (anyone's: none should, PeaceRules.h), for `peace`.
					const auto target = a_event->target.get();
					const auto cause = a_event->cause.get();
					if (const auto remote = RemoteOf(target)) {
						if (++remote->hitsOn == 1) {
							logs::warn("Players: stand-in {:08X} was hit here by {:08X} (weapons and spells should pass through it; counted in `peace`)", remote->standInId,
								cause ? cause->GetFormID() : 0u);
						}
					} else if (target && cause && target != cause && target->IsActor() && cause == RE::PlayerCharacter::GetSingleton() &&
							   !watchedIds.Contains(target->GetFormID())) {
						++ourHitsElsewhere;  // someone else: a bystander behind a stand-in, say (the rig's)
						lastHitElsewhere = target->GetFormID();
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
			// A getting up asked for by the furniture tick is started here, in the actor's own update, on its thread (its
			// sit state shares a word with states the update writes): every actor kind, stand-in or copy (Furniture.h).
			Furniture::BeforeUpdate(a_this);
			if (!Watching()) {
				originalUpdate(a_this, a_delta);
				return;
			}
			std::optional<StateBuffer::Sampled> sample;
			Gait::Step                          step;
			bool                                ours = false;
			std::uint32_t                       playerId = 0;
			std::vector<NpcRules::Event>        actions;
			float                               aim = 0.0f;
			const bool                          eventsHere = !eventsFromTick.load(std::memory_order_relaxed);
			// Seated on the copy of its friend's horse (Riding.h): the engine carries it in the saddle, so it is not placed,
			// turned or told its walk (its actions still play: a mounted swing, a bow from the saddle).
			// The same in furniture (Furniture.h): in it, getting in or out, or waiting by the furniture its friend is in,
			// the engine and the furniture's marker place it, not its friend's states.
			const bool                          seated = Riding::Seated(a_this) || Furniture::Held(a_this) || Pairs::Held(a_this);
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
							step = Gait::Next(remote.shown, sample->state, sample->held, SwimsAsMode(a_this));
							if (seated) {
								step.events.clear();
							} else if (eventsHere) {
								remote.shown = step.shown;  // else the tick tells the graph (TellGraph from Frame)
							}
							remote.lastDrawn = sample->state;
							remote.hasDrawn = true;
							++remote.drawn;
							// Its friend's actions, each at the moment it was done, as the stand-in is drawn (a delay behind),
							// told its graph where its legs' events are (the tick, in tick mode: Frame).
							if (eventsHere) {
								actions = remote.actions.DueEvents(Clock::now() - StateBuffer::Buffer::Delay());
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
			// One that fell here (StandInRules::FallOf: killed by a console or a script, brought down) is no longer its
			// friend's: it is neither placed nor told its friend's walk and actions (a body would be dragged along), and the
			// next tick takes it away and places another (Frame).
			const auto lifeState = a_this->AsActorState();
			if (lifeState && StandInRules::FallOf(std::to_underlying(lifeState->GetLifeState()), false) != StandInRules::Fall::kStanding) {
				sample.reset();
			}
			// Its own AI never runs: no packages, no combat, no greetings. Where its friend is, the engine's no-AI update
			// places its 3D.
			if (sample && seated) {
				drive.Set(a_this, a_this->GetFormID(), { step.speed, step.direction }, Clock::now());
			} else if (sample) {
				a_this->SetPosition({ sample->state.x, sample->state.y, sample->state.z }, true);
				a_this->SetHeading(sample->state.heading);
				// Aimed where its friend aims while a weapon or a spell is out (the way its arrows and spells leave), pitched
				// as its friend swims under water, level otherwise.
				a_this->data.angle.x = aim;
				ShowMoves(a_this, Gait::Bits(step, sample->state));
				// What its graph's Speed and Direction channels give it from now on (SpeedPoll, DirectionPoll).
				drive.Set(a_this, a_this->GetFormID(), { step.speed, step.direction }, Clock::now());
			}
			// Getting in or out of furniture, the engine's own update runs for it (its sit procedure is the AI's), for a few
			// seconds at most (SitRules::ENTRY_MAX, STAND_WAIT).
			if (Furniture::Assisted(a_this) || Pairs::Held(a_this)) {
				originalUpdate(a_this, a_delta);
			} else {
				a_this->UpdateNoAI(a_delta);
			}
			// Weapons and spells pass through it, after whatever its update did (StandInPeace.h).
			StandInPeace::AfterUpdate(a_this);
			// In water, at its friend's depth: the water's pull to the surface does not lift it (Gait::Refloat).
			if (sample) {
				float      drift = 0.0f;
				const bool held = HoldDepth(a_this, sample->state, &drift);
				if ((sample->state.flags & Protocol::kSwimming) != 0) {
					std::scoped_lock l{ lock };
					if (const auto player = players.find(playerId); player != players.end() && player->second.standInId == a_this->GetFormID()) {
						player->second.drift = drift;
						player->second.refloats += held ? 1 : 0;
					}
				}
			}
			if (sample && eventsHere) {
				TellGraph(a_this, playerId, step);
			}
			if (sample && eventsHere) {
				PlayActions(a_this, playerId, actions);
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
			// Off its mount first, or its rider off it (Riding.h): no actor leaves the world, or goes in a save, still
			// in a ride with one that stays.
			Riding::Release(a_actor);
			// And out of its furniture (Furniture.h), for the same reason.
			Furniture::Release(a_actor);
			// And out of any paired animation playing here (Pairs.h): the other actor is let go with it.
			Pairs::Release(a_actor);
			// The co-save names the preset even for one standing on a copy of it: it has the preset back before any save
			// (PutPresetBack), and a load looks for an actor of that base.
			const auto base = a_actor->GetObjectReference();
			const auto preset = Face::PresetOf(base);
			const auto named = preset ? static_cast<const RE::TESBoundObject*>(preset) : base;
			presetsNamed += preset ? 1 : 0;
			graveyard.push_back({ .actor = a_actor->GetHandle(),
				.id = id,
				.base = named ? named->GetFormID() : 0,
				.placedAt = a_placedAt,
				.copy = preset ? base->As<RE::TESNPC>() : nullptr });
			std::scoped_lock l{ lock };
			retiring[a_actor] = id;
			CountWatched();
		}

		// A stand-in standing on a copy of its preset (its friend's face) is given the preset back as its base on its way
		// out, so a save written while it is still there (disabled, not yet deleted) holds a plain preset actor, which loads
		// as one and the load's sweep removes: never an actor on a form made at run time (docs/DESIGN.md, "Nothing of the
		// session in a save"). Only once it is out of the world or going: at the delete step (its 3D gone) and before a save
		// (disabled in the same frame). The engine's own base setter is not called: it would set the actor up again.
		void PutPresetBack(RE::Actor* a_actor)
		{
			const auto preset = a_actor ? Face::PresetOf(a_actor->GetObjectReference()) : nullptr;
			if (!preset) {
				return;
			}
			a_actor->data.objectReference = preset;
			++presetsPutBack;
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
					PutPresetBack(actor.get());
					a_grave.copy = nullptr;  // its 3D is gone: the copy may hold another face now
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
				if (a_save) {
					// The save holds it on its preset, whatever step it is at. One still loading its 3D (never disabled)
					// may be building its head from the copy on a job thread as its base changes: counted and said, so a
					// rig run sees whether it happens (its copy stays in use, never written again, until it is deleted).
					if (!a_grave.disabledAt && a_grave.copy && actor->GetObjectReference() == a_grave.copy) {
						++putBackLoading;
						logs::warn("Players: stand-in {:08X} given its preset back for the save while its 3D was still loading from copy {:08X} ({} times so far)", a_grave.id,
							a_grave.copy->GetFormID(), putBackLoading);
					}
					PutPresetBack(actor.get());
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
			a_remote.copy = nullptr;
			a_remote.faceShown = 0;
			a_remote.faceTried = 0;
			a_remote.watch = {};
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

		// Talking, pickpocketing, trading, follower commands and looting its body (a dead actor's container menu is an
		// activation too): all blocked, the prompt hidden where the game knows that flag.
		void BlockActivation(RE::Actor* a_actor)
		{
			a_actor->extraList.SetExtraFlags(static_cast<RE::ExtraFlags::Flag>(
				std::to_underlying(RE::ExtraFlags::Flag::kBlockActivate) | std::to_underlying(RE::ExtraFlags::Flag::kBlockPlayerActivate) |
				std::to_underlying(RE::ExtraFlags::Flag::kBlockActivateText)), true);
		}

		// Whether a stand-in fell here (StandInRules::FallOf): its life state, and the engine's own word on its death.
		StandInRules::Fall FallOf(RE::Actor* a_actor)
		{
			const auto state = a_actor->AsActorState();
			return StandInRules::FallOf(state ? std::to_underlying(state->GetLifeState()) : StandInRules::kLifeAlive, a_actor->IsDead());
		}

		// Whether its friend's face goes on a stand-in standing on a_preset (FaceRules::Fits), and the face it should show
		// then (its hash; 0: the preset's). The wanted face leaves out a full copy book: a stand-in placed with its preset's
		// face for that reason is not placed again and again.
		FaceRules::Fit FitOf(const Remote& a_remote, const RE::TESNPC* a_preset, bool a_full)
		{
			const auto race = a_preset && a_preset->race ? a_preset->race->GetFormID() : 0;
			return FaceRules::Fits(a_remote.face, race, a_preset && a_preset->IsFemale(), a_remote.faceGivenUp, a_full);
		}

		std::uint32_t WantedFace(const Remote& a_remote, const RE::TESNPC* a_preset)
		{
			return FitOf(a_remote, a_preset, false) == FaceRules::Fit::kOwn ? FaceRules::Hash(*a_remote.face) : 0;
		}

		// Whether an actor stands on that copy: a stand-in here, or one on its way out whose 3D may still be built from it.
		bool CopyInUse(const RE::TESNPC* a_copy)
		{
			return std::ranges::any_of(players, [&](const auto& a_entry) { return a_entry.second.copy == a_copy; }) ||
			       std::ranges::any_of(graveyard, [&](const Grave& a_grave) { return a_grave.copy == a_copy; });
		}

		void Place(std::uint32_t a_id, Remote& a_remote, const Protocol::PlayerState& a_at)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto race = a_remote.looks->race;  // none, or a runtime form (another game's ID): a Nord
			const auto preset = BaseFor(race != 0 && !FormIds::IsRuntime(race) ? race : NORD_RACE, a_remote.looks->female);
			if (!preset || !player) {
				a_remote.placeAfter = Clock::now() + 10s;  // said once in the log; looked at again now and then
				return;
			}
			// Its friend's face, on a copy of the preset, when the face is for that preset's race and sex (FaceRules::Fits).
			auto fit = FitOf(a_remote, preset, Face::Full());
			RE::TESNPC* base = preset;
			if (fit == FaceRules::Fit::kOwn) {
				const auto copy = Face::CopyFor(a_id, preset, *a_remote.face, a_remote.looks->weight, CopyInUse);
				if (copy.base) {
					base = copy.base;
				} else if (copy.full) {
					fit = FaceRules::Fit::kFull;
				} else {
					fit = FaceRules::Fit::kGivenUp;  // the game made no copy: its preset's face (Face.cpp says it once)
				}
			}
			a_remote.fit = fit;
			a_remote.faceTried = WantedFace(a_remote, preset);
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
			// follower commands and any other activation are all one activation, blocked here (BlockActivation). It is
			// a friend, not an NPC with dialogue of its own.
			BlockActivation(actor);
			if (!a_remote.name.empty()) {
				actor->SetDisplayName(a_remote.name, true);
			}
			Neutralise(actor);
			a_remote.baseShouts = Gear::BaseShouts(actor);
			HookCharacterMagic(actor);
			placedThisRun = true;
			a_remote.standIn = actor->GetHandle();
			a_remote.base = preset;
			a_remote.copy = base != preset ? base : nullptr;
			a_remote.faceShown = a_remote.copy ? FaceRules::Hash(*a_remote.face) : 0;
			a_remote.weightShown = a_remote.looks->weight;
			a_remote.watch = {};
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
			logs::info("Players: player {}'s stand-in {:08X} placed (base {:08X}, preset {:08X}, face {}) at ({:.0f},{:.0f},{:.0f}) in {:08X}", a_id, a_remote.standInId,
				base->GetFormID(), preset->GetFormID(), FaceRules::FitName(fit), a_at.x, a_at.y, a_at.z, a_at.cell ? a_at.cell : a_at.worldspace);
		}

		// A placed stand-in's face: placed again (no sooner than FACE_REMAKE_COOL after the last time) when the face it
		// should show is not the one it was placed for (a face came, changed, or was given up), and its head watched while
		// it stands on a copy (FaceRules::Watch): built again once, then its friend's face given up for the session when
		// it shows none, or when our own player's head went while it stood there. True: taken away (placed again soon).
		bool FaceStep(std::uint32_t a_id, Remote& a_remote, RE::Actor* a_actor, Clock::time_point a_now)
		{
			if (WantedFace(a_remote, a_remote.base) != a_remote.faceTried) {
				if (a_now < a_remote.faceRemakeAfter) {
					return false;
				}
				a_remote.faceRemakeAfter = a_now + FACE_REMAKE_COOL;
				++a_remote.faceRemakes;
				TakeAway(a_id, a_remote, "their face changed");
				return true;
			}
			if (!a_remote.copy) {
				return false;
			}
			using Step = FaceRules::Watch::Step;
			const auto                    ourHead = Face::OurHead();
			const FaceRules::Watch::Facts facts{
				.loaded = a_actor->Is3DLoaded(),
				.head = Face::HasHead(a_actor),
				.oursLoaded = ourHead.loaded,
				.oursHead = ourHead.head,
				.oursJudged = ourHead.judged,
			};
			const auto step = a_remote.watch.Next(facts, a_now);
			switch (step) {
			case Step::kNone:
				return false;
			case Step::kBuilt:
				logs::info("Face: player {}'s stand-in {:08X} has its head (copy {:08X} of preset {:08X})", a_id, a_remote.standInId, a_remote.copy->GetFormID(),
					a_remote.base ? a_remote.base->GetFormID() : 0);
				return false;
			case Step::kRebuild:
				logs::warn("Face: player {}'s stand-in {:08X} shows no head {} s after its 3D loaded; its 3D is built again", a_id, a_remote.standInId,
					std::chrono::duration_cast<std::chrono::seconds>(FaceRules::Watch::NO_HEAD_FOR).count());
				Face::Rebuild(a_actor);
				return false;
			case Step::kGiveUp:
			case Step::kGiveUpOurs:
				break;
			}
			const bool ours = step == Step::kGiveUpOurs;
			a_remote.faceGivenUp = true;
			if (ours) {
				++oursGivenUp;
				repairOursAt = a_now + REPAIR_OURS_AFTER;
				logs::warn("Face: our own player's head went while player {}'s stand-in {:08X} stood on a copy of its preset; their face is given up here for the session",
					a_id, a_remote.standInId);
			} else {
				++facesGivenUp;
				logs::warn("Face: player {}'s stand-in {:08X} still shows no head after its 3D was built again; their face is given up here for the session (the preset's)",
					a_id, a_remote.standInId);
			}
			TakeAway(a_id, a_remote, "its face showed no head");
			return true;
		}

		// Our own head asked back once, a while after a stand-in seemed to take it, if it is still gone then.
		void RepairOursIfGone(Clock::time_point a_now)
		{
			if (!repairOursAt || a_now < *repairOursAt) {
				return;
			}
			repairOursAt.reset();
			const auto ours = Face::OurHead();
			if (!oursRepaired && ours.loaded && ours.judged && !ours.head) {
				oursRepaired = Face::RepairOurs();  // never a beast form's, first person's or a face editor's 3D
			}
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
			const auto dressed = Gear::Dress(a_remote.gear, true, a_actor);
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
			a_remote.held = Gear::Check(a_remote.gear, a_actor);
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

		// Its weapon drawn or put away by hand when its graph has not followed its friend's (StandInRules::DrawFix).
		void FixDraw(std::uint32_t a_id, Remote& a_remote, RE::Actor* a_actor, const Protocol::PlayerState& a_state, Clock::time_point a_now)
		{
			const auto state = a_actor->AsActorState();
			if (!state) {
				return;
			}
			using WS = RE::WEAPON_STATE;
			const auto weapon = state->GetWeaponState();
			const bool busy = weapon == WS::kWantToDraw || weapon == WS::kDrawing || weapon == WS::kWantToSheathe || weapon == WS::kSheathing;
			const bool wanted = (a_state.flags & Protocol::kWeaponDrawn) != 0;
			if (a_remote.drawFix.Due(wanted, weapon == WS::kDrawn, busy, a_now)) {
				a_actor->DrawWeaponMagicHands(wanted);
				if (a_remote.drawFix.fixes <= 3) {
					logs::info("Players: player {}'s stand-in {:08X} {} its weapon by hand: its graph had not followed its friend's ({} times so far)", a_id,
						a_remote.standInId, wanted ? "drew" : "put away", a_remote.drawFix.fixes);
				}
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

	void Faced(std::uint32_t a_playerId, const Protocol::PlayerFace& a_face)
	{
		const auto it = players.find(a_playerId);
		if (it == players.end()) {
			return;
		}
		auto& remote = it->second;
		++remote.facesIn;
		if (remote.face && *remote.face == a_face) {
			return;
		}
		remote.face = a_face;
		logs::info("Players: player {}'s face {:08X}: race {:08X}, {}, {} head parts, {} tints", a_playerId, FaceRules::Hash(a_face), a_face.race,
			a_face.female ? "female" : "male", a_face.headParts.size(), a_face.tints.size());
		// A stand-in already there is placed again with it in the next frame (FaceStep), when it is one that goes on.
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
			remote.actions.Push(NpcRules::Event{ action.event, action.stagger }, action.sentMs ? remote.buffer.sender.At(action.sentMs) : std::nullopt, now);
		}
	}

	void Frame()
	{
		if (players.empty()) {
			return;
		}
		const auto now = Clock::now();
		RepairOursIfGone(now);
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
			// One that fell here (StandInRules::Falls): taken away at once and another placed, its friend being up. A pair it
			// is held in (a kill move) ends first. Until then it is a body wearing its friend's gear, and never looted: its
			// activation, blocked when it was placed, is blocked again in case a script opened it (BlockActivation).
			if (actor && remote.standInId != 0 && FallOf(actor.get()) != StandInRules::Fall::kStanding) {
				BlockActivation(actor.get());
			}
			if (actor && remote.standInId != 0 && !Pairs::Held(actor.get())) {
				if (const auto fall = FallOf(actor.get()); fall != StandInRules::Fall::kStanding) {
					const auto wait = remote.falls.Fell(now);
					remote.lastFall = fall;
					if (remote.falls.total <= 3 || wait != Clock::duration::zero()) {
						logs::warn("Players: player {}'s stand-in {:08X} is {} here (life state {}): taken away, another placed{} ({} so far)", id, remote.standInId,
							StandInRules::FallName(fall), actor->AsActorState() ? std::to_underlying(actor->AsActorState()->GetLifeState()) : 0u,
							wait != Clock::duration::zero() ? " in 10 s: they keep falling" : "", remote.falls.total);
					}
					TakeAway(id, remote, fall == StandInRules::Fall::kDead ? "it died here" : "it fell here");
					remote.placeAfter = now + RESPAWN_AFTER + wait;
					continue;
				}
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
			if (FaceStep(id, remote, actor.get(), now)) {
				continue;  // placed again with the face it should show
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
				std::vector<NpcRules::Event> actions;
				{
					std::scoped_lock l{ lock };
					step = Gait::Next(remote.shown, sample->state, sample->held, SwimsAsMode(actor.get()));
					if (Riding::Seated(actor.get()) || Furniture::Held(actor.get()) || Pairs::Held(actor.get())) {
						step.events.clear();  // in the saddle (Riding.h) or furniture (Furniture.h): its walk is told again once it is out
					} else {
						remote.shown = step.shown;
					}
					actions = remote.actions.DueEvents(now - StateBuffer::Buffer::Delay());
				}
				TellGraph(actor.get(), id, step);
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
		Face::ForgetCopies();  // a load may free the copies made before it: new ones are made
		std::scoped_lock l{ lock };
		for (auto& [id, remote] : players) {
			remote.standIn = {};
			remote.standInId = 0;
			remote.copy = nullptr;
			remote.faceShown = 0;
			remote.faceTried = 0;
			remote.watch = {};
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

	void HookMagic(RE::Actor* a_character)
	{
		if (a_character && !a_character->IsPlayerRef()) {
			HookCharacterMagic(a_character);
		}
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

	bool IsStandInId(RE::FormID a_id)
	{
		return watched.load(std::memory_order_relaxed) != 0 && watchedIds.Contains(a_id);
	}

	bool AnyStandIn()
	{
		return watched.load(std::memory_order_relaxed) != 0;
	}

	std::string Peace()
	{
		// What each stand-in's game objects say, read before the lock (the engine's own reads).
		struct Read
		{
			int        ghost = -1;         // the engine's Actor::IsGhost
			int        processGhost = -1;  // the flag StandInPeace keeps on, in its process's cached values (-1: no process)
			int        inCombat = -1;
			RE::FormID target = 0;  // its own combat target (none expected: it picks none)
			RE::FormID crime = 0;   // its crime faction (its preset's)
			float      health = -1.0f;  // its health now and at most (kept full: KeepFull, the health damage hook)
			float      healthMax = -1.0f;
			int        dead = -1;
			int        bleedout = -1;  // kneeling (an essential actor brought down)
		};
		std::unordered_map<std::uint32_t, Read> reads;
		for (const auto& [id, remote] : players) {
			if (const auto actor = remote.standIn.get(); actor && actor->GetFormID() == remote.standInId) {
				const auto process = actor->GetActorRuntimeData().currentProcess;
				const auto cached = process ? process->cachedValues : nullptr;
				const auto target = actor->GetActorRuntimeData().currentCombatTarget.get();
				const auto crime = actor->GetCrimeFaction();
				const auto values = actor->AsActorValueOwner();
				const auto state = actor->AsActorState();
				reads[id] = { actor->IsGhost() ? 1 : 0, cached ? (cached->flags.all(RE::CachedValues::Flags::kActorIsGhost) ? 1 : 0) : -1, actor->IsInCombat() ? 1 : 0,
					target ? target->GetFormID() : 0u, crime ? crime->GetFormID() : 0u, values ? values->GetActorValue(RE::ActorValue::kHealth) : -1.0f,
					actor->GetActorValueMax(RE::ActorValue::kHealth), actor->IsDead() ? 1 : 0, state ? (state->IsBleedingOut() ? 1 : 0) : -1 };
			}
		}
		std::scoped_lock l{ lock };
		std::string      out = std::format(" ourHitsElsewhere={} lastHitElsewhere={:08X}", ourHitsElsewhere, lastHitElsewhere);
		for (const auto& [id, remote] : players) {
			const auto read = reads.contains(id) ? reads[id] : Read{};
			out += std::format(" | standin player={} id={:08X} ghost={} processGhost={} inCombat={} combatTarget={:08X} crimeFaction={:08X} hp={:.1f} hpMax={:.1f} dead={}"
							   " bleedout={} hitsOn={} woundsOn={} healthBack={:.1f} magicOn={} blows={} magicRefused={} fell={} fellWaits={} lastFall={}",
				id, remote.standInId, read.ghost, read.processGhost, read.inCombat, read.target, read.crime, read.health, read.healthMax, read.dead, read.bleedout,
				remote.hitsOn, remote.woundsOn, remote.healthBack, remote.magicOnRefused, remote.blows, remote.magicRefused, remote.falls.total, remote.falls.waits,
				StandInRules::FallName(remote.lastFall));
		}
		return out;
	}

	bool MadeAsStandIn(const RE::Actor* a_actor)
	{
		return a_actor && a_actor->GetFormID() >= 0xFF000000 && a_actor != RE::PlayerCharacter::GetSingleton() &&
		       IsStandInBase(a_actor->GetObjectReference());
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

	std::string NameOf(std::uint32_t a_playerId)
	{
		std::scoped_lock l{ lock };
		const auto       it = players.find(a_playerId);
		return it == players.end() ? std::string{} : it->second.name;
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

	std::optional<Clock::time_point> SenderTime(std::uint32_t a_playerId, std::uint32_t a_sentMs)
	{
		if (a_sentMs == 0) {
			return std::nullopt;
		}
		std::scoped_lock l{ lock };
		const auto       it = players.find(a_playerId);
		return it != players.end() ? it->second.buffer.sender.At(a_sentMs) : std::nullopt;
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

	void ShowMoves(RE::Actor* a_actor, const Gait::MoveBits& a_bits)
	{
		const auto state = a_actor ? a_actor->AsActorState() : nullptr;
		if (!state) {
			return;
		}
		auto& flags = state->actorState1;
		flags.sneaking = a_bits.sneaking ? 1 : 0;
		flags.sprinting = a_bits.sprinting ? 1 : 0;
		flags.running = a_bits.running ? 1 : 0;
		flags.walking = a_bits.walking ? 1 : 0;
		flags.movingForward = a_bits.forward ? 1 : 0;
		flags.swimming = a_bits.swimming ? 1 : 0;
	}

	bool SwimsAsMode(const RE::Actor* a_actor)
	{
		const auto race = a_actor ? a_actor->GetRace() : nullptr;
		return !race || race->data.flags.any(RE::RACE_DATA::Flag::kWalks);
	}

	bool HoldDepth(RE::Actor* a_actor, const Protocol::PlayerState& a_state, float* a_drift)
	{
		if ((a_state.flags & Protocol::kSwimming) == 0 || !a_actor) {
			return false;
		}
		const float after = a_actor->GetPositionZ();
		if (a_drift) {
			*a_drift = after - a_state.z;
		}
		if (!Gait::Refloat(a_state, a_state.z, after)) {
			return false;
		}
		a_actor->SetPosition({ a_state.x, a_state.y, a_state.z }, true);
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
			out += std::format(" | player {} standin={:08X} placed={} dressed={} at={} target={} graphSpeed={} graphDir={} graphSampled={} shown={}{}{}{}{} drawn={} refused={}{} taken={} states={} newest={} dist={}",
				id, remote.standInId, remote.placed, remote.dressed ? 1 : 0, at,
				remote.hasDrawn ? std::format("({:.0f},{:.0f},{:.0f}) speed={:.0f} dir={:.3f} flags={:#04x}", remote.lastDrawn.x, remote.lastDrawn.y, remote.lastDrawn.z, remote.lastDrawn.speed,
					Protocol::UnpackDirection(remote.lastDrawn.direction), remote.lastDrawn.flags) : "none",
				graphFloat("Speed", "{:.0f}"), graphFloat("Direction", "{:.3f}"), graphFloat("SpeedSampled", "{:.0f}"), remote.shown.moving ? "m" : "-", remote.shown.sneaking ? "s" : "-", remote.shown.sprinting ? "r" : "-",
				remote.shown.inAir ? "j" : "-", remote.shown.swimming ? "w" : "-", remote.drawn, remote.refusedEvents, remote.lastRefused.empty() ? "" : " last=" + remote.lastRefused, remote.takenEvents,
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
		};
		std::unordered_map<std::uint32_t, Read> reads;
		for (const auto& [id, remote] : players) {
			if (const auto actor = remote.standIn.get(); actor && actor->GetFormID() == remote.standInId) {
				const auto state = actor->AsActorState();
				reads[id] = { state ? (state->GetWeaponState() == RE::WEAPON_STATE::kDrawn ? 1 : 0) : -1, actor->GetAngleX(), Gear::BaseShouts(actor.get()) };
			}
		}
		std::scoped_lock l{ lock };
		for (const auto& [id, remote] : players) {
			const auto read = reads.contains(id) ? reads[id] : Read{};
			const auto wanted = remote.hasDrawn ? ((remote.lastDrawn.flags & Protocol::kWeaponDrawn) != 0 ? 1 : 0) : -1;
			const auto handOk = [&](bool a_ok) { return remote.checked ? (a_ok ? "1" : "0") : "?"; };
			out += std::format(" | player {} standin={:08X} in={} unplaced={} waiting={} lost={} played={} byKind={} refused={} refusedKinds={}{} drawn={} wanted={} drawFixes={}"
							   " aim={:.2f} angleX={:.2f} right={:08X}:{} left={:08X}:{} ammo={:08X}:{} voice={:08X}/{:08X} gearOk={} missing={} refusedForms={} redressed={} baseShouts={}/{}"
							   " ammoGiven={} magicRefused={} magicKept={} blows={} casts={} damaged={} dealt={:.1f} undone={:.1f}",
				id, remote.standInId, remote.actionsIn, remote.actionsUnplaced, remote.actions.Size(), remote.actions.Dropped(), remote.played.Total(),
				remote.played.Line(), remote.refused.Total(), remote.refused.Line(), remote.lastActionRefused.empty() ? "" : " lastRefused=" + remote.lastActionRefused,
				read.drawnNow, wanted, remote.drawFix.fixes, remote.aim, read.angleX, remote.gear.right, handOk(remote.held.right), remote.gear.left,
				handOk(remote.held.left), remote.gear.ammo, handOk(remote.held.ammo), remote.looks ? remote.looks->voice : 0u, remote.voiceShown,
				remote.checked ? (remote.held.Complete(remote.gearMissing) && !StandInRules::VoiceDrifted(remote.voiceWanted, !remote.voiceJudged, remote.voiceShown) ? "1" : "0") : "?",
				remote.gearMissing, remote.refusedForms, remote.redressed, read.baseShouts, remote.baseShouts,
				remote.ammoGiven, remote.magicRefused, remote.magicKept, remote.blows, remote.casts, remote.damaged, remote.dealt, remote.undone);
		}
		return out;
	}

	std::string Faces(const std::function<std::uint32_t(const Protocol::PlayerFace&)>& a_sessionHash)
	{
		std::string out = std::format("facesGivenUp={} oursGivenUp={} presetsPutBack={} presetsNamed={} putBackLoading={} oursRepaired={}", facesGivenUp, oursGivenUp,
			presetsPutBack, presetsNamed, putBackLoading, oursRepaired ? 1 : 0);
		for (const auto& [id, remote] : players) {
			const auto actor = remote.standIn.get();
			const bool here = actor && actor->GetFormID() == remote.standInId;
			const auto on = here ? actor->GetActorBase() : nullptr;
			out += std::format(" | player {} face={} sessionFace={:08X} in={} fit={} standin={:08X} base={:08X} preset={:08X} copy={:08X} shown={:08X} ownBase={} head={} watch={} givenUp={} remakes={}", id,
				remote.face ? std::format("{:08X}", FaceRules::Hash(*remote.face)) : std::string{ "none" }, remote.face && a_sessionHash ? a_sessionHash(*remote.face) : 0u, remote.facesIn, FaceRules::FitName(remote.fit), remote.standInId,
				on ? on->GetFormID() : 0u, remote.base ? remote.base->GetFormID() : 0u, remote.copy ? remote.copy->GetFormID() : 0u, remote.faceShown,
				here && remote.copy && on == remote.copy ? 1 : 0, here && Face::HasHead(actor.get()) ? 1 : 0, remote.watch.State(), remote.faceGivenUp ? 1 : 0, remote.faceRemakes);
			// What its built head shows, read from its 3D: the copy's head parts under its face node, none of the preset's
			// the copy left out, its hair the colour that came. Then what the copy holds against what came (all equal when
			// the face went on the form whole; it says nothing of what the engine built from it).
			if (remote.face && remote.copy) {
				out += " " + FaceRules::Line(Face::Shown3D(here ? actor.get() : nullptr, remote.copy, remote.base, remote.face->hairRgb));
				if (const auto shown = Face::ReadBase(remote.copy)) {
					const auto agreement = FaceRules::Compare(*remote.face, *shown);
					out += std::format(" agree={} {}", agreement.All() ? 1 : 0, FaceRules::Line(agreement));
				}
			}
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

	std::string Swim()
	{
		// What each stand-in's game objects say, read before the lock (the game's own locks).
		struct Read
		{
			int                  swimming = -1;  // its actor state's bit (-1: no stand-in)
			int                  engineSwimming = -1;
			int                  inWater = -1;
			int                  charState = -1;  // its character controller's state (hkpCharacterStateType; 5 is swimming)
			float                z = 0.0f;
			float                pitch = 0.0f;  // its angle x, up positive, in degrees
			std::optional<float> body;          // its body's rise as drawn (Water::BodyRise)
			std::optional<float> water;
		};
		std::unordered_map<std::uint32_t, Read> reads;
		for (const auto& [id, remote] : players) {
			const auto actor = remote.standIn.get();
			if (!actor || actor->GetFormID() != remote.standInId) {
				continue;
			}
			Read read;
			const auto state = actor->AsActorState();
			read.swimming = state && state->IsSwimming() ? 1 : 0;
			const auto& data = actor->GetActorRuntimeData();
			read.engineSwimming = data.boolBits.all(RE::Actor::BOOL_BITS::kSwimming) ? 1 : 0;
			read.inWater = data.boolBits.all(RE::Actor::BOOL_BITS::kInWater) ? 1 : 0;
			if (const auto controller = actor->GetCharController()) {
				read.charState = static_cast<int>(std::to_underlying(controller->context.currentState));
			}
			const auto at = actor->GetPosition();
			read.z = at.z;
			read.pitch = -actor->GetAngleX() * 180.0f / std::numbers::pi_v<float>;
			read.body = Water::BodyRise(actor.get());
			read.water = Water::HeightAt(at, actor->GetParentCell());
			reads[id] = read;
		}
		std::string      out;
		std::scoped_lock l{ lock };
		for (const auto& [id, remote] : players) {
			const auto  read = reads.contains(id) ? reads[id] : Read{};
			const auto& friendState = remote.lastDrawn;
			const bool  known = remote.hasDrawn && read.swimming >= 0;
			out += std::format(" | player {} standin={:08X} friendSwims={} friendUnder={} shown={} swimming={} engineSwimming={} inWater={} charState={} z={} friendZ={} depthErr={}"
							   " water={} pitch={} friendPitch={} body={} swimTaken={} swimRefused={} swimGivenUp={} refloats={} drift={:.0f}",
				id, remote.standInId, remote.hasDrawn ? ((friendState.flags & Protocol::kSwimming) != 0 ? 1 : 0) : -1,
				remote.hasDrawn ? ((friendState.flags & Protocol::kUnderwater) != 0 ? 1 : 0) : -1, remote.shown.swimming ? "w" : "-", read.swimming,
				read.engineSwimming, read.inWater, read.charState, known ? std::format("{:.0f}", read.z) : "none",
				remote.hasDrawn ? std::format("{:.0f}", friendState.z) : "none", known ? std::format("{:.0f}", read.z - friendState.z) : "none",
				read.water ? std::format("{:.0f}", *read.water) : "none", known ? std::format("{:.0f}", read.pitch) : "none",
				remote.hasDrawn ? std::to_string(friendState.pitch) : "none", read.body ? std::format("{:.0f}", *read.body) : "none", remote.swimTaken, remote.swimRefused,
				remote.swimGivenUp, remote.refloats, remote.drift);
		}
		return out;
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
			return "error: usage: graph <player id> [vars | event <name> | float <name> | bool <name> | int <name>] | graph from hook|tick";
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
