#include "Test.h"

#include "FormTranslate.h"
#include "QuestBook.h"
#include "QuestRuns.h"
#include "ServerFixture.h"
#include "game/QuestRules.h"
#include "game/QuestRunRules.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

// Quest runs and their aliases (docs/DESIGN.md, "Quests", "Runs and aliases"): the message (common/Protocol.h), its
// form IDs on the wire (common/FormTranslate.h), the server's book (common/QuestRuns.h) and passing on
// (common/Server.cpp), and each game's rules (src/game/QuestRunRules.h, and the hold in src/game/QuestRules.h). What
// reads and changes the game itself (src/game/QuestSync.cpp) is tested in a game only: tools/rig/steps/jobs.sh.

using Protocol::AliasFill;
using Protocol::QuestBatch;
using Protocol::QuestEntry;
using Protocol::QuestReset;
using Protocol::QuestRun;

namespace
{
	using MT = Protocol::MessageType;
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }
	const Clock::time_point T0 = Clock::time_point{} + 1h;

	AliasFill Ref(std::uint32_t a_alias, std::uint32_t a_ref) { return { a_alias, a_ref, AliasFill::kRef }; }
	AliasFill Empty(std::uint32_t a_alias) { return { a_alias, 0, AliasFill::kEmpty }; }
	AliasFill Runtime(std::uint32_t a_alias) { return { a_alias, 0, AliasFill::kRuntime }; }

	QuestRun Start(std::uint32_t a_quest, std::uint32_t a_run, bool a_fresh = false)
	{
		QuestRun r;
		r.questId = a_quest;
		r.run = a_run;
		r.flags = a_fresh ? QuestRun::kFresh : 0;
		return r;
	}
	QuestRun Filled(std::uint32_t a_quest, std::uint32_t a_run, std::vector<AliasFill> a_fills)
	{
		QuestRun r;
		r.questId = a_quest;
		r.run = a_run;
		r.flags = QuestRun::kFilled;
		r.fills = std::move(a_fills);
		return r;
	}
	QuestRun Stop(std::uint32_t a_quest, std::uint32_t a_run)
	{
		QuestRun r;
		r.questId = a_quest;
		r.run = a_run;
		r.flags = QuestRun::kStopped;
		return r;
	}

	QuestEntry Stage(std::uint32_t a_quest, std::uint16_t a_stage) { return { a_quest, QuestEntry::kStage, a_stage, 0 }; }
	std::vector<std::uint8_t> Report(std::vector<QuestEntry> a_entries) { return Protocol::Encode(QuestBatch{ 0, std::move(a_entries) }); }

	std::vector<QuestRun> RunsIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<QuestRun> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == MT::kRemoteQuestRun) {
				if (const auto run = Protocol::DecodeQuestRun(packet)) {
					out.push_back(*run);
				}
			}
		}
		return out;
	}

	std::vector<std::pair<std::uint32_t, QuestEntry>> EntriesIn(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type = MT::kRemoteQuests)
	{
		std::vector<std::pair<std::uint32_t, QuestEntry>> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == a_type) {
				if (const auto batch = Protocol::DecodeQuestBatch(packet)) {
					for (const auto& e : batch->entries) {
						out.emplace_back(batch->playerId, e);
					}
				}
			}
		}
		return out;
	}

	QuestRunRules::Here RunsHere(bool a_radiant = false)
	{
		QuestRunRules::Here here;
		here.exists = true;
		here.shared = !a_radiant;
		here.radiant = a_radiant;
		return here;
	}
}

// ---- the message ---------------------------------------------------------------------------------------------------

TEST("quest runs: a run goes to the server and back, the host named on the way back; its fills only with kFilled")
{
	const auto start = Start(0x0001F0A1, 3, true);
	const auto back = Protocol::DecodeQuestRun(Protocol::Encode(start));
	REQUIRE(back);
	CHECK(*back == start);
	CHECK(Protocol::PeekType(Protocol::Encode(start)) == MT::kQuestRun);

	auto filled = Filled(0x0001F0A1, 3, { Ref(0, 0x00012345), Empty(1), Runtime(2), { 3, 0, AliasFill::kUnknown } });
	filled.playerId = 7;
	const auto remote = Protocol::Encode(filled, MT::kRemoteQuestRun);
	CHECK(Protocol::PeekType(remote) == MT::kRemoteQuestRun);
	const auto heard = Protocol::DecodeQuestRun(remote);
	REQUIRE(heard);
	CHECK(*heard == filled);

	const auto stop = Protocol::DecodeQuestRun(Protocol::Encode(Stop(0x0001F0A1, 3)));
	REQUIRE(stop);
	CHECK(stop->flags == QuestRun::kStopped);
	CHECK(stop->fills.empty());

	CHECK(FormTranslate::CarriesForms(MT::kQuestRun));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteQuestRun));
}

