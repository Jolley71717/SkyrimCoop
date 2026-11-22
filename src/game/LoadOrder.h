#pragma once

// The plugins this game has loaded, in order: what a joining game tells the host so that both play the same world
// (Protocol::ComparedLoadOrder, Protocol::LoadOrderHash). Read from the game's compiled file lists (TESDataHandler).
namespace LoadOrder
{
	struct Plugins
	{
		std::vector<std::string> full;   // .esm and .esp files, in load order
		std::vector<std::string> light;  // light plugins (.esl and flagged .esp), in load order
	};

	// Main thread, after the game's data is loaded (SKSE's kDataLoaded).
	[[nodiscard]] Plugins Read();
}
