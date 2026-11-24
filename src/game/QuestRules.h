#pragma once

#include "FormIds.h"
#include "Protocol.h"
#include "QuestBook.h"
#include "StageRows.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Shared quests (docs/DESIGN.md, "Quests"): which quests a session shares, what a game reports, and how the session's
// word is made in a game that is behind: a stage at a time, in order, forward only. Kept free of the game so every rule
// is tested (tests/QuestTests.cpp); src/game/QuestSync.cpp reads the game and makes what these decide.
//
// A quest is judged by what its record says, never by a name list alone, so a mod's quest is shared as the base game's
// are and the load order does not matter: its type (the journal's tab: the main quest, the guilds, the Daedric quests,
// side quests, the Civil War, Dawnguard and Dragonborn are shared; misc and untyped quests are each game's own in this
// version), and whether the Story Manager starts it for an event again and again (a radiant job: shared while it runs as
// the host's run, with the host's targets, QuestRunRules.h; a friend's own job stays its own). A short list names the typed
// quests that are each player's own anyway (the Helgen opening). Every game judges again by its own record before it
// makes anything.
namespace QuestRules
{
	using Clock = std::chrono::steady_clock;
	using Entry = Protocol::QuestEntry;

	// ---- which quests are shared ----------------------------------------------------------------------------------

	// What the game knows of a quest (QuestSync reads it from TESQuest).
	struct Facts
	{
		std::uint8_t  type = 0;     // QUEST_DATA::Type: 0 none, 1 main, 2 College, 3 Thieves Guild, 4 Dark Brotherhood,
		                            // 5 Companions, 6 misc, 7 Daedric, 8 side, 9 Civil War, 10 Dawnguard, 11 Dragonborn
		std::int32_t  event = -1;   // QuestEvent: the Story Manager event that starts it, -1 none
		std::uint16_t flags = 0;    // QuestFlag
		std::string   editorId;     // its editor ID (quests keep theirs in the game)
	};

	inline constexpr std::uint16_t FLAG_RUN_ONCE = 1 << 8;  // QuestFlag::kRunOnce

	enum class Class : std::uint8_t
	{
		kShared,   // its stages, objectives and end are the session's
		kOwn,      // a typed quest each player plays for themselves (OWN_QUESTS)
		kRadiant,  // the Story Manager starts it for an event, again and again: a run of the host's is shared (QuestRunRules.h)
		kMisc,     // the misc tab ("more quests": shared later, once a scan shows which play the same everywhere)
		kUntyped,  // no journal tab: the game's machinery (dialogue, followers, radiant trackers, encounters)
		kNoName,   // no editor ID: nothing to name it by when it is made here (QuestSync makes stages by the console)
	};

	[[nodiscard]] constexpr std::string_view ClassName(Class a_class)
	{
		switch (a_class) {
		case Class::kShared:
			return "shared";
		case Class::kOwn:
			return "own";
		case Class::kRadiant:
			return "radiant";
		case Class::kMisc:
			return "misc";
		case Class::kUntyped:
			return "untyped";
		case Class::kNoName:
			return "noname";
		}
		return "?";
	}

	// Typed quests that are each player's own, by editor ID (any case), each for a reason the record does not show.
	// The audit of every quest (docs/FEATURES.md, "What stays each player's own") adds to it; a mod's quest is judged
	// by its record like any other.
	struct OwnQuest
	{
		std::string_view editorId;
		std::string_view why;
	};
	inline constexpr std::array OWN_QUESTS{
		OwnQuest{ "MQ101", "the Helgen opening: each player plays it alone, and joins after it" },
		OwnQuest{ "RelationshipMarriage", "marriage is a character's own" },
		OwnQuest{ "BYOHRelationshipAdoption", "adoption is a character's own" },
	};

	[[nodiscard]] inline bool SameName(std::string_view a_a, std::string_view a_b)
	{
		return a_a.size() == a_b.size() &&
		       std::equal(a_a.begin(), a_a.end(), a_b.begin(), [](char a_x, char a_y) { return std::tolower(static_cast<unsigned char>(a_x)) == std::tolower(static_cast<unsigned char>(a_y)); });
	}

	[[nodiscard]] inline const OwnQuest* OwnQuestOf(std::string_view a_editorId)
	{
		const auto it = std::ranges::find_if(OWN_QUESTS, [&](const OwnQuest& a_own) { return SameName(a_own.editorId, a_editorId); });
		return it == OWN_QUESTS.end() ? nullptr : &*it;
	}

	[[nodiscard]] inline Class Classify(const Facts& a_facts)
	{
		if (a_facts.type == 0 || a_facts.type > 11) {
			return Class::kUntyped;
		}
		if (a_facts.type == 6) {
			return Class::kMisc;
		}
		if (a_facts.editorId.empty()) {
			return Class::kNoName;
		}
		if (OwnQuestOf(a_facts.editorId)) {
			return Class::kOwn;
		}
		// Started by the Story Manager for an event and not once only: a job each game's Story Manager picks for itself
		// (its targets come from the event), so the same quest is a different job in each game.
		if (a_facts.event >= 0 && (a_facts.flags & FLAG_RUN_ONCE) == 0) {
			return Class::kRadiant;
		}
		return Class::kShared;
	}

	// ---- stages that are not made as they are ---------------------------------------------------------------------