TEST("quest runs: a run that can't be read is none: no quest, run 0, flags not known, filled and stopped, a fill out of range, short or long")
{
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Start(0, 1))));
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Start(0x10, 0))));
	auto unknown = Start(0x10, 1);
	unknown.flags = 0x08;
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(unknown)));
	auto both = Filled(0x10, 1, {});
	both.flags |= QuestRun::kStopped;
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(both)));
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Filled(0x10, 1, { { 0, 5, 4 } }))));                  // kind past KIND_MAX
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Filled(0x10, 1, { { 0, 0, AliasFill::kRef } }))));    // a reference of none
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Filled(0x10, 1, { { 0, 5, AliasFill::kEmpty } }))));  // empty, naming one
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Filled(0x10, 1, { { 0, 5, AliasFill::kRuntime } }))));
	auto packet = Protocol::Encode(Filled(0x10, 1, { Ref(0, 0x20) }));
	auto shorter = packet;
	shorter.pop_back();
	CHECK(!Protocol::DecodeQuestRun(shorter));
	packet.push_back(0);
	CHECK(!Protocol::DecodeQuestRun(packet));
	// past MAX_ALIAS_FILLS: the writer keeps the most, and a count over it is none
	std::vector<AliasFill> many;
	for (std::uint32_t i = 0; i < Protocol::MAX_ALIAS_FILLS + 10; ++i) {
		many.push_back(Ref(i, 0x100 + i));
	}
	const auto kept = Protocol::DecodeQuestRun(Protocol::Encode(Filled(0x10, 1, many)));
	REQUIRE(kept);
	CHECK(kept->fills.size() == Protocol::MAX_ALIAS_FILLS);
	Protocol::Writer w{ MT::kQuestRun };
	w.U32(0x10);
	w.U32(1);
	w.U8(QuestRun::kFilled);
	w.U16(static_cast<std::uint16_t>(Protocol::MAX_ALIAS_FILLS + 1));
	CHECK(!Protocol::DecodeQuestRun(w.Data()));
	CHECK(!Protocol::DecodeQuestRun(Report({ Stage(0x10, 1) })));
}

TEST("quest runs: a run is named by its plugin on the wire; a quest the other game lacks is dropped, a reference it lacks is unknown")
{
	using FormTranslate::Result;
	using FormTranslate::Way;
	using Names = std::vector<std::string>;
	const Names host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm", "A.esp", "B.esp" }, {});
	const Names guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm", "B.esp" }, {});
	Names       session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const FormIds::Mapper hostEdge(host, session);
	const FormIds::Mapper guestEdge(guest, session);

	// The host's quest of Skyrim.esm with an alias holding a reference of A.esp (the guest lacks it) and one of B.esp.
	auto run = Filled(0x0001F0A1, 2, { Ref(0, 0x02000800), Ref(1, 0x03000D62), Ref(2, 0x00012345), Empty(3) });
	const auto out = FormTranslate::Translate(hostEdge, Way::kToSession, Protocol::Encode(run));
	CHECK(out.kind != Result::Kind::kDrop);
	auto remote = out.kind == Result::Kind::kChanged ? Protocol::DecodeQuestRun(out.packet) : Protocol::DecodeQuestRun(Protocol::Encode(run));
	REQUIRE(remote);
	remote->playerId = 1;
	const auto in = FormTranslate::Translate(guestEdge, Way::kToLocal, Protocol::Encode(*remote, MT::kRemoteQuestRun));
	REQUIRE(in.kind == Result::Kind::kChanged);
	const auto heard = Protocol::DecodeQuestRun(in.packet);
	REQUIRE(heard);
	REQUIRE(heard->fills.size() == 4);
	CHECK(heard->questId == 0x0001F0A1);
	CHECK(heard->fills[0] == (AliasFill{ 0, 0, AliasFill::kUnknown }));  // A.esp's: the guest's own pick stays
	CHECK(heard->fills[1] == Ref(1, 0x02000D62));                         // B.esp is the guest's 02
	CHECK(heard->fills[2] == Ref(2, 0x00012345));
	CHECK(heard->fills[3] == Empty(3));

	// A run of a quest of A.esp is no message for the guest at all.
	auto lacked = Start(0x02000900, 1);
	lacked.playerId = 1;
	CHECK(FormTranslate::Translate(guestEdge, Way::kToLocal, Protocol::Encode(lacked, MT::kRemoteQuestRun)).kind == Result::Kind::kDrop);
	CHECK(FormTranslate::Translate(FormIds::Mapper(), Way::kToSession, Protocol::Encode(Start(0xFF000123, 1))).kind == Result::Kind::kDrop);
}

