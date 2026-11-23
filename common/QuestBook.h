#pragma once

#include "FormIds.h"
#include "Protocol.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// What the session says of each shared quest (docs/DESIGN.md, "Quests"): the server's book of the stages done, each
// objective's state and the quest's end, kept for the session's run and sent to each game that joins, and again to one
// that loads a save. It only grows: a stage once done stays done, an objective goes forward only (an end, completed or
// failed, once said stays; shown stays shown, at the latest stage it was read shown at), and the first end a quest is
// said to have stands. The one thing that clears it is the host's game saying its world is new (a welcome, a load:
// kQuestReset), after which its own quests come in as the session's: the host's save is the world. Free of the game and
// of the transports: tests/QuestTests.cpp.
class QuestBook
{
public:
	using Entry = Protocol::QuestEntry;

	enum class Take
	{
		kNews,     // kept, and new to the session: pass it on
		kSame,     // the session had it (or more) already
		kRefused,  // no quest, a runtime one, a dormant objective (says nothing), or the other end of an ended quest
		kFull,     // a new quest past MAX_QUESTS, or a new entry past MAX_QUEST_ENTRIES of its quest
	};

	explicit QuestBook(std::size_t a_maxQuests = Protocol::MAX_QUESTS, std::size_t a_maxEntries = Protocol::MAX_QUEST_ENTRIES) :
		maxQuests(a_maxQuests),
		maxEntries(a_maxEntries)
	{}

	// RE::QUEST_OBJECTIVE_STATE as two parts: whether it is shown (the odd states), and its end (0 none, 1 completed,
	// 2 failed).
	[[nodiscard]] static constexpr bool         Shown(std::uint8_t a_state) { return (a_state & 1) != 0; }
	[[nodiscard]] static constexpr std::uint8_t End(std::uint8_t a_state) { return static_cast<std::uint8_t>(a_state >> 1); }
	[[nodiscard]] static constexpr std::uint8_t State(bool a_shown, std::uint8_t a_end) { return static_cast<std::uint8_t>((a_end << 1) | (a_shown ? 1 : 0)); }

	// a_said laid over a_known, forward only: shown stays shown, and an end once said stays (the first one).
	[[nodiscard]] static constexpr std::uint8_t MergeObjective(std::uint8_t a_known, std::uint8_t a_said)
	{
		const auto end = End(a_known) != 0 ? End(a_known) : End(a_said);
		return State(Shown(a_known) || Shown(a_said), end);
	}

	// Whether an objective's word a_said is ahead of a_known: ended where it was not, shown where it was not, or shown at a
	// later stage of its quest (the quest moved on and the objective is still shown: it holds there too).
	[[nodiscard]] static constexpr bool Ahead(const Entry& a_said, const Entry& a_known)
	{
		if (a_said.kind != Entry::kObjective) {
			return false;  // a stage or an end the session has: nothing more to it
		}
		return End(MergeObjective(a_known.value, a_said.value)) != End(a_known.value) || ShownLater(a_said, a_known);
	}

	// a_said laid over a_known (one objective): the state forward only, and the stage of the latest word that read it shown.
	[[nodiscard]] static constexpr Entry Merge(Entry a_known, const Entry& a_said)
	{
		if (a_known.kind != Entry::kObjective) {
			return a_known;
		}
		if (ShownLater(a_said, a_known)) {
			a_known.stage = a_said.stage;
		}
		a_known.value = MergeObjective(a_known.value, a_said.value);
		return a_known;
	}