	// The stages known to move the player (StageVerdict::kMoves) or to be each game's own (kOwn), StageRule.h's rows in
	// StageRows.h: written by tools/questscan.ps1 -Write from a scan of the game's own quest fragments (every stage of a
	// typed quest whose fragment moves the player, fades the screen out or takes their controls away, itself or through
	// the scripts it calls and the stages it sets, as the sister project's quest scan finds its stages that move the
	// player), sorted (RuleLess). Every stage the session's word made here that moved our player is still said in the
	// log (MoveGuard): one the scan did not see (a scene's or a magic effect's move) is a row to add by hand there.
	[[nodiscard]] inline const StageRule* StageRowOf(std::span<const StageRule> a_rules, std::string_view a_editorId, std::uint16_t a_stage)
	{
		const StageRule key{ a_editorId, a_stage };
		const auto      it = std::lower_bound(a_rules.begin(), a_rules.end(), key, RuleLess);
		if (it != a_rules.end() && it->stage == a_stage && CompareName(it->editorId, a_editorId) == 0) {
			return &*it;
		}
		return nullptr;
	}

	[[nodiscard]] inline std::optional<StageVerdict> StageRuleOf(std::span<const StageRule> a_rules, std::string_view a_editorId, std::uint16_t a_stage)
	{
		if (const auto row = StageRowOf(a_rules, a_editorId, a_stage)) {
			return row->verdict;
		}
		return std::nullopt;
	}

	// The row of a quest's start-up stage, if it has one (StageRule::startUp). Any stage made of a quest not started here
	// starts it (setstage), and the engine then runs its start-up stage unjudged: so a stage made while the quest is not
	// started is judged by this row too (StageRowHere).
	[[nodiscard]] inline const StageRule* StartUpRowOf(std::span<const StageRule> a_rules, std::string_view a_editorId)
	{
		const StageRule key{ a_editorId, 0 };
		for (auto it = std::lower_bound(a_rules.begin(), a_rules.end(), key, RuleLess); it != a_rules.end() && CompareName(it->editorId, a_editorId) == 0; ++it) {
			if (it->startUp) {
				return &*it;
			}
		}
		return nullptr;
	}

	// The row a stage is judged by here: its own, or, while its quest is not started here, its quest's start-up stage's.
	[[nodiscard]] inline const StageRule* StageRowHere(std::span<const StageRule> a_rules, std::string_view a_editorId, std::uint16_t a_stage, bool a_started)
	{
		if (const auto row = StageRowOf(a_rules, a_editorId, a_stage)) {
			return row;
		}
		return a_started ? nullptr : StartUpRowOf(a_rules, a_editorId);
	}

	// ---- reading the game's stages: can the done bit be trusted? ---------------------------------------------------

	// The game keeps, for each of a quest's stages, whether it is done: bit 0 of the stage's flags as the engine holds
	// them (the record's own flags use bits 1-3). That is read from the engine, not documented, so each game checks it
	// against what it must be before it says its world from it (the host's snapshot) or skips a stage as done: a quest
	// never started has no stage done, and a running quest's current stage (the last one set) is done.
	struct BitSample
	{
		bool          neverRun = false;   // not running, never run, not completed
		bool          running = false;    // enabled
		std::uint16_t current = 0;        // its current stage
		bool          currentDone = false;  // ... read done by the bit
		std::size_t   doneCount = 0;      // its stages read done
	};

	struct BitVerdict
	{
		bool        trusted = false;
		std::size_t neverRunWithDone = 0;  // quests never run with a stage read done (must be 0)
		std::size_t currentChecked = 0;    // running quests past stage 0
		std::size_t currentDone = 0;       // ... whose current stage reads done
	};

	// Trusted when no quest never run reads a stage done, and at least 9 in 10 of the running quests past stage 0 read
	// their current stage done (a stage set by a script waits a frame or so before it is done; a game that runs no such
	// quest yet, a new game, says nothing against it).
	[[nodiscard]] inline BitVerdict JudgeBit(std::span<const BitSample> a_samples)
	{
		BitVerdict out;
		for (const auto& s : a_samples) {
			if (s.neverRun && s.doneCount != 0) {
				++out.neverRunWithDone;
			}
			if (s.running && s.current != 0) {
				++out.currentChecked;
				out.currentDone += s.currentDone ? 1 : 0;
			}
		}
		out.trusted = out.neverRunWithDone == 0 && out.currentDone * 10 >= out.currentChecked * 9;
		return out;
	}


	// ---- what a game reports ---------------------------------------------------------------------------------------

	// What this game knows of the session's quests: the session's word as heard (a QuestBook, unlimited); the reports of
	// ours still out (sent, not answered), so a change is reported once, not again with every poll; what this game held
	// when the session's word was last new to it (its base: a welcome, the host's reset, a load), which is this game's
	// own past and never reported; and what the session would not take of ours (refused, or its book full), which stays
	// this game's own.
	class View
	{
	public:
		// Our game reads a_read (a stage done, an objective's state, an end): whether to report it. It is reported when
		// it is ahead of the session's word, of this game's base and of what the session would not take, and no report
		// of the same is out; it then counts as out until answered.
		bool Offer(const Entry& a_read)
		{
			if (a_read.kind == Entry::kObjective && a_read.value == 0) {
				return false;  // dormant says nothing
			}
			for (const auto* book : { &session, &base, &own }) {
				if (const auto known = book->Find(a_read); known && !QuestBook::Ahead(a_read, *known)) {
					return false;
				}
			}
			if (IsOut(a_read)) {
				return false;
			}
			out.push_back(a_read);
			return true;
		}

