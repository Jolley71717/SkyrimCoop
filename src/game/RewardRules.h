#pragma once

#include "game/LootRules.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

// Quest rewards for everyone (docs/DESIGN.md, "Quests", "Rewards"), free of the game so tests/RewardTests.cpp can run
// every rule; src/game/QuestRewards.cpp reads the game and makes what these decide.
//
// A shared quest's stages are made in every game by the game's own `setstage`, so a reward a stage's fragment gives
// (most of them: gold, the quest's item) is each game's own already. What is not is a reward the conversation gives:
// a topic info's fragment (`Game.GetPlayer().AddItem(...)` as the quest giver says "here, take this") runs only in the
// game of the player who talks. The class is told by what the game shows, never by a list of topics:
// - an item that comes to our player from nothing (no container gave it and no reference was picked up: a script's
//   AddItem) while our player talks with an NPC (the Dialogue Menu open, or shut less than TALK_GRACE ago: an info's
//   end fragment runs as it shuts), with no trade under way (barter, gift, container or training menu open, or shut less
//   than TRADE_GRACE ago);
// - of a quest the session shares here: the quest of the topic info said last, no longer than INFO_WINDOW before;
// - an item kind a reward shares: anything a player carries but a key (a key is a door's: the quest gives it to who
//   opens it);
// - not given by a stage of that quest this game made from the session's word just before (MADE_GRACE): that stage is
//   another player's step, made in every game, so every game's fragment gave it already.
// Such an item is said to the session as that quest's reward (several of the same item within SEND_AFTER are one), and
// every other game gives its player the same. The stage the conversation set may give a reward of its own in each game
// (its fragment), and the two cannot be told apart in the talker's game, so a game gives only what its player did not
// get already: it waits until the quest's stages from the session are made here and SETTLE has passed since the last
// (their fragments ran), then gives the reward less what its player got from nothing since that stage was made
// (Ledger). A reward from the conversation alone is given whole; one its stage gave everywhere is given nowhere twice.
namespace RewardRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	inline constexpr auto TALK_GRACE = 2s;   // an item this long after the Dialogue Menu shut is still the talk's
	inline constexpr auto TRADE_GRACE = 1s;  // a trading menu shut this long ago: still a trade
	inline constexpr auto MADE_GRACE = 2s;   // an item this long after a stage of the quest made here from the session's word is that stage's
	inline constexpr auto INFO_WINDOW = 10s;  // the topic info said last names the quest when said this long before at most
	inline constexpr auto SEND_AFTER = 1s;   // a reward goes this long after its last item (its stage is reported first)
	inline constexpr auto SETTLE = 5s;       // given this long after the quest's last stage made here (its fragments ran)
	inline constexpr auto START_WAIT = 10s;  // a quest not started here: its stages may still come for this long
	inline constexpr auto RECENT = 30s;      // a stage made here this long before the reward came is the reward's stage
	inline constexpr auto GIVE_UP = 120s;    // a reward not given by then is dropped (its quest never came here)
	inline constexpr auto KEEP = 150s;       // the ledger keeps what came this long (longer than GIVE_UP)

	inline constexpr std::uint32_t MAX_COUNT = 1'000'000;  // the most of one item a reward says (Protocol::QuestReward)
	inline constexpr std::size_t   MAX_OUTBOX = 64;
	inline constexpr std::size_t   MAX_LEDGER = 512;
	inline constexpr std::size_t   MAX_WAITING = 64;

	// The item kinds a reward shares: anything a player carries but a key.
	[[nodiscard]] constexpr bool KindShared(LootRules::ItemKind a_kind)
	{
		return a_kind != LootRules::ItemKind::kOther && a_kind != LootRules::ItemKind::kKey;
	}

	// ---- the talker's game: is this item a reward -------------------------------------------------------------------

	// What our game saw of our player's talk, read each tick (the menus) and from the game's topic info event.
	struct Talk
	{
		std::optional<Clock::time_point> dialogue;  // the Dialogue Menu last seen open
		std::optional<Clock::time_point> trade;     // a barter, gift, container or training menu last seen open
		std::uint32_t                    infoQuest = 0;  // the quest of the topic info said last, if the session shares it here
		std::optional<Clock::time_point> infoAt;         // when that info was said
		std::optional<Clock::time_point> madeAt;         // the last stage of that quest made here from the session's word
	};

	// Why an item that came from nothing is or isn't a reward (counted by name in `rewards`).
	enum class Why : std::uint8_t
	{
		kReward,
		kNotTalking,  // no conversation open or just shut: crafting, harvest, a script of its own
		kTrading,     // a trading menu open or just shut
		kNoQuest,     // no topic info said lately, or its quest is not shared here
		kKind,        // a key, or no item a player carries
		kMade,        // a stage of the quest made here from the session's word just gave it (another player's step)
	};
	inline constexpr std::size_t WHY_COUNT = static_cast<std::size_t>(Why::kMade) + 1;

	[[nodiscard]] constexpr const char* WhyName(Why a_why)
	{
		switch (a_why) {
		case Why::kReward:
			return "reward";
		case Why::kNotTalking:
			return "notTalking";
		case Why::kTrading:
			return "trading";
		case Why::kNoQuest:
			return "noQuest";
		case Why::kKind:
			return "kind";
		case Why::kMade:
			return "made";
		}
		return "?";
	}

	// Whether an item that came to our player from nothing at a_at is the quest's reward (Talk::infoQuest).
	[[nodiscard]] inline Why Judge(const Talk& a_talk, Clock::time_point a_at, LootRules::ItemKind a_kind)
	{
		if (!KindShared(a_kind)) {
			return Why::kKind;
		}
		if (!a_talk.dialogue || a_at - *a_talk.dialogue > TALK_GRACE) {
			return Why::kNotTalking;
		}
		if (a_talk.trade && a_at - *a_talk.trade <= TRADE_GRACE) {
			return Why::kTrading;
		}
		if (a_talk.infoQuest == 0 || !a_talk.infoAt || a_at - *a_talk.infoAt > INFO_WINDOW) {
			return Why::kNoQuest;
		}
		if (a_talk.madeAt && a_at >= *a_talk.madeAt && a_at - *a_talk.madeAt <= MADE_GRACE) {
			return Why::kMade;
		}
		return Why::kReward;
	}

	// A reward to say: its quest, its item, how many.
	struct Reward
	{
		std::uint32_t quest = 0;
		std::uint32_t item = 0;
		std::uint32_t count = 0;
		bool operator==(const Reward&) const = default;
	};

	// The rewards our player got, each said SEND_AFTER after its last item: several of the same item for the same quest
	// within that time are one (gold given in parts; a stage's fragment and the info's both giving gold).
	class Outbox
	{
	public:
		// False when full (dropped: a game giving more than MAX_OUTBOX rewards a second is broken, not generous).
		bool Add(const Reward& a_reward, Clock::time_point a_now)
		{
			for (auto& w : waiting) {
				if (w.reward.quest == a_reward.quest && w.reward.item == a_reward.item) {
					w.reward.count = (std::min)(MAX_COUNT, w.reward.count + a_reward.count);
					w.last = a_now;
					return true;
				}
			}
			if (waiting.size() >= MAX_OUTBOX) {
				return false;
			}
			waiting.push_back({ { a_reward.quest, a_reward.item, (std::min)(MAX_COUNT, a_reward.count) }, a_now });
			return true;
		}

		// The rewards due now, in the order they came.
		[[nodiscard]] std::vector<Reward> Due(Clock::time_point a_now)
		{
			std::vector<Reward> out;
			std::erase_if(waiting, [&](const Waiting& a_w) {
				if (a_now - a_w.last < SEND_AFTER) {
					return false;
				}
				out.push_back(a_w.reward);
				return true;
			});
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return waiting.size(); }
		void Clear() { waiting.clear(); }

	private:
		struct Waiting
		{
			Reward            reward;
			Clock::time_point last;
		};
		std::vector<Waiting> waiting;
	};

	// ---- every game: what its player got from nothing ----------------------------------------------------------------

	// The items our player got from nothing lately, whatever gave them (a stage's fragment, a conversation, crafting):
	// what a reward heard from the session is less of (Judge). What a reward takes from it is used up, so two rewards of
	// the same item are each counted against their own.
	class Ledger
	{
	public:
		void Add(std::uint32_t a_item, std::uint32_t a_count, Clock::time_point a_at)
		{
			if (a_item == 0 || a_count == 0) {
				return;
			}
			if (got.size() >= MAX_LEDGER) {
				got.pop_front();
			}
			got.push_back({ a_item, a_count, a_at });
		}

		// Uses up to a_most of a_item that came at a_since or after; how many it used.
		std::uint32_t Take(std::uint32_t a_item, Clock::time_point a_since, std::uint32_t a_most)
		{
			std::uint32_t used = 0;
			for (auto& g : got) {
				if (used >= a_most) {
					break;
				}
				if (g.item != a_item || g.at < a_since || g.count == 0) {
					continue;
				}
				const auto n = (std::min)(g.count, a_most - used);
				g.count -= n;
				used += n;
			}
			return used;
		}

		void Prune(Clock::time_point a_now)
		{
			std::erase_if(got, [&](const Got& a_g) { return a_g.count == 0 || a_now - a_g.at > KEEP; });
		}

		[[nodiscard]] std::size_t Size() const { return got.size(); }
		void Clear() { got.clear(); }

	private:
		struct Got
		{
			std::uint32_t     item = 0;
			std::uint32_t     count = 0;
			Clock::time_point at;
		};
		std::deque<Got> got;
	};

	// ---- another game: giving a reward heard from the session --------------------------------------------------------

	// A reward heard: who got it, and when it came.
	struct Heard
	{
		std::uint32_t     from = 0;
		Reward            reward;
		Clock::time_point at;
	};

	// What this game is with the reward's quest (QuestSync).
	struct QuestHere
	{
		bool                             shared = false;  // the quest is here and the session shares it here
		bool                             ran = false;     // it runs here, has ended, or was stopped part way
		bool                             stagesWaiting = false;  // the session's stages of it still to make here
		std::optional<Clock::time_point> lastMade;        // the last stage of it made here from the session's word
	};

	enum class Verdict : std::uint8_t
	{
		kWait,
		kGive,       // give `give` (0: our player got it all from the stage already)
		kNotShared,  // the quest is not here, or not shared here
		kNotHere,    // the quest never started here: its stages were not made (held far and given up, its own, apart)
		kStale,      // waited GIVE_UP for its stages
	};

	struct Decision
	{
		Verdict       verdict = Verdict::kWait;
		std::uint32_t give = 0;  // with kGive: how many to give
		std::uint32_t had = 0;   // with kGive: how many our player got already (from the ledger, used up)
	};

	// What to do with a heard reward now. With kGive the ledger's share of it is used up.
	[[nodiscard]] inline Decision Judge(const Heard& a_heard, const QuestHere& a_quest, Ledger& a_ledger, Clock::time_point a_now)
	{
		if (!a_quest.shared) {
			return { Verdict::kNotShared };
		}
		if (a_now - a_heard.at >= GIVE_UP) {
			return { Verdict::kStale };
		}
		if (a_quest.stagesWaiting) {
			return { Verdict::kWait };
		}
		if (!a_quest.ran) {
			return { a_now - a_heard.at < START_WAIT ? Verdict::kWait : Verdict::kNotHere };
		}
		// The reward's stage is the last one made here, when it was made lately: what came from nothing since a moment
		// before it is what its fragment gave. Otherwise (a conversation that set no stage) only what came since the word.
		const bool recent = a_quest.lastMade && *a_quest.lastMade + RECENT >= a_heard.at;
		const auto from = recent ? (std::max)(*a_quest.lastMade, a_heard.at) : a_heard.at;
		if (a_now < from + SETTLE) {
			return { Verdict::kWait };
		}
		const auto since = recent ? *a_quest.lastMade - 1s : a_heard.at - 2s;
		const auto had = a_ledger.Take(a_heard.reward.item, since, a_heard.reward.count);
		return { Verdict::kGive, a_heard.reward.count - had, had };
	}
}
