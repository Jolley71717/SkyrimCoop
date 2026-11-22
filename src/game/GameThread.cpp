#include "game/GameThread.h"

namespace GameThread
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto INTERVAL = 16ms;
		// A tick queued and not run by then: SKSE's queue is not being run (a loading screen can do that). Said once.
		constexpr auto STALL = 10s;

		std::thread::id            loadThread;
		std::atomic<Tick>          tick{ nullptr };
		std::atomic<bool>          queued{ false };
		std::atomic<std::uint64_t> ticks{ 0 };
		Clock::time_point          installedAt;

		void RunTick()
		{
			const auto count = ++ticks;
			if (count == 1) {
				logs::info("Game thread: the first tick ran {} ms after the pump started (SKSE's task queue, a stand-in for the frame hook); "
						   "on the game's main thread: {}",
					std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - installedAt).count(), IsLoadThread() ? "yes" : "NO");
			}
			if (const auto fn = tick.load()) {
				fn();
			}
			queued = false;
		}

		void Pump()
		{
			const auto tasks = SKSE::GetTaskInterface();
			auto       queuedAt = Clock::now();
			bool       stallTold = false;
			for (;;) {
				std::this_thread::sleep_for(INTERVAL);
				// One at a time: a queue that is not being run must not fill with ticks.
				if (!queued.exchange(true)) {
					queuedAt = Clock::now();
					stallTold = false;
					tasks->AddTask([] {
						// An exception must never escape into the game's task loop.
						try {
							RunTick();
						} catch (const std::exception& e) {
							queued = false;
							logs::error("Game thread: the tick failed: {}", e.what());
						} catch (...) {
							queued = false;
							logs::error("Game thread: the tick failed");
						}
					});
				} else if (!stallTold && Clock::now() - queuedAt >= STALL) {
					stallTold = true;
					logs::warn("Game thread: a tick queued {} s ago has not run yet (SKSE's task queue is not being run: a loading screen?)",
						std::chrono::duration_cast<std::chrono::seconds>(STALL).count());
				}
			}
		}
	}

	bool Install(Tick a_tick)
	{
		if (tick.load()) {
			return true;  // once is enough: a second kDataLoaded (none is expected) changes nothing
		}
		if (!SKSE::GetTaskInterface()) {
			logs::error("Game thread: SKSE gives no task interface; nothing of the plugin runs each frame");
			return false;
		}
		tick = a_tick;
		installedAt = Clock::now();
		std::thread(Pump).detach();
		logs::info("Game thread: ticking every {} ms through SKSE's task queue (the frame hook is not written yet)", INTERVAL.count());
		return true;
	}

	std::uint64_t Ticks()
	{
		return ticks.load();
	}

	void NoteLoadThread()
	{
		loadThread = std::this_thread::get_id();
	}

	bool IsLoadThread()
	{
		return std::this_thread::get_id() == loadThread;
	}
}