		// The session's word (kRemoteQuests): true when it is new to this game's view. a_ours: it is our own report,
		// which it answers.
		bool Heard(const Entry& a_entry, bool a_ours)
		{
			if (a_ours) {
				TakeOut(a_entry);
			}
			Entry merged;
			return session.Report(a_entry, merged) == QuestBook::Take::kNews;
		}

		// The server's answer to the oldest report of ours out on that thing (kQuestAnswer; its value says why it was
		// not news): the session has it (learnt as the session's), it is refused or the book is full (kept as this game's
		// own: not offered again unless it moves on), or busy (the caller sends it again). The report it answers, if one
		// was out.
		std::optional<Entry> Answered(const Entry& a_answer)
		{
			auto mine = TakeOut(a_answer);
			if (!mine) {
				return std::nullopt;
			}
			Entry merged;
			switch (a_answer.value) {
			case Entry::kSame:
				session.Report(*mine, merged);
				break;
			case Entry::kRefused:
			case Entry::kFull:
				own.Report(*mine, merged);
				break;
			default:
				break;
			}
			return mine;
		}

		// A report sent again that was never offered (one of the host's snapshot answered busy): out until answered.
		void MarkOut(const Entry& a_entry)
		{
			if (!IsOut(a_entry)) {
				out.push_back(a_entry);
			}
		}

		// What this game holds now, taken as its base (a welcome, the host's reset, a load): none of it is reported, only
		// what moves on from it. The session's word on it is still made here where this game is behind.
		void Rebase(std::span<const Entry> a_held)
		{
			base.Clear();
			Entry merged;
			for (const auto& e : a_held) {
				base.Report(e, merged);
			}
		}

		[[nodiscard]] const Entry* Known(const Entry& a_what) const { return session.Find(a_what); }
		[[nodiscard]] bool         Reached(std::uint32_t a_questId, std::uint16_t a_stage) const { return session.Reached(a_questId, a_stage); }
		[[nodiscard]] std::size_t  Quests() const { return session.Quests(); }
		[[nodiscard]] std::size_t  Entries() const { return session.Entries(); }
		[[nodiscard]] std::size_t  Outstanding() const { return out.size(); }
		[[nodiscard]] std::size_t  Based() const { return base.Entries(); }
		[[nodiscard]] std::size_t  Kept() const { return own.Entries(); }
		[[nodiscard]] std::vector<Entry> All() const { return session.All(); }

		// The session's word forgotten (a welcome, a load, the host's world new), and with it what it would not take;
		// reports out stay out until answered.
		void Clear()
		{
			session.Clear();
			own.Clear();
		}
		void ClearAll()
		{
			Clear();
			base.Clear();
			out.clear();
		}

		// Every report of ours out forgotten, unanswered (a friend's game at the host's reset): the server drops what it
		// hears of the world before the reset without an answer, and the answers to the reports it handled before arrived
		// ahead of the reset; an entry left out would take the answer to a later report on the same thing. How many.
		std::size_t DropOut() { return std::exchange(out, {}).size(); }

		// Everything of one quest forgotten (a radiant job begun again or stopped: its earlier run's word is not this
		// run's): the session's word on it, this game's base and own, and its reports out (their answers, when they come,
		// answer nothing).
		void Forget(std::uint32_t a_questId)
		{
			session.Forget(a_questId);
			base.Forget(a_questId);
			own.Forget(a_questId);
			std::erase_if(out, [&](const Entry& a_out) { return a_out.questId == a_questId; });
		}

	private:
		[[nodiscard]] bool IsOut(const Entry& a_entry) const
		{
			return std::ranges::any_of(out, [&](const Entry& a_out) { return a_out.SameThing(a_entry) && !QuestBook::Ahead(a_entry, a_out); });
		}

		// The oldest report of ours out on that thing, taken out (answers come in the order the reports went).
		std::optional<Entry> TakeOut(const Entry& a_entry)
		{
			const auto it = std::ranges::find_if(out, [&](const Entry& a_out) { return a_out.SameThing(a_entry); });
			if (it == out.end()) {
				return std::nullopt;
			}
			auto mine = *it;
			out.erase(it);
			return mine;
		}

		QuestBook          session{ SIZE_MAX, SIZE_MAX };
		QuestBook          base{ SIZE_MAX, SIZE_MAX };
		QuestBook          own{ SIZE_MAX, SIZE_MAX };
		std::vector<Entry> out;
	};

	// A quest further on in this game than in the session when the session's word was last new to it (a friend that
	// joins with a save ahead of the host's, or whose host loaded an older save): its stages and objectives here are
	// this game's own until the session reaches the stage this game was at, so a friend's world apart from the host's
	// never runs on in the host's. Judged a while after the base is taken (APART_WAIT: the session's word comes right
	// behind the welcome or the reset); the host's game is never apart, its world is the session's.
	inline constexpr auto APART_WAIT = std::chrono::seconds{ 5 };