// ---- the server's book ---------------------------------------------------------------------------------------------

TEST("quest runs book: a new run is news, the same is not; changed fills are; a fresh run says to forget the quest's word")
{
	QuestRuns runs;
	auto      r = runs.Report(Start(0x10, 1));
	CHECK(r.take == QuestRuns::Take::kNews);
	CHECK(!r.forget);
	CHECK(runs.Report(Start(0x10, 1)).take == QuestRuns::Take::kSame);
	CHECK(runs.Report(Filled(0x10, 1, { Ref(1, 0x20), Ref(0, 0x21) })).take == QuestRuns::Take::kNews);
	CHECK(runs.Report(Filled(0x10, 1, { Ref(0, 0x21), Ref(1, 0x20) })).take == QuestRuns::Take::kSame);  // the order is not news
	CHECK(runs.Report(Filled(0x10, 1, { Ref(0, 0x21), Ref(1, 0x22) })).take == QuestRuns::Take::kNews);
	CHECK(runs.Report(Start(0x10, 1)).take == QuestRuns::Take::kSame);  // its start said again keeps its fills
	auto live = runs.Live();
	REQUIRE(live.size() == 1);
	CHECK(live[0].flags == QuestRun::kFilled);
	CHECK(live[0].playerId == 0);
	CHECK(live[0].fills == (std::vector<AliasFill>{ Ref(0, 0x21), Ref(1, 0x22) }));

	r = runs.Report(Start(0x10, 2, true));
	CHECK(r.take == QuestRuns::Take::kNews);
	CHECK(r.forget);
	live = runs.Live();
	REQUIRE(live.size() == 1);
	CHECK(live[0].run == 2);
	CHECK(live[0].flags == 0);  // its fills are this run's, not said yet
	CHECK(live[0].fills.empty());

	CHECK(runs.Report(Start(0, 1)).take == QuestRuns::Take::kRefused);
	CHECK(runs.Report(Start(0xFF000001, 1)).take == QuestRuns::Take::kRefused);
	CHECK(runs.Report(Start(0x11, 0)).take == QuestRuns::Take::kRefused);
}

TEST("quest runs book: a stop of the run on ends it and says to forget; a stop of another run, or again, is nothing")
{
	QuestRuns runs;
	runs.Report(Start(0x10, 4, true));
	runs.Report(Filled(0x10, 4, { Ref(0, 0x20) }));
	CHECK(!runs.Stopped(0x10));
	CHECK(runs.Report(Stop(0x10, 3)).take == QuestRuns::Take::kSame);
	CHECK(runs.Report(Stop(0x99, 1)).take == QuestRuns::Take::kSame);
	const auto r = runs.Report(Stop(0x10, 4));
	CHECK(r.take == QuestRuns::Take::kNews);
	CHECK(r.forget);
	CHECK(runs.Stopped(0x10));
	CHECK(runs.Live().empty());
	CHECK(runs.Report(Stop(0x10, 4)).take == QuestRuns::Take::kSame);
	CHECK(runs.Report(Filled(0x10, 4, { Ref(0, 0x20) })).take == QuestRuns::Take::kSame);  // a late word on the run that ended
	CHECK(runs.Stopped(0x10));
	CHECK(runs.Report(Start(0x10, 5, true)).take == QuestRuns::Take::kNews);  // begun again
	CHECK(!runs.Stopped(0x10));
}

TEST("quest runs book: past its most a new quest is full, never the oldest dropped; live runs in the order first said; Clear forgets all")
{
	QuestRuns runs{ 2 };
	runs.Report(Start(0x30, 1));
	runs.Report(Start(0x20, 2));
	CHECK(runs.Report(Start(0x40, 3)).take == QuestRuns::Take::kFull);
	CHECK(runs.Report(Start(0x30, 4)).take == QuestRuns::Take::kNews);  // a quest kept already takes a new run
	const auto live = runs.Live();
	REQUIRE(live.size() == 2);
	CHECK(live[0].questId == 0x30);
	CHECK(live[1].questId == 0x20);
	CHECK(runs.Quests() == 2);
	runs.Clear();
	CHECK(runs.Quests() == 0);
	CHECK(runs.Live().empty());
	CHECK(runs.Report(Start(0x40, 3)).take == QuestRuns::Take::kNews);
}

