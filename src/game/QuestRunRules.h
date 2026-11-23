#pragma once

#include "FormIds.h"
#include "Protocol.h"
#include "QuestRuns.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Quest runs and their aliases (docs/DESIGN.md, "Quests", "Runs and aliases"): the rules, free of the game, by which the
// host's game says the quests it starts and the references their aliases hold, and a friend's game does the same job:
// it starts the quest with the host's references in its aliases, or forces them into a quest that runs there already.
// Tested in tests/QuestRunTests.cpp; src/game/QuestSync.cpp reads and changes the game.
//
// The class is every quest the session shares by its record (QuestRules::Class::kShared), and every radiant job (the
// Story Manager's repeating ones, QuestRules::Class::kRadiant) the host's game starts: a radiant job is the session's
// when, and only when, the host's game runs it, with the host's targets; one a friend's game starts on its own stays
// that friend's own job. Each reference alias is the host's: one filled by conditions, from an event, from another
// alias, a unique actor or a forced reference alike. Not yet: an actor the quest creates for its alias (each game makes
// its own until NPCs are paired), and location aliases (the engine's layout of them is not known: each game's own
// pick).
namespace QuestRunRules
{
	using Clock = std::chrono::steady_clock;
	using Fill = Protocol::AliasFill;
	using Run = Protocol::QuestRun;

	// How long a friend's game waits for a run's fills, and then for the quest it starts with them to run, before it
	// gives the run up: a shared quest's stages are then made as before (its own picks), a radiant job's are dropped (it
	// is never this game's own pick of the job).
	inline constexpr auto START_WAIT = std::chrono::seconds{ 10 };
	// How often a run's references are forced again into the aliases where they do not hold (a reference not loaded
	// yet, or a script that filled the alias again).
	inline constexpr auto FORCE_EVERY = std::chrono::seconds{ 2 };

	// ---- the host's game: its runs ---------------------------------------------------------------------------------

	// The quests the host's game runs as the session's: numbers each start (never twice in the game's session), and says
	// each run's fills when first read and again when they change.
	class Keeper
	{
	public:
		// a_questId started in our game (its event), a radiant job when a_repeats: the word to send.
		Run Started(std::uint32_t a_questId, bool a_repeats)
		{
			auto& q = quests[a_questId];
			q = Quest{};
			q.run = ++counter;
			q.live = true;
			q.repeats = a_repeats;
			Run run;
			run.questId = a_questId;
			run.run = q.run;
			run.flags = a_repeats ? Run::kFresh : 0;
			return run;
		}

		// The references our game's quest holds in its aliases now (polled while it runs): the word to send when they
		// are new to the session (the first read of the run, or a change).
		std::optional<Run> Read(std::uint32_t a_questId, std::vector<Fill> a_fills)
		{
			const auto it = quests.find(a_questId);
			if (it == quests.end() || !it->second.live) {
				return std::nullopt;
			}
			auto& q = it->second;
			auto  fills = QuestRuns::Sorted(std::move(a_fills));
			if (q.filled && q.fills == fills) {
				return std::nullopt;
			}
			q.filled = true;
			q.fills = fills;
			Run run;
			run.questId = a_questId;
			run.run = q.run;
			run.flags = Run::kFilled;
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
			bool              filled = false;
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
		kWaiting,   // heard: waiting for its fills, or for the quest to be started with them
		kStarting,  // this game started the quest with the run's references in its aliases: waiting for it to run
		kRunning,   // the quest runs here as the session's run (started here for it, or adopted: it ran here already)
		kGaveUp,    // its fills or its start did not come within START_WAIT
		kEnded,     // the quest is ended here already: nothing to start
		kNotHere,   // the quest is missing here, or not shared by this game's record
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
		}
		return "?";
	}