	class Apart
	{
	public:
		// The base taken at a_now: each quest started here and the stage it is at.
		void Rebase(std::span<const std::pair<std::uint32_t, std::uint16_t>> a_at, Clock::time_point a_now)
		{
			at.clear();
			for (const auto& [quest, stage] : a_at) {
				if (stage != 0) {
					at[quest] = Quest{ stage, false };
				}
			}
			since = a_now;
		}

		enum class Say : std::uint8_t
		{
			kReport,  // the quest is with the session: report what moves on here
			kWait,    // the session's word on it may still be coming: hold the report
			kOwn,     // this game is further on than the session: what moves on here is its own
		};

		// What moves on here in a_questId: whether to report it.
		Say Judge(std::uint32_t a_questId, const View& a_view, Clock::time_point a_now)
		{
			const auto it = at.find(a_questId);
			if (it == at.end()) {
				return Say::kReport;
			}
			if (a_view.Reached(a_questId, it->second.stage)) {
				at.erase(it);  // the session came this far: with it from now on
				return Say::kReport;
			}
			if (a_now - since < APART_WAIT) {
				return Say::kWait;
			}
			if (!it->second.said) {
				it->second.said = true;
				++quests;
				lastQuest = a_questId;
			}
			return Say::kOwn;
		}

		// The stage this game was at in a quest still apart (0 when it is not).
		[[nodiscard]] std::uint16_t StageOf(std::uint32_t a_questId) const
		{
			const auto it = at.find(a_questId);
			return it == at.end() ? 0 : it->second.stage;
		}

		[[nodiscard]] std::uint64_t Quests() const { return quests; }  // quests found apart (each once a base)
		[[nodiscard]] std::uint32_t LastQuest() const { return lastQuest; }
		void Clear() { at.clear(); }
		// A radiant job begun again or stopped: where this game was in its earlier run says nothing of this one.
		void Forget(std::uint32_t a_questId) { at.erase(a_questId); }

	private:
		struct Quest
		{
			std::uint16_t stage = 0;
			bool          said = false;
		};
		std::unordered_map<std::uint32_t, Quest> at;
		Clock::time_point                        since{};
		std::uint64_t                            quests = 0;
		std::uint32_t                            lastQuest = 0;
	};


	// Reports the server answered busy, to send again after AGAIN_AFTER.
	inline constexpr auto AGAIN_AFTER = std::chrono::seconds{ 2 };

	// ---- making the session's word here ---------------------------------------------------------------------------

	// How long a stage made here may take to read done before the next stage of its quest goes anyway (counted stuck).
	inline constexpr auto STAGE_WAIT = std::chrono::seconds{ 5 };
	// Between two things made here (each is a console command, and a stage runs its fragment): a burst of catching up
	// runs a few a second, not a frame's worth.
	inline constexpr auto MAKE_EVERY = std::chrono::milliseconds{ 250 };
	// After a quest's last stage was made here, how long its objectives and its end are left to its own fragments before
	// the session's word on them is made directly.
	inline constexpr auto SETTLE = std::chrono::seconds{ 5 };
	// The session's past (a join, a load, the host's world) comes in batches, a quest's end after its stages, and a
	// quest's entries may span two of them. A quest not started here is not started from it until no word of the past
	// has come for PAST_SETTLE: its end is known by then, and one the session ended is put aside (kParked), not begun.
	inline constexpr auto PAST_SETTLE = std::chrono::seconds{ 3 };
	// The most entries waiting to be made: past it a new one is dropped (counted).
	inline constexpr std::size_t MAX_WANTS = 8192;
	// A friend this near (in one space) is "near" for a stage that moves the player.
	inline constexpr float NEAR_FRIEND = 4096.0f;
	// Nearness for such a stage is decided when its word is heard, not when it is made: the stage has moved its doer by
	// then (its fragment runs in the frame it is set), so their stand-in may already be at the destination. A friend
	// read near within NEAR_LATELY before the word came was near at the step.
	inline constexpr auto NEAR_LATELY = std::chrono::seconds{ 5 };
	// A stage held for a far friend (kFar) is given up (made the past: not made, later stages go on) after FAR_WAIT, or
	// at once when that friend leaves the session: a held stage holds its quest's later stages, and no friend is left
	// stuck behind one.
	inline constexpr auto FAR_WAIT = std::chrono::seconds{ 60 };

	// What this game reads of one thing the session says, for Judge.
	struct Here
	{
		bool                        exists = false;   // the quest is in this game
		Class                       cls = Class::kUntyped;
		bool                        has = false;      // the stage is done here, the objective as far on, the end so
		bool                        started = false;  // the quest runs here, or has ended here
		std::uint16_t               current = 0;      // its current stage here (the last one set)
		std::optional<StageVerdict> rule;             // a stage's row in STAGE_RULES
		bool                        reporterNear = false;  // the player who did it is near ours
		// The quest's run is heard and this game has not started the quest for it yet (QuestRunRules::Follower::Hold):
		// everything of it waits, so its stages are made in the quest started with the host's references.
		bool                        hold = false;
		// The session's word has the quest ended (completed or failed): one the host's world finished, before the session
		// or in it.
		bool                        sessionEnded = false;

		// This game ran the quest: it runs, has ended, or was stopped part way (it has a current stage). The one notion of
		// "started here" the rules use (kParked, PAST_SETTLE, kPassed, kStopped).
		[[nodiscard]] bool Ran() const { return started || current != 0; }
	};