TEST("quest runs book: fills are kept one per alias, the last said, by alias id, at most MAX_ALIAS_FILLS")
{
	const auto sorted = QuestRuns::Sorted({ Ref(5, 0x1), Ref(2, 0x2), Ref(5, 0x3), Empty(0) });
	CHECK(sorted == (std::vector<AliasFill>{ Empty(0), Ref(2, 0x2), Ref(5, 0x3) }));
	std::vector<AliasFill> many;
	for (std::uint32_t i = 0; i < Protocol::MAX_ALIAS_FILLS + 5; ++i) {
		many.push_back(Ref(i, 0x100 + i));
	}
	CHECK(QuestRuns::Sorted(many).size() == Protocol::MAX_ALIAS_FILLS);
}

TEST("quest book: Forget takes one quest's entries out, and the rest stay in order")
{
	QuestBook  book;
	QuestEntry out;
	book.Report(Stage(0x10, 10), out);
	book.Report(Stage(0x20, 5), out);
	book.Report(Stage(0x10, 20), out);
	book.Report(Stage(0x30, 1), out);
	CHECK(book.Entries() == 4);
	book.Forget(0x10);
	book.Forget(0x99);
	CHECK(book.Quests() == 2);
	CHECK(book.Entries() == 2);
	const auto all = book.All();
	REQUIRE(all.size() == 2);
	CHECK(all[0] == Stage(0x20, 5));
	CHECK(all[1] == Stage(0x30, 1));
	CHECK(book.Report(Stage(0x10, 10), out) == QuestBook::Take::kNews);  // news again: another run's
}

// ---- the host's game: its runs ---------------------------------------------------------------------------------------

TEST("quest keeper: each start is a new run, never numbered twice, fresh for a radiant job; fills said when first read and when changed")
{
	QuestRunRules::Keeper keeper;
	const auto            a = keeper.Started(0x10, false);
	CHECK(a.run == 1);
	CHECK(a.flags == 0);
	const auto b = keeper.Started(0x20, true);
	CHECK(b.run == 2);
	CHECK(b.flags == QuestRun::kFresh);
	CHECK(keeper.Live(0x10));
	CHECK(keeper.LiveQuests() == (std::vector<std::uint32_t>{ 0x10, 0x20 }));

	auto said = keeper.Read(0x10, { Ref(1, 0x50), Empty(0) });
	REQUIRE(said);
	CHECK(said->flags == QuestRun::kFilled);
	CHECK(said->run == 1);
	CHECK(said->fills == (std::vector<AliasFill>{ Empty(0), Ref(1, 0x50) }));
	CHECK(!keeper.Read(0x10, { Empty(0), Ref(1, 0x50) }));
	CHECK(keeper.Read(0x10, { Empty(0), Ref(1, 0x51) }));
	CHECK(!keeper.Read(0x99, { Ref(1, 0x50) }));  // not a run

	// A shared quest's stop is its own scripts' in every game; a radiant job's ends its run.
	CHECK(!keeper.Stopped(0x10));
	CHECK(!keeper.Live(0x10));
	const auto stop = keeper.Stopped(0x20);
	REQUIRE(stop);
	CHECK(stop->flags == QuestRun::kStopped);
	CHECK(stop->run == 2);
	CHECK(!keeper.Stopped(0x20));
	CHECK(!keeper.Read(0x20, { Ref(0, 0x1) }));

	keeper.Clear();
	CHECK(keeper.LiveQuests().empty());
	CHECK(keeper.Started(0x20, true).run == 3);  // the numbers go on after a load
	CHECK(QuestRunRules::FillOf(4, 0) == Empty(4));
	CHECK(QuestRunRules::FillOf(4, 0xFF000010) == Runtime(4));
	CHECK(QuestRunRules::FillOf(4, 0x00012345) == Ref(4, 0x00012345));
}

// ---- a friend's game: the host's runs, made here -----------------------------------------------------------------

TEST("quest follower: a run not running here is started with its fills, held until it runs, then forced every FORCE_EVERY")
{
	using QuestRunRules::Act;
	using QuestRunRules::Phase;
	QuestRunRules::Follower f;
	auto                    now = T0;
	auto                    here = RunsHere();
	CHECK(!f.Hear(Start(0x10, 1), here, now));
	CHECK(f.Hold(0x10));
	CHECK(f.Due(0x10, here, now) == Act::kNothing);  // its fills are not said yet
	CHECK(f.PhaseOf(0x10) == Phase::kWaiting);
	f.Hear(Filled(0x10, 1, { Ref(0, 0x50) }), here, now);
	CHECK(f.Due(0x10, here, now) == Act::kStart);
	CHECK(f.PhaseOf(0x10) == Phase::kStarting);
	CHECK(f.Hold(0x10));
	CHECK(!f.ForceDue(0x10, now));
	here.starting = true;
	CHECK(f.Due(0x10, here, now) == Act::kNothing);
	here.starting = false;
	here.running = true;
	CHECK(f.Due(0x10, here, now + 1s) == Act::kStarted);
	CHECK(!f.Hold(0x10));
	CHECK(f.Ours(0x10));
	CHECK(f.ForceDue(0x10, now + 1s));
	CHECK(!f.ForceDue(0x10, now + 2s));
	CHECK(f.ForceDue(0x10, now + 1s + QuestRunRules::FORCE_EVERY));
	CHECK(f.Due(0x10, here, now + 5s) == Act::kNothing);
	// changed fills are forced at once
	f.Hear(Filled(0x10, 1, { Ref(0, 0x51) }), here, now + 4s);
	CHECK(f.ForceDue(0x10, now + 4s));
	REQUIRE(f.Fills(0x10));
	CHECK(f.Fills(0x10)->front() == Ref(0, 0x51));
	CHECK(f.Started() == 1);
}

