#include "Test.h"

#include "game/StateBuffer.h"

#include <cmath>
#include <numbers>

// Where a friend's stand-in is drawn between the states their game sends (src/game/StateBuffer.h).

namespace
{
	using Clock = StateBuffer::Clock;
	using namespace std::chrono_literals;

	// A state of a friend walking along +x at a_speed units a second, sampled a_ms after their first.
	Protocol::PlayerState Walking(std::uint32_t a_sequence, std::uint32_t a_ms, float a_speed = 200.0f)
	{
		Protocol::PlayerState state;
		state.sequence = a_sequence;
		state.worldspace = 0x3C;
		state.x = a_speed * static_cast<float>(a_ms) / 1000.0f;
		state.y = 100.0f;
		state.z = 50.0f;
		state.heading = 1.0f;
		state.speed = a_speed;
		state.sentMs = 1000 + a_ms;
		return state;
	}

	bool Near(float a_a, float a_b, float a_within = 0.5f)
	{
		return std::fabs(a_a - a_b) <= a_within;
	}

	// States every 50 ms from a_start, each arriving the moment it was sent (a link with no jitter).
	StateBuffer::Buffer Steady(Clock::time_point a_start, std::uint32_t a_count)
	{
		StateBuffer::Buffer buffer;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			REQUIRE(buffer.Push(Walking(i + 1, i * 50), a_start + std::chrono::milliseconds(i * 50)));
		}
		return buffer;
	}
}

TEST("stand-in drawing: between two states it is drawn on the line between them, the delay in the past")
{
	const auto start = Clock::now();
	const auto buffer = Steady(start, 10);
	// The newest state was sampled at 450 ms; drawn at 500 ms less the delay, it is where the friend was then.
	const auto at = start + 500ms;
	const auto sampled = buffer.Sample(at);
	REQUIRE(sampled);
	const float seconds = std::chrono::duration<float>(500ms - StateBuffer::Buffer::Delay()).count();
	CHECK(Near(sampled->state.x, 200.0f * seconds));
	CHECK(Near(sampled->state.y, 100.0f));
	CHECK(!sampled->held);
	CHECK(!sampled->ahead);
	CHECK(sampled->state.speed == 200.0f);
}

TEST("stand-in drawing: a state no newer than the newest held is refused, across a wrap of the sequence too")
{
	const auto          start = Clock::now();
	StateBuffer::Buffer buffer;
	REQUIRE(buffer.Push(Walking(5, 0), start));
	CHECK(!buffer.Push(Walking(5, 50), start + 50ms));
	CHECK(!buffer.Push(Walking(4, 50), start + 50ms));
	CHECK(buffer.refused == 2);
	CHECK(buffer.snapshots.size() == 1);

	StateBuffer::Buffer wrapping;
	REQUIRE(wrapping.Push(Walking(0xFFFFFFFF, 0), start));
	CHECK(wrapping.Push(Walking(0, 50), start + 50ms));
	CHECK(wrapping.snapshots.size() == 2);

	// The count is of states turned away since one was last taken.
	REQUIRE(buffer.Push(Walking(6, 100), start + 100ms));
	CHECK(buffer.refused == 0);
}

TEST("stand-in drawing: a friend whose states stop goes on the way they went for a moment, then stands with its legs stopped")
{
	const auto start = Clock::now();
	const auto buffer = Steady(start, 10);  // the last sampled at 450 ms, at x = 90
	const auto delay = StateBuffer::Buffer::Delay();

	// A little past the newest state: carried on along +x.
	const auto ahead = buffer.Sample(start + 450ms + delay + 50ms);
	REQUIRE(ahead);
	CHECK(ahead->ahead);
	CHECK(!ahead->held);
	CHECK(Near(ahead->state.x, 90.0f + 200.0f * 0.05f));
	CHECK(ahead->state.speed == 200.0f);

	// Long past it: standing where the carry-on ended, not running off.
	const auto held = buffer.Sample(start + 450ms + delay + 2s);
	REQUIRE(held);
	CHECK(held->held);
	CHECK(Near(held->state.x, 90.0f + 200.0f * std::chrono::duration<float>(StateBuffer::EXTRAPOLATE).count()));
	CHECK(held->state.speed == 0.0f);

	// Past STALE_AFTER: nothing to draw.
	CHECK(!buffer.Sample(start + 450ms + delay + StateBuffer::STALE_AFTER + 1s).has_value());
}

TEST("stand-in drawing: a friend who stands still is not carried anywhere")
{
	const auto          start = Clock::now();
	StateBuffer::Buffer buffer;
	for (std::uint32_t i = 0; i < 5; ++i) {
		REQUIRE(buffer.Push(Walking(i + 1, i * 50, 0.0f), start + std::chrono::milliseconds(i * 50)));
	}
	const auto sampled = buffer.Sample(start + 2s);
	REQUIRE(sampled);
	CHECK(Near(sampled->state.x, 0.0f));
	CHECK(sampled->state.speed == 0.0f);
}

