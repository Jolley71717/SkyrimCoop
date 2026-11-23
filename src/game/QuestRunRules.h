#pragma once

#include "FormIds.h"
#include "Protocol.h"
#include "QuestRuns.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Quest runs and their aliases (docs/DESIGN.md, "Quests", "Runs and aliases"): the rules, free of the game, by which the
// host's game says the quests it starts and the references and locations their aliases hold, and a friend's game does
// the same job: it starts the quest with the host's references and locations in its aliases, or forces the references
// into a quest that runs there already.
// Tested in tests/QuestRunTests.cpp; src/game/QuestSync.cpp reads and changes the game.
//
// The class is every quest the session shares by its record (QuestRules::Class::kShared), and every radiant job (the
// Story Manager's repeating ones, QuestRules::Class::kRadiant) the host's game starts: a radiant job is the session's
// when, and only when, the host's game runs it, with the host's targets; one a friend's game starts on its own stays
// that friend's own job. Each alias is the host's: a reference alias or a location alias, filled by conditions, from the
// Story Manager's event, from another alias, a unique actor or a forced form alike. Not yet: an actor the quest creates
// for its alias (each game makes its own until NPCs are paired). A location can only be given to a quest as it starts
// (the engine has no call to force one into a running quest's alias that is known here): a radiant job running here with
// another location is stopped and started again for the run; a shared quest's is counted, said once.
namespace QuestRunRules
{
	using Clock = std::chrono::steady_clock;
	using Fill = Protocol::AliasFill;
	using Run = Protocol::QuestRun;

	// How long a friend's game waits for a run's fills, and then for the quest it starts with them to run, before it
	// gives the run up: a shared quest's stages are then made as before (its own picks), a radiant job's are dropped (it
	// is never this game's own pick of the job). A run given up is not over: fills that come later start it, a start that
	// runs later is taken as the run (its stages made then), and the host's stop stops a quest this game started for it.
	inline constexpr auto START_WAIT = std::chrono::seconds{ 10 };
	// How often a run's references are forced again into the aliases where they do not hold (a reference not loaded
	// yet, or a script that filled the alias again).
	inline constexpr auto FORCE_EVERY = std::chrono::seconds{ 2 };
	// How long after a start the game would not make (or one that never ran) it is tried again, while the run is on.
	inline constexpr auto RETRY_EVERY = std::chrono::seconds{ 30 };
	// The host's game says a run's fills again this often unchanged (the server takes the same word as nothing new and
	// passes nothing on): a word its budget dropped, or one lost with a start, is said again.
	inline constexpr auto RESAY_EVERY = std::chrono::seconds{ 30 };

	// ---- the host's game: its runs ---------------------------------------------------------------------------------

	// The quests the host's game runs as the session's: numbers each start (never twice in the game's session), and says
	// each run's fills when first read, again when they change, and again every RESAY_EVERY.
	class Keeper
	{
	public:
		// a_questId started in our game (its event, or the snapshot: a_fresh false), a radiant job when a_repeats: the word
		// to send.
		Run Started(std::uint32_t a_questId, bool a_repeats, bool a_fresh = true)
		{
			auto& q = quests[a_questId];
			q = Quest{};
			q.run = ++counter;
			q.live = true;
			q.repeats = a_repeats;
			q.fresh = a_repeats && a_fresh;
			Run run;
			run.questId = a_questId;
			run.run = q.run;
			run.flags = q.fresh ? Run::kFresh : 0;
			return run;
		}