TEST("quest follower: a run that runs here already is adopted; one already ended here is not started; a quest lacking or not shared is not here")
{
	using QuestRunRules::Act;
	using QuestRunRules::Phase;
	QuestRunRules::Follower f;
	auto                    running = RunsHere();
	running.running = true;
	f.Hear(Start(0x10, 1), running, T0);
	CHECK(f.Due(0x10, running, T0) == Act::kAdopt);
	CHECK(f.Ours(0x10));
	CHECK(f.Adopted() == 1);
	CHECK(!f.ForceDue(0x10, T0));  // nothing to force until its fills are said
	f.Hear(Filled(0x10, 1, { Ref(0, 0x50) }), running, T0 + 1s);
	CHECK(f.ForceDue(0x10, T0 + 1s));

	auto ended = RunsHere();
	ended.ended = true;
	f.Hear(Filled(0x20, 1, { Ref(0, 0x50) }), ended, T0);
	CHECK(f.Due(0x20, ended, T0) == Act::kNothing);
	CHECK(f.PhaseOf(0x20) == Phase::kEnded);
	CHECK(!f.Hold(0x20));

	f.Hear(Filled(0x30, 1, { Ref(0, 0x50) }), QuestRunRules::Here{}, T0);
	CHECK(f.Due(0x30, QuestRunRules::Here{}, T0) == Act::kNothing);
	CHECK(f.PhaseOf(0x30) == Phase::kNotHere);
	CHECK(!f.Hold(0x30));
	QuestRunRules::Here own;
	own.exists = true;  // a quest this game's record keeps its own (misc, untyped)
	f.Hear(Filled(0x31, 1, { Ref(0, 0x50) }), own, T0);
	CHECK(f.Due(0x31, own, T0) == Act::kNothing);
	CHECK(f.PhaseOf(0x31) == Phase::kNotHere);
}

TEST("quest follower: no fills, or no start, within START_WAIT gives the run up and lets its stages go")
{
	using QuestRunRules::Act;
	using QuestRunRules::Phase;
	QuestRunRules::Follower f;
	auto                    here = RunsHere();
	f.Hear(Start(0x10, 1), here, T0);
	CHECK(f.Due(0x10, here, T0 + QuestRunRules::START_WAIT - 1s) == Act::kNothing);
	CHECK(f.Due(0x10, here, T0 + QuestRunRules::START_WAIT) == Act::kGiveUp);
	CHECK(f.PhaseOf(0x10) == Phase::kGaveUp);
	CHECK(!f.Hold(0x10));
	CHECK(!f.Ours(0x10));

	f.Hear(Filled(0x20, 1, { Ref(0, 0x50) }), here, T0);
	CHECK(f.Due(0x20, here, T0) == Act::kStart);
	CHECK(f.Due(0x20, here, T0 + QuestRunRules::START_WAIT - 1s) == Act::kNothing);
	CHECK(f.Due(0x20, here, T0 + QuestRunRules::START_WAIT) == Act::kGiveUp);
	CHECK(f.GaveUp() == 2);
}

