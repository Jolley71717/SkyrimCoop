#pragma once

// The game's main thread, about once a frame: where everything that touches the game runs (reading the player, moving
// the other players' stand-ins, Steam's callbacks).
//
// FOR NOW, A STAND-IN: SKSE has no "every frame" task (its task queue runs a call once), and the real answer is a hook
// in the game's own main update, found and checked on an installed game (docs/PORTING.md, "The frame"). Until that is
// written, a thread of ours queues a_tick on SKSE's task queue every ~16 ms, one at a time, which needs no engine
// address. It is enough for Steam and the first test; it is not a frame (it may run more or less often than the game
// draws, and not at all while SKSE's queue is not run).
namespace GameThread
{
	using Tick = void (*)();

	// From now on a_tick is called about once a frame on the game's main thread. False when that can't be set up; the
	// reason is in the log.
	bool Install(Tick a_tick);

	// How many times the tick has run.
	[[nodiscard]] std::uint64_t Ticks();

	// The id of the thread the plugin was loaded on (the game's main thread). For checks, not for work.
	void               NoteLoadThread();
	[[nodiscard]] bool IsLoadThread();
}