		// The references and locations our game's quest holds in its aliases now (polled while it runs): the word to send
		// when they are new to the session (the first read of the run, or a change), or RESAY_EVERY has passed. A fresh
		// run's fills are said fresh too: the server forgets nothing for a run it has (the same number), and a fills word
		// that comes without its start (the start dropped) begins the run as the start would have.
		std::optional<Run> Read(std::uint32_t a_questId, std::vector<Fill> a_fills, Clock::time_point a_now)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || !it->second.live) {
				return std::nullopt;
			}
			auto& q = it->second;
			auto  fills = QuestRuns::Sorted(std::move(a_fills));
			if (q.filled && q.fills == fills && a_now - q.saidAt < RESAY_EVERY) {
				return std::nullopt;
			}
			q.filled = true;
			q.fills = fills;
			q.saidAt = a_now;
			Run run;
			run.questId = a_questId;
			run.run = q.run;
			run.flags = static_cast<std::uint8_t>(Run::kFilled | (q.fresh ? Run::kFresh : 0));
			run.fills = std::move(fills);
			return run;
		}

		// a_questId stopped in our game (its event): the word to send for a radiant job (its run is over); none for a
		// shared quest (each game's own scripts stop it once it is done).
		std::optional<Run> Stopped(std::uint32_t a_questId)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || !it->second.live) {
				return std::nullopt;
			}
			it->second.live = false;
			if (!it->second.repeats) {
				return std::nullopt;
			}
			Run run;
			run.questId = a_questId;
			run.run = it->second.run;
			run.flags = Run::kStopped;
			return run;
		}

		[[nodiscard]] bool Live(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() && it->second.live;
		}

		[[nodiscard]] std::vector<std::uint32_t> LiveQuests() const
		{
			std::vector<std::uint32_t> out;
			for (const auto& [id, q] : quests) {
				if (q.live) {
					out.push_back(id);
				}
			}
			std::ranges::sort(out);
			return out;
		}

		// The fills last said of a run (the dev command).
		[[nodiscard]] const std::vector<Fill>* Fills(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() && it->second.filled ? &it->second.fills : nullptr;
		}
		[[nodiscard]] std::uint32_t RunOf(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() ? it->second.run : 0;
		}

		// A new world (a welcome, a load): its quests that run are started again as runs (the snapshot); the numbers go
		// on, so no run is numbered twice in the game's session.
		void Clear() { quests.clear(); }

	private:
		struct Quest
		{
			std::uint32_t     run = 0;
			bool              live = false;
			bool              repeats = false;
			bool              fresh = false;
			bool              filled = false;
			Clock::time_point saidAt{};
			std::vector<Fill> fills;
		};
		std::unordered_map<std::uint32_t, Quest> quests;
		std::uint32_t                            counter = 0;
	};

	// ---- a friend's game: the host's runs, made here ---------------------------------------------------------------

	// What this game reads of a quest a run is about.
	struct Here
	{
		bool exists = false;    // the quest is in this game
		bool shared = false;    // its record shares it (QuestRules::Class::kShared)
		bool radiant = false;   // its record makes it a radiant job (QuestRules::Class::kRadiant)
		bool running = false;   // it runs here (enabled, and not still starting)
		bool starting = false;  // it is starting here
		bool ended = false;     // it is completed or failed here
	};

	// Where a run stands in this game.
	enum class Phase : std::uint8_t
	{
		kWaiting,     // heard: waiting for its fills, or for the quest to be started with them
		kStarting,    // this game started the quest with the run's references in its aliases: waiting for it to run
		kRunning,     // the quest runs here as the session's run (started here for it, or adopted: it ran here already)
		kGaveUp,      // its fills or its start did not come within START_WAIT, or the game would not start it (tried again)
		kEnded,       // the quest is ended here already: nothing to start
		kNotHere,     // the quest is missing here, or not shared by this game's record
		kRestarting,  // a radiant job that ran here with another location: stopped, to be started again for the run
	};

	[[nodiscard]] constexpr std::string_view PhaseName(Phase a_phase)
	{
		switch (a_phase) {
		case Phase::kWaiting:
			return "waiting";
		case Phase::kStarting:
			return "starting";
		case Phase::kRunning:
			return "running";
		case Phase::kGaveUp:
			return "gaveup";
		case Phase::kEnded:
			return "ended";
		case Phase::kNotHere:
			return "nothere";
		case Phase::kRestarting:
			return "restarting";
		}
		return "?";
	}

	// What Due decided, for the caller to do.
	enum class Act : std::uint8_t
	{
		kNothing,  // nothing now
		kStart,    // start the quest here with the run's references and locations forced into its aliases (StartPlan)
		kStarted,  // the quest started here runs: put the aliases' records back, and force what did not hold
		kAdopt,    // the quest ran here already: it is the session's run now; force the run's references into it
		kGiveUp,   // the run's fills or its start did not come: put any records back; its stages go on as Phase says
		kStop,     // a radiant job the session's run of stopped: stop it here too
	};

	// One reference (or location) to force into an alias here.
	struct Force
	{
		std::uint32_t aliasId = 0;
		std::uint32_t refId = 0;
		bool operator==(const Force&) const = default;
	};

	// What a start for a run gave the quest's aliases (StartPlan), kept for the run (the dev command, the rig).
	struct StartNote
	{
		std::size_t        refs = 0;       // reference aliases given the host's reference
		std::size_t        locs = 0;       // location aliases given the host's location
		std::size_t        nones = 0;      // optional aliases the host's game left empty, made empty here
		std::size_t        own = 0;        // aliases left to this game's own pick
		std::size_t        notLoaded = 0;  // ... of them, the host's form not loaded (or lacking) here
		std::vector<Force> forces;         // the references given (a forcing of one of them after the start: missed)
	};

	// One run's counts here, for the dev command and the rig.
	struct RunCounts
	{
		bool        startedHere = false;  // this game started the quest for the run (not adopted)
		StartNote   start;                // what that start gave its aliases
		std::size_t missed = 0;           // distinct references the start gave that had to be forced after it ran (the start did not take them)
		std::size_t later = 0;            // distinct references forced while it ran that the start did not give (the host's alias changed, or adopted)
		std::size_t notLoaded = 0;        // distinct references of the run not loaded here when forced (said once each)
		std::size_t locApart = 0;         // distinct location aliases holding another location than the host's while it runs (said once each)
		std::size_t retries = 0;          // starts tried again after the run was given up
		bool        restarted = false;    // stopped and started again for the host's location (a radiant job)
	};

	class Follower
	{
	public:
		// The server's word on a run (kRemoteQuestRun). True when what this game knows of the quest's stages,
		// objectives and end is to be forgotten: a radiant job begun again (fresh) or stopped, whose earlier run's word
		// is not this one's. a_here is this game's quest now; a stop of the run this game runs as the session's is made
		// by the next Due (kStop).
		bool Hear(const Run& a_run, const Here& a_here, Clock::time_point a_now)
		{
			if ((a_run.flags & Run::kStopped) != 0) {
				const auto it = quests.find(a_run.questId);
				if (it == quests.end() || it->second.run != a_run.run || !it->second.live) {
					return false;  // not the run this game knows of (the server passes on a stop of the run on only)
				}
				auto&      q = it->second;
				// a quest this game started or took for the run, even one given up before it ran (it may run yet)
				const bool ours = q.phase == Phase::kRunning || q.phase == Phase::kStarting || q.phase == Phase::kRestarting ||
				                  (q.phase == Phase::kGaveUp && q.counts.startedHere);
				q = Quest{};
				q.run = a_run.run;
				q.stop = ours && a_here.radiant;
				return a_here.radiant;
			}
			auto& q = quests[a_run.questId];
			bool  forget = false;
			if (q.run != a_run.run || !q.live) {
				forget = (a_run.flags & Run::kFresh) != 0;
				const bool wasOurs = q.live && (q.phase == Phase::kRunning || q.phase == Phase::kStarting);
				q = Quest{};
				q.run = a_run.run;
				q.live = true;
				q.since = a_now;
				// the quest runs here as an earlier run of the session's: this run is adopted by the next Due
				q.wasOurs = wasOurs;
			}
			if ((a_run.flags & Run::kFilled) != 0) {
				q.filled = true;
				q.fills = QuestRuns::Sorted(a_run.fills);
				q.nextForce = a_now;  // force what changed at once
				if (q.phase == Phase::kGaveUp) {
					q.retryAt = a_now;  // fills that came late, or changed after a start that failed: tried at once
				}
			}
			return forget;
		}

		// What to do now about a_questId, its quest here being a_here; called every frame for each quest this game has
		// heard a run of (Quests), before the session's stages are made (Hold reads the phase this sets).
		Act Due(std::uint32_t a_questId, const Here& a_here, Clock::time_point a_now)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end()) {
				return Act::kNothing;
			}
			auto& q = it->second;
			if (q.stop) {
				q.stop = false;
				return a_here.running || a_here.starting ? Act::kStop : Act::kNothing;
			}
			if (!q.live) {
				return Act::kNothing;
			}
			if (!a_here.exists || (!a_here.shared && !a_here.radiant)) {
				q.phase = Phase::kNotHere;
				return Act::kNothing;
			}
			switch (q.phase) {
			case Phase::kRestarting:
				if (a_here.running || a_here.starting) {
					if (a_now - q.since >= START_WAIT) {
						return GiveUp(q, a_now);  // it never stopped: Restart's stop did not take
					}
					return Act::kNothing;
				}
				q.phase = Phase::kWaiting;
				q.since = a_now;
				[[fallthrough]];
			case Phase::kWaiting:
				if (a_here.ended) {
					q.phase = Phase::kEnded;
					return Act::kNothing;
				}
				if (a_here.running) {
					q.phase = Phase::kRunning;
					q.nextForce = a_now;
					++adopted;
					return Act::kAdopt;
				}
				if (a_here.starting) {
					return Act::kNothing;  // its own start is under way: adopted once it runs
				}
				if (!q.filled) {
					if (a_now - q.since >= START_WAIT) {
						q.retryAt = a_now;  // fills that come later start it at once
						return GiveUp(q, a_now);
					}
					return Act::kNothing;
				}
				q.phase = Phase::kStarting;
				q.since = a_now;
				++started;
				return Act::kStart;
			case Phase::kStarting:
				if (a_here.running) {
					q.phase = Phase::kRunning;
					q.nextForce = a_now;
					return Act::kStarted;
				}
				if (a_now - q.since >= START_WAIT) {
					q.retryAt = a_now + RETRY_EVERY;
					return GiveUp(q, a_now);
				}
				return Act::kNothing;
			case Phase::kGaveUp:
				if (a_here.running) {
					// a start of ours that ran after all, or the quest started here since: the run's from now on, and the
					// stages dropped meanwhile are made again from the session's word (TakeLate)
					q.phase = Phase::kRunning;
					q.nextForce = a_now;
					q.late = true;
					if (q.counts.startedHere) {
						return Act::kStarted;
					}
					++adopted;
					return Act::kAdopt;
				}
				if (a_here.ended && !a_here.radiant) {
					q.phase = Phase::kEnded;
					return Act::kNothing;
				}
				if (a_here.starting || !q.filled || a_now < q.retryAt) {
					return Act::kNothing;
				}
				q.phase = Phase::kStarting;
				q.since = a_now;
				++q.counts.retries;
				++retried;
				return Act::kStart;
			case Phase::kRunning:
			case Phase::kEnded:
			case Phase::kNotHere:
				return Act::kNothing;
			}
			return Act::kNothing;
		}

		// The caller's start (Act::kStart) gave the quest's aliases a_note and the game took it: kept for the run.
		void StartMade(std::uint32_t a_questId, StartNote a_note)
		{
			const auto it = quests.find(a_questId);
			if (it != quests.end()) {
				it->second.counts.startedHere = true;
				it->second.counts.start = std::move(a_note);
			}
		}

		// The caller's start could not be made: the game would not start the quest (a_retry: tried again after
		// RETRY_EVERY, or at once when new fills come), or the plan left an alias that needs the Story Manager's event
		// without a fill (it cannot start here: tried again only when new fills come). Given up at once, not after
		// START_WAIT: nothing will run.
		void StartFailed(std::uint32_t a_questId, Clock::time_point a_now, bool a_retry)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || it->second.phase != Phase::kStarting) {
				return;
			}
			auto& q = it->second;
			q.retryAt = a_retry ? a_now + RETRY_EVERY : Clock::time_point::max();
			GiveUp(q, a_now);
		}

		// A radiant job this game runs as the run (adopted: it ran here with this game's own pick) holds another location
		// than the host's: the caller stops it, and it is started again for the run once stopped. Once a run: false when
		// it was restarted already (or is no radiant job this game runs as the run).
		bool Restart(std::uint32_t a_questId, Clock::time_point a_now)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || !it->second.live || it->second.phase != Phase::kRunning || it->second.counts.restarted) {
				return false;
			}
			auto& q = it->second;
			q.phase = Phase::kRestarting;
			q.since = a_now;
			q.late = true;  // what was made in it is undone by its stop: made again once it runs for the run
			q.counts.restarted = true;
			++restarts;
			return true;
		}

		// Whether the run's references are to be forced into a_questId's aliases now (it runs here as the session's run,
		// its fills are said, and FORCE_EVERY has passed since the last time): the caller forces what does not hold.
		bool ForceDue(std::uint32_t a_questId, Clock::time_point a_now)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || !it->second.live || it->second.phase != Phase::kRunning || !it->second.filled || a_now < it->second.nextForce) {
				return false;
			}
			it->second.nextForce = a_now + FORCE_EVERY;
			return true;
		}

		// A reference forced into a_questId's alias while it runs: one its start gave (missed: the start did not take it)
		// or another (later), each counted once a run. True the first time in the run for that alias and reference (the
		// caller says it then: a script that fills the alias again and again is not a line every FORCE_EVERY).
		bool Forced(std::uint32_t a_questId, const Force& a_force)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || std::ranges::find(it->second.forcedSaid, a_force) != it->second.forcedSaid.end()) {
				return false;
			}
			it->second.forcedSaid.push_back(a_force);
			auto& c = it->second.counts;
			if (std::ranges::find(c.start.forces, a_force) != c.start.forces.end()) {
				++c.missed;
			} else {
				++c.later;
			}
			return true;
		}

		// A reference of the run not loaded here when forced: true the first time in the run for that alias and
		// reference (the caller says it then; it is tried again every FORCE_EVERY all the same, quietly).
		bool NotLoaded(std::uint32_t a_questId, const Force& a_force)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || std::ranges::find(it->second.notLoadedSaid, a_force) != it->second.notLoadedSaid.end()) {
				return false;
			}
			it->second.notLoadedSaid.push_back(a_force);
			++it->second.counts.notLoaded;
			++notLoaded;
			return true;
		}

		// A location alias of a_questId holding another location than the host's while it runs here: true the first time
		// in the run for that alias and location.
		bool LocApart(std::uint32_t a_questId, const Force& a_force)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || std::ranges::find(it->second.locApartSaid, a_force) != it->second.locApartSaid.end()) {
				return false;
			}
			it->second.locApartSaid.push_back(a_force);
			++it->second.counts.locApart;
			++locApart;
			return true;
		}

		// Whether a_questId came to run as the run after it was given up (its stages dropped meanwhile are to be made
		// again from the session's word): true once.
		bool TakeLate(std::uint32_t a_questId)
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() && std::exchange(it->second.late, false);
		}

		// The session's stages, objectives and end of a_questId wait (QuestRules::Verdict::kHold): its run is heard and
		// this game has not started the quest for it yet (waiting for its fills, for its start to run, or for its stop to
		// start it again).
		[[nodiscard]] bool Hold(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() && it->second.live &&
			       (it->second.phase == Phase::kWaiting || it->second.phase == Phase::kStarting || it->second.phase == Phase::kRestarting);
		}

		// Whether a_questId runs here as the session's run: a radiant job shared only then (its stages reported and made).
		[[nodiscard]] bool Ours(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() && it->second.live && it->second.phase == Phase::kRunning;
		}

		// Our game stopped a_questId itself (its event): a run it ran here is not run here any more. A stop made for a
		// restart is the restart's.
		void StoppedHere(std::uint32_t a_questId)
		{
			const auto it = quests.find(a_questId);
			if (it != quests.end() && it->second.phase == Phase::kRunning) {
				it->second.phase = Phase::kEnded;
			}
		}

		[[nodiscard]] const std::vector<Fill>* Fills(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() && it->second.filled ? &it->second.fills : nullptr;
		}
		[[nodiscard]] std::optional<Phase> PhaseOf(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || !it->second.live) {
				return std::nullopt;
			}
			return it->second.phase;
		}
		[[nodiscard]] std::uint32_t RunOf(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() ? it->second.run : 0;
		}
		[[nodiscard]] bool WasOurs(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() && it->second.wasOurs;
		}
		[[nodiscard]] RunCounts CountsOf(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() ? it->second.counts : RunCounts{};
		}

		// The quests heard of, for the caller's frame (each one's Due).
		[[nodiscard]] std::vector<std::uint32_t> Quests() const
		{
			std::vector<std::uint32_t> out;
			out.reserve(quests.size());
			for (const auto& [id, q] : quests) {
				if (q.live || q.stop) {
					out.push_back(id);
				}
			}
			std::ranges::sort(out);
			return out;
		}
		[[nodiscard]] std::size_t Live() const
		{
			return static_cast<std::size_t>(std::ranges::count_if(quests, [](const auto& a_q) { return a_q.second.live; }));
		}
		[[nodiscard]] std::uint64_t Started() const { return started; }
		[[nodiscard]] std::uint64_t Adopted() const { return adopted; }
		[[nodiscard]] std::uint64_t GaveUp() const { return gaveUp; }
		[[nodiscard]] std::uint64_t Retried() const { return retried; }
		[[nodiscard]] std::uint64_t Restarts() const { return restarts; }
		[[nodiscard]] std::uint64_t NotLoadedCount() const { return notLoaded; }  // distinct, over every run
		[[nodiscard]] std::uint64_t LocApartCount() const { return locApart; }    // distinct, over every run

		// The session's runs are no longer this game's word (a welcome, the host's world new, a load, the session's end).
		void Clear() { quests.clear(); }

	private:
		struct Quest
		{
			std::uint32_t      run = 0;
			bool               live = false;
			bool               filled = false;
			bool               stop = false;     // a stop of the run this game ran: made by the next Due
			bool               wasOurs = false;  // an earlier run of the quest ran here as the session's
			bool               late = false;     // runs here after it was given up: its dropped stages to be made again
			Phase              phase = Phase::kWaiting;
			Clock::time_point  since{};
			Clock::time_point  nextForce{};
			Clock::time_point  retryAt = Clock::time_point::max();
			std::vector<Fill>  fills;
			RunCounts          counts;
			std::vector<Force> notLoadedSaid;
			std::vector<Force> locApartSaid;
			std::vector<Force> forcedSaid;
		};

		// Its stages are no longer held (a radiant job's dropped, a shared quest's made as they come): should the run come
		// to run here after all, what the session said of it is made again then (late).
		Act GiveUp(Quest& a_q, Clock::time_point)
		{
			a_q.phase = Phase::kGaveUp;
			a_q.late = true;
			++gaveUp;
			return Act::kGiveUp;
		}

		std::unordered_map<std::uint32_t, Quest> quests;
		std::uint64_t                            started = 0;
		std::uint64_t                            adopted = 0;
		std::uint64_t                            gaveUp = 0;
		std::uint64_t                            retried = 0;
		std::uint64_t                            restarts = 0;
		std::uint64_t                            notLoaded = 0;
		std::uint64_t                            locApart = 0;
	};

	// ---- a start for a run: which alias gets what --------------------------------------------------------------------

	// How an alias's record says it is filled (the engine's numbering, RE::BGSBaseAlias::FILL_TYPE; a location alias uses
	// the same field and numbers: a specific location is kForced, find a matching location kConditions).
	enum class FillType : std::uint16_t
	{
		kConditions = 0,
		kForced = 1,
		kFromAlias = 2,
		kFromEvent = 3,
		kCreated = 4,
		kFromExternal = 5,
		kUniqueActor = 6,
		kNearAlias = 7,
	};

	// One alias of the quest as its record has it.
	struct AliasRecord
	{
		std::uint32_t aliasId = 0;
		bool          location = false;  // a location alias (else a reference alias)
		FillType      fill = FillType::kConditions;
		bool          optional = false;
	};

	// What a start for a run makes of one alias's record for the start.
	enum class Way : std::uint8_t
	{
		kOwn,   // left as it is: this game's own pick
		kRef,   // a forced fill of the host's reference (formId)
		kLoc,   // a forced fill of the host's location (formId)
		kNone,  // a forced fill of none (an optional alias the host's game left empty)
	};

	struct PlanStep
	{
		std::uint32_t aliasId = 0;
		bool          location = false;
		Way           way = Way::kOwn;
		std::uint32_t formId = 0;
		bool operator==(const PlanStep&) const = default;
	};

	struct StartPlan
	{
		std::vector<PlanStep>      steps;       // one per alias of the record, in its order
		StartNote                  note;        // the counts the start gives (refs, locs, nones, own, notLoaded, forces)
		std::size_t                locsKept = 0;  // location aliases left this game's own: no location can be written here
		std::vector<std::uint32_t> needsEvent;  // aliases left this game's own that only the Story Manager's event fills
		// A start without the event fails when an alias the event fills, and the quest needs, has nothing forced into it.
		[[nodiscard]] bool CanStart() const { return needsEvent.empty(); }
	};

	// The plan of a start for a run: each alias the host's game filled with a form this game has (loaded: a_have) is made
	// a forced fill of it, an optional one the host left empty a forced fill of none, and the rest keep this game's own
	// pick (a created actor, a form not loaded here or of a plugin this game lacks, or a location while a_locWritable is
	// false: the layout of a location alias's record is not known here). An alias left so that only the Story Manager's
	// event fills, and not optional, means the quest cannot start here (CanStart false).
	[[nodiscard]] inline StartPlan PlanStart(const std::vector<AliasRecord>& a_aliases, const std::vector<Fill>* a_fills, bool a_locWritable,
		const std::function<bool(std::uint32_t a_formId, bool a_location)>& a_have)
	{
		StartPlan plan;
		for (const auto& alias : a_aliases) {
			const Fill* said = nullptr;
			if (a_fills) {
				const auto it = std::ranges::find(*a_fills, alias.aliasId, &Fill::aliasId);
				said = it != a_fills->end() ? &*it : nullptr;
			}
			PlanStep step{ alias.aliasId, alias.location, Way::kOwn, 0 };
			const auto want = alias.location ? Fill::kLoc : Fill::kRef;
			if (alias.location && !a_locWritable) {
				++plan.locsKept;
			} else if (said && said->kind == want) {
				if (a_have(said->refId, alias.location)) {
					step.way = alias.location ? Way::kLoc : Way::kRef;
					step.formId = said->refId;
				} else {
					++plan.note.notLoaded;
				}
			} else if (said && said->kind == Fill::kEmpty && alias.optional) {
				step.way = Way::kNone;
			}
			switch (step.way) {
			case Way::kRef:
				++plan.note.refs;
				plan.note.forces.push_back({ step.aliasId, step.formId });
				break;
			case Way::kLoc:
				++plan.note.locs;
				break;
			case Way::kNone:
				++plan.note.nones;
				break;
			case Way::kOwn:
				++plan.note.own;
				if (alias.fill == FillType::kFromEvent && !alias.optional) {
					plan.needsEvent.push_back(alias.aliasId);
				}
				break;
			}
			plan.steps.push_back(step);
		}
		return plan;
	}

	// ---- a location alias's record and the quest's locations, found and checked ------------------------------------

	// Where a location alias's record keeps the location it is forced to: one of LOC_SLOTS pointer-sized slots after the
	// base alias (RE::BGSLocAlias's unknown members), found at the catalog by the aliases the load order forces to a
	// specific location: the slot that holds a known location in every one of them (LOC_PROBE_MIN at least), and in no
	// other slot in all of them. None when the records do not agree: then no location is written here (each game's own).
	inline constexpr std::size_t LOC_SLOTS = 6;
	inline constexpr std::size_t LOC_PROBE_MIN = 5;

	class LocProbe
	{
	public:
		// One location alias forced by its record to a location: a_hits bit i set when slot i holds a known location.
		void Sample(std::uint8_t a_hits)
		{
			++samples;
			for (std::size_t i = 0; i < LOC_SLOTS; ++i) {
				if ((a_hits & (1u << i)) != 0) {
					++hits[i];
				}
			}
		}
		[[nodiscard]] std::optional<std::size_t> Slot() const
		{
			if (samples < LOC_PROBE_MIN) {
				return std::nullopt;
			}
			std::optional<std::size_t> found;
			for (std::size_t i = 0; i < LOC_SLOTS; ++i) {
				if (hits[i] == samples) {
					if (found) {
						return std::nullopt;  // two slots agree: which one the engine reads is not known
					}
					found = i;
				}
			}
			return found;
		}
		[[nodiscard]] std::size_t Samples() const { return samples; }

	private:
		std::array<std::size_t, LOC_SLOTS> hits{};
		std::size_t                        samples = 0;
	};

	// Whether the quest's map of filled location aliases (RE::TESQuest's alias map beside its references', read as alias
	// id -> location) is what this game reads it as: checked on the running quests' location aliases their records force
	// to a location, each of which must hold that one. Until LOC_CHECK_MIN agree (at least LOC_CHECK_SHARE of them) the
	// locations read are said as unknown (each game's own pick); one that disagrees too often makes it bad for good.
	inline constexpr std::size_t LOC_CHECK_MIN = 5;
	inline constexpr std::size_t LOC_CHECK_SHARE = 80;  // per cent

	class LocCheck
	{
	public:
		enum class State : std::uint8_t
		{
			kUnread,    // not enough read yet
			kVerified,  // the map holds the forced locations: locations read are said
			kBad,       // it does not: locations are never said (each game's own pick)
		};

		// A forced location alias of a running quest: whether the map held its record's location.
		void Sample(bool a_held)
		{
			if (state != State::kUnread) {
				return;
			}
			++samples;
			held += a_held ? 1 : 0;
			if (samples >= LOC_CHECK_MIN) {
				state = held * 100 >= samples * LOC_CHECK_SHARE ? State::kVerified : State::kBad;
			}
		}
		[[nodiscard]] State Now() const { return state; }
		[[nodiscard]] std::size_t Samples() const { return samples; }
		[[nodiscard]] std::size_t Held() const { return held; }

	private:
		State       state = State::kUnread;
		std::size_t samples = 0;
		std::size_t held = 0;
	};

	[[nodiscard]] constexpr std::string_view LocStateName(LocCheck::State a_state)
	{
		switch (a_state) {
		case LocCheck::State::kUnread:
			return "unread";
		case LocCheck::State::kVerified:
			return "verified";
		case LocCheck::State::kBad:
			return "bad";
		}
		return "?";
	}

	// ---- the references, compared --------------------------------------------------------------------------------

	// How the session's fills of a quest stand against this game's (a_ours: what its aliases hold here, in this game's
	// numbering, as kEmpty, kRef, kLoc, kRuntime or kUnknown).
	struct Compare
	{
		std::vector<Force> force;      // a reference of the load order the session's alias holds and ours does not
		std::vector<Force> locs;       // a location the session's alias holds and ours does not (given only at a start)
		std::size_t        same = 0;     // the session's references and locations our aliases hold already
		std::size_t        runtime = 0;  // the host's alias holds a reference its game made as it ran: ours stays our own
		std::size_t        unknown = 0;  // ... a form of a plugin this game lacks (or not read): ours stays our own
		std::size_t        extra = 0;    // the host's alias holds nothing, ours something (no alias is cleared here)
		std::size_t        unread = 0;   // the session's alias holds a form, ours cannot be read here (a location while
		                                 // this game's location map is not checked): neither same nor apart
		[[nodiscard]] bool Same() const { return force.empty() && locs.empty() && unread == 0; }
		[[nodiscard]] bool RefsSame() const { return force.empty(); }    // the references alone: what the plain legs read
		[[nodiscard]] bool LocsSame() const { return locs.empty() && unread == 0; }  // only where both games read locations
	};

	[[nodiscard]] inline Compare Against(const std::vector<Fill>& a_session, const std::vector<Fill>& a_ours)
	{
		const auto ours = QuestRuns::Sorted(a_ours);
		const auto find = [&](std::uint32_t a_alias) -> const Fill* {
			const auto it = std::ranges::lower_bound(ours, a_alias, {}, &Fill::aliasId);
			return it != ours.end() && it->aliasId == a_alias ? &*it : nullptr;
		};
		Compare out;
		for (const auto& fill : QuestRuns::Sorted(a_session)) {
			const auto here = find(fill.aliasId);
			switch (fill.kind) {
			case Fill::kRef:
			case Fill::kLoc:
				if (here && here->kind == fill.kind && here->refId == fill.refId) {
					++out.same;
				} else if (here && here->kind == Fill::kUnknown) {
					// this game cannot read what its alias holds (a location while its location map is not checked): not
					// known to be apart, so never forced, restarted for or said apart; each game's own pick
					++out.unread;
				} else if (fill.kind == Fill::kRef) {
					out.force.push_back({ fill.aliasId, fill.refId });
				} else {
					out.locs.push_back({ fill.aliasId, fill.refId });
				}
				break;
			case Fill::kRuntime:
				++out.runtime;
				break;
			case Fill::kUnknown:
				++out.unknown;
				break;
			case Fill::kEmpty:
				if (here && here->kind != Fill::kEmpty) {
					++out.extra;
				}
				break;
			default:
				break;
			}
		}
		return out;
	}

	// The host's game: what one reference alias holds, for the wire (a reference made as the game ran is no other game's).
	[[nodiscard]] inline Fill FillOf(std::uint32_t a_aliasId, std::uint32_t a_refId)
	{
		if (a_refId == 0) {
			return { a_aliasId, 0, Fill::kEmpty };
		}
		if (FormIds::IsRuntime(a_refId)) {
			return { a_aliasId, 0, Fill::kRuntime };
		}
		return { a_aliasId, a_refId, Fill::kRef };
	}

	// ... and one location alias: a_read false when the location could not be read (the map's layout not checked yet, or
	// bad): unknown, each game's own pick.
	[[nodiscard]] inline Fill LocFillOf(std::uint32_t a_aliasId, std::uint32_t a_locId, bool a_read)
	{
		if (!a_read) {
			return { a_aliasId, 0, Fill::kUnknown };
		}
		if (a_locId == 0) {
			return { a_aliasId, 0, Fill::kEmpty };
		}
		if (FormIds::IsRuntime(a_locId)) {
			return { a_aliasId, 0, Fill::kRuntime };
		}
		return { a_aliasId, a_locId, Fill::kLoc };
	}

	// How many of the fills are references (kRef) and locations (kLoc): what a start for the run gives, at most.
	[[nodiscard]] inline std::size_t CountOf(const std::vector<Fill>& a_fills, Fill::Kind a_kind)
	{
		return static_cast<std::size_t>(std::ranges::count(a_fills, static_cast<std::uint8_t>(a_kind), &Fill::kind));
	}

	// The fills as the dev command and the rig read them: "alias:ref" by alias id, ref in hex, "alias:Lloc" for a
	// location, "-" for empty, "rt" for a runtime form, "?" for one of a plugin this game lacks (or not read); "-" for
	// none at all.
	[[nodiscard]] inline std::string Describe(const std::vector<Fill>& a_fills)
	{
		std::string out;
		for (const auto& fill : QuestRuns::Sorted(a_fills)) {
			if (!out.empty()) {
				out += ',';
			}
			out += std::to_string(fill.aliasId);
			out += ':';
			switch (fill.kind) {
			case Fill::kRef:
			case Fill::kLoc:
				{
					char hex[9];
					std::snprintf(hex, sizeof hex, "%08X", fill.refId);
					if (fill.kind == Fill::kLoc) {
						out += 'L';
					}
					out += hex;
					break;
				}
			case Fill::kRuntime:
				out += "rt";
				break;
			case Fill::kUnknown:
				out += '?';
				break;
			default:
				out += '-';
				break;
			}
		}
		return out.empty() ? "-" : out;
	}
}
