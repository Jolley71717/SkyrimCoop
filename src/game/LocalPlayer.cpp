#include "game/LocalPlayer.h"

#include "FormIds.h"
#include "game/Gear.h"

#include <algorithm>
#include <numbers>

namespace LocalPlayer
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// The ground covered between two reads, for a speed when the graph has none to read.
		struct Last
		{
			RE::NiPoint3      at;
			Clock::time_point when;
			float             speed = 0.0f;
		};
		std::optional<Last> last;

		// The name the game gives a character that has none yet ("Prisoner" in English), read at data load.
		std::string defaultName;

		float GraphFloat(const RE::Actor* a_actor, const char* a_name, float a_default)
		{
			float value = a_default;
			if (!a_actor->GetGraphVariableFloat(a_name, value) || !std::isfinite(value)) {
				return a_default;
			}
			return value;
		}

		// Units a second across the ground since the last read, smoothed a little (reads come at the tick's pace).
		float GroundSpeed(const RE::NiPoint3& a_at)
		{
			const auto now = Clock::now();
			float      speed = 0.0f;
			if (last) {
				const float seconds = std::chrono::duration<float>(now - last->when).count();
				if (seconds > 0.005f && seconds < 0.5f) {
					const float raw = std::hypot(a_at.x - last->at.x, a_at.y - last->at.y) / seconds;
					speed = raw < 3000.0f ? last->speed * 0.5f + raw * 0.5f : 0.0f;  // faster: a door or the console, no run
				} else if (seconds <= 0.005f) {
					return last->speed;
				}
			}
			last = Last{ a_at, now, speed };
			return speed;
		}
	}

	bool InWorld()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell() || !player->Is3DLoaded()) {
			return false;
		}
		const auto ui = RE::UI::GetSingleton();
		return !ui || !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
	}

	std::optional<Protocol::PlayerState> Read()
	{
		if (!InWorld()) {
			last.reset();
			return std::nullopt;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player->GetParentCell();
		const auto at = player->GetPosition();

		Protocol::PlayerState state;
		if (cell->IsInteriorCell()) {
			state.cell = cell->GetFormID();
		} else if (const auto world = player->GetWorldspace()) {
			state.worldspace = world->GetFormID();
		} else {
			state.cell = cell->GetFormID();
		}
		state.x = at.x;
		state.y = at.y;
		state.z = at.z;
		state.heading = player->GetAngleZ();
		const float ground = GroundSpeed(at);
		// The graph's own speed is what its legs cycle at, so the stand-in's graph is given the same; the ground covered
		// when the graph has none.
		state.speed = GraphFloat(player, "Speed", ground);
		state.direction = Protocol::PackDirection(GraphFloat(player, "Direction", 0.0f));
		state.pitch = Protocol::PackPitch(-player->GetAngleX() * 180.0f / std::numbers::pi_v<float>);  // the game's x is down positive

		std::uint8_t flags = Protocol::kNone;
		const auto   actorState = player->AsActorState();
		if (player->IsSneaking()) {
			flags |= Protocol::kSneaking;
		}
		if (actorState && actorState->IsSprinting()) {
			flags |= Protocol::kSprinting;
		}
		if (actorState && actorState->IsWeaponDrawn()) {
			flags |= Protocol::kWeaponDrawn;
		}
		if (actorState && actorState->IsSwimming()) {
			flags |= Protocol::kSwimming;
		}
		if (player->IsInMidair()) {
			flags |= Protocol::kJumping;
		}
		if (player->IsOnMount()) {
			flags |= Protocol::kMounted;
		}
		state.flags = flags;
		if (!state.IsFinite()) {
			return std::nullopt;
		}
		return state;
	}

	std::optional<Protocol::PlayerLooks> Looks()
	{
		if (!InWorld()) {
			return std::nullopt;
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto base = player->GetActorBase();
		const auto race = player->GetRace();
		if (!base || !race) {
			return std::nullopt;
		}
		Protocol::PlayerLooks looks;
		looks.race = race->GetFormID();
		looks.female = base->GetSex() == RE::SEX::kFemale;
		looks.weight = std::isfinite(base->weight) ? std::clamp(base->weight, 0.0f, 100.0f) : 50.0f;
		// A runtime armor (FF: made as this game ran, by a mod's script or plugin) names nothing in another game: the
		// plugin armor it was made from goes instead (its template), or nothing (FormIds::Shareable). A piece worn twice
		// that way is named once.
		const auto templateOf = [](std::uint32_t a_id) -> std::uint32_t {
			const auto armor = RE::TESForm::LookupByID<RE::TESObjectARMO>(a_id);
			return armor && armor->templateArmor ? armor->templateArmor->GetFormID() : 0;
		};
		const auto inventory = player->GetInventory([](RE::TESBoundObject& a_object) { return a_object.IsArmor(); });
		for (const auto& [item, data] : inventory) {
			const auto& [count, entry] = data;
			if (count <= 0 || !entry || !entry->IsWorn() || looks.worn.size() >= Protocol::MAX_WORN) {
				continue;
			}
			const auto shared = FormIds::Shareable(item->GetFormID(), templateOf);
			if (shared && std::ranges::find(looks.worn, *shared) == looks.worn.end()) {
				looks.worn.push_back(*shared);
			}
		}
		std::ranges::sort(looks.worn);  // the same gear says the same looks, whatever order the inventory holds it in
		const auto hands = Gear::HandsOf(player);
		looks.right = hands.right;
		looks.left = hands.left;
		looks.ammo = hands.ammo;
		looks.voice = hands.voice;
		looks.unnamed = hands.unnamed;
		return looks;
	}

	std::optional<Gear::Hands> Hands()
	{
		if (!InWorld()) {
			return std::nullopt;
		}
		return Gear::HandsOf(RE::PlayerCharacter::GetSingleton());
	}

	void NoteDefaultName()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto name = player ? player->GetName() : nullptr;
		defaultName = name ? std::string{ name } : std::string{};
		logs::info("Players: a character not yet named is called \"{}\" by the game; that name is never sent as ours", defaultName);
	}

	std::string CharacterName()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return {};
		}
		const auto name = player->GetName();
		if (!name || (!defaultName.empty() && defaultName == name)) {
			return {};  // no name yet (a new game before the character is made, or one started with coc from the menu)
		}
		return std::string{ name };
	}

	std::string OpenMenus()
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			return "?";
		}
		std::string out;
		for (const auto& [name, entry] : ui->menuMap) {
			if (entry.menu && entry.menu->OnStack()) {
				out += out.empty() ? "" : ",";
				for (const char c : std::string_view{ name.c_str() }) {
					out += c == ' ' ? '_' : c;  // one word in a status line ("Tutorial_Menu")
				}
			}
		}
		return std::format("{} modal={}", out.empty() ? "none" : out, ui->IsModalMenuOpen() ? 1 : 0);
	}

	std::string Describe()
	{
		if (!InWorld()) {
			return "not in world menus=" + OpenMenus();
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto state = Read();
		if (!state) {
			return "in world, no state";
		}
		bool       sprinting = false;
		bool       sneaking = false;
		const bool hasSprint = player->GetGraphVariableBool("IsSprinting", sprinting);
		const bool hasSneak = player->GetGraphVariableBool("IsSneaking", sneaking);
		float      graphSpeed = -1.0f;
		const bool hasSpeed = player->GetGraphVariableFloat("Speed", graphSpeed);
		return std::format("cell={:08X} world={:08X} pos=({:.0f},{:.0f},{:.0f}) heading={:.3f} speed={:.0f} graphSpeed={} dir={} pitch={} flags={:#04x} graphSprint={} graphSneak={} menus={}",
			state->cell, state->worldspace, state->x, state->y, state->z, state->heading, state->speed, hasSpeed ? std::format("{:.0f}", graphSpeed) : "none",
			state->direction, state->pitch, state->flags, hasSprint ? (sprinting ? "1" : "0") : "none", hasSneak ? (sneaking ? "1" : "0") : "none", OpenMenus());
	}
}
