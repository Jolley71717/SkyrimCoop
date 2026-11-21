#include "Test.h"

#include "TokenBucket.h"

#include <chrono>

// The budget the server gives each player's heartbeats (common/TokenBucket.h). The time is passed in, so none of
// this waits.

namespace
{
	using Clock = TokenBucket::Clock;
	using namespace std::chrono_literals;

	const Clock::time_point START{ std::chrono::hours(1) };
}

TEST("token bucket: starts full, and is empty once the burst is spent")
{
	TokenBucket bucket{ 3.0, 1.0 };
	CHECK(bucket.Take(START));
	CHECK(bucket.Take(START));
	CHECK(bucket.Take(START));
	CHECK(!bucket.Take(START));
	CHECK(!bucket.Take(START));
}

TEST("token bucket: gains back what its rate says, and never more than the burst")
{
	TokenBucket bucket{ 2.0, 10.0 };
	CHECK(bucket.Take(START));
	CHECK(bucket.Take(START));
	CHECK(!bucket.Take(START));

	// A tenth of a second at ten a second: one.
	CHECK(!bucket.Take(START + 50ms));
	CHECK(bucket.Take(START + 150ms));
	CHECK(!bucket.Take(START + 150ms));

	// A long quiet time fills it to the burst and no further.
	CHECK(bucket.Take(START + 1h));
	CHECK(bucket.Take(START + 1h));
	CHECK(!bucket.Take(START + 1h));
}

TEST("token bucket: a refused take spends nothing")
{
	TokenBucket bucket{ 1.0, 1.0 };
	CHECK(bucket.Take(START));
	// Ten refusals over half a second leave the half token gained standing.
	for (int i = 1; i <= 10; ++i) {
		CHECK(!bucket.Take(START + i * 50ms));
	}
	CHECK(bucket.Take(START + 1050ms));
}

TEST("token bucket: a clock that steps back gains nothing and breaks nothing")
{
	TokenBucket bucket{ 1.0, 1.0 };
	CHECK(bucket.Take(START));
	CHECK(!bucket.Take(START - 10s));
	CHECK(!bucket.Take(START + 500ms));
	CHECK(bucket.Take(START + 1100ms));
}
