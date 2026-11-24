#pragma once

#include "Protocol.h"
#include "game/ObjectRules.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

// Bodies of the dead, the rules (docs/DESIGN.md, "Bodies"; the game's side is src/game/Bodies.cpp, the server's
// common/BodyBook.h):
// - Bones are named by a hash of their node's name (NameHash): the skeleton is the race's, the same in every game.
// - The runner's body settles once no bone moved more than MOVE_EPSILON for STILL_FOR (never before MIN_FALL after the
//   death), or GIVE_UP after it (a body on a slope that slides for ever).
// - A copy is laid where the runner's lies only when it is more than CORPSE_SLACK off (two games' ragdolls rarely fall
//   the same; a few units is not worth a twitch), loaded within APPLY_RANGE of where the runner's lies, and dead here
//   for DEAD_FOR (its own fall is over).
// No game here: tests/BodyTests.cpp.
namespace BodyRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;
	using ObjectRules::Vec;

	inline constexpr float MOVE_EPSILON = 2.0f;   // units: a bone that moved less is still
	inline constexpr auto  STILL_FOR = 1s;
	inline constexpr auto  MIN_FALL = 500ms;
	inline constexpr auto  GIVE_UP = 10s;
	inline constexpr float CORPSE_SLACK = 96.0f;
	inline constexpr float APPLY_RANGE = 4096.0f;
	inline constexpr auto  DEAD_FOR = 2s;
	inline constexpr auto  FALL_WAIT = 3s;  // a fall heard before the copy died here waits this long for its death
	inline constexpr std::size_t MAX_WAITING = 512;

	// FNV-1a over the name, lower-cased (a node's name is the same in every game; its case is not worth trusting).
	[[nodiscard]] constexpr std::uint32_t NameHash(std::string_view a_name)
	{
		std::uint32_t hash = 2166136261u;
		for (const char c : a_name) {
			const auto lower = static_cast<unsigned char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
			hash ^= lower;
			hash *= 16777619u;
		}
		return hash == 0 ? 1u : hash;  // 0 is no name
	}

	[[nodiscard]] inline Vec At(const Protocol::Bone& a_bone) { return { a_bone.x, a_bone.y, a_bone.z }; }

	// The bones a body sends: named, finite, at most MAX_BONES, each name once (the first), the pelvis kept when there are
	// too many (a_first: its name, 0 for none).
	[[nodiscard]] inline std::vector<Protocol::Bone> Pick(const std::vector<Protocol::Bone>& a_bones, std::uint32_t a_first = 0)
	{
		std::vector<Protocol::Bone> out;
		const auto usable = [&](const Protocol::Bone& a_bone) {
			return a_bone.name != 0 && Protocol::InWorld(a_bone.x) && Protocol::InWorld(a_bone.y) && Protocol::InWorld(a_bone.z) && Protocol::QuatHolds(a_bone.rot) &&
			       std::none_of(out.begin(), out.end(), [&](const Protocol::Bone& a_out) { return a_out.name == a_bone.name; });
		};
		if (a_first != 0) {
			for (const auto& bone : a_bones) {
				if (bone.name == a_first && usable(bone)) {
					out.push_back(bone);
					break;
				}
			}
		}
		for (const auto& bone : a_bones) {
			if (out.size() >= Protocol::MAX_BONES) {
				break;
			}
			if (usable(bone)) {
				out.push_back(bone);
			}
		}
		return out;
	}

	// The farthest any bone named in both lies from its other (none named in both: none).
	[[nodiscard]] inline std::optional<float> Farthest(const std::vector<Protocol::Bone>& a_here, const std::vector<Protocol::Bone>& a_there)
	{
		std::optional<float> farthest;
		for (const auto& there : a_there) {
			for (const auto& here : a_here) {
				if (here.name == there.name) {
					const auto d = ObjectRules::Distance(At(here), At(there));
					farthest = farthest ? (std::max)(*farthest, d) : d;
					break;
				}
			}
		}
		return farthest;
	}

	// Whether a copy lying a_here is laid as its runner's a_there: more than CORPSE_SLACK off somewhere.
	[[nodiscard]] inline bool NeedsLaying(const std::vector<Protocol::Bone>& a_here, const std::vector<Protocol::Bone>& a_there)
	{
		const auto farthest = Farthest(a_here, a_there);
		return farthest && *farthest > CORPSE_SLACK;
	}

	// The runner's body, from its death to its rest.
	class Settle
	{
	public:
		Settle() = default;
		Settle(const std::vector<Protocol::Bone>& a_bones, Clock::time_point a_now) :
			died(a_now), lastMove(a_now), still(a_bones)
		{}

		// The newest look at its bones: true once it is at rest.
		bool Look(const std::vector<Protocol::Bone>& a_bones, Clock::time_point a_now)
		{
			if (rested) {
				return true;
			}
			const auto farthest = Farthest(a_bones, still);
			if (!farthest || *farthest > MOVE_EPSILON || a_bones.size() != still.size()) {
				still = a_bones;
				lastMove = a_now;
			}
			const bool stillLongEnough = a_now - lastMove >= STILL_FOR && a_now - died >= MIN_FALL;
			rested = stillLongEnough || a_now - died >= GIVE_UP;
			gaveUp = rested && !stillLongEnough;
			return rested;
		}

		[[nodiscard]] const std::vector<Protocol::Bone>& Bones() const { return still; }
		[[nodiscard]] bool                               GaveUp() const { return gaveUp; }

	private:
		Clock::time_point           died{};
		Clock::time_point           lastMove{};
		std::vector<Protocol::Bone> still;
		bool                        rested = false;
		bool                        gaveUp = false;
	};

	// The velocity a fall carries: finite, at most MAX_OBJECT_SPEED.
	[[nodiscard]] inline Vec Capped(const Vec& a_v)
	{
		const auto speed = ObjectRules::Length(a_v);
		if (!std::isfinite(speed)) {
			return {};
		}
		return speed > Protocol::MAX_OBJECT_SPEED ? a_v * (Protocol::MAX_OBJECT_SPEED / speed) : a_v;
	}

	[[nodiscard]] inline Protocol::Body FallOf(const Protocol::NpcKey& a_key, std::uint32_t a_cell, std::uint32_t a_worldspace, const Vec& a_at, const Vec& a_velocity)
	{
		Protocol::Body body;
		body.key = a_key;
		body.kind = Protocol::BodyKind::kFall;
		body.cell = a_cell;
		body.worldspace = a_worldspace;
		body.x = a_at.x;
		body.y = a_at.y;
		body.z = a_at.z;
		const auto v = Capped(a_velocity);
		body.vx = v.x;
		body.vy = v.y;
		body.vz = v.z;
		return body;
	}

	// A rest: none when no bone is left to send (or one lies past MAX_BONE_REACH: a body that does not hold is never said).
	[[nodiscard]] inline std::optional<Protocol::Body> RestOf(const Protocol::NpcKey& a_key, std::uint32_t a_cell, std::uint32_t a_worldspace, const Vec& a_at,
		const std::vector<Protocol::Bone>& a_bones, std::uint32_t a_first = 0)
	{
		Protocol::Body body;
		body.key = a_key;
		body.kind = Protocol::BodyKind::kRest;
		body.cell = a_cell;
		body.worldspace = a_worldspace;
		body.x = a_at.x;
		body.y = a_at.y;
		body.z = a_at.z;
		body.bones = Pick(a_bones, a_first);
		if (!Protocol::BodyHolds(body)) {
			return std::nullopt;
		}
		return body;
	}

	// What a game heard of bodies it has not laid yet: the newest rest of each (at most MAX_WAITING; past it the newest
	// is refused and counted), and each fall until its copy dies here or FALL_WAIT passes.
	class Waiting
	{
	public:
		void Rest(const Protocol::Body& a_body)
		{
			if (rests.size() >= MAX_WAITING && !rests.contains(a_body.key)) {
				++refused;
				return;
			}
			rests[a_body.key] = a_body;
		}
		void Fall(const Protocol::Body& a_body, Clock::time_point a_now) { falls[a_body.key] = { a_body, a_now + FALL_WAIT }; }
		void Forget(const Protocol::NpcKey& a_key)
		{
			rests.erase(a_key);
			falls.erase(a_key);
		}
		void Clear()
		{
			rests.clear();
			falls.clear();
		}

		// Each rest a_lay laid (true) leaves.
		template <class Fn>
		std::size_t TryRests(Fn&& a_lay)
		{
			std::size_t laid = 0;
			for (auto it = rests.begin(); it != rests.end();) {
				if (a_lay(it->second)) {
					it = rests.erase(it);
					++laid;
				} else {
					++it;
				}
			}
			return laid;
		}
		// Each fall a_push pushed (true) leaves, and so does one past FALL_WAIT (counted).
		template <class Fn>
		std::size_t TryFalls(Clock::time_point a_now, Fn&& a_push)
		{
			std::size_t pushed = 0;
			for (auto it = falls.begin(); it != falls.end();) {
				if (a_push(it->second.body)) {
					it = falls.erase(it);
					++pushed;
				} else if (a_now >= it->second.until) {
					it = falls.erase(it);
					++late;
				} else {
					++it;
				}
			}
			return pushed;
		}

		[[nodiscard]] std::size_t Rests() const { return rests.size(); }
		[[nodiscard]] std::size_t Falls() const { return falls.size(); }
		[[nodiscard]] bool        HasRest(const Protocol::NpcKey& a_key) const { return rests.contains(a_key); }

		std::uint64_t refused = 0, late = 0;

	private:
		struct Fallen
		{
			Protocol::Body    body;
			Clock::time_point until{};
		};
		std::map<Protocol::NpcKey, Protocol::Body> rests;
		std::map<Protocol::NpcKey, Fallen>         falls;
	};
}
