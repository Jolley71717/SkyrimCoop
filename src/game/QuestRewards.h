#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Quest rewards for everyone (docs/DESIGN.md, "Quests", "Rewards"; the rules, free of the game, are
// src/game/RewardRules.h). A shared quest's stages are made in every game, so what a stage's fragment gives is each
// game's already; what a conversation gives (a topic info's own fragment) runs only in the talker's game. That game
// sees the item come to its player from nothing while the Dialogue Menu is open, names the quest by the topic info said
// last, and says it (kQuestReward); every other game gives its player the same (`player.additem`) once the quest's
// stages from the session are made there, less what its player got from them already.
//
// Quest items stay each world's own: an item the quest holds in an alias comes as a reference, not from nothing, and is
// not a reward here (whoever holds it hands it in, and the quest's stages follow for everyone).
//
// From the tick (GameThread), and from SKSE's messages on the main thread (a load): every entry point takes one lock
// over QuestRewards' state. The event sinks never take it; they only queue.
namespace QuestRewards
{
	using SendFn = std::function<void(std::vector<std::uint8_t>)>;

	// At kDataLoaded: the event sinks (our player's items from nothing, the topic infos said). False when the game's
	// event source is not there.
	bool Install();

	// The session let us in as a_playerId: rewards heard or still to say from before are of no session now.
	void Welcomed(std::uint32_t a_playerId);

	// Once a tick while in a session and let in: judges what our player got, says the rewards due, and gives the rewards
	// heard whose quest is ready here.
	void Frame(const SendFn& a_send);

	// From the session: another player's reward (kRemoteQuestReward).
	void Heard(const Protocol::QuestReward& a_reward);

	// A save was loaded or a new game started: what our player got before is of another world.
	void AfterLoad();

	// The session ended.
	void Clear();

	// Developer channel: `rewards` (one line of counts), `rewards list` (the rewards waiting to be given here).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