	// What of an objective's word this game is to make: show it, and end it (1 completed, 2 failed; 0 nothing). Its end
	// is made whatever stage the quest is at here (an end stays). Its shown part only while the quest here is at the
	// stage the word was read at: a later stage's own script may have withdrawn it (SetObjectiveDisplayed(n, false)),
	// and the game at that stage did so here too, so it is not shown again from an older word.
	struct ObjectiveMake
	{
		bool         show = false;
		std::uint8_t end = 0;
		[[nodiscard]] bool Any() const { return show || end != 0; }
	};

	[[nodiscard]] inline ObjectiveMake ObjectiveToMake(const Entry& a_said, std::uint8_t a_ours, std::uint16_t a_current)
	{
		ObjectiveMake make;
		make.show = QuestBook::Shown(a_said.value) && !QuestBook::Shown(a_ours) && a_said.stage == a_current;
		if (QuestBook::End(a_ours) == 0) {
			make.end = QuestBook::End(a_said.value);
		}
		return make;
	}

	enum class Verdict : std::uint8_t
	{
		kMake,        // make it here now
		kHas,         // this game has it already: done
		kMissing,     // no such quest here (a mod this game lacks, or a form of another kind): dropped
		kNotHere,     // the quest is not shared by this game's own record: dropped
		kOwn,         // a stage the rules keep each game's own: dropped
		kFar,         // a stage that moves the player, said by a friend who was not near when they set it: held until
		              // they are near (Maker gives it up after FAR_WAIT, or when they leave: kFarPast)
		kFarPast,     // ... said as the session's word (a join, a load: no friend did it now), or given up: dropped, not made
		kPassed,      // a stage below the current one of a quest that ran here (Here::Ran: stopped part way too): this game
		              // went past it (done, or a branch it did not take), so it is never set again (its fragment would run
		              // twice): dropped
		kNotStarted,  // an objective or end of a quest not started here (its stages bring it, or were not made): dropped
		kHold,        // the quest's run is heard and not started here yet (its fills, its start): held, its later wants too
		kParked,      // the session's past (a join, a load) on a quest it has ended that this game never started: put aside,
		              // not made and not dropped, until the quest starts here (its own play, or a quest made here starts it)
		kStopped,     // a quest this game stopped part way without ending it (another branch, its own stop): its stages from
		              // there on, objectives and end are not made (setstage would start it again): dropped
	};

	[[nodiscard]] constexpr std::string_view VerdictName(Verdict a_verdict)
	{
		switch (a_verdict) {
		case Verdict::kMake:
			return "make";
		case Verdict::kHas:
			return "has";
		case Verdict::kMissing:
			return "missing";
		case Verdict::kNotHere:
			return "nothere";
		case Verdict::kOwn:
			return "own";
		case Verdict::kFar:
			return "far";
		case Verdict::kFarPast:
			return "farpast";
		case Verdict::kPassed:
			return "passed";
		case Verdict::kNotStarted:
			return "notstarted";
		case Verdict::kHold:
			return "hold";
		case Verdict::kParked:
			return "parked";
		case Verdict::kStopped:
			return "stopped";
		}
		return "?";
	}

	// One thing the session said, to be made here; a_reporter 0 is the session's word sent again (a join, a load).
	[[nodiscard]] inline Verdict Judge(const Entry& a_entry, std::uint32_t a_reporter, const Here& a_here)
	{
		if (!a_here.exists) {
			return Verdict::kMissing;
		}
		if (a_here.hold) {
			return Verdict::kHold;  // judged again once the quest is started here for its run (or the run is given up)
		}
		if (a_here.cls != Class::kShared) {
			return Verdict::kNotHere;
		}
		if (a_here.has) {
			return Verdict::kHas;
		}
		// A quest the host's world finished that this game never started (the host played it before the session, or the
		// friend's save is from before it): the friend's world is not walked through every quest the host ever ended, one
		// after another, at a join. Its word waits until the quest starts here, and is made then; a quest started here, a
		// stage or end said live, are made as ever.
		if (!a_here.Ran() && a_reporter == 0 && a_here.sessionEnded) {
			return Verdict::kParked;
		}
		// Stopped here part way (ran, not running, not ended): the session's word does not start it again. Its stages below
		// where it stopped are passed (below), the rest dropped.
		if (a_here.Ran() && !a_here.started && !(a_entry.kind == Entry::kStage && a_entry.index < a_here.current)) {
			return Verdict::kStopped;
		}
		if (a_entry.kind != Entry::kStage) {
			return a_here.started ? Verdict::kMake : Verdict::kNotStarted;
		}
		if (a_here.Ran() && a_entry.index < a_here.current) {
			return Verdict::kPassed;
		}
		if (a_here.rule) {
			if (*a_here.rule == StageVerdict::kOwn) {
				return Verdict::kOwn;
			}
			if (!a_here.reporterNear) {
				return a_reporter == 0 ? Verdict::kFarPast : Verdict::kFar;
			}
		}
		return Verdict::kMake;
	}

	// The most wants put aside for quests not started here (kParked); past it a new one is dropped (counted).
	inline constexpr std::size_t MAX_PARKED = 8192;

