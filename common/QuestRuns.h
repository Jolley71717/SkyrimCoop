#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// The runs of the session's quests (docs/DESIGN.md, "Quests", "Runs and aliases"): the server's book of the host's word
// on each shared quest and radiant job its game started, the references the quest's aliases hold there, and whether a
// radiant job's run is over. Only the host's game says runs (its save is the world: common/Server.cpp refuses anyone
// else's); the book is sent to each game that joins, and again to one that loads a save, before the quests' stages
// (QuestBook), so a game starts a quest with the host's references in its aliases before it makes the quest's stages.
// It is cleared with the quest book when the host's game says its world is new (kQuestReset). Free of the game and of
// the transports: tests/QuestRunTests.cpp.
class QuestRuns
{
public:
	using Run = Protocol::QuestRun;

	enum class Take
	{
		kNews,     // kept, and new to the session: pass it on
		kSame,     // the session had it already (the same run and fills; or a stop of a run that is not the quest's now)
		kRefused,  // no quest, a runtime one, or run 0
		kFull,     // a new quest past the most kept
	};

	struct Result
	{
		Take take = Take::kRefused;
		// The quest's stages, objectives and end the session keeps go (QuestBook::Forget): a radiant job begun again (a
		// new run said fresh) or stopped. Its earlier run's word is not this run's.
		bool forget = false;
	};

	explicit QuestRuns(std::size_t a_maxQuests = Protocol::MAX_QUESTS) :
		maxQuests(a_maxQuests)
	{}

	// The host's word on a quest's run.
	Result Report(const Run& a_said)
	{
		if (a_said.questId == 0 || FormIds::IsRuntime(a_said.questId) || a_said.run == 0) {
			return { Take::kRefused };
		}
		auto it = runs.find(a_said.questId);
		if ((a_said.flags & Run::kStopped) != 0) {
			if (it == runs.end() || it->second.run != a_said.run || !it->second.live) {
				return { Take::kSame };  // not the run the session has on: nothing of it to stop
			}
			it->second.live = false;
			it->second.filled = false;
			it->second.fills.clear();
			return { Take::kNews, true };
		}
		if (it == runs.end()) {
			if (runs.size() >= maxQuests) {
				return { Take::kFull };
			}
			it = runs.emplace(a_said.questId, Kept{}).first;
			order.push_back(a_said.questId);
		}
		auto& kept = it->second;
		const bool filled = (a_said.flags & Run::kFilled) != 0;
		if (kept.run != a_said.run) {
			kept.run = a_said.run;
			kept.live = true;
			kept.filled = filled;
			kept.fills = filled ? Sorted(a_said.fills) : std::vector<Protocol::AliasFill>{};
			return { Take::kNews, (a_said.flags & Run::kFresh) != 0 };
		}
		if (!kept.live || !filled) {
			return { Take::kSame };  // a word on a run already stopped, or its start said again
		}
		auto fills = Sorted(a_said.fills);
		if (kept.filled && kept.fills == fills) {
			return { Take::kSame };
		}
		kept.filled = true;
		kept.fills = std::move(fills);
		return { Take::kNews };
	}

	// Whether the quest's last run is over (a radiant job the host's game stopped, and none begun since): a report of its
	// stages is refused (a friend's word from the run that ended must not start it again in the host's world).
	[[nodiscard]] bool Stopped(std::uint32_t a_questId) const
	{
		const auto it = runs.find(a_questId);
		return it != runs.end() && !it->second.live;
	}

	// Every run that is on, in the order the quests were first said, as the session's word (player 0): with its fills
	// when they were said. Not fresh: what the session keeps of the quest is this run's.
	[[nodiscard]] std::vector<Run> Live() const
	{
		std::vector<Run> out;
		for (const auto id : order) {
			const auto& kept = runs.at(id);
			if (!kept.live) {
				continue;
			}
			Run run;
			run.questId = id;
			run.run = kept.run;
			run.flags = kept.filled ? Run::kFilled : 0;
			run.fills = kept.fills;
			out.push_back(std::move(run));
		}
		return out;
	}

	[[nodiscard]] std::size_t Quests() const { return runs.size(); }

	void Clear()
	{
		runs.clear();
		order.clear();
	}

	// Fills by alias id, one each (the last said of an alias), at most MAX_ALIAS_FILLS.
	[[nodiscard]] static std::vector<Protocol::AliasFill> Sorted(std::vector<Protocol::AliasFill> a_fills)
	{
		std::ranges::stable_sort(a_fills, {}, &Protocol::AliasFill::aliasId);
		std::vector<Protocol::AliasFill> out;
		out.reserve(a_fills.size());
		for (const auto& fill : a_fills) {
			if (!out.empty() && out.back().aliasId == fill.aliasId) {
				out.back() = fill;
			} else {
				out.push_back(fill);
			}
		}
		if (out.size() > Protocol::MAX_ALIAS_FILLS) {
			out.resize(Protocol::MAX_ALIAS_FILLS);
		}
		return out;
	}

private:
	struct Kept
	{
		std::uint32_t                    run = 0;
		bool                             live = false;
		bool                             filled = false;
		std::vector<Protocol::AliasFill> fills;
	};

	std::size_t                              maxQuests;
	std::unordered_map<std::uint32_t, Kept>  runs;
	std::vector<std::uint32_t>               order;
};
