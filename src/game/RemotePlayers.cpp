#include "game/RemotePlayers.h"

#include "DevLine.h"
#include "game/Gait.h"
#include "game/LocalPlayer.h"
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
			std::uint32_t     placed = 0;

			Gait::Shown   shown;  // under lock
			std::uint64_t drawn = 0;
			std::uint64_t refusedEvents = 0;
			std::string   lastRefused;
			Protocol::PlayerState lastDrawn;
			bool          hasDrawn = false;
		};

		std::mutex                                lock;  // players' buffers and graph state, and byActor
		std::unordered_map<std::uint32_t, Remote> players;
		std::unordered_map<RE::Actor*, std::uint32_t> byActor;  // stand-in -> player id
		std::atomic<std::size_t>                  standIns{ 0 };

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

		void UpdateHook(RE::Actor* a_this, float a_delta)
		{
			if (standIns.load(std::memory_order_relaxed) == 0) {
				originalUpdate(a_this, a_delta);
				return;
			}
			std::optional<StateBuffer::Sampled> sample;
			Gait::Step                          step;
			Gait::Shown                         was;
			bool                                ours = false;
			{
				std::scoped_lock l{ lock };
				if (const auto it = byActor.find(a_this); it != byActor.end()) {
					if (const auto player = players.find(it->second); player != players.end() && player->second.standInId == a_this->GetFormID()) {
						ours = true;
						auto& remote = player->second;
						sample = remote.buffer.Sample(Clock::now());
						if (sample) {
							was = remote.shown;
							step = Gait::Next(remote.shown, sample->state, sample->held);
							remote.shown = step.shown;
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
			}
			a_this->UpdateNoAI(a_delta);
			if (!sample) {
				return;
			}
			a_this->SetGraphVariableFloat(Name("Speed"), step.speed);
			a_this->SetGraphVariableFloat(Name("Direction"), step.direction);
			std::string refused;
			for (const auto event : step.events) {
				if (!a_this->NotifyAnimationGraph(Name(event))) {
					refused = event;
				}
			}
			if (!refused.empty()) {
				// Not taken: the graph is told again next frame (what it shows stays what it was).
				std::scoped_lock l{ lock };
				if (const auto it = byActor.find(a_this); it != byActor.end()) {
					if (const auto player = players.find(it->second); player != players.end()) {
						player->second.shown = was;
						++player->second.refusedEvents;
						player->second.lastRefused = refused;
					}
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

		// Out of the world: disabled now, and deleted, so the game drops it once it may. Never left for a save.
		void Remove(RE::Actor* a_actor)
		{
			a_actor->Disable();
			a_actor->SetDelete(true);
		}

		void TakeAway(std::uint32_t a_id, Remote& a_remote, std::string_view a_why, bool a_forSave = false)
		{
			const auto actor = a_remote.standIn.get();
			{
				std::scoped_lock l{ lock };
				for (auto it = byActor.begin(); it != byActor.end();) {
					it = it->second == a_id ? byActor.erase(it) : std::next(it);
				}
				standIns = byActor.size();
				a_remote.shown = {};
			}
			if (actor && actor->GetFormID() == a_remote.standInId) {
				if (a_forSave) {
					takenForSave.push_back(a_remote.standInId);
				}
				Remove(actor.get());
				logs::info("Players: player {}'s stand-in {:08X} taken away ({})", a_id, a_remote.standInId, a_why);
			}
			a_remote.standIn = {};
			a_remote.standInId = 0;
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
				Remove(actor);
				a_remote.placeAfter = Clock::now() + 30s;
				return;
			}
			actor->SetPosition({ a_at.x, a_at.y, a_at.z }, true);
			actor->SetHeading(a_at.heading);
			if (!a_remote.name.empty()) {
				actor->SetDisplayName(a_remote.name, true);
			}
			placedThisRun = true;
			a_remote.standIn = actor->GetHandle();
			a_remote.standInId = actor->GetFormID();
			a_remote.base = base;
			a_remote.dressed = false;
			++a_remote.placed;
			{
				std::scoped_lock l{ lock };
				byActor[actor] = a_id;
				standIns = byActor.size();
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
		const auto slot = characterVtable + sizeof(void*) * UPDATE_INDEX;
		const auto current = *reinterpret_cast<const std::uintptr_t*>(slot);
		const auto image = reinterpret_cast<std::uintptr_t>(REX::W32::GetModuleHandleW(nullptr));
		// The function in the slot must be the game's own: in its image, not some other plugin's hook (whose own original
		// we would call twice) or junk.
		MEMORY_BASIC_INFORMATION info{};
		if (!::VirtualQuery(reinterpret_cast<void*>(current), &info, sizeof(info)) || reinterpret_cast<std::uintptr_t>(info.AllocationBase) != image) {
			logs::error("Players: the Character update slot holds {:#x}, outside the game's image: not hooked, no stand-ins", current);
			return false;
		}
		originalUpdate = reinterpret_cast<UpdateFn>(current);
		REL::safe_write(slot, reinterpret_cast<std::uintptr_t>(&UpdateHook));
		hooked = true;
		logs::info("Players: hooked the Character update (game+{:X})", current - image);
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
		}
	}

	void Clear()
	{
		for (auto& [id, remote] : players) {
			TakeAway(id, remote, "the session ended");
		}
		std::scoped_lock l{ lock };
		players.clear();
		byActor.clear();
		standIns = 0;
	}

	void ForgetStandIns()
	{
		std::scoped_lock l{ lock };
		for (auto& [id, remote] : players) {
			remote.standIn = {};
			remote.standInId = 0;
			remote.dressed = false;
			remote.shown = {};
			remote.buffer.Clear();  // the states before the load are of another moment
		}
		byActor.clear();
		standIns = 0;
	}

	void BeforeSave()
	{
		takenForSave.clear();
		std::size_t taken = 0;
		for (auto& [id, remote] : players) {
			if (remote.standInId != 0) {
				TakeAway(id, remote, "a save is written", true);
				++taken;
			}
		}
		// And any other placed preset actor (one an earlier save brought back that a sweep missed): none is saved.
		for (const auto actor : SweepWanted() ? PresetActors() : std::vector<RE::Actor*>{}) {
			if (!actor->IsDisabled()) {
				takenForSave.push_back(actor->GetFormID());
				Remove(actor);
				++taken;
			}
		}
		if (taken > 0 || !players.empty()) {
			logs::info("Save: {} stand-in(s) taken out of the world before the save; placed again after", taken);
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
				Remove(actor);
				++named;
			}
		}
		std::size_t swept = 0;
		for (const auto actor : SweepWanted() ? PresetActors() : std::vector<RE::Actor*>{}) {
			if (!IsCurrentStandIn(actor) && !actor->IsDisabled()) {
				Remove(actor);
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
		std::string out = std::format("players={} standins={}", players.size(), standIns.load());
		const auto  here = LocalPlayer::Read();
		std::scoped_lock l{ lock };
		for (const auto& [id, remote] : players) {
			const auto actor = remote.standIn.get();
			std::string at = "none";
			if (actor) {
				const auto p = actor->GetPosition();
				at = std::format("({:.0f},{:.0f},{:.0f}) heading={:.3f}", p.x, p.y, p.z, actor->GetAngleZ());
			}
			float      speed = -1.0f;
			const bool hasSpeed = actor && actor->GetGraphVariableFloat(Name("Speed"), speed);
			const auto& newest = remote.buffer.snapshots.empty() ? std::optional<Protocol::PlayerState>{} : std::optional{ remote.buffer.snapshots.back().state };
			out += std::format(" | player {} standin={:08X} placed={} dressed={} at={} target={} graphSpeed={} shown={}{}{}{} drawn={} refused={}{} states={} newest={} dist={}",
				id, remote.standInId, remote.placed, remote.dressed ? 1 : 0, at,
				remote.hasDrawn ? std::format("({:.0f},{:.0f},{:.0f}) speed={:.0f} flags={:#04x}", remote.lastDrawn.x, remote.lastDrawn.y, remote.lastDrawn.z, remote.lastDrawn.speed, remote.lastDrawn.flags) : "none",
				hasSpeed ? std::format("{:.0f}", speed) : "none", remote.shown.moving ? "m" : "-", remote.shown.sneaking ? "s" : "-", remote.shown.sprinting ? "r" : "-",
				remote.shown.inAir ? "j" : "-", remote.drawn, remote.refusedEvents, remote.lastRefused.empty() ? "" : " last=" + remote.lastRefused,
				remote.buffer.snapshots.size(),
				newest ? std::format("({:.0f},{:.0f},{:.0f}) in {:08X}", newest->x, newest->y, newest->z, newest->cell ? newest->cell : newest->worldspace) : "none",
				here && newest ? std::format("{:.0f}", SameSpace(*here, *newest) ? Distance(*here, *newest) : -1.0f) : "?");
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
		const auto id = words.empty() ? std::nullopt : DevLine::Number(words[0]);
		if (!id) {
			return "error: usage: graph <player id> [vars | event <name> | float <name> | bool <name> | int <name>]";
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