	// The session's entries waiting to be made here, in the order heard, and the pace they are made at: one thing at a
	// time; one stage of a quest at a time (its fragment runs, and may set the next stage itself); a quest's objectives
	// and end only once its stages are made and SETTLE has passed (its fragments set most of them); a stage held for a
	// far friend holds nothing else. A want judged kParked is put aside by its quest until Unpark.
	class Maker
	{
	public:
		struct Want
		{
			Entry             entry;
			std::uint32_t     reporter = 0;
			Clock::time_point heard{};
			bool              nearAtStep = false;  // the friend who did it was near ours when its word was heard
			bool              gone = false;        // ... and has left the session since
			bool              held = false;        // judged kFar once (said once, TakeHeld)
		};

		// Why a held stage (kFar) was given up.
		enum class Release : std::uint8_t
		{
			kNone,
			kLeft,    // the friend who did it left the session
			kWaited,  // FAR_WAIT passed with them not near
		};

		// What Next decided, for the caller to make (kMake) or count.
		struct Step
		{
			Want    want;
			Verdict verdict = Verdict::kMake;
			Release release = Release::kNone;  // a kFarPast given up from a hold
		};

		using Look = std::function<Here(const Entry&, std::uint32_t)>;

		// a_entry heard from the session (new to this game's view). False when the queue is full (dropped). a_nearAtStep:
		// the friend who did it (a_reporter) was near ours as the word came (NEAR_LATELY), for a stage that moves them.
		bool Add(const Entry& a_entry, std::uint32_t a_reporter, Clock::time_point a_now, bool a_nearAtStep = false)
		{
			if (a_reporter == 0) {
				lastPast = a_now;
			}
			if (wants.size() >= MAX_WANTS) {
				++dropped;
				return false;
			}
			// a newer word on the same objective replaces the older one waiting
			for (auto& w : wants) {
				if (w.entry.SameThing(a_entry)) {
					w.entry = a_entry.kind == Entry::kObjective ? QuestBook::Merge(w.entry, a_entry) : a_entry;
					if (w.reporter != a_reporter) {
						w.nearAtStep = false;
						w.gone = false;
					}
					w.reporter = a_reporter;
					w.nearAtStep = w.nearAtStep || a_nearAtStep;
					return true;
				}
			}
			wants.push_back({ a_entry, a_reporter, a_now, a_nearAtStep });
			return true;
		}

		// A friend left the session: the stages held for them are given up at the next Next.
		void Left(std::uint32_t a_playerId)
		{
			if (a_playerId == 0) {
				return;
			}
			for (auto& w : wants) {
				if (w.reporter == a_playerId) {
					w.gone = true;
				}
			}
		}

		// The wants newly held for a far friend since the last call (each said once in the log).
		[[nodiscard]] std::vector<Want> TakeHeld() { return std::exchange(newlyHeld, {}); }

		// Our game read a stage of a quest done (its event): a stage made here and waited for is done.
		void Done(std::uint32_t a_questId, std::uint16_t a_stage, Clock::time_point a_now)
		{
			const auto it = flight.find(a_questId);
			if (it != flight.end() && it->second.stage == a_stage) {
				flight.erase(it);
				lastStage[a_questId] = a_now;
			}
		}

		// The next thing to do now: every want judged in order until one is to be made (returned, kMake, taken out and,
		// for a stage, waited for) or none is due. Wants judged dropped (has, missing, not here, own, far past) are taken
		// out and returned one at a time too, so the caller counts them; one held (far) stays.
		std::optional<Step> Next(Clock::time_point a_now, const Look& a_look)
		{
			// stages made and never read done: their quests go on anyway
			for (auto it = flight.begin(); it != flight.end();) {
				if (a_now - it->second.since >= STAGE_WAIT) {
					++stuck;
					lastStuck = Entry{ it->first, Entry::kStage, it->second.stage, 0 };
					lastStage[it->first] = a_now;
					it = flight.erase(it);
				} else {
					++it;
				}
			}
			const bool paced = a_now - lastMade < MAKE_EVERY;
			std::unordered_set<std::uint32_t> blocked;  // quests with a stage before this want still to make
			for (auto it = wants.begin(); it != wants.end(); ++it) {
				const auto& e = it->entry;
				if (flight.contains(e.questId) || blocked.contains(e.questId)) {
					continue;
				}
				if (e.kind != Entry::kStage) {
					// objectives and ends: after the quest's stages, and SETTLE after the last one made here
					const auto last = lastStage.find(e.questId);
					if (HasStageWaiting(e.questId) || (last != lastStage.end() && a_now - last->second < SETTLE)) {
						continue;
					}
				}
				auto release = Release::kNone;
				Here       here;
				const auto verdict = JudgeWant(*it, a_now, a_look, release, here);
				if (verdict == Verdict::kFar && !it->held) {
					it->held = true;
					newlyHeld.push_back(*it);
				}
				if (verdict == Verdict::kFar || verdict == Verdict::kHold) {
					blocked.insert(e.questId);  // its later stages wait behind it
					continue;
				}
				if ((verdict == Verdict::kMake || verdict == Verdict::kNotStarted) && it->reporter == 0 && !here.Ran() && a_now - lastPast < PAST_SETTLE) {
					blocked.insert(e.questId);  // the past may still say it ended (PAST_SETTLE)
					continue;
				}
				if (verdict == Verdict::kMake) {
					if (paced) {
						return std::nullopt;
					}
					lastMade = a_now;
					if (e.kind == Entry::kStage) {
						flight[e.questId] = Flight{ e.index, a_now };
					}
				}
				Step step{ *it, verdict, release };
				wants.erase(it);
				if (verdict == Verdict::kParked) {
					Park(step.want);
				}
				return step;
			}
			return std::nullopt;
		}

