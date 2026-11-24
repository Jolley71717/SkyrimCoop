#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

// The parts of sharing the map (docs/DESIGN.md, "Map markers shared") with no game in them: what a marker gained, what
// a heard marker still lacks here, the messages a list of finds is sent in, and how many "found" notices are shown. The
// game's side is src/game/MapShare.cpp; tests/MapShareTests.cpp.
namespace MapRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// How often our player's markers are read for new finds (a discovery is not in a hurry: a few hundred lookups each).
	inline constexpr auto POLL_EVERY = 2s;
	// Heard markers put on our map each frame at most (each a Papyrus call): a joiner's book of hundreds takes seconds,
	// not one frame.
	inline constexpr std::size_t APPLY_PER_TICK = 8;
	// "Found" notices: at most this many in NOTICE_WINDOW (a friend who discovers a town and its five farms at once is
	// one line or three, not a column).
	inline constexpr std::size_t NOTICE_MOST = 3;
	inline constexpr auto        NOTICE_WINDOW = 5s;

	// The known flags a_now has that a_last had not: 0 when nothing new (a flag lost, as a quest taking a marker off
	// the map, is not news).
	[[nodiscard]] constexpr std::uint8_t Gained(std::uint8_t a_last, std::uint8_t a_now)
	{
		return static_cast<std::uint8_t>(a_now & ~a_last & Protocol::kMarkerAllFlags);
	}

	// Whether a marker with a_have here still lacks one of a_want's flags (to put on our map).
	[[nodiscard]] constexpr bool Lacks(std::uint8_t a_have, std::uint8_t a_want)
	{
		return Gained(a_have, a_want) != 0;
	}

	// a_markers in messages' worth, at most a_most each, in order.
	[[nodiscard]] inline std::vector<std::vector<Protocol::MapMarker>> Batches(const std::vector<Protocol::MapMarker>& a_markers, std::size_t a_most)
	{
		std::vector<std::vector<Protocol::MapMarker>> out;
		for (const auto& marker : a_markers) {
			if (out.empty() || out.back().size() >= a_most) {
				out.emplace_back();
			}
			out.back().push_back(marker);
		}
		return out;
	}

	// Whether a notice may be shown now: at most NOTICE_MOST in any NOTICE_WINDOW.
	class Feed
	{
	public:
		[[nodiscard]] bool Take(Clock::time_point a_now)
		{
			while (!shown.empty() && a_now - shown.front() >= NOTICE_WINDOW) {
				shown.pop_front();
			}
			if (shown.size() >= NOTICE_MOST) {
				return false;
			}
			shown.push_back(a_now);
			return true;
		}

	private:
		std::deque<Clock::time_point> shown;
	};
}
