#include "game/TravelTogether.h"

#include "Config.h"
#include "Text.h"
#include "game/LocalPlayer.h"
#include "game/MapShare.h"
#include "game/RemotePlayers.h"
#include "game/TravelRules.h"

#include <atomic>
#include <format>
#include <map>

namespace TravelTogether
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::atomic<bool>                                  pressed{ false };  // the follow key, since the last frame
		bool                                               installed = false;
		bool                                               wasQueued = false;
		TravelRules::Offers                                offers;
		std::uint64_t                                      tripsSent = 0;
		std::uint64_t                                      tripsHeard = 0;
		std::uint64_t                                      offersMade = 0;
		std::uint64_t                                      follows = 0;
		std::uint64_t                                      controlsOff = 0;  // follows the game's fast travel controls refused
		std::uint64_t                                      placeOff = 0;     // offers not made: fast travel is off where the trip began
		std::map<TravelRules::Refusal, std::uint64_t>      refused;
		std::string                                        lastTrip = "none";  // ours, for the dev channel

		void Show(const std::string& a_text)
		{
			RE::SendHUDMessage::ShowHUDMessage(a_text.c_str());
		}

		std::string NameOf(std::uint32_t a_playerId)
		{
			for (const auto& friendOf : RemotePlayers::Friends()) {
				if (friendOf.id == a_playerId) {
					return friendOf.name;
				}
			}
			return {};
		}

		class KeySink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				const auto key = Config::Get().followKey;
				for (auto event = a_event ? *a_event : nullptr; key != 0 && event; event = event->next) {
					if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
						continue;
					}
					const auto button = event->AsButtonEvent();
					if (button && button->GetIDCode() == key && button->IsDown()) {
						pressed = true;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// The player's queued move as the engine holds it: set, into another space, how far (Began adds the rest).
		TravelRules::Queued ReadQueue(RE::PlayerCharacter* a_player)
		{
			const auto&         target = a_player->GetPlayerRuntimeData().queuedTargetLoc;
			TravelRules::Queued queued;
			queued.valid = target.isValid;
			if (!queued.valid) {
				return queued;
			}
			const auto cell = a_player->GetParentCell();
			const bool inside = cell && cell->IsInteriorCell();
			queued.otherSpace = target.interior ? target.interior != cell : (inside || target.world != a_player->GetWorldspace());
			queued.distance = a_player->GetPosition().GetDistance(target.location);
			return queued;
		}

		// Whether the game's data turns fast travel off at a place: an interior's cell (the DATA bit the Creation Kit
		// calls "can't travel from here", CommonLibSSE-NG's kCanTravelFromHere), outside its worldspace's "can't fast
		// travel" (Sovngarde, the Soul Cairn). Game.FastTravel from a script does not ask, so we do.
		bool PlaceOff(const RE::TESObjectCELL* a_cell, const RE::TESWorldSpace* a_world)
		{
			const bool interior = a_cell && a_cell->IsInteriorCell();
			return TravelRules::TravelOff(interior, a_cell && a_cell->cellFlags.any(RE::TESObjectCELL::Flag::kCanTravelFromHere),
				a_world && a_world->flags.any(RE::TESWorldSpace::Flag::kCantFastTravel));
		}

		bool PlaceOff(RE::PlayerCharacter* a_player)
		{
			const auto cell = a_player->GetParentCell();
			return PlaceOff(cell, cell && cell->IsInteriorCell() ? nullptr : a_player->GetWorldspace());
		}

		// Where a trip began, as this game has it (FormTranslate gave both or neither).
		bool PlaceOff(const Protocol::Trip& a_trip)
		{
			const auto cell = a_trip.fromCell ? RE::TESForm::LookupByID<RE::TESObjectCELL>(a_trip.fromCell) : nullptr;
			const auto world = a_trip.fromWorld ? RE::TESForm::LookupByID<RE::TESWorldSpace>(a_trip.fromWorld) : nullptr;
			if (a_trip.fromWorld == 0) {
				return cell && PlaceOff(cell, nullptr);
			}
			return world && PlaceOff(nullptr, world);
		}

		// A load door near the player that goes where the queued move does: a door's move, not a trip.
		bool DoorGoes(RE::PlayerCharacter* a_player, const RE::NiPoint3& a_to)
		{
			const auto tes = RE::TES::GetSingleton();
			if (!tes) {
				return false;
			}
			bool door = false;
			tes->ForEachReferenceInRange(a_player, 1000.0f, [&](RE::TESObjectREFR* a_ref) {
				const auto teleport = a_ref ? a_ref->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
				const auto data = teleport ? teleport->teleportData : nullptr;
				if (data && data->position.GetDistance(a_to) < 512.0f) {
					door = true;
					return RE::BSContainer::ForEachResult::kStop;
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
			return door;
		}

		// The map marker nearest a spot outside, within MARKER_NEAR: where a carriage (moved by script) takes the player.
		std::uint32_t NearestPlace(const RE::TESWorldSpace* a_world, const RE::NiPoint3& a_at)
		{
			std::uint32_t best = 0;
			float         bestDistance = TravelRules::MARKER_NEAR;
			for (const auto id : MapShare::Places()) {
				const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				if (!ref || ref->GetWorldspace() != a_world) {
					continue;
				}
				if (const auto distance = ref->GetPosition().GetDistance(a_at); distance < bestDistance) {
					best = id;
					bestDistance = distance;
				}
			}
			return best;
		}

		// A queued move first seen: a trip of ours is said to the session with where we leave from.
		void Began(RE::PlayerCharacter* a_player, TravelRules::Queued a_queued, const SendFn& a_send)
		{
			const auto& target = a_player->GetPlayerRuntimeData().queuedTargetLoc;
			const auto  cell = a_player->GetParentCell();
			const bool  inside = cell && cell->IsInteriorCell();
			const auto  world = a_player->GetWorldspace();
			const auto  at = a_player->GetPosition();
			const auto  marker = RE::TESObjectREFR::LookupByHandle(target.fastTravelMarker);

			auto& queued = a_queued;
			queued.marker = static_cast<bool>(marker);
			queued.door = !queued.marker && DoorGoes(a_player, target.location);

			std::uint32_t destination = 0;
			switch (TravelRules::TripOf(queued)) {
			case TravelRules::Start::kNone:
				return;
			case TravelRules::Start::kMarker:
				destination = marker->GetFormID();
				break;
			case TravelRules::Start::kNearest:
				destination = target.interior ? 0 : NearestPlace(target.world, target.location);
				break;
			}
			if (destination == 0 || destination >= 0xFF000000) {
				lastTrip = std::format("to nowhere another game has ({:08X})", destination);
				logs::info("Travel: our player is travelling, but to no marker another game has ({:08X}): not said", destination);
				return;
			}
			Protocol::Trip trip;
			trip.destination = destination;
			trip.fromCell = cell ? cell->GetFormID() : 0;
			trip.fromWorld = inside || !world ? 0 : world->GetFormID();
			trip.x = at.x;
			trip.y = at.y;
			trip.z = at.z;
			++tripsSent;
			lastTrip = std::format("{:08X} ({}) from {:08X}/{:08X}", destination, queued.marker ? "marker" : "nearest", trip.fromCell, trip.fromWorld);
			logs::info("Travel: our player is travelling to {:08X} ({}): said to the others", destination, queued.marker ? "its marker" : "the map marker nearest");
			a_send(Protocol::Encode(trip), true);
		}

		// The game's own fast travel to the offer's marker, once the game says fast travel is not taken by a scene.
		void Go(std::uint32_t a_destination)
		{
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			const auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_destination);
			if (!vm || !ref) {
				logs::warn("Travel: can't follow to {:08X}: {}", a_destination, ref ? "no Papyrus" : "no such reference here");
				return;
			}
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			if (vm->DispatchStaticCall("Game", "FastTravel", RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(ref)), none)) {
				++follows;
				logs::info("Travel: following to {:08X}", a_destination);
			} else {
				logs::warn("Travel: Game.FastTravel to {:08X} could not be called", a_destination);
			}
		}

		// Game.IsFastTravelControlsEnabled's answer (a script or scene can take fast travel away), on the Papyrus thread:
		// the travel itself goes from the main thread.
		class AfterControls final : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit AfterControls(std::uint32_t a_destination) :
				destination(a_destination) {}

			void operator()(RE::BSScript::Variable a_result) override
			{
				const bool enabled = a_result.IsBool() && a_result.GetBool();
				const auto to = destination;
				SKSE::GetTaskInterface()->AddTask([enabled, to] {
					if (!enabled) {
						++controlsOff;
						Show(Text::Get("travel.refused_now"));
						logs::info("Travel: not following: the game has fast travel turned off now");
						return;
					}
					Go(to);
				});
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

		private:
			std::uint32_t destination;
		};

		void Follow(RE::PlayerCharacter* a_player)
		{
			const auto now = Clock::now();
			const auto offer = offers.Live(now);
			const auto ui = RE::UI::GetSingleton();
			const auto controls = RE::ControlMap::GetSingleton();

			TravelRules::Now state;
			state.offer = offer.has_value();
			state.travelling = TravelRules::Active(ReadQueue(a_player));
			state.placeOff = PlaceOff(a_player) || (offer && PlaceOff(*offer));
			state.busy = (ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME))) || (controls && !controls->IsMovementControlsEnabled());
			state.combat = a_player->IsInCombat();
			state.overWeight = a_player->IsOverEncumbered();
			const auto refusal = TravelRules::Refuse(state);
			if (refusal != TravelRules::Refusal::kNone) {
				++refused[refusal];
				logs::info("Travel: not following ({})", TravelRules::RefusalName(refusal));
				if (const auto key = TravelRules::RefusalText(refusal); !key.empty()) {
					Show(Text::Get(key));
				}
				return;
			}
			offers.End();
			const auto vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> then{ new AfterControls(offer->destination) };
			if (!vm || !vm->DispatchStaticCall("Game", "IsFastTravelControlsEnabled", RE::MakeFunctionArguments(), then)) {
				logs::warn("Travel: Game.IsFastTravelControlsEnabled could not be called: following without it");
				Go(offer->destination);
			}
		}
	}

	void Install()
	{
		const auto input = RE::BSInputDeviceManager::GetSingleton();
		if (!input) {
			logs::warn("Travel: no input manager: the follow key does nothing (dev `travel follow` still works)");
			return;
		}
		static KeySink sink;
		input->AddEventSink(&sink);
		installed = true;
		logs::info("Travel: the follow key is {}", Config::Get().followKey == 0 ? std::string{ "off" } : TravelRules::KeyName(Config::Get().followKey));
	}

	void Frame(const SendFn& a_send)
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !LocalPlayer::InWorld()) {
			pressed = false;  // a key pressed on a loading screen is no follow
			return;
		}
		// The start of a move under way, not of a queue the engine left set after it landed (TravelRules::Active).
		const auto queue = ReadQueue(player);
		const bool queued = TravelRules::Active(queue);
		if (queued && !wasQueued) {
			Began(player, queue, a_send);
		}
		wasQueued = queued;
		if (pressed.exchange(false)) {
			Follow(player);
		}
	}

	void Heard(const Protocol::Trip& a_trip)
	{
		++tripsHeard;
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto cell = player ? player->GetParentCell() : nullptr;
		if (!cell || !LocalPlayer::InWorld()) {
			return;
		}
		const bool inside = cell->IsInteriorCell();
		const auto world = player->GetWorldspace();
		const auto at = player->GetPosition();
		if (!TravelRules::Near(a_trip, cell->GetFormID(), inside || !world ? 0 : world->GetFormID(), at.x, at.y, at.z)) {
			logs::info("Travel: player {} is travelling to {:08X}; we were not near", a_trip.traveller, a_trip.destination);
			return;
		}
		if (PlaceOff(a_trip) || PlaceOff(player)) {
			++placeOff;
			logs::info("Travel: player {} is travelling to {:08X} from a place where fast travel is off: no offer", a_trip.traveller, a_trip.destination);
			return;
		}
		offers.Take(a_trip, Clock::now());
		++offersMade;
		const auto who = NameOf(a_trip.traveller);
		const auto place = MapShare::PlaceName(a_trip.destination);
		const auto key = Config::Get().followKey;
		logs::info("Travel: player {} is travelling to {:08X} ({}): we may follow", a_trip.traveller, a_trip.destination, place.empty() ? "no name" : "named");
		if (key == 0 || who.empty()) {
			return;
		}
		Show(place.empty() ? Text::Format("travel.offer_somewhere", { { "player", who }, { "key", TravelRules::KeyName(key) } }) :
		                     Text::Format("travel.offer", { { "player", who }, { "place", place }, { "key", TravelRules::KeyName(key) } }));
	}

	void Clear()
	{
		offers.End();
		pressed = false;
	}

	std::string Command(std::string_view a_args)
	{
		if (a_args == "follow") {
			pressed = true;
			return "follow pressed (taken next frame)";
		}
		const auto offer = offers.Live(Clock::now());
		std::string refusals;
		for (const auto& [why, count] : refused) {
			refusals += std::format(" {}={}", TravelRules::RefusalName(why), count);
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto here = player ? (PlaceOff(player) ? "off" : "on") : "?";
		return std::format("key={} installed={} sent={} last={} heard={} offers={} offer={} follows={} controlsOff={} placeOff={} here={} refused:{}",
			TravelRules::KeyName(Config::Get().followKey), installed ? 1 : 0, tripsSent, lastTrip, tripsHeard, offersMade,
			offer ? std::format("{:08X} from player {}", offer->destination, offer->traveller) : std::string{ "none" }, follows, controlsOff, placeOff, here,
			refusals.empty() ? std::string{ " none" } : refusals);
	}
}