		// The wants held for a far friend (kFar), for the dev command.
		[[nodiscard]] std::size_t Held(const Look& a_look, Clock::time_point a_now) const
		{
			std::size_t n = 0;
			for (const auto& w : wants) {
				auto release = Release::kNone;
				Here here;
				if (w.entry.kind == Entry::kStage && JudgeWant(w, a_now, a_look, release, here) == Verdict::kFar) {
					++n;
				}
			}
			return n;
		}

		// The wants, in order (the dev command shows the first few).
		[[nodiscard]] const std::deque<Want>& Wants() const { return wants; }

		[[nodiscard]] std::size_t Size() const { return wants.size(); }
		[[nodiscard]] std::size_t InFlight() const { return flight.size(); }
		[[nodiscard]] std::uint64_t Dropped() const { return dropped; }
		[[nodiscard]] std::uint64_t Stuck() const { return stuck; }
		[[nodiscard]] std::optional<Entry> LastStuck() const { return lastStuck; }

		// What waits is no longer the session's word (a load, the host's world new, the session's end).
		void Clear()
		{
			wants.clear();
			flight.clear();
			lastStage.clear();
			parked.clear();
			parkedCount = 0;
		}

		// The quest started here: what was put aside for it is judged again, in the order it was heard, before anything
		// heard later of it. The number of wants given back.
		std::size_t Unpark(std::uint32_t a_questId, Clock::time_point a_now)
		{
			const auto it = parked.find(a_questId);
			if (it == parked.end()) {
				return 0;
			}
			auto back = std::move(it->second);
			parked.erase(it);
			parkedCount -= back.size();
			// ahead of the quest's own wants heard since (none, nearly always: its word came whole with the join)
			const auto first = std::ranges::find_if(wants, [&](const Want& a_w) { return a_w.entry.questId == a_questId; });
			std::size_t n = 0;
			auto        at = first;
			for (auto& w : back) {
				if (wants.size() >= MAX_WANTS) {
					dropped += back.size() - n;
					break;
				}
				w.heard = a_now;
				at = std::next(wants.insert(at, w));
				++n;
			}
			unparked += n;
			return n;
		}

		[[nodiscard]] std::size_t   Parked() const { return parkedCount; }
		[[nodiscard]] std::size_t   ParkedQuests() const { return parked.size(); }
		[[nodiscard]] std::uint64_t Unparked() const { return unparked; }

		// Whether anything of a_questId waits here: a want to judge, or a stage made and not read done yet (a radiant job's
		// stop waits for it, QuestRunRules::Follower::Due).
		[[nodiscard]] bool Has(std::uint32_t a_questId) const
		{
			return flight.contains(a_questId) || std::ranges::any_of(wants, [&](const Want& a_w) { return a_w.entry.questId == a_questId; });
		}

		// What waits of one quest is no longer the session's word (a radiant job begun again or stopped).
		void Forget(std::uint32_t a_questId)
		{
			std::erase_if(wants, [&](const Want& a_w) { return a_w.entry.questId == a_questId; });
			flight.erase(a_questId);
			lastStage.erase(a_questId);
			if (const auto it = parked.find(a_questId); it != parked.end()) {
				parkedCount -= it->second.size();
				parked.erase(it);
			}
		}

		// The wants held for their quest's run (kHold), for the dev command.
		[[nodiscard]] std::size_t HeldForRun(const Look& a_look) const
		{
			std::size_t n = 0;
			for (const auto& w : wants) {
				if (Judge(w.entry, w.reporter, a_look(w.entry, w.reporter)) == Verdict::kHold) {
					++n;
				}
			}
			return n;
		}

	private:
		struct Flight
		{
			std::uint16_t     stage = 0;
			Clock::time_point since{};
		};

		// A want judged as it stands now: near at its step counts as near; a hold whose friend left, or that waited
		// FAR_WAIT, is given up (kFarPast, a_release says why).
		[[nodiscard]] static Verdict JudgeWant(const Want& a_want, Clock::time_point a_now, const Look& a_look, Release& a_release, Here& a_here)
		{
			a_here = a_look(a_want.entry, a_want.reporter);
			a_here.reporterNear = a_here.reporterNear || a_want.nearAtStep;
			const auto verdict = QuestRules::Judge(a_want.entry, a_want.reporter, a_here);
			a_release = Release::kNone;
			if (verdict == Verdict::kFar) {
				if (a_want.gone) {
					a_release = Release::kLeft;
				} else if (a_now - a_want.heard >= FAR_WAIT) {
					a_release = Release::kWaited;
				}
				if (a_release != Release::kNone) {
					return Verdict::kFarPast;
				}
			}
			return verdict;
		}

		void Park(const Want& a_want)
		{
			if (parkedCount >= MAX_PARKED) {
				++dropped;
				return;
			}
			auto& list = parked[a_want.entry.questId];
			// a newer word on the same thing replaces the one put aside (as Add does)
			for (auto& w : list) {
				if (w.entry.SameThing(a_want.entry)) {
					w.entry = a_want.entry.kind == Entry::kObjective ? QuestBook::Merge(w.entry, a_want.entry) : a_want.entry;
					return;
				}
			}
			list.push_back(a_want);
			++parkedCount;
		}

