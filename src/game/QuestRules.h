#pragma once

#include "FormIds.h"
#include "Protocol.h"
#include "QuestBook.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Shared quests (docs/DESIGN.md, "Quests"): which quests a session shares, what a game reports, and how the session's
// word is made in a game that is behind: a stage at a time, in order, forward only. Kept free of the game so every rule
// is tested (tests/QuestTests.cpp); src/game/QuestSync.cpp reads the game and makes what these decide.
//
// A quest is judged by what its record says, never by a name list alone, so a mod's quest is shared as the base game's
// are and the load order does not matter: its type (the journal's tab: the main quest, the guilds, the Daedric quests,
// side quests, the Civil War, Dawnguard and Dragonborn are shared; misc and untyped quests are each game's own in this
// version), and whether the Story Manager starts it for an event again and again (a radiant job: each game's own until
// the jobs' targets are shared). A short list names the typed quests that are each player's own anyway (the Helgen
// opening). Every game judges again by its own record before it makes anything.
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
		kRadiant,  // the Story Manager starts it for an event, again and again: each game picks its own job
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

	enum class StageVerdict : std::uint8_t
	{
		kMoves,  // its fragment moves the player (a summons, "come with me"): made only with the friend who did it near
		kOwn,    // each game's own: never made from the session's word
	};

	struct StageRule
	{
		std::string_view editorId;
		std::uint16_t    stage = 0;
		StageVerdict     verdict = StageVerdict::kMoves;
	};

	// The stages known to move the player or to be each game's own. Empty in this version: the rows come from a scan of
	// the game's quest fragments (as the sister project's quest scan finds its stages that move the player), and from
	// the logs: every stage the session's word made here that moved our player is said in the log (MoveGuard), so the
	// rig and players' reports name the rows. Until then a moving stage moves everyone it is made for.
	inline constexpr std::array<StageRule, 0> STAGE_RULES{};

	[[nodiscard]] inline std::optional<StageVerdict> StageRuleOf(std::span<const StageRule> a_rules, std::string_view a_editorId, std::uint16_t a_stage)
	{
		for (const auto& rule : a_rules) {
			if (rule.stage == a_stage && SameName(rule.editorId, a_editorId)) {
				return rule.verdict;
			}
		}
		return std::nullopt;
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

	// What this game knows of the session's quests: the session's word as heard (a QuestBook, unlimited), and the
	// reports of ours still out (sent, not answered), so a change is reported once, not again with every poll.
	class View
	{
	public:
		// Our game reads a_read (a stage done, an objective's state, an end): whether to report it. It is reported when
		// the session does not know it yet (an objective: when ours is ahead of the session's) and no report of the same
		// thing is out; it then counts as out until answered.
		bool Offer(const Entry& a_read)
		{
			if (a_read.kind == Entry::kObjective && a_read.value == 0) {
				return false;  // dormant says nothing
			}
			if (const auto known = session.Find(a_read)) {
				if (a_read.kind != Entry::kObjective || !QuestBook::Ahead(a_read.value, known->value)) {
					return false;
				}
			}
			if (IsOut(a_read)) {
				return false;
			}
			out.push_back(a_read);
			return true;
		}

		// The session's word (kRemoteQuests): true when it is new to this game's view. A report of ours is answered.
		bool Heard(const Entry& a_entry)
		{
			Answered(a_entry);
			Entry merged;
			return session.Report(a_entry, merged) == QuestBook::Take::kNews;
		}

		// A report of ours answered (kRemoteQuests or kQuestAnswer): no longer out.
		void Answered(const Entry& a_entry)
		{
			std::erase_if(out, [&](const Entry& a_out) { return a_out.SameThing(a_entry); });
		}

		[[nodiscard]] const Entry* Known(const Entry& a_what) const { return session.Find(a_what); }
		[[nodiscard]] std::size_t  Quests() const { return session.Quests(); }
		[[nodiscard]] std::size_t  Entries() const { return session.Entries(); }
		[[nodiscard]] std::size_t  Outstanding() const { return out.size(); }
		[[nodiscard]] std::vector<Entry> All() const { return session.All(); }

		// The session's word forgotten (a welcome, a load, the host's world new); reports out stay out until answered.
		void Clear() { session.Clear(); }
		void ClearAll()
		{
			session.Clear();
			out.clear();
		}

	private:
		[[nodiscard]] bool IsOut(const Entry& a_entry) const
		{
			return std::ranges::any_of(out, [&](const Entry& a_out) { return a_out.SameThing(a_entry) && (a_entry.kind != Entry::kObjective || !QuestBook::Ahead(a_entry.value, a_out.value)); });
		}

		QuestBook          session{ SIZE_MAX, SIZE_MAX };
		std::vector<Entry> out;
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
	// The most entries waiting to be made: past it a new one is dropped (counted).
	inline constexpr std::size_t MAX_WANTS = 8192;
	// A friend this near (in one space) is "near" for a stage that moves the player.
	inline constexpr float NEAR = 4096.0f;

	// What this game reads of one thing the session says, for Judge.
	struct Here
	{
		bool                        exists = false;   // the quest is in this game
		Class                       cls = Class::kUntyped;
		bool                        has = false;      // the stage is done here, the objective as far on, the end so
		std::optional<StageVerdict> rule;             // a stage's row in STAGE_RULES
		bool                        reporterNear = false;  // the player who did it is near ours
	};

	enum class Verdict : std::uint8_t
	{
		kMake,     // make it here now
		kHas,      // this game has it already: done
		kMissing,  // no such quest here (a mod this game lacks, or a form of another kind): dropped
		kNotHere,  // the quest is not shared by this game's own record: dropped
		kOwn,      // a stage the rules keep each game's own: dropped
		kFar,      // a stage that moves the player, said by a friend who is not near: held until they are
		kFarPast,  // ... said as the session's word (a join, a load: no friend did it now): dropped, not made
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
		}
		return "?";
	}

	// One thing the session said, to be made here; a_reporter 0 is the session's word sent again (a join, a load).
	[[nodiscard]] inline Verdict Judge(const Entry& a_entry, std::uint32_t a_reporter, const Here& a_here)
	{
		if (!a_here.exists) {
			return Verdict::kMissing;
		}
		if (a_here.cls != Class::kShared) {
			return Verdict::kNotHere;
		}
		if (a_here.has) {
			return Verdict::kHas;
		}
		if (a_entry.kind == Entry::kStage && a_here.rule) {
			if (*a_here.rule == StageVerdict::kOwn) {
				return Verdict::kOwn;
			}
			if (!a_here.reporterNear) {
				return a_reporter == 0 ? Verdict::kFarPast : Verdict::kFar;
			}
		}
		return Verdict::kMake;
	}

	// The session's entries waiting to be made here, in the order heard, and the pace they are made at: one thing at a
	// time; one stage of a quest at a time (its fragment runs, and may set the next stage itself); a quest's objectives
	// and end only once its stages are made and SETTLE has passed (its fragments set most of them); a stage held for a
	// far friend holds nothing else.
	class Maker
	{
	public:
		struct Want
		{
			Entry             entry;
			std::uint32_t     reporter = 0;
			Clock::time_point heard{};
		};

		// What Next decided, for the caller to make (kMake) or count.
		struct Step
		{
			Want    want;
			Verdict verdict = Verdict::kMake;
		};

		using Look = std::function<Here(const Entry&, std::uint32_t)>;

		// a_entry heard from the session (new to this game's view). False when the queue is full (dropped).
		bool Add(const Entry& a_entry, std::uint32_t a_reporter, Clock::time_point a_now)
		{
			if (wants.size() >= MAX_WANTS) {
				++dropped;
				return false;
			}
			// a newer word on the same objective replaces the older one waiting
			for (auto& w : wants) {
				if (w.entry.SameThing(a_entry)) {
					w.entry.value = a_entry.kind == Entry::kObjective ? QuestBook::MergeObjective(w.entry.value, a_entry.value) : a_entry.value;
					w.reporter = a_reporter;
					return true;
				}
			}
			wants.push_back({ a_entry, a_reporter, a_now });
			return true;
		}

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
				const auto here = a_look(e, it->reporter);
				const auto verdict = Judge(e, it->reporter, here);
				if (verdict == Verdict::kFar) {
					blocked.insert(e.questId);  // its later stages wait behind it
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
				Step step{ *it, verdict };
				wants.erase(it);
				return step;
			}
			return std::nullopt;
		}

		// The wants held for a far friend (kFar), for the dev command.
		[[nodiscard]] std::size_t Held(const Look& a_look) const
		{
			std::size_t n = 0;
			for (const auto& w : wants) {
				if (w.entry.kind == Entry::kStage && Judge(w.entry, w.reporter, a_look(w.entry, w.reporter)) == Verdict::kFar) {
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
		}

	private:
		struct Flight
		{
			std::uint16_t     stage = 0;
			Clock::time_point since{};
		};

		[[nodiscard]] bool HasStageWaiting(std::uint32_t a_questId) const
		{
			return std::ranges::any_of(wants, [&](const Want& a_w) { return a_w.entry.questId == a_questId && a_w.entry.kind == Entry::kStage; });
		}

		std::deque<Want>                                         wants;
		std::unordered_map<std::uint32_t, Flight>                flight;
		std::unordered_map<std::uint32_t, Clock::time_point>     lastStage;
		Clock::time_point                                        lastMade{};
		std::uint64_t                                            dropped = 0;
		std::uint64_t                                            stuck = 0;
		std::optional<Entry>                                     lastStuck;
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
		std::vector<std::uint16_t>                            done;        // stages done
		std::vector<std::pair<std::uint16_t, std::uint8_t>>   objectives;  // index, state
		bool                                                  completed = false;
		bool                                                  failed = false;
	};

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
			std::ranges::sort(q.objectives);
			for (const auto& [index, state] : q.objectives) {
				if (state != 0 && state <= Entry::OBJECTIVE_STATE_MAX) {
					out.push_back({ q.questId, Entry::kObjective, index, state });
				}
			}
			if (q.completed) {
				out.push_back({ q.questId, Entry::kCompleted, 0, 0 });
			} else if (q.failed) {
				out.push_back({ q.questId, Entry::kFailed, 0, 0 });
			}
		}
		return out;
	}
}