TEST("quest follower: the host's stop of a radiant job this game runs for it stops it here; a stop says to forget the job's word")
{
	using QuestRunRules::Act;
	QuestRunRules::Follower f;
	auto                    job = RunsHere(true);
	job.running = true;
	CHECK(f.Hear(Start(0x10, 2, true), job, T0));  // fresh: the earlier run's word goes
	CHECK(f.Due(0x10, job, T0) == Act::kAdopt);
	CHECK(f.Hear(Stop(0x10, 2), job, T0 + 1s));
	CHECK(!f.Ours(0x10));
	CHECK(f.Quests() == (std::vector<std::uint32_t>{ 0x10 }));
	CHECK(f.Due(0x10, job, T0 + 1s) == Act::kStop);
	CHECK(f.Due(0x10, job, T0 + 2s) == Act::kNothing);  // once
	CHECK(f.Quests().empty());

	// A stop of a run this game never ran for the host stops nothing here (a friend's own job stays its own).
	QuestRunRules::Follower g;
	g.Hear(Start(0x10, 2, true), RunsHere(true), T0);  // heard while the job did not run here
	CHECK(g.Hear(Stop(0x10, 2), job, T0 + 1s));
	CHECK(g.Due(0x10, job, T0 + 1s) == Act::kNothing);
	// A stop of an earlier run than the one this game runs stops nothing either.
	QuestRunRules::Follower h;
	h.Hear(Start(0x10, 3, true), job, T0);
	CHECK(h.Due(0x10, job, T0) == Act::kAdopt);
	CHECK(!h.Hear(Stop(0x10, 2), job, T0 + 1s));
	CHECK(h.Due(0x10, job, T0 + 1s) == Act::kNothing);
	CHECK(h.Ours(0x10));
	CHECK(h.RunOf(0x10) == 3);
	// a shared quest's stop is not this game's to make: its own scripts end it
	QuestRunRules::Follower s;
	auto                    quest = RunsHere();
	quest.running = true;
	s.Hear(Start(0x20, 1), quest, T0);
	CHECK(s.Due(0x20, quest, T0) == Act::kAdopt);
	CHECK(!s.Hear(Stop(0x20, 1), quest, T0 + 1s));
	CHECK(s.Due(0x20, quest, T0 + 1s) == Act::kNothing);
}

TEST("quest follower: a new run of a quest this game runs as an earlier one is adopted again; its own stop ends it here; Clear forgets all")
{
	using QuestRunRules::Act;
	using QuestRunRules::Phase;
	QuestRunRules::Follower f;
	auto                    job = RunsHere(true);
	job.running = true;
	f.Hear(Start(0x10, 1, true), job, T0);
	CHECK(f.Due(0x10, job, T0) == Act::kAdopt);
	CHECK(f.Hear(Start(0x10, 2, true), job, T0 + 1s));
	CHECK(f.WasOurs(0x10));
	CHECK(f.Hold(0x10));
	CHECK(f.Due(0x10, job, T0 + 1s) == Act::kAdopt);
	CHECK(f.RunOf(0x10) == 2);
	f.StoppedHere(0x10);
	CHECK(f.PhaseOf(0x10) == Phase::kEnded);
	CHECK(!f.Ours(0x10));
	CHECK(f.Live() == 1);
	f.Clear();
	CHECK(f.Live() == 0);
	CHECK(f.Quests().empty());
	CHECK(!f.PhaseOf(0x10));
}

TEST("quest fills compared: the host's references not held here are forced; created, unknown and extra ones stay this game's own")
{
	const std::vector<AliasFill> session{ Ref(0, 0x50), Ref(1, 0x51), Runtime(2), { 3, 0, AliasFill::kUnknown }, Empty(4), Ref(5, 0x55) };
	const std::vector<AliasFill> ours{ Ref(0, 0x50), Ref(1, 0x99), Ref(2, 0x98), Ref(3, 0x97), Ref(4, 0x96), Empty(5) };
	const auto                   c = QuestRunRules::Against(session, ours);
	CHECK(c.force == (std::vector<QuestRunRules::Force>{ { 1, 0x51 }, { 5, 0x55 } }));
	CHECK(c.same == 1);
	CHECK(c.runtime == 1);
	CHECK(c.unknown == 1);
	CHECK(c.extra == 1);
	CHECK(!c.Same());
	CHECK(QuestRunRules::Against({ Ref(0, 0x50) }, { Ref(0, 0x50), Ref(1, 0x1) }).Same());
	CHECK(QuestRunRules::Against({ Ref(7, 0x50) }, {}).force.size() == 1);  // an alias this game's quest lacks a fill of

	CHECK(QuestRunRules::Describe({}) == "-");
	CHECK(QuestRunRules::Describe({ Runtime(2), Ref(0, 0x00012345), Empty(1), { 3, 0, AliasFill::kUnknown } }) == "0:00012345,1:-,2:rt,3:?");
	CHECK(QuestRunRules::PhaseName(QuestRunRules::Phase::kGaveUp) == "gaveup");
}

// ---- the hold, and forgetting one quest ----------------------------------------------------------------------------

