#include "game/GameThread.h"

namespace GameThread
{
	namespace
	{
		std::thread::id loadThread;
	}

	bool Install(Tick)
	{
		logs::warn("Game thread: the per-frame hook is not written yet; nothing of the plugin runs each frame");
		return false;
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