		[[nodiscard]] bool HasStageWaiting(std::uint32_t a_questId) const
		{
			return std::ranges::any_of(wants, [&](const Want& a_w) { return a_w.entry.questId == a_questId && a_w.entry.kind == Entry::kStage; });
		}

		std::deque<Want>                                         wants;
		std::unordered_map<std::uint32_t, Flight>                flight;
		std::unordered_map<std::uint32_t, Clock::time_point>     lastStage;
		Clock::time_point                                        lastMade{};
		Clock::time_point                                        lastPast{};  // the last word of the session's past heard
		std::uint64_t                                            dropped = 0;
		std::uint64_t                                            stuck = 0;
		std::optional<Entry>                                     lastStuck;
		std::vector<Want>                                        newlyHeld;
		std::unordered_map<std::uint32_t, std::vector<Want>>     parked;  // by quest, in the order put aside
		std::size_t                                              parkedCount = 0;
		std::uint64_t                                            unparked = 0;
	};

	// ---- a stage that moved our player ------------------------------------------------------------------------------

	// Where our player is: a space (an interior cell's or a worldspace's form ID) and a point in it.
	struct Place
	{
		std::uint32_t space = 0;
		float         x = 0.0f;
		float         y = 0.0f;
		float         z = 0.0f;
	};

	// How long after a stage is made here a jump of our player is laid at its door, and how far a jump is.
	inline constexpr auto        MOVE_WATCH = std::chrono::seconds{ 3 };
	inline constexpr float       MOVE_FAR = 4096.0f;

	// Watches our player for a while after each stage the session's word made here: a change of space or a jump of
	// MOVE_FAR is that stage moving the player (its fragment's MoveTo), said once per stage so the stage rules get the
	// row. Our own load door in that window is taken for it too (a "may have" in the log).
	class MoveGuard
	{
	public:
		void Made(const Entry& a_stage, const Place& a_where, Clock::time_point a_now)
		{
			watching = Watch{ a_stage, a_where, a_now + MOVE_WATCH };
		}

		// Our player is at a_where now: the stage that moved them, once, when one did.
		std::optional<Entry> Check(const Place& a_where, Clock::time_point a_now)
		{
			if (!watching) {
				return std::nullopt;
			}
			const auto& w = *watching;
			const bool moved = a_where.space != w.from.space ||
			                   std::hypot(a_where.x - w.from.x, a_where.y - w.from.y, a_where.z - w.from.z) > MOVE_FAR;
			if (moved) {
				const auto stage = w.stage;
				watching.reset();
				++moves;
				return stage;
			}
			if (a_now >= w.until) {
				watching.reset();
			}
			return std::nullopt;
		}

		[[nodiscard]] std::uint64_t Moves() const { return moves; }
		void Clear() { watching.reset(); }

	private:
		struct Watch
		{
			Entry             stage;
			Place             from;
			Clock::time_point until;
		};
		std::optional<Watch> watching;
		std::uint64_t        moves = 0;
	};

	// ---- the host's world ------------------------------------------------------------------------------------------

	// One quest as the host's game says it (its snapshot): its done stages, its objectives, its end.
	struct QuestRead
	{
		std::uint32_t                                         questId = 0;
		std::uint16_t                                         current = 0;  // its current stage (objectives are read at it)
		std::vector<std::uint16_t>                            done;        // stages done
		std::vector<std::pair<std::uint16_t, std::uint8_t>>   objectives;  // index, state
		bool                                                  completed = false;
		bool                                                  failed = false;
	};

	// A quest's objectives that are not dormant (by index, each with the stage it is at), then its end: what a game
	// polls, takes as its base and says in a snapshot.
	[[nodiscard]] inline std::vector<Entry> ObjectivesAndEnd(QuestRead a_quest)
	{
		std::vector<Entry> out;
		std::ranges::sort(a_quest.objectives);
		for (const auto& [index, state] : a_quest.objectives) {
			if (state != 0 && state <= Entry::OBJECTIVE_STATE_MAX) {
				out.push_back({ a_quest.questId, Entry::kObjective, index, state, a_quest.current });
			}
		}
		if (a_quest.completed) {
			out.push_back({ a_quest.questId, Entry::kCompleted, 0, 0 });
		} else if (a_quest.failed) {
			out.push_back({ a_quest.questId, Entry::kFailed, 0, 0 });
		}
		return out;
	}

	// The entries of a snapshot, in the order a game behind makes them: quest by quest (by form ID), each one's stages
	// in ascending order (the order a quest's own scripts set them, nearly always), then its objectives that are not
	// dormant, then its end.
	[[nodiscard]] inline std::vector<Entry> Snapshot(std::vector<QuestRead> a_quests)
	{
		std::ranges::sort(a_quests, {}, &QuestRead::questId);
		std::vector<Entry> out;
		for (auto& q : a_quests) {
			if (q.questId == 0 || FormIds::IsRuntime(q.questId)) {
				continue;
			}
			std::ranges::sort(q.done);
			const auto [first, last] = std::ranges::unique(q.done);
			q.done.erase(first, last);
			for (const auto stage : q.done) {
				out.push_back({ q.questId, Entry::kStage, stage, 0 });
			}
			const auto rest = ObjectivesAndEnd(std::move(q));
			out.insert(out.end(), rest.begin(), rest.end());
		}
		return out;
	}
}
