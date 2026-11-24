#include "game/Party.h"

#include "game/LocalPlayer.h"
#include "game/MarkerRules.h"
#include "Text.h"

#include <cmath>
#include <format>
#include <map>

namespace Party
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto READ_EVERY = std::chrono::milliseconds{ 100 };

		std::map<std::uint32_t, Protocol::PlayerStatus> theirs;
		std::map<std::uint32_t, std::uint64_t>          heard;  // statuses that came, by player

		MarkerRules::WayIn                    wayIn;
		std::optional<Protocol::PlayerStatus> sent;  // the last we sent this session
		Clock::time_point                     sentAt{};
		Clock::time_point                     nextRead{};
		std::uint64_t                         sentCount = 0;
		std::uint64_t                         travels = 0;     // times we were seen starting to travel
		std::uint64_t                         doorReads = 0;   // interiors searched for a door out (nothing kept: after a load)
		// The door out of the interior last searched, kept while we are in it (searched once, not at every read).
		std::uint32_t                                               doorCell = 0;
		std::optional<std::pair<std::uint32_t, MarkerRules::Point>> doorOut;
		float                                                       doorZ = 0.0f;
		bool                                  wasTravelling = false;

		// A load door or a fast travel the player's game has queued: its target is somewhere other than where the player
		// stands. The queue is the engine's (PlayerCharacter's queued target); one it leaves set after the move lands on
		// the player's own spot, so it is not read as a travel there.
		bool Queued(RE::PlayerCharacter* a_player)
		{
			const auto& target = a_player->GetPlayerRuntimeData().queuedTargetLoc;
			if (!target.isValid) {
				return false;
			}
			const auto cell = a_player->GetParentCell();
			const bool inside = cell && cell->IsInteriorCell();
			if (target.interior ? target.interior != cell : (inside || target.world != a_player->GetWorldspace())) {
				return true;
			}
			const auto at = a_player->GetPosition();
			return std::hypot(target.location.x - at.x, target.location.y - at.y, target.location.z - at.z) > 512.0f;
		}

		// The door out of an interior: a load door of a_cell whose other side is outside (its worldspace and where it
		// puts the player). Nested interiors (a cave's deeper level) have none: nothing then.
		std::optional<std::pair<std::uint32_t, MarkerRules::Point>> DoorOut(const RE::TESObjectCELL* a_cell, float& a_z)
		{
			std::optional<std::pair<std::uint32_t, MarkerRules::Point>> out;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				const auto base = a_ref ? a_ref->GetBaseObject() : nullptr;
				if (!base || !base->Is(RE::FormType::Door)) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const auto teleport = a_ref->extraList.GetByType<RE::ExtraTeleport>();
				const auto data = teleport ? teleport->teleportData : nullptr;
				const auto other = data ? data->linkedDoor.get() : RE::NiPointer<RE::TESObjectREFR>{};
				const auto world = other ? other->GetWorldspace() : nullptr;
				if (!world) {
					return RE::BSContainer::ForEachResult::kContinue;
				}
				out = std::pair{ world->GetFormID(), MarkerRules::Point{ data->position.x, data->position.y } };
				a_z = data->position.z;
				return RE::BSContainer::ForEachResult::kStop;
			});
			return out;
		}

		std::optional<Protocol::PlayerStatus> Read()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player) {
				return std::nullopt;
			}
			Protocol::PlayerStatus status;
			const bool inWorld = LocalPlayer::InWorld();
			if (!inWorld || Queued(player)) {
				status.flags |= Protocol::kStatusTravelling;
			}
			if (const auto owner = player->AsActorValueOwner()) {
				const float whole = owner->GetPermanentActorValue(RE::ActorValue::kHealth) +
				                    player->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth);
				status.health = MarkerRules::HealthPercent(owner->GetActorValue(RE::ActorValue::kHealth), whole);
			}
			const auto cell = player->GetParentCell();
			if (!cell) {
				// Between worlds (a loading screen): the place and the way in as last said, so a friend's markers stay where
				// they were until we arrive.
				if (sent) {
					status.location = sent->location;
					status.flags |= sent->flags & Protocol::kStatusInside;
					status.mapWorld = sent->mapWorld;
					status.mapX = sent->mapX;
					status.mapY = sent->mapY;
					status.mapZ = sent->mapZ;
				}
				return status;
			}
			if (const auto location = player->GetCurrentLocation()) {
				status.location = location->GetFormID();
			}
			if (cell->IsInteriorCell()) {
				if (!wayIn.Known() && doorCell != cell->GetFormID()) {
					doorCell = cell->GetFormID();
					doorOut = DoorOut(cell, doorZ);
					++doorReads;
				}
				wayIn.Fill(status, wayIn.Known() ? std::nullopt : doorOut, doorZ);
			} else if (const auto world = player->GetWorldspace(); world && inWorld) {
				const auto at = player->GetPosition();
				wayIn.Outside(world->GetFormID(), at.x, at.y, at.z);
			}
			return status;
		}
	}

	void Started()
	{
		sent.reset();
		sentAt = {};
		nextRead = {};
	}

	void Frame(const SendFn& a_send)
	{
		const auto now = Clock::now();
		if (now < nextRead) {
			return;
		}
		nextRead = now + READ_EVERY;
		const auto status = Read();
		if (!status) {
			return;
		}
		const bool travelling = (status->flags & Protocol::kStatusTravelling) != 0;
		if (travelling && !wasTravelling) {
			++travels;
		}
		wasTravelling = travelling;
		if (!MarkerRules::Worth(sent, *status, now - sentAt)) {
			return;
		}
		a_send(Protocol::Encode(*status), true);
		if (!sent || ((sent->flags ^ status->flags) & (Protocol::kStatusTravelling | Protocol::kStatusInside)) != 0) {
			logs::info("Party: our status: {}{}health {}%", travelling ? "travelling, " : "", (status->flags & Protocol::kStatusInside) ? "inside, " : "", status->health);
		}
		sent = status;
		sentAt = now;
		++sentCount;
	}

	void Heard(const Protocol::RemoteStatus& a_msg)
	{
		const auto it = theirs.find(a_msg.playerId);
		const bool travel = it == theirs.end() || ((it->second.flags ^ a_msg.status.flags) & Protocol::kStatusTravelling) != 0;
		theirs[a_msg.playerId] = a_msg.status;
		++heard[a_msg.playerId];
		if (travel && (a_msg.status.flags & Protocol::kStatusTravelling) != 0) {
			logs::info("Party: player {} is travelling (a door, a fast travel or a loading screen)", a_msg.playerId);
		}
	}

	void Left(std::uint32_t a_playerId)
	{
		theirs.erase(a_playerId);
		heard.erase(a_playerId);
	}

	void Clear()
	{
		theirs.clear();
		heard.clear();
		sent.reset();
	}

	void ForgetWorld()
	{
		wayIn.Forget();
		doorCell = 0;
		doorOut.reset();
	}

	std::optional<Protocol::PlayerStatus> StatusOf(std::uint32_t a_playerId)
	{
		const auto it = theirs.find(a_playerId);
		return it == theirs.end() ? std::nullopt : std::optional{ it->second };
	}

	std::string Tag(std::uint32_t a_playerId, std::string_view a_name)
	{
		const auto status = StatusOf(a_playerId);
		const auto health = status ? MarkerRules::HealthShown(status->health) : std::nullopt;
		if (!health || a_name.empty()) {
			return std::string{ a_name };
		}
		return Text::Format("party.tag_hurt", { { "name", a_name }, { "health", *health } });
	}

	std::optional<Protocol::PlayerStatus> Ours()
	{
		return Read();
	}

	bool Travelling()
	{
		const auto status = Read();
		return status && (status->flags & Protocol::kStatusTravelling) != 0;
	}

	std::string Command(std::string_view)
	{
		const auto describe = [](const Protocol::PlayerStatus& a_s) {
			return std::format("health={} travelling={} inside={} location={:08X} mapWorld={:08X} map=({:.0f},{:.0f},{:.0f})", a_s.health,
				(a_s.flags & Protocol::kStatusTravelling) ? 1 : 0, (a_s.flags & Protocol::kStatusInside) ? 1 : 0, a_s.location, a_s.mapWorld, a_s.mapX, a_s.mapY, a_s.mapZ);
		};
		const auto ours = Read();
		std::string out = std::format("ours {} sent={} travels={} doorReads={} wayInKept={}", ours ? describe(*ours) : std::string{ "none" }, sentCount, travels,
			doorReads, wayIn.Known() ? 1 : 0);
		for (const auto& [id, status] : theirs) {
			out += std::format(" | player {} {} heard={}", id, describe(status), heard[id]);
		}
		return out;
	}
}