	// A player's entry. a_out is the entry as the session now holds it (an objective's merged state), for kNews and
	// kSame.
	Take Report(const Entry& a_entry, Entry& a_out)
	{
		if (a_entry.questId == 0 || FormIds::IsRuntime(a_entry.questId) || a_entry.kind > Entry::KIND_MAX ||
			(a_entry.kind == Entry::kObjective && (a_entry.value > Entry::OBJECTIVE_STATE_MAX || a_entry.value == 0))) {
			return Take::kRefused;
		}
		Entry said = a_entry;
		if (said.kind == Entry::kCompleted || said.kind == Entry::kFailed) {
			said.index = 0;
			said.value = 0;
			said.stage = 0;
		} else if (said.kind == Entry::kStage) {
			said.value = 0;
			said.stage = 0;
		}
		auto it = quests.find(said.questId);
		if (it == quests.end()) {
			if (quests.size() >= maxQuests) {
				return Take::kFull;
			}
			it = quests.emplace(said.questId, Quest{}).first;
			order.push_back(said.questId);
		}
		auto& quest = it->second;
		if (said.kind == Entry::kCompleted || said.kind == Entry::kFailed) {
			const auto other = said.kind == Entry::kCompleted ? Entry::kFailed : Entry::kCompleted;
			if (Find(quest, Entry{ said.questId, other, 0, 0 })) {
				return Take::kRefused;  // it ended the other way first: that stands
			}
		}
		if (auto known = Find(quest, said)) {
			if (!Ahead(said, *known)) {
				a_out = *known;
				return Take::kSame;
			}
			*known = Merge(*known, said);
			a_out = *known;
			return Take::kNews;
		}
		if (quest.entries.size() >= maxEntries) {
			return Take::kFull;
		}
		quest.entries.push_back(said);
		++entries;
		a_out = said;
		return Take::kNews;
	}

	// Every kept entry: the quests in the order first reported, each one's entries in the order they came (its stages
	// in the order they were done, which is the order a game sets them in).
	[[nodiscard]] std::vector<Entry> All() const
	{
		std::vector<Entry> out;
		out.reserve(entries);
		for (const auto id : order) {
			const auto& quest = quests.at(id);
			out.insert(out.end(), quest.entries.begin(), quest.entries.end());
		}
		return out;
	}

	// The session's entry for that thing (a stage, an objective, an end), when it has one.
	[[nodiscard]] const Entry* Find(const Entry& a_what) const
	{
		const auto it = quests.find(a_what.questId);
		return it == quests.end() ? nullptr : Find(it->second, a_what);
	}

	// Whether the session has reached a_stage of the quest: it holds that stage done, or a later one (by number: the
	// order a quest's own scripts set them in, nearly always), or the quest's end.
	[[nodiscard]] bool Reached(std::uint32_t a_questId, std::uint16_t a_stage) const
	{
		const auto it = quests.find(a_questId);
		if (it == quests.end()) {
			return false;
		}
		for (const auto& e : it->second.entries) {
			if ((e.kind == Entry::kStage && e.index >= a_stage) || e.kind == Entry::kCompleted || e.kind == Entry::kFailed) {
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] std::size_t Quests() const { return quests.size(); }
	[[nodiscard]] std::size_t Entries() const { return entries; }

	void Clear()
	{
		quests.clear();
		order.clear();
		entries = 0;
	}

private:
	struct Quest
	{
		std::vector<Entry> entries;  // a few dozen at most: looked through, not indexed
	};

	// a_said reads the objective shown, and a_known does not, or a_known did at an earlier stage.
	[[nodiscard]] static constexpr bool ShownLater(const Entry& a_said, const Entry& a_known)
	{
		return Shown(a_said.value) && (!Shown(a_known.value) || a_said.stage > a_known.stage);
	}

	static Entry* Find(Quest& a_quest, const Entry& a_what)
	{
		for (auto& e : a_quest.entries) {
			if (e.SameThing(a_what)) {
				return &e;
			}
		}
		return nullptr;
	}
	static const Entry* Find(const Quest& a_quest, const Entry& a_what)
	{
		for (const auto& e : a_quest.entries) {
			if (e.SameThing(a_what)) {
				return &e;
			}
		}
		return nullptr;
	}

	std::size_t                               maxQuests;
	std::size_t                               maxEntries;
	std::unordered_map<std::uint32_t, Quest>  quests;
	std::vector<std::uint32_t>                order;
	std::size_t                               entries = 0;
};
