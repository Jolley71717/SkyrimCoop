#include "game/Water.h"

#include "DevLine.h"
#include "game/NpcSync.h"
#include "game/RemotePlayers.h"
#include "game/WaterRules.h"

#include <cmath>
#include <limits>
#include <mutex>
#include <numbers>

namespace Water
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::mutex          heardLock;  // the hook's thread and the main thread
		WaterRules::Recent  recent;
		WaterRules::Watch   watch;  // main thread

		std::string Joined(const std::vector<std::string>& a_names)
		{
			std::string out;
			for (const auto& name : a_names) {
				out += (out.empty() ? "" : ",") + name;
			}
			return out.empty() ? "none" : out;
		}

		// The events our player's graph took around a change (none when it never changed).
		std::string HeardAround(const std::optional<Clock::time_point>& a_at)
		{
			if (!a_at) {
				return "none";
			}
			std::scoped_lock l{ heardLock };
			return Joined(recent.Near(*a_at, WaterRules::AROUND));
		}

		std::string Ours()
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->Is3DLoaded() || !player->GetParentCell()) {
				return "ours none";
			}
			const auto  state = player->AsActorState();
			const bool  swimming = state && state->IsSwimming();
			const auto& data = player->GetActorRuntimeData();
			const bool  engineUnder = data.boolFlags.all(RE::Actor::BOOL_FLAGS::kUnderwater);
			const auto  at = player->GetPosition();
			const auto  cell = player->GetParentCell();
			const float submerged = player->GetSubmergedLevel(at.z, cell);
			const auto  water = HeightAt(at, cell);
			std::string controller = "none";
			if (const auto charController = player->GetCharController()) {
				controller = std::format("{} surface={} shoulders={} controllerWater={:.0f}", std::to_underlying(charController->context.currentState),
					charController->flags.all(RE::CHARACTER_FLAGS::kSwimAtWaterSurface) ? 1 : 0, charController->flags.all(RE::CHARACTER_FLAGS::kShouldersUnderWater) ? 1 : 0,
					charController->waterHeight);
			}
			const auto body = BodyRise(player);
			return std::format("ours swimming={} engineSwimming={} inWater={} under={} engineUnder={} submerged={:.2f} charState={} z={:.0f} water={} depth={} pitch={:.0f} body={}"
							   " entered={} left={} dived={} surfaced={} heardIn={} heardOut={} heardDive={} heardSurface={}",
				swimming ? 1 : 0, data.boolBits.all(RE::Actor::BOOL_BITS::kSwimming) ? 1 : 0, data.boolBits.all(RE::Actor::BOOL_BITS::kInWater) ? 1 : 0,
				Under(player, swimming) ? 1 : 0, engineUnder ? 1 : 0, std::isfinite(submerged) ? submerged : -1.0f, controller, at.z,
				water ? std::format("{:.0f}", *water) : "none", water ? std::format("{:.0f}", *water - at.z) : "none",
				-player->GetAngleX() * 180.0f / std::numbers::pi_v<float>, body ? std::format("{:.0f}", *body) : "none", watch.entered, watch.left, watch.dived, watch.surfaced, HeardAround(watch.enteredAt),
				HeardAround(watch.leftAt), HeardAround(watch.divedAt), HeardAround(watch.surfacedAt));
		}

		std::string Find(const std::vector<std::string_view>& a_words)
		{
			float radius = 4096.0f;
			if (a_words.size() > 2) {
				return "error: usage: swim find [radius]";
			}
			if (a_words.size() == 2) {
				const auto number = DevLine::Number(a_words[1]);
				if (!number || *number == 0 || *number > static_cast<std::uint64_t>(WaterRules::MAX_RADIUS)) {
					return std::format("error: a radius is 1 to {:.0f} units", WaterRules::MAX_RADIUS);
				}
				radius = static_cast<float>(*number);
			}
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto tes = RE::TES::GetSingleton();
			const auto here = player ? player->GetParentCell() : nullptr;
			if (!player || !tes || !here) {
				return "error: our player is not in the world";
			}
			if (here->IsInteriorCell()) {
				return "error: our player is inside: the search reads the land, which only the outside has";
			}
			const auto at = player->GetPosition();
			const auto depthAt = [&](float a_x, float a_y) -> std::optional<float> {
				RE::NiPoint3 point{ a_x, a_y, at.z };
				float        land = 0.0f;
				if (!tes->GetLandHeight(point, land) || !std::isfinite(land)) {
					return std::nullopt;
				}
				point.z = land + 1.0f;
				const auto water = HeightAt(point, tes->GetCell(point));
				return water ? std::optional{ *water - land } : std::nullopt;
			};
			std::size_t looked = 0;
			const auto  spot = WaterRules::FindSpot(at.x, at.y, radius, depthAt, &looked);
			if (!spot) {
				return std::format("none radius={:.0f} looked={} from=({:.0f},{:.0f})", radius, looked, at.x, at.y);
			}
			return std::format("spot=({:.0f},{:.0f}) depth={:.0f} dist={:.0f} radius={:.0f} looked={} from=({:.0f},{:.0f})", spot->x, spot->y, spot->depth,
				spot->distance, radius, looked, at.x, at.y);
		}
	}

	std::optional<float> BodyRise(RE::Actor* a_actor)
	{
		const auto root = a_actor ? a_actor->Get3D(false) : nullptr;
		if (!root) {
			return std::nullopt;
		}
		static const RE::BSFixedString pelvisName{ "NPC Pelvis [Pelv]" };
		static const RE::BSFixedString headName{ "NPC Head [Head]" };
		const auto pelvis = root->GetObjectByName(pelvisName);
		const auto head = root->GetObjectByName(headName);
		if (!pelvis || !head) {
			return std::nullopt;
		}
		const auto& p = pelvis->world.translate;
		const auto& h = head->world.translate;
		return WaterRules::BodyRise(p.x, p.y, p.z, h.x, h.y, h.z);
	}

	std::optional<float> HeightAt(const RE::NiPoint3& a_at, RE::TESObjectCELL* a_cell)
	{
		const auto tes = RE::TES::GetSingleton();
		if (!tes || !a_cell) {
			return std::nullopt;
		}
		const float height = tes->GetWaterHeight(a_at, a_cell);
		// Heights in Skyrim's worlds lie well within this; the engine's "no water" is far outside it.
		if (!std::isfinite(height) || std::abs(height) > 1.0e6f) {
			return std::nullopt;
		}
		return height;
	}

	bool Under(RE::Actor* a_actor, bool a_swimming)
	{
		if (!a_actor || !a_swimming) {
			return false;
		}
		const bool engine = a_actor->GetActorRuntimeData().boolFlags.all(RE::Actor::BOOL_FLAGS::kUnderwater);
		float      submerged = std::numeric_limits<float>::quiet_NaN();
		if (!engine) {
			if (const auto cell = a_actor->GetParentCell()) {
				submerged = a_actor->GetSubmergedLevel(a_actor->GetPositionZ(), cell);
			}
		}
		return WaterRules::Under(a_swimming, engine, submerged);
	}

	void Sample()
	{
		const auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded() || !player->GetParentCell()) {
			watch.Forget();
			return;
		}
		const auto state = player->AsActorState();
		const bool swimming = state && state->IsSwimming();
		watch.Feed(swimming, Under(player, swimming), Clock::now());
	}

	void Heard(std::string_view a_event, Clock::time_point a_at)
	{
		std::scoped_lock l{ heardLock };
		recent.Push(std::string{ a_event }, a_at);
	}

	std::string Command(std::string_view a_args)
	{
		const auto words = DevLine::Words(a_args);
		if (!words.empty() && words[0] == "find") {
			return Find(words);
		}
		if (!words.empty()) {
			return "error: usage: swim [find [radius]]";
		}
		return Ours() + RemotePlayers::Swim() + NpcSync::Swim();
	}
}
