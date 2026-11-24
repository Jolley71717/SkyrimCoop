#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Conversations (docs/DESIGN.md, "Conversations"; the rules, free of the game, are src/game/TalkRules.h): one player
// talks with an NPC, and the others near see it in their subtitle area and hear it from the NPC.
//
// The talker's game reads its conversation each tick: the subtitle of the NPC it talks with (each new one is a line,
// sent with the recording it plays, kLine), the topic its player picks (their own line, said by their stand-in in the
// others' games) and the end of a line said early (skipped, or the conversation left). It tells the session whom it
// talks with (kTalk): one player at a time talks with an NPC, and a game told another player talks with it ends its
// own talk with a notice, when its player started it (a forcegreet is left to run, its lines kept home).
//
// A friend's line is shown as the game shows a nearby NPC's (in the subtitle area over the speaker) when the game's own
// settings would show one, as a notice otherwise, and nowhere when the speaker is far; its recording is played from the
// speaker as a 3D sound with a hard end (src/game/LineSounds.h), never through the dialogue system.
//
// From the tick only (GameThread) and SKSE's messages on the main thread; the topic event's sink only queues.
namespace Conversations
{
	using SendFn = std::function<void(std::vector<std::uint8_t>)>;

	// At kDataLoaded: watches the dialogue's topic events. False when it can't (the reason is in the log).
	bool Install();

	// The session let us in as a_playerId.
	void Welcomed(std::uint32_t a_playerId);

	// Once a tick while in a session and let in: our conversation read and said, friends' lines' sounds and subtitles
	// ended when due.
	void Frame(const SendFn& a_send);

	// A friend's line (kRemoteLine) and the session's answer to our talk (kTalkAnswer).
	void Heard(const Protocol::Line& a_line);
	void Answered(const Protocol::TalkAnswer& a_answer);

	// A player left: nothing of their lines is waited on.
	void Left(std::uint32_t a_playerId);

	// A save is about to be loaded or a new game started: friends' lines' sounds and subtitles go with the world.
	void BeforeLoad();
	// A save was loaded or a new game started: our conversation is read afresh.
	void AfterLoad();

	// The session ended: every sound and subtitle of a friend's ended, nothing of our talk kept.
	void Clear();

	// Developer channel: `talk` (one line: our talk, the counts), `talk pick [n]` (the n-th NPC near our player who can
	// talk with it, lowest form ID first: both games in one place pick the same) and `talk subs` (the game's subtitle
	// settings and what its subtitle list holds).
	[[nodiscard]] std::string Command(std::string_view a_args);
}
