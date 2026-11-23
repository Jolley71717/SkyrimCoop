#pragma once

#include "Protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Shared quests in a session (docs/DESIGN.md, "Quests"; the rules, free of the game, are src/game/QuestRules.h and, for
// runs and aliases, src/game/QuestRunRules.h).
// What our game does is read from the game itself: a stage done (TESQuestStageEvent), a quest started or stopped
// (TESQuestStartStopEvent), and, polled every second, the objectives and ends of the shared quests that run and the
// references their aliases hold. Each is reported when the session doesn't know it yet. The session's word is made here
// where this game is behind, by the game's own console commands (setstage, SetObjectiveDisplayed,
// SetObjectiveCompleted, CompleteQuest), a stage at a time, so each quest's own scripts run here as they ran in the game
// that did the step. The host's game says its world's quests at its welcome and after each load (kQuestReset, then its
// runs and its snapshot): the host's save is the world.
//
// Runs: the host's game says each shared quest and radiant job it starts (kQuestRun) with the references its aliases
// hold; a friend's game starts the quest with those references (or forces them into the quest that runs there
// already) before it makes the run's stages, and stops a radiant job the host's game stopped.
//
// From the tick only (GameThread), except the event sinks, which only queue.
namespace QuestSync
{
	using SendFn = std::function<void(std::vector<std::uint8_t>)>;

	// At kDataLoaded: the event sinks. False when the game's event source is not there (said in the log).
	bool Install();

	// The session let us in as a_playerId (the host's game when a_host): what this game knew of the session's quests
	// is forgotten; the host's game says its world's quests next.
	void Welcomed(std::uint32_t a_playerId, bool a_host);

	// Once a tick while in a session and let in: reports what is new here, makes the session's word where this game is
	// behind, and says the host's world or asks for the session's word again after a load.
	void Frame(const SendFn& a_send);

	// From the session: its word (kRemoteQuests), an answer to our reports (kQuestAnswer), the host's world new
	// (kQuestReset, from the server), the host's run of a quest (kRemoteQuestRun).
	void Heard(const Protocol::QuestBatch& a_batch);
	void Answered(const Protocol::QuestBatch& a_batch);
	void HostReset(std::uint32_t a_hostId);
	void HeardRun(const Protocol::QuestRun& a_run);

	// A save is about to load: the alias records rewritten for a run's start are put back.
	void BeforeLoad();

	// A save was loaded or a new game started: the host's game says its world again; a friend's asks for the session's
	// word again and makes what its save is behind on.
	void AfterLoad();

	// The session ended.
	void Clear();

	// Developer channel: `quests` (counts), `quests pick [n]` (a shared quest not started here, for the rig), `quests
	// runpick <radiant|shared> [n]` (one a console start can begin), `quests run <id>` (its run and its aliases' references
	// here), `quests start <id>` (started by the engine's own start), `quest <editor id or hex id>` (one quest: its class,
	// stages, objectives, and the session's word on it).
	[[nodiscard]] std::string Command(std::string_view a_args);
	[[nodiscard]] std::string One(std::string_view a_args);
}
