#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

// Work handed to the game's main thread by other threads (the developer channel's listener): Post from any thread,
// Drain on the main thread once a tick (main.cpp's OnFrame). No game here, so it is unit-tested.
//
// The idea of F4Multiplayer's src/MainThread.h (commit 1d29178b1), without its window hook: there the queue was
// drained by the hooked game window, here by the tick GameThread gives.
class MainQueue
{
public:
	void Post(std::function<void()> a_work)
	{
		std::scoped_lock lock{ mutex };
		work.push_back(std::move(a_work));
		if (work.size() > most) {
			most = work.size();
		}
	}

	// Runs everything posted before the call, in the order posted, and returns how many ran. Work posted while it
	// runs (by the work itself) waits for the next Drain: one tick never runs for ever.
	std::size_t Drain()
	{
		std::vector<std::function<void()>> now;
		{
			std::scoped_lock lock{ mutex };
			now.swap(work);
		}
		for (auto& each : now) {
			if (each) {
				each();
			}
		}
		return now.size();
	}

	[[nodiscard]] std::size_t Waiting() const
	{
		std::scoped_lock lock{ mutex };
		return work.size();
	}

	// The most pieces of work ever waiting at once.
	[[nodiscard]] std::size_t Most() const
	{
		std::scoped_lock lock{ mutex };
		return most;
	}

private:
	mutable std::mutex                 mutex;
	std::vector<std::function<void()>> work;
	std::size_t                        most = 0;
};