TEST("stand-in drawing: a friend whose newest state says they stopped is not carried past where they stood")
{
	const auto          start = Clock::now();
	StateBuffer::Buffer buffer;
	for (std::uint32_t i = 0; i < 9; ++i) {
		REQUIRE(buffer.Push(Walking(i + 1, i * 50), start + std::chrono::milliseconds(i * 50)));
	}
	// The last step, at 450 ms: a little further on, and standing.
	auto stopped = Walking(10, 450);
	stopped.x = 82.0f;
	stopped.speed = 0.0f;
	REQUIRE(buffer.Push(stopped, start + 450ms));
	CHECK(!buffer.Velocity().has_value());

	const auto delay = StateBuffer::Buffer::Delay();
	for (const auto past : { 20ms, 50ms, 100ms, 2000ms }) {
		const auto sampled = buffer.Sample(start + 450ms + delay + past);
		REQUIRE(sampled);
		CHECK(Near(sampled->state.x, 82.0f));
		CHECK(sampled->state.speed == 0.0f);
	}
}

TEST("stand-in drawing: no blend across a door; the stand-in is where the newer state says")
{
	const auto          start = Clock::now();
	StateBuffer::Buffer buffer;
	auto                outside = Walking(1, 0);
	auto                inside = Walking(2, 50);
	inside.worldspace = 0;
	inside.cell = 0x000165A3;
	inside.x = 5000.0f;
	REQUIRE(buffer.Push(outside, start));
	REQUIRE(buffer.Push(inside, start + 50ms));
	const auto sampled = buffer.Sample(start + 25ms + StateBuffer::Buffer::Delay());
	REQUIRE(sampled);
	CHECK(sampled->state.cell == inside.cell);
	CHECK(sampled->state.x == 5000.0f);
	// And nothing is carried on past it: two states in different spaces say no way of going.
	CHECK(!buffer.Velocity().has_value());
}

TEST("stand-in drawing: a jump far faster than anyone runs is not carried on")
{
	const auto          start = Clock::now();
	StateBuffer::Buffer buffer;
	REQUIRE(buffer.Push(Walking(1, 0), start));
	auto far = Walking(2, 50);
	far.x = 10000.0f;  // 10000 units in 50 ms
	REQUIRE(buffer.Push(far, start + 50ms));
	CHECK(!buffer.Velocity().has_value());
	const auto sampled = buffer.Sample(start + 1s);
	REQUIRE(sampled);
	CHECK(sampled->state.x == 10000.0f);
	CHECK(sampled->held);
}

TEST("stand-in drawing: the heading turns the short way round, across the half turn")
{
	CHECK(Near(StateBuffer::TurnBetween(3.0f, -3.0f), 2.0f * std::numbers::pi_v<float> - 6.0f, 0.001f));
	CHECK(Near(StateBuffer::TurnBetween(-3.0f, 3.0f), 6.0f - 2.0f * std::numbers::pi_v<float>, 0.001f));
	CHECK(Near(StateBuffer::LerpAngle(3.0f, -3.0f, 0.5f), 3.0f + (2.0f * std::numbers::pi_v<float> - 6.0f) * 0.5f, 0.001f));
	CHECK(Near(StateBuffer::TurnBetween(0.5f, 1.0f), 0.5f, 0.001f));
}

TEST("stand-in drawing: states that came late together are drawn by when they were sent, not when they came")
{
	const auto          start = Clock::now();
	StateBuffer::Buffer buffer;
	// Four states on time, then five held up on the way that all come at once.
	for (std::uint32_t i = 0; i < 4; ++i) {
		REQUIRE(buffer.Push(Walking(i + 1, i * 50), start + std::chrono::milliseconds(i * 50)));
	}
	for (std::uint32_t i = 4; i < 9; ++i) {
		REQUIRE(buffer.Push(Walking(i + 1, i * 50), start + 400ms));
	}
	// Sampled at 400 ms after the first, less the delay: on the line, where the friend was then (x = 200 * t).
	const auto at = start + 400ms;
	const auto sampled = buffer.Sample(at);
	REQUIRE(sampled);
	const float seconds = std::chrono::duration<float>(400ms - StateBuffer::Buffer::Delay()).count();
	CHECK(Near(sampled->state.x, 200.0f * seconds, 2.0f));
}

TEST("stand-in drawing: old states are dropped once drawn past, the two around the drawn time kept")
{
	const auto start = Clock::now();
	auto       buffer = Steady(start, 20);
	buffer.Frame(start + 1000ms + StateBuffer::Buffer::Delay());
	CHECK(buffer.snapshots.size() >= 1);
	CHECK(buffer.snapshots.size() <= 2);
	auto fresh = Steady(start, 20);
	fresh.Frame(start + 500ms);
	CHECK(fresh.snapshots.front().stamp <= start + 500ms - StateBuffer::Buffer::Delay());
	CHECK(fresh.snapshots.size() < 20);
}
