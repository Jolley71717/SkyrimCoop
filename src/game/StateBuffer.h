#pragma once

#include "Protocol.h"
#include "game/Prediction.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <numbers>
#include <optional>

// A friend's states as they come, and where their stand-in is drawn between them. No game here, so it is unit-tested.
//
// Cut down from F4Multiplayer's src/game/StateBuffer.h (commit a7bddd24b). Kept: each state stamped with when its
// sender sampled it (Prediction::SenderClock), the stand-in drawn a fixed time in the past so there are two states to
// blend between, no blend across a cell or worldspace change, a short carry-on past the newest state when the next is
// late, then standing. Left for later, each with the problem it answered there: the delay that follows the link's
// lateness (a relay route that hiccups), easing out a correction instead of jumping, and the path handed to the
// engine's update between our frames.
namespace StateBuffer
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// How far in the past the stand-in is drawn: a state every 50 ms (a game is to send 20 a second) and a relay trip's
	// jitter fit in it. A developer command may change it to measure.
	constexpr auto             DEFAULT_DELAY = 120ms;
	inline std::atomic<int>    delayMs{ static_cast<int>(DEFAULT_DELAY.count()) };
	// Past the newest state, a friend who was moving goes on the way they went for this long, rather than stopping
	// until the next comes and jumping on.
	constexpr auto             EXTRAPOLATE = 100ms;
	// Faster than this between two states is a jump (a door, the console), not a run: not carried on. Units a second;
	// to measure in a game against Skyrim's fastest movement (a sprint, a mounted gallop).
	constexpr float            EXTRAPOLATE_SPEED = 1500.0f;
	// Slower than this in the newest state: it says they stand, so they are not carried on.
	constexpr float            EXTRAPOLATE_FROM = 10.0f;
	// No state for this long: nothing to draw (the friend is on a loading screen, or gone).
	constexpr auto             STALE_AFTER = 5s;
	constexpr std::size_t      MAX_SNAPSHOTS = 32;

	// The short way round from one heading to another, radians, in (-pi, pi].
	inline float TurnBetween(float a_from, float a_to)
	{
		constexpr float turn = 2.0f * std::numbers::pi_v<float>;
		float           delta = std::fmod(a_to - a_from, turn);
		if (delta > turn * 0.5f) {
			delta -= turn;
		} else if (delta <= -turn * 0.5f) {
			delta += turn;
		}
		return delta;
	}

	inline float LerpAngle(float a_from, float a_to, float a_t)
	{
		return a_from + TurnBetween(a_from, a_to) * a_t;
	}

	struct Snapshot
	{
		Clock::time_point     stamp;  // when it was sampled, on our clock
		Protocol::PlayerState state;
	};

	struct Sampled
	{
		Protocol::PlayerState state;  // where it is drawn, its heading blended; speed, direction and flags of the state it is going to
		bool                  ahead = false;  // past the newest state, going on the way they went
		bool                  held = false;   // past that too: it stands where the carry-on ended
	};

	struct Buffer
	{
		std::deque<Snapshot>     snapshots;
		Prediction::SenderClock  sender;
		Prediction::ArrivalClock arrivals;  // for states that don't say when they were sent
		// States turned away since one was last taken: a sender whose count started over (a restarted game) shows here.
		std::uint32_t            refused = 0;

		// Takes a state that came at a_now; false (and nothing kept) when it is no newer than the newest held. The
		// sequence is compared as signed, so it may wrap.
		bool Push(const Protocol::PlayerState& a_state, Clock::time_point a_now)
		{
			if (!snapshots.empty() && static_cast<std::int32_t>(a_state.sequence - snapshots.back().state.sequence) <= 0) {
				++refused;
				return false;
			}
			const auto steps = snapshots.empty() ? 1u : a_state.sequence - snapshots.back().state.sequence;
			const auto stamp = a_state.sentMs ? sender.Next(a_now, a_state.sentMs) : arrivals.Next(a_now, steps);
			if (a_state.sentMs && sender.steppedBack > Clock::duration::zero()) {
				// The first state came late, so the ones kept were stamped too late as well: they move back with it.
				for (auto& snapshot : snapshots) {
					snapshot.stamp -= sender.steppedBack;
				}
			}
			snapshots.push_back({ stamp, a_state });
			refused = 0;
			while (snapshots.size() > MAX_SNAPSHOTS) {
				snapshots.pop_front();
			}
			return true;
		}

		void Clear() { *this = Buffer{}; }

		[[nodiscard]] static Clock::duration Delay() { return std::chrono::milliseconds(std::clamp(delayMs.load(), 0, 1000)); }

		// The way the newest state was going, units a second, from the one before it: none when the newest says they
		// stand, the two are in different spaces, too far apart in time to say, or faster than anyone runs (a jump).
		[[nodiscard]] std::optional<std::array<float, 3>> Velocity() const
		{
			if (snapshots.size() < 2) {
				return std::nullopt;
			}
			const auto& a = snapshots[snapshots.size() - 2];
			const auto& b = snapshots.back();
			const float span = std::chrono::duration<float>(b.stamp - a.stamp).count();
			if (b.state.speed < EXTRAPOLATE_FROM || a.state.cell != b.state.cell || a.state.worldspace != b.state.worldspace || span <= 0.0f || span > 0.5f) {
				return std::nullopt;
			}
			// In the air the fall is no line: only the way across is carried on.
			const bool                 inAir = (b.state.flags & Protocol::kJumping) != 0;
			const std::array<float, 3> v{ (b.state.x - a.state.x) / span, (b.state.y - a.state.y) / span, inAir ? 0.0f : (b.state.z - a.state.z) / span };
			if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > EXTRAPOLATE_SPEED * EXTRAPOLATE_SPEED) {
				return std::nullopt;
			}
			return v;
		}

		// Where the stand-in is drawn at a_now: a_now less the delay, blended between the two states either side of it.
		// None with nothing recent.
		[[nodiscard]] std::optional<Sampled> Sample(Clock::time_point a_now) const
		{
			if (snapshots.empty() || a_now - snapshots.back().stamp > STALE_AFTER + Delay()) {
				return std::nullopt;
			}
			const auto drawn = a_now - Delay();
			if (drawn <= snapshots.front().stamp) {
				return Sampled{ snapshots.front().state };
			}
			for (std::size_t i = 1; i < snapshots.size(); ++i) {
				const auto& a = snapshots[i - 1];
				const auto& b = snapshots[i];
				if (drawn > b.stamp) {
					continue;
				}
				// Not blended across a door or fast travel: it is where the newer state says.
				if (a.state.cell != b.state.cell || a.state.worldspace != b.state.worldspace) {
					return Sampled{ b.state };
				}
				const float span = std::chrono::duration<float>(b.stamp - a.stamp).count();
				const float t = span > 0.0f ? std::chrono::duration<float>(drawn - a.stamp).count() / span : 1.0f;
				auto        out = b.state;
				out.x = std::lerp(a.state.x, b.state.x, t);
				out.y = std::lerp(a.state.y, b.state.y, t);
				out.z = std::lerp(a.state.z, b.state.z, t);
				out.heading = LerpAngle(a.state.heading, b.state.heading, t);
				return Sampled{ out };
			}
			// Past the newest state: on the way it went for up to EXTRAPOLATE, then standing there.
			const auto& newest = snapshots.back();
			Sampled     out{ newest.state };
			const auto  past = drawn - newest.stamp;
			if (const auto v = Velocity()) {
				const float seconds = std::chrono::duration<float>((std::min)(past, std::chrono::duration_cast<Clock::duration>(EXTRAPOLATE))).count();
				out.state.x += (*v)[0] * seconds;
				out.state.y += (*v)[1] * seconds;
				out.state.z += (*v)[2] * seconds;
				out.ahead = past < EXTRAPOLATE;
			}
			out.held = !out.ahead;
			if (out.held) {
				out.state.speed = 0.0f;  // its legs stop with it
			}
			return out;
		}

		// Drops states too old to be drawn again (all but the two either side of the drawn time).
		void Frame(Clock::time_point a_now)
		{
			const auto drawn = a_now - Delay();
			while (snapshots.size() > 2 && snapshots[1].stamp < drawn) {
				snapshots.pop_front();
			}
		}
	};
}
