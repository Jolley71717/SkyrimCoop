#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Players and NPCs in water (docs/DESIGN.md, "Players": "In water"): when a swimmer counts as under water, where the
// rig finds water deep enough to swim in, and what our own player's graph was told as it went into or out of the water
// (the names the rig reads to learn which events the game's own swimming sends). The game's side is src/game/Water.cpp,
// Gait.h (what a stand-in or copy is shown doing) and RemotePlayers/NpcSync. No game here: tests/WaterTests.cpp.
namespace WaterRules
{
	using Clock = std::chrono::steady_clock;

	// ---- under water ----

	// The share of an actor's height under water (Actor::GetSubmergedLevel, 0 to 1) from which its head is under, when
	// the engine's own flag for it (Actor::BOOL_FLAGS::kUnderwater) does not say so.
	inline constexpr float SUBMERGED_UNDER = 0.9f;

	// A swimmer is under water when the engine says so, or when this much of it is.
	[[nodiscard]] inline bool Under(bool a_swimming, bool a_engineUnderwater, float a_submerged)
	{
		return a_swimming && (a_engineUnderwater || (std::isfinite(a_submerged) && a_submerged >= SUBMERGED_UNDER));
	}

	// ---- water to swim in (the rig's `swim find`) ----

	// Water this many units deeper than the land under it is deep enough to swim in (an actor is about 128 tall and
	// swims once most of it is under).
	inline constexpr float SWIM_DEPTH = 160.0f;
	// The grid the land is read on, and the farthest a search looks.
	inline constexpr float GRID = 128.0f;
	inline constexpr float MAX_RADIUS = 8192.0f;

	struct Spot
	{
		float       x = 0.0f, y = 0.0f;
		float       depth = 0.0f;     // water above the land there
		float       distance = 0.0f;  // from where the search began
		std::size_t looked = 0;       // points read
	};

	// The depth of water at a point (its height less the land's), none where it can't be read (no land loaded there,
	// or no water).
	using DepthAt = std::function<std::optional<float>(float a_x, float a_y)>;

	// The nearest point within a_radius of (a_x, a_y), on a grid of GRID, where the water is at least SWIM_DEPTH deep;
	// none when there is no such point. The answer carries how many points were read, found or not.
	[[nodiscard]] inline std::optional<Spot> FindSpot(float a_x, float a_y, float a_radius, const DepthAt& a_depthAt, std::size_t* a_looked = nullptr)
	{
		const float radius = std::clamp(std::isfinite(a_radius) ? a_radius : 0.0f, 0.0f, MAX_RADIUS);
		const int   steps = static_cast<int>(radius / GRID);
		std::optional<Spot> best;
		std::size_t         looked = 0;
		for (int i = -steps; i <= steps; ++i) {
			for (int j = -steps; j <= steps; ++j) {
				const float dx = static_cast<float>(i) * GRID;
				const float dy = static_cast<float>(j) * GRID;
				const float distance = std::hypot(dx, dy);
				if (distance > radius || (best && distance >= best->distance)) {
					continue;
				}
				++looked;
				const auto depth = a_depthAt(a_x + dx, a_y + dy);
				if (depth && std::isfinite(*depth) && *depth >= SWIM_DEPTH) {
					best = Spot{ .x = a_x + dx, .y = a_y + dy, .depth = *depth, .distance = distance };
				}
			}
		}
		if (a_looked) {
			*a_looked = looked;
		}
		if (best) {
			best->looked = looked;
		}
		return best;
	}

	// ---- what our player's graph was told around the water (the rig's probe) ----

	// The last events our player's graph took, each with when (PlayerActions' hook, whether in a session or not).
	class Recent
	{
	public:
		static constexpr std::size_t MAX = 64;

		void Push(std::string a_event, Clock::time_point a_at)
		{
			events.push_back({ std::move(a_event), a_at });
			while (events.size() > MAX) {
				events.pop_front();
			}
		}

		// The names taken within a_window of a_at, each once, in the order first heard.
		[[nodiscard]] std::vector<std::string> Near(Clock::time_point a_at, Clock::duration a_window) const
		{
			std::vector<std::string> out;
			for (const auto& entry : events) {
				const auto apart = entry.at > a_at ? entry.at - a_at : a_at - entry.at;
				if (apart <= a_window && std::ranges::find(out, entry.event) == out.end()) {
					out.push_back(entry.event);
				}
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return events.size(); }

	private:
		struct Entry
		{
			std::string       event;
			Clock::time_point at;
		};
		std::deque<Entry> events;
	};

	// How long either side of a change into or out of the water the events heard are said (dev `swim`).
	inline constexpr auto AROUND = std::chrono::milliseconds(1500);

	// Our player's swimming and being under water, read each tick: how often each changed, and when it last went each
	// way. The first read after Forget (or ever) is where it is, not a change.
	class Watch
	{
	public:
		// Returns true when either changed.
		bool Feed(bool a_swimming, bool a_under, Clock::time_point a_now)
		{
			bool changed = false;
			if (known && a_swimming != swimming) {
				++(a_swimming ? entered : left);
				(a_swimming ? enteredAt : leftAt) = a_now;
				changed = true;
			}
			if (known && a_under != under) {
				++(a_under ? dived : surfaced);
				(a_under ? divedAt : surfacedAt) = a_now;
				changed = true;
			}
			swimming = a_swimming;
			under = a_under;
			known = true;
			return changed;
		}

		// Out of the world (a load, the main menu): what comes next is not a change.
		void Forget() { known = false; }

		bool                             known = false;
		bool                             swimming = false;
		bool                             under = false;
		std::uint64_t                    entered = 0, left = 0, dived = 0, surfaced = 0;
		std::optional<Clock::time_point> enteredAt, leftAt, divedAt, surfacedAt;
	};
}