TEST("quest judge: a quest whose run is not started here yet holds its stages and those after, not other quests'")
{
	using QuestRules::Here;
	using QuestRules::Judge;
	using QuestRules::Verdict;
	Here held{ .exists = true, .cls = QuestRules::Class::kShared };
	held.hold = true;
	CHECK(Judge(Stage(1, 10), 2, held) == Verdict::kHold);
	CHECK(Judge(Stage(1, 10), 2, Here{ .hold = true }) == Verdict::kMissing);
	CHECK(QuestRules::VerdictName(Verdict::kHold) == "hold");

	QuestRules::Maker maker;
	bool              hold = true;
	const auto        look = [&](const QuestEntry& a_e, std::uint32_t) {
        Here here{ .exists = true, .cls = QuestRules::Class::kShared };
        here.hold = hold && a_e.questId == 0x10;
        return here;
	};
	maker.Add(Stage(0x10, 10), 2, T0);
	maker.Add(Stage(0x10, 20), 2, T0);
	maker.Add(Stage(0x20, 5), 2, T0);
	CHECK(maker.HeldForRun(look) == 2);
	auto step = maker.Next(T0, look);
	REQUIRE(step);
	CHECK(step->want.entry == Stage(0x20, 5));
	CHECK(maker.Size() == 2);
	hold = false;
	step = maker.Next(T0 + QuestRules::MAKE_EVERY, look);
	REQUIRE(step);
	CHECK(step->verdict == Verdict::kMake);
	CHECK(step->want.entry == Stage(0x10, 10));
}

TEST("quest view and maker: forgetting one quest drops its word, its base and what waits of it, and nothing else")
{
	QuestRules::View view;
	CHECK(view.Heard(Stage(0x10, 10), false));
	CHECK(view.Heard(Stage(0x20, 5), false));
	view.Forget(0x10);
	CHECK(view.Quests() == 1);
	CHECK(view.Heard(Stage(0x10, 10), false));  // new again: another run's
	CHECK(!view.Heard(Stage(0x20, 5), false));

	QuestRules::Maker maker;
	maker.Add(Stage(0x10, 10), 2, T0);
	maker.Add(Stage(0x20, 5), 2, T0);
	maker.Forget(0x10);
	CHECK(maker.Size() == 1);
	CHECK(maker.Wants().front().entry == Stage(0x20, 5));

	QuestRules::Apart apart;
	const std::vector<std::pair<std::uint32_t, std::uint16_t>> at{ { 0x10, 30 } };
	apart.Rebase(at, T0);
	CHECK(apart.StageOf(0x10) == 30);
	apart.Forget(0x10);
	CHECK(apart.StageOf(0x10) == 0);
}

// ---- the server ------------------------------------------------------------------------------------------------------

namespace
{
	struct HostedSession
	{
		Server::Options                  options = [] {
            Server::Options o;
            o.hostIdentity = 0x4057;
            return o;
		}();
		Fixture                          f{ options };
		std::optional<Protocol::Welcome> host;

		HostedSession()
		{
			f.fake->SetLocal(1);
			host = f.Join(1, Fixture::MakeHello("Host", 0x4057));
		}
	};
}

TEST("server: the host's runs go to the others, named; a friend's are never passed on")
{
	HostedSession s;
	auto&         f = s.f;
	REQUIRE(s.host);
	REQUIRE(s.host->hostId == s.host->playerId);
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));
	f.fake->Deliver(1, Protocol::Encode(Start(0x0001F0A1, 1)));
	f.fake->Deliver(1, Protocol::Encode(Filled(0x0001F0A1, 1, { Ref(0, 0x00012345) })));
	f.fake->Deliver(1, Protocol::Encode(Filled(0x0001F0A1, 1, { Ref(0, 0x00012345) })));  // the same: not news
	f.fake->Deliver(1, Report({ Stage(0x0001F0A1, 10) }));
	const auto toFriend = f.fake->TakeThrough(2, Type(MT::kRemoteQuests));
	const auto runs = RunsIn(toFriend);
	REQUIRE(runs.size() == 2);
	CHECK(runs[0].playerId == s.host->playerId);
	CHECK(runs[0].flags == 0);
	CHECK(runs[1].fills == (std::vector<AliasFill>{ Ref(0, 0x00012345) }));
	REQUIRE(f.fake->Settle());
	CHECK(RunsIn(f.fake->Take(1)).empty());  // not back to the host

	f.fake->Deliver(2, Protocol::Encode(Start(0x0001F0A2, 1)));
	REQUIRE(f.LoggedSoon("is not the host: its quest runs are not passed on"));
	REQUIRE(f.fake->Settle());
	CHECK(RunsIn(f.fake->Take(1)).empty());
}

