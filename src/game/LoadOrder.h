#pragma once

// The plugins this game has loaded, in order: what a joining game tells the host so that both play the same world
// (Protocol::ComparedLoadOrder, Protocol::LoadOrderHash).
//
// NOT WRITTEN YET (docs/PORTING.md, "The load order"): the answer is empty lists until the reading of the game's
// loaded files is written and checked in a game.
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
