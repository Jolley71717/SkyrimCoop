#pragma once

// The game's main thread, once a frame: where everything that touches the game runs (reading the player, moving the
// other players' stand-ins, Steam's callbacks).
//
// NOT WRITTEN YET. SKSE has no "every frame" task (its task queue runs a call once), so this needs a hook in the
// game's own main update, found and checked on an installed game: see docs/PORTING.md ("The frame"). Until then
// Install returns false and nothing of the plugin runs per frame.
namespace GameThread
{
	using Tick = void (*)();

	// From now on a_tick is called once a frame on the game's main thread. False when that can't be set up; the
	// reason is in the log.
	bool Install(Tick a_tick);

	// The id of the thread the plugin was loaded on (the game's main thread). For checks, not for work.
	void               NoteLoadThread();
	[[nodiscard]] bool IsLoadThread();
}