	// What Due decided, for the caller to do.
	enum class Act : std::uint8_t
	{
		kNothing,  // nothing now
		kStart,    // start the quest here with the run's references forced into its aliases
		kStarted,  // the quest started here runs: put the aliases' records back, and force what did not hold
		kAdopt,    // the quest ran here already: it is the session's run now; force the run's references into it
		kGiveUp,   // the run's fills or its start did not come: put any records back; its stages go on as Phase says
		kStop,     // a radiant job the session's run of stopped: stop it here too
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
				const bool ours = q.phase == Phase::kRunning || q.phase == Phase::kStarting;
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
			case Phase::kWaiting:
				if (a_here.ended) {
					q.phase = Phase::kEnded;
					return Act::kNothing;
				}
				if (a_here.running) {
					q.phase = Phase::kRunning;
					++adopted;
					return Act::kAdopt;
				}
				if (a_here.starting) {
					return Act::kNothing;  // its own start is under way: adopted once it runs
				}
				if (!q.filled) {
					if (a_now - q.since >= START_WAIT) {
						q.phase = Phase::kGaveUp;
						++gaveUp;
						return Act::kGiveUp;
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
					q.phase = Phase::kGaveUp;
					++gaveUp;
					return Act::kGiveUp;
				}
				return Act::kNothing;
			case Phase::kRunning:
			case Phase::kGaveUp:
			case Phase::kEnded:
			case Phase::kNotHere:
				return Act::kNothing;
			}
			return Act::kNothing;
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

		// The session's stages, objectives and end of a_questId wait (QuestRules::Verdict::kHold): its run is heard and
		// this game has not started the quest for it yet (waiting for its fills, or for its start to run).
		[[nodiscard]] bool Hold(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() && it->second.live && (it->second.phase == Phase::kWaiting || it->second.phase == Phase::kStarting);
		}

		// Whether a_questId runs here as the session's run: a radiant job shared only then (its stages reported and made).
		[[nodiscard]] bool Ours(std::uint32_t a_questId) const
		{
			const auto it = quests.find(a_questId);
			return it != quests.end() && it->second.live && it->second.phase == Phase::kRunning;
		}

		// Our game stopped a_questId itself (its event): a run it ran here is not run here any more.
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

		// The session's runs are no longer this game's word (a welcome, the host's world new, a load, the session's end).
		void Clear() { quests.clear(); }

	private:
		struct Quest
		{
			std::uint32_t     run = 0;
			bool              live = false;
			bool              filled = false;
			bool              stop = false;     // a stop of the run this game ran: made by the next Due
			bool              wasOurs = false;  // an earlier run of the quest ran here as the session's
			Phase             phase = Phase::kWaiting;
			Clock::time_point since{};
			Clock::time_point nextForce{};
			std::vector<Fill> fills;
		};
		std::unordered_map<std::uint32_t, Quest> quests;
		std::uint64_t                            started = 0;
		std::uint64_t                            adopted = 0;
		std::uint64_t                            gaveUp = 0;
	};

	// ---- the references, compared --------------------------------------------------------------------------------

	// One reference to force into an alias here.
	struct Force
	{
		std::uint32_t aliasId = 0;
		std::uint32_t refId = 0;
		bool operator==(const Force&) const = default;
	};

	// How the session's fills of a quest stand against this game's (a_ours: what its aliases hold here, in this game's
	// numbering, as kEmpty, kRef or kRuntime).
	struct Compare
	{
		std::vector<Force> force;    // a reference of the load order the session's alias holds and ours does not
		std::size_t        same = 0;     // the session's references our aliases hold already
		std::size_t        runtime = 0;  // the host's alias holds a reference its game made as it ran: ours stays our own
		std::size_t        unknown = 0;  // ... a reference of a plugin this game lacks: ours stays our own
		std::size_t        extra = 0;    // the host's alias holds nothing, ours something (no alias is cleared here)
		[[nodiscard]] bool Same() const { return force.empty(); }
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
				if (here && here->kind == Fill::kRef && here->refId == fill.refId) {
					++out.same;
				} else {
					out.force.push_back({ fill.aliasId, fill.refId });
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

	// The host's game: what one alias holds, for the wire (a reference made as the game ran is no other game's).
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

	// The fills as the dev command and the rig read them: "alias:ref" by alias id, ref in hex, "-" for empty, "rt" for a
	// runtime reference, "?" for one of a plugin this game lacks; "-" for none at all.
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
				{
					char hex[9];
					std::snprintf(hex, sizeof hex, "%08X", fill.refId);
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
