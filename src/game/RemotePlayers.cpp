#include "game/RemotePlayers.h"

#include "DevLine.h"
#include "game/Gait.h"
#include "game/GaitDrive.h"
#include "game/GameThread.h"
#include "game/LocalPlayer.h"
#include "game/Retire.h"
#include "game/StateBuffer.h"

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
		};

		std::mutex                                lock;  // players' buffers and graph state, byActor and retiring
		std::unordered_map<std::uint32_t, Remote> players;
		std::unordered_map<RE::Actor*, std::uint32_t> byActor;   // stand-in -> player id
		std::unordered_map<RE::Actor*, RE::FormID>    retiring;  // stand-ins on their way out -> their form id
		std::atomic<std::size_t>                  standIns{ 0 };
		std::atomic<std::size_t>                  watched{ 0 };  // byActor and retiring: the hook looks only when some

		void CountWatched()  // under lock
		{
			standIns = byActor.size();
			watched = byActor.size() + retiring.size();
		}

		// The stand-ins on their way out of the world (main thread): disabled, then deleted once that is done
		// (src/game/Retire.h), then held until the game lets go of them. The hook keeps their AI off all the while.
		struct Grave
		{
			RE::ActorHandle                  actor;
			RE::FormID                       id = 0;
			Clock::time_point                placedAt{};  // long ago for one this game did not place
			std::optional<Clock::time_point> disabledAt;
			std::uint64_t                    disabledTick = 0;
			bool                             deleted = false;
			bool                             toldStuck = false;
		};
		std::vector<Grave> graveyard;

		// Stand-ins taken out of the world for the save being written (their form ids), for the co-save; and those the
		// co-save of a loaded save named, removed once the load is done.
		std::vector<RE::FormID> takenForSave;
		std::vector<RE::FormID> namedByLoad;
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

		// The values to give a channel's actor, if it is a stand-in this game moves. The polls run as graphs update, on
		// the game's job threads: only the table's own lock is taken (GaitDrive.h), never the players' one.
		std::optional<GaitDrive::Values> DrivenFor(void* a_channel)
		{
			if (watched.load(std::memory_order_relaxed) == 0) {
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

		// Puts a_hook in a vtable's slot when the function there is the game's own: in its image, not another plugin's
		// hook (whose own original we would call twice) or junk. a_original holds the function the slot held before the
		// slot changes (a job thread may call the hook at once); false: not hooked, a_original left as it was.
		template <class Fn>
		bool HookSlot(std::uintptr_t a_vtable, std::size_t a_index, Fn a_hook, Fn& a_original, std::string_view a_what)
		{
			const auto slot = a_vtable + sizeof(void*) * a_index;
			const auto current = *reinterpret_cast<const std::uintptr_t*>(slot);
			const auto image = reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(nullptr));
			MEMORY_BASIC_INFORMATION info{};
			if (!::VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) || reinterpret_cast<std::uintptr_t>(info.AllocationBase) != image) {
				logs::error("Players: the {} slot holds {:#x}, outside the game's image: not hooked", a_what, current);
				return false;
			}
			a_original = reinterpret_cast<Fn>(current);
			REL::safe_write(slot, reinterpret_cast<std::uintptr_t>(a_hook));
			logs::info("Players: hooked the {} (game+{:X})", a_what, current - image);
			return true;
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

		void UpdateHook(RE::Actor* a_this, float a_delta)
		{
			if (watched.load(std::memory_order_relaxed) == 0) {
				originalUpdate(a_this, a_delta);
				return;
			}
			std::optional<StateBuffer::Sampled> sample;
			Gait::Step                          step;
			Gait::Shown                         was;
			bool                                ours = false;
			std::uint32_t                       playerId = 0;
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
						}
					}
				}
			}
			if (!ours) {
				originalUpdate(a_this, a_delta);
				return;
			}
			// Its own AI never runs: no packages, no combat, no greetings. Where its friend is, the engine's no-AI update
			// places its 3D.
			if (sample) {
				a_this->SetPosition({ sample->state.x, sample->state.y, sample->state.z }, true);
				a_this->SetHeading(sample->state.heading);
				SetMoveFlags(a_this, step, sample->state);
				// What its graph's Speed and Direction channels give it from now on (SpeedPoll, DirectionPoll).
				drive.Set(a_this, a_this->GetFormID(), { step.speed, step.direction }, Clock::now());
			}
			a_this->UpdateNoAI(a_delta);
			if (sample && eventsHere) {
				TellGraph(a_this, playerId, step, was);
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
			graveyard.push_back({ .actor = a_actor->GetHandle(), .id = id, .placedAt = a_placedAt });
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
					takenForSave.push_back(a_grave.id);
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
			}
			a_remote.standIn = {};
			a_remote.dressed = false;
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

		void Place(std::uint32_t a_id, Remote& a_remote, const Protocol::PlayerState& a_at)
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto base = BaseFor(a_remote.looks->race, a_remote.looks->female);
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
			placedThisRun = true;
			a_remote.standIn = actor->GetHandle();
			a_remote.base = base;
			a_remote.dressed = false;
			a_remote.placedAt = Clock::now();
			++a_remote.placed;
			{
				std::scoped_lock l{ lock };
				a_remote.standInId = actor->GetFormID();
				byActor[actor] = a_id;
				CountWatched();
				a_remote.shown = {};
			}
			logs::info("Players: player {}'s stand-in {:08X} placed (base {:08X}) at ({:.0f},{:.0f},{:.0f}) in {:08X}", a_id, a_remote.standInId, base->GetFormID(),
				a_at.x, a_at.y, a_at.z, a_at.cell ? a_at.cell : a_at.worldspace);
		}

		void Dress(std::uint32_t a_id, Remote& a_remote, RE::Actor* a_actor)
		{
			const auto manager = RE::ActorEquipManager::GetSingleton();
			if (!manager || !a_actor->Is3DLoaded()) {
				return;
			}
			std::size_t worn = 0;
			for (const auto id : a_remote.looks->worn) {
				// Another player's game named it: only armor a player can wear (no creature skins, no non-playable
				// pieces).
				const auto armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(id);
				if (!armor || !armor->GetPlayable()) {
					continue;
				}
				a_actor->AddObjectToContainer(armor, nullptr, 1, nullptr);
				manager->EquipObject(a_actor, armor, nullptr, 1, nullptr, false, true, false, true);
				++worn;
			}
			a_remote.dressed = true;
			logs::info("Players: player {}'s stand-in {:08X} dressed in {} of {} worn items", a_id, a_remote.standInId, worn, a_remote.looks->worn.size());
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
		const bool changed = remote.looks && *remote.looks != a_looks;
		remote.looks = a_looks;
		logs::info("Players: player {}'s looks: race {:08X}, {}, {} worn items", a_playerId, a_looks.race, a_looks.female ? "female" : "male", a_looks.worn.size());
		if (changed && remote.standInId != 0) {
			TakeAway(a_playerId, remote, "their looks changed");
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
			}
			if (eventsFromTick && actor->Is3DLoaded()) {
				Gait::Step  step;
				Gait::Shown was;
				{
					std::scoped_lock l{ lock };
					was = remote.shown;
					step = Gait::Next(was, sample->state, sample->held);
					remote.shown = step.shown;
				}
				TellGraph(actor.get(), id, step, was);
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
		const auto count = static_cast<std::uint32_t>(takenForSave.size());
		a_intf->WriteRecordData(&count, sizeof(count));
		a_intf->WriteRecordData(takenForSave.data(), static_cast<std::uint32_t>(takenForSave.size() * sizeof(RE::FormID)));
		logs::info("Co-save: {} stand-in id(s) written, to be removed should a load bring any back", count);
		takenForSave.clear();
	}

	void ReadCoSave(SKSE::SerializationInterface* a_intf, std::uint32_t a_version, std::uint32_t a_length)
	{
		namedByLoad.clear();
		coSaveRecord = true;
		std::uint32_t count = 0;
		if (a_version != COSAVE_STANDINS_VERSION || a_length < sizeof(count) || a_intf->ReadRecordData(&count, sizeof(count)) != sizeof(count) ||
			count > 1024 || a_length != sizeof(count) + count * sizeof(RE::FormID)) {
			logs::warn("Co-save: a stand-ins record that can't be read (version {}, {} bytes); the load's sweep still runs", a_version, a_length);
			return;
		}
		namedByLoad.resize(count);
		const auto bytes = count * static_cast<std::uint32_t>(sizeof(RE::FormID));
		if (const auto read = a_intf->ReadRecordData(namedByLoad.data(), bytes); read != bytes) {
			logs::warn("Co-save: the stand-ins record gave {} of {} bytes; the load's sweep still runs", read, bytes);
		}
		logs::info("Co-save: this save was written with {} stand-in(s) taken out", count);
	}

	void AfterLoad()
	{
		std::size_t named = 0;
		for (const auto id : namedByLoad) {
			const auto actor = RE::TESForm::LookupByID<RE::Actor>(id);
			if (actor && IsStandInBase(actor->GetObjectReference()) && !actor->IsDeleted()) {
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
			out += std::format(" named={} aimed={}", named, aimed ? 1 : 0);
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