TEST("server: a joiner and a load's ask hear the live runs before the quests' stages, from nobody; a fresh run forgets the job's stages")
{
	HostedSession s;
	auto&         f = s.f;
	REQUIRE(s.host);
	f.fake->Deliver(1, Protocol::Encode(Start(0x50, 1, true)));
	f.fake->Deliver(1, Protocol::Encode(Filled(0x50, 1, { Ref(0, 0x60) })));
	f.fake->Deliver(1, Protocol::Encode(Start(0x51, 2)));
	f.fake->Deliver(1, Report({ Stage(0x50, 10), Stage(0x51, 5) }));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteQuests)));
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));
	auto packets = f.fake->TakeThrough(2, Type(MT::kRemoteQuests));
	auto runs = RunsIn(packets);
	REQUIRE(runs.size() == 2);
	CHECK(runs[0].questId == 0x50);
	CHECK(runs[0].playerId == 0);
	CHECK(runs[0].flags == QuestRun::kFilled);  // the session's word: not fresh
	CHECK(runs[1].questId == 0x51);
	CHECK(EntriesIn(packets).size() == 2);
	const auto firstRun = std::ranges::find_if(packets, [](const auto& a_p) { return Protocol::PeekType(a_p) == MT::kRemoteQuestRun; });
	const auto firstStage = std::ranges::find_if(packets, [](const auto& a_p) { return Protocol::PeekType(a_p) == MT::kRemoteQuests; });
	CHECK(firstRun < firstStage);

	// The radiant job begun again: what the session had of it goes, the shared quest's stays.
	f.fake->Deliver(1, Protocol::Encode(Start(0x50, 3, true)));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kRemoteQuestRun)));
	f.fake->Deliver(2, Protocol::Writer{ MT::kAskQuests }.Data());
	packets = f.fake->TakeThrough(2, Type(MT::kRemoteQuests));
	runs = RunsIn(packets);
	REQUIRE(runs.size() == 2);
	CHECK(runs[0].run == 3);
	CHECK(runs[0].fills.empty());
	const auto stages = EntriesIn(packets);
	REQUIRE(stages.size() == 1);
	CHECK(stages[0].second == Stage(0x51, 5));
}

TEST("server: a radiant job's stop goes to the others and its run's stages are refused after; the host's reset clears the runs")
{
	HostedSession s;
	auto&         f = s.f;
	REQUIRE(s.host);
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));
	f.fake->Deliver(1, Protocol::Encode(Start(0x50, 1, true)));
	f.fake->Deliver(1, Report({ Stage(0x50, 10) }));
	CHECK(RunsIn(f.fake->TakeThrough(2, Type(MT::kRemoteQuests))).size() == 1);
	f.fake->Deliver(1, Protocol::Encode(Stop(0x50, 1)));
	const auto stop = RunsIn(f.fake->TakeThrough(2, Type(MT::kRemoteQuestRun)));
	REQUIRE(stop.size() == 1);
	CHECK(stop[0].flags == QuestRun::kStopped);
	CHECK(stop[0].playerId == s.host->playerId);
	REQUIRE(f.fake->Settle());
	f.fake->Take(1);

	// The friend's late word on the run that ended is refused, and passed on to nobody.
	f.fake->Deliver(2, Report({ Stage(0x50, 20) }));
	const auto answers = EntriesIn(f.fake->TakeThrough(2, Type(MT::kQuestAnswer)), MT::kQuestAnswer);
	REQUIRE(answers.size() == 1);
	CHECK(answers[0].second.value == QuestEntry::kRefused);
	REQUIRE(f.fake->Settle());
	CHECK(EntriesIn(f.fake->Take(1)).empty());
	f.fake->Deliver(2, Protocol::Writer{ MT::kAskQuests }.Data());
	REQUIRE(f.fake->Settle());
	auto toFriend = f.fake->Take(2);
	CHECK(RunsIn(toFriend).empty());
	CHECK(EntriesIn(toFriend).empty());

	// Begun again, the job's stages are taken; the host's reset clears its runs with its quests.
	f.fake->Deliver(1, Protocol::Encode(Start(0x50, 2, true)));
	f.fake->Deliver(1, Report({ Stage(0x50, 10) }));
	const auto again = f.fake->TakeThrough(2, Type(MT::kRemoteQuests));
	CHECK(RunsIn(again).size() == 1);
	CHECK(EntriesIn(again).size() == 1);
	f.fake->Deliver(1, Protocol::Encode(QuestReset{}, false));
	REQUIRE(!f.fake->TakeThrough(2, Type(MT::kQuestReset)).empty());
	f.fake->Deliver(2, Protocol::Writer{ MT::kAskQuests }.Data());
	REQUIRE(f.fake->Settle());
	toFriend = f.fake->Take(2);
	CHECK(RunsIn(toFriend).empty());
	CHECK(EntriesIn(toFriend).empty());
}
