#include "Test.h"

#include "FormTranslate.h"
#include "QuestBook.h"
#include "QuestRuns.h"
#include "ServerFixture.h"
#include "game/QuestRules.h"
#include "game/QuestRunRules.h"

#include <algorithm>
#include <chrono>
#include <functional>
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
	AliasFill Loc(std::uint32_t a_alias, std::uint32_t a_loc) { return { a_alias, a_loc, AliasFill::kLoc }; }
	AliasFill Unknown(std::uint32_t a_alias) { return { a_alias, 0, AliasFill::kUnknown }; }

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

	auto filled = Filled(0x0001F0A1, 3, { Ref(0, 0x00012345), Empty(1), Runtime(2), { 3, 0, AliasFill::kUnknown }, Loc(4, 0x00018A5A) });
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
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Filled(0x10, 1, { { 0, 5, 5 } }))));                  // kind past KIND_MAX
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Filled(0x10, 1, { { 0, 0, AliasFill::kRef } }))));    // a reference of none
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Filled(0x10, 1, { { 0, 0, AliasFill::kLoc } }))));    // a location of none
	CHECK(!Protocol::DecodeQuestRun(Protocol::Encode(Filled(0x10, 1, { { 0, 5, AliasFill::kUnknown } }))));
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

	// The host's quest of Skyrim.esm with an alias holding a reference of A.esp (the guest lacks it) and one of B.esp, and
	// location aliases holding a location of A.esp and one of B.esp.
	auto run = Filled(0x0001F0A1, 2, { Ref(0, 0x02000800), Ref(1, 0x03000D62), Ref(2, 0x00012345), Empty(3), Loc(4, 0x02000900), Loc(5, 0x03000A00) });
	const auto out = FormTranslate::Translate(hostEdge, Way::kToSession, Protocol::Encode(run));
	CHECK(out.kind != Result::Kind::kDrop);
	auto remote = out.kind == Result::Kind::kChanged ? Protocol::DecodeQuestRun(out.packet) : Protocol::DecodeQuestRun(Protocol::Encode(run));
	REQUIRE(remote);
	remote->playerId = 1;
	const auto in = FormTranslate::Translate(guestEdge, Way::kToLocal, Protocol::Encode(*remote, MT::kRemoteQuestRun));
	REQUIRE(in.kind == Result::Kind::kChanged);
	const auto heard = Protocol::DecodeQuestRun(in.packet);
	REQUIRE(heard);
	REQUIRE(heard->fills.size() == 6);
	CHECK(heard->fills[4] == Unknown(4));          // A.esp's location: the guest's own pick stays
	CHECK(heard->fills[5] == Loc(5, 0x02000A00));  // B.esp's, as the guest numbers it
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

	auto said = keeper.Read(0x10, { Ref(1, 0x50), Empty(0) }, T0);
	REQUIRE(said);
	CHECK(said->flags == QuestRun::kFilled);
	CHECK(said->run == 1);
	CHECK(said->fills == (std::vector<AliasFill>{ Empty(0), Ref(1, 0x50) }));
	CHECK(!keeper.Read(0x10, { Empty(0), Ref(1, 0x50) }, T0 + 1s));
	CHECK(keeper.Read(0x10, { Empty(0), Ref(1, 0x51) }, T0 + 2s));
	CHECK(!keeper.Read(0x99, { Ref(1, 0x50) }, T0));  // not a run
	// said again unchanged after RESAY_EVERY (a word the server's budget dropped is not lost for the run)
	CHECK(!keeper.Read(0x10, { Empty(0), Ref(1, 0x51) }, T0 + 2s + QuestRunRules::RESAY_EVERY - 1s));
	CHECK(keeper.Read(0x10, { Empty(0), Ref(1, 0x51) }, T0 + 2s + QuestRunRules::RESAY_EVERY));
	// a radiant job's fills are said fresh (a fills word without its start begins the run as the start would), a
	// snapshot's run is not
	const auto jobFills = keeper.Read(0x20, { Ref(0, 0x1) }, T0);
	REQUIRE(jobFills);
	CHECK(jobFills->flags == (QuestRun::kFilled | QuestRun::kFresh));
	CHECK(Protocol::DecodeQuestRun(Protocol::Encode(*jobFills)));
	QuestRunRules::Keeper snap;
	CHECK(snap.Started(0x30, true, false).flags == 0);
	const auto snapFills = snap.Read(0x30, { Ref(0, 0x1) }, T0);
	REQUIRE(snapFills);
	CHECK(snapFills->flags == QuestRun::kFilled);

	// A shared quest's stop is its own scripts' in every game; a radiant job's ends its run, said after STOP_LINGER.
	CHECK(!keeper.Stopped(0x10, T0));
	CHECK(!keeper.Live(0x10));
	CHECK(keeper.Stopped(0x20, T0));
	CHECK(!keeper.Stopped(0x20, T0));  // once
	CHECK(keeper.StopsDue(T0 + QuestRunRules::STOP_LINGER - 1ms).empty());
	const auto stops = keeper.StopsDue(T0 + QuestRunRules::STOP_LINGER);
	REQUIRE(stops.size() == 1);
	CHECK(stops[0].flags == QuestRun::kStopped);
	CHECK(stops[0].run == 2);
	CHECK(stops[0].questId == 0x20);
	CHECK(keeper.StopsDue(T0 + 10s).empty());
	CHECK(!keeper.Live(0x20));
	CHECK(!keeper.Read(0x20, { Ref(0, 0x1) }, T0));

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

TEST("quest follower: the host's stop of a radiant job this game runs for it stops it here once its last word is made; a stop of one it does not run says to forget")
{
	using QuestRunRules::Act;
	using QuestRunRules::Phase;
	QuestRunRules::Follower f;
	auto                    job = RunsHere(true);
	job.running = true;
	CHECK(f.Hear(Start(0x10, 2, true), job, T0));  // fresh: the earlier run's word goes
	CHECK(f.Due(0x10, job, T0) == Act::kAdopt);
	CHECK(!f.Hear(Stop(0x10, 2), job, T0 + 1s));  // not forgotten yet: its last stages are made first
	CHECK(f.PhaseOf(0x10) == Phase::kStopping);
	CHECK(f.Ours(0x10));   // its stages are still made here as the run's
	CHECK(!f.Hold(0x10));
	CHECK(!f.ForceDue(0x10, T0 + 1s));
	CHECK(!f.Hear(Stop(0x10, 2), job, T0 + 1s));  // the same stop again: nothing
	CHECK(f.Quests() == (std::vector<std::uint32_t>{ 0x10 }));
	CHECK(f.Due(0x10, job, T0 + 1s, true) == Act::kNothing);  // the session's word on it still waits to be made
	CHECK(f.Due(0x10, job, T0 + 5s, true) == Act::kNothing);
	CHECK(f.Due(0x10, job, T0 + 6s, false) == Act::kStop);    // made: stopped here, its word forgotten now
	CHECK(!f.Ours(0x10));
	CHECK(f.Due(0x10, job, T0 + 7s) == Act::kNothing);  // once
	CHECK(f.Quests().empty());
	// what waits for a far friend, or is stuck, does not keep the job running past STOP_WAIT
	QuestRunRules::Follower w;
	w.Hear(Start(0x11, 1, true), job, T0);
	CHECK(w.Due(0x11, job, T0) == Act::kAdopt);
	CHECK(!w.Hear(Stop(0x11, 1), job, T0 + 1s));
	CHECK(w.Due(0x11, job, T0 + QuestRunRules::STOP_WAIT, true) == Act::kNothing);
	CHECK(w.Due(0x11, job, T0 + 1s + QuestRunRules::STOP_WAIT, true) == Act::kStop);
	// one that stopped here on its own (its last stage, made from the session's word, runs Stop) is not stopped again
	QuestRunRules::Follower e;
	e.Hear(Start(0x12, 1, true), job, T0);
	CHECK(e.Due(0x12, job, T0) == Act::kAdopt);
	CHECK(!e.Hear(Stop(0x12, 1), job, T0 + 1s));
	auto stopped = job;
	stopped.running = false;
	e.StoppedHere(0x12);
	CHECK(e.Due(0x12, stopped, T0 + 2s, true) == Act::kEnd);
	CHECK(e.Quests().empty());

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

TEST("quest keeper: a radiant job's last stage and its stop in one frame: the stage is the run's, the stop is said after it; a new start says the stop first")
{
	QuestRunRules::Keeper keeper;
	keeper.Started(0x20, true);
	REQUIRE(keeper.Read(0x20, { Ref(0, 0x1) }, T0));
	// the frame's batch: the job's stop event, and its last stage's event after it (the engine may report it so)
	CHECK(keeper.Stopped(0x20, T0));
	CHECK(keeper.Live(0x20));  // its last stage is still reported as the run's (QuestSync's SharedHere)
	CHECK(keeper.Stopping(0x20));
	CHECK(keeper.LiveQuests().empty());              // its fills are not read again
	CHECK(!keeper.Read(0x20, { Ref(0, 0x2) }, T0));  // ... nor said
	CHECK(keeper.StopsDue(T0).empty());              // the frame's stages go out first; the stop later
	REQUIRE(keeper.StopsDue(T0 + QuestRunRules::STOP_LINGER).size() == 1);
	CHECK(!keeper.Live(0x20));

	// begun again before the stop was said: the stop is said first, then the new run
	keeper.Started(0x30, true);
	CHECK(keeper.Stopped(0x30, T0));
	const auto stop = keeper.StopNow(0x30);
	REQUIRE(stop);
	CHECK(stop->flags == QuestRun::kStopped);
	const auto again = keeper.Started(0x30, true);
	CHECK(again.run > stop->run);
	CHECK(!keeper.StopNow(0x30));
	CHECK(keeper.StopsDue(T0 + 1min).empty());  // said once
	CHECK(keeper.Live(0x30));
	// a shared quest's stop is no word at all
	keeper.Started(0x40, false);
	CHECK(!keeper.Stopped(0x40, T0));
	CHECK(!keeper.StopNow(0x40));
}

TEST("quest follower: a new run heard while the earlier one still stops here stops the earlier first, then starts the new one with its word held")
{
	using QuestRunRules::Act;
	using QuestRunRules::Phase;
	QuestRunRules::Follower f;
	auto                    job = RunsHere(true);
	job.running = true;
	f.Hear(Filled(0x10, 1, { Ref(0, 0x50) }), job, T0);
	CHECK(f.Due(0x10, job, T0) == Act::kAdopt);
	CHECK(!f.Hear(Stop(0x10, 1), job, T0 + 1s));
	CHECK(f.Due(0x10, job, T0 + 1s, true) == Act::kNothing);
	CHECK(f.Hear(Start(0x10, 2, true), job, T0 + 2s));  // fresh: the earlier run's word goes
	CHECK(f.PhaseOf(0x10) == Phase::kRestarting);
	CHECK(f.Hold(0x10));      // the new run's word waits for its start
	CHECK(!f.WasOurs(0x10));  // not adopted: started afresh for the new run
	CHECK(f.Due(0x10, job, T0 + 2s) == Act::kStopOld);
	CHECK(f.Due(0x10, job, T0 + 3s) == Act::kNothing);  // its stop not made yet
	f.Hear(Filled(0x10, 2, { Ref(0, 0x51) }), job, T0 + 3s);
	job.running = false;
	CHECK(f.Due(0x10, job, T0 + 4s) == Act::kStart);
	f.StartMade(0x10, { .refs = 1, .forces = { { 0, 0x51 } } });
	job.running = true;
	CHECK(f.Due(0x10, job, T0 + 5s) == Act::kStarted);
	CHECK(f.RunOf(0x10) == 2);
	CHECK(f.Ours(0x10));
}

TEST("quest follower: starts for runs are paced, one per START_PACE, the oldest run heard first; adoptions are not held")
{
	using QuestRunRules::Act;
	QuestRunRules::Follower f;
	const auto              idle = RunsHere(true);
	// a welcome into a long game: forty runs heard at once, with their fills, newest quest ids first
	for (std::uint32_t i = 0; i < 40; ++i) {
		f.Hear(Filled(0x100 + 40 - i, 1, { Ref(0, 0x50 + i) }), idle, T0);
	}
	auto running = RunsHere(true);
	running.running = true;
	f.Hear(Filled(0x90, 1, { Ref(0, 0x50) }), running, T0);
	const auto order = f.Quests();
	REQUIRE(order.size() == 41);
	CHECK(order.front() == 0x128);  // heard first
	std::vector<std::uint32_t> starts;
	auto                       now = T0;
	for (int frame = 0; frame < 1000 && starts.size() < 40; ++frame, now += 16ms) {
		std::size_t thisFrame = 0;
		for (const auto id : f.Quests()) {
			const auto act = f.Due(id, id == 0x90 ? running : idle, now);
			if (act == Act::kStart) {
				starts.push_back(id);
				++thisFrame;
			}
			if (id == 0x90 && frame == 0) {
				CHECK(act == Act::kAdopt);  // taking a quest that runs here already is no start
			}
		}
		CHECK(thisFrame <= 1);
	}
	REQUIRE(starts.size() == 40);
	CHECK(starts.front() == 0x128);
	CHECK(starts.back() == 0x101);
	CHECK(std::ranges::is_sorted(starts, std::greater<>{}));  // the order heard
	CHECK(now - T0 >= 39 * QuestRunRules::START_PACE);
	// a run whose turn waits is held, not given up: START_WAIT counts from its start
	QuestRunRules::Follower g;
	g.Hear(Filled(0x1, 1, { Ref(0, 0x50) }), idle, T0);
	g.Hear(Filled(0x2, 1, { Ref(0, 0x50) }), idle, T0);
	CHECK(g.Due(0x1, idle, T0) == Act::kStart);
	CHECK(g.Due(0x2, idle, T0) == Act::kNothing);
	CHECK(g.Hold(0x2));
	CHECK(g.Due(0x2, idle, T0 + QuestRunRules::START_PACE) == Act::kStart);
}

TEST("quest follower: a quest that stays starting here on its own start does not hold the run's stages past START_WAIT")
{
	using QuestRunRules::Act;
	using QuestRunRules::Phase;
	QuestRunRules::Follower f;
	auto                    starting = RunsHere(true);
	starting.starting = true;
	f.Hear(Filled(0x10, 1, { Ref(0, 0x50) }), starting, T0);
	CHECK(f.Due(0x10, starting, T0) == Act::kNothing);  // its own start is under way: adopted once it runs
	CHECK(f.Hold(0x10));
	CHECK(f.Due(0x10, starting, T0 + QuestRunRules::START_WAIT - 1s) == Act::kNothing);
	CHECK(f.Due(0x10, starting, T0 + QuestRunRules::START_WAIT) == Act::kGiveUp);
	CHECK(!f.Hold(0x10));
	// it runs after all: the run's, its dropped stages made again
	auto running = RunsHere(true);
	running.running = true;
	CHECK(f.Due(0x10, running, T0 + 20s) == Act::kAdopt);
	CHECK(f.TakeLate(0x10));
	CHECK(f.PhaseOf(0x10) == Phase::kRunning);
}

TEST("quest follower: a radiant job this game ran for a run is stopped after the host's reset or a load when no run claims it within START_WAIT")
{
	using QuestRunRules::Act;
	auto job = RunsHere(true);
	job.running = true;
	// the host dies mid-bounty and loads a save from before it: its world's runs come again without the bounty
	QuestRunRules::Follower f;
	f.Hear(Filled(0x10, 1, { Ref(0, 0x50) }), RunsHere(true), T0);
	CHECK(f.Due(0x10, RunsHere(true), T0) == Act::kStart);
	f.StartMade(0x10, {});
	CHECK(f.Due(0x10, job, T0 + 1s) == Act::kStarted);
	f.Hear(Filled(0x20, 1, { Ref(0, 0x51) }), job, T0);  // another job, taken (adopted)
	CHECK(f.Due(0x20, job, T0 + 1s) == Act::kAdopt);
	f.Hear(Filled(0x30, 1, { Ref(0, 0x52) }), RunsHere(), T0);  // a shared quest: the caller never stops it
	CHECK(f.Due(0x30, RunsHere(), T0 + 1s) == Act::kStart);
	f.Hear(Start(0x40, 1, true), RunsHere(true), T0);  // heard, never this game's
	CHECK(f.Ran() == (std::vector<std::uint32_t>{ 0x10, 0x20, 0x30 }));
	f.Clear();  // the host's world new (kQuestReset)
	CHECK(f.Candidates() == 3);
	CHECK(f.Orphans(T0 + 1h).empty());  // not armed: nothing is judged
	f.Arm(T0 + 2s);
	f.Hear(Filled(0x20, 2, { Ref(0, 0x51) }), job, T0 + 3s);  // the new world runs that job too: claimed
	CHECK(f.Orphans(T0 + 2s + QuestRunRules::START_WAIT - 1s).empty());
	CHECK(f.Orphans(T0 + 2s + QuestRunRules::START_WAIT) == (std::vector<std::uint32_t>{ 0x10, 0x30 }));
	CHECK(f.Orphans(T0 + 1h).empty());  // once
	CHECK(f.Candidates() == 0);
	CHECK(f.OrphansFound() == 2);
	CHECK(f.Due(0x20, job, T0 + 4s) == Act::kAdopt);

	// a load of this game's save that ran a job for a run (its co-save): stopped unless a run claims it; a new game or a
	// save without the record leaves none; a job this game stopped, or started on its own since, is not stopped
	QuestRunRules::Follower g;
	g.Loaded({ 0x10, 0x11, 0x12 });
	g.StoppedHere(0x11);
	g.StartedHere(0x12);
	CHECK(g.Ran() == (std::vector<std::uint32_t>{ 0x10 }));
	g.Arm(T0);
	CHECK(g.Orphans(T0 + QuestRunRules::START_WAIT) == (std::vector<std::uint32_t>{ 0x10 }));
	g.Loaded({});
	g.Arm(T0);
	CHECK(g.Orphans(T0 + 1h).empty());

	// the session's end keeps them (a friend playing on alone keeps its jobs) until it is a friend in a session again;
	// a game that becomes the host keeps its world's jobs as its own
	QuestRunRules::Follower h;
	h.Hear(Filled(0x10, 1, { Ref(0, 0x50) }), job, T0);
	CHECK(h.Due(0x10, job, T0) == Act::kAdopt);
	h.Clear();
	CHECK(h.Orphans(T0 + 1h).empty());
	CHECK(h.Ran() == (std::vector<std::uint32_t>{ 0x10 }));  // into the next save's co-save
	h.Forsake();
	h.Arm(T0);
	CHECK(h.Orphans(T0 + 1h).empty());
	CHECK(h.Ran().empty());
}

TEST("quest maker: Has says what of a quest still waits to be made here, a stage in flight included")
{
	QuestRules::Maker maker;
	const auto        look = [](const QuestEntry&, std::uint32_t) { return QuestRules::Here{ .exists = true, .cls = QuestRules::Class::kShared }; };
	CHECK(!maker.Has(0x10));
	maker.Add(Stage(0x10, 200), 1, T0);
	maker.Add(Stage(0x20, 5), 1, T0);
	CHECK(maker.Has(0x10));
	auto step = maker.Next(T0, look);
	REQUIRE(step);
	CHECK(step->want.entry == Stage(0x10, 200));
	CHECK(maker.Has(0x10));  // made, not read done yet
	maker.Done(0x10, 200, T0 + 100ms);
	CHECK(!maker.Has(0x10));
	CHECK(maker.Has(0x20));
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

TEST("quest follower: a run given up for want of fills starts when they come late; a start that runs late is the run, its stages made again")
{
	using QuestRunRules::Act;
	using QuestRunRules::Phase;
	// (a) fills after START_WAIT: the run is given up, then started at once when they come
	QuestRunRules::Follower f;
	auto                    job = RunsHere(true);
	f.Hear(Start(0x10, 1, true), job, T0);
	CHECK(f.Due(0x10, job, T0 + QuestRunRules::START_WAIT) == Act::kGiveUp);
	CHECK(f.Due(0x10, job, T0 + QuestRunRules::START_WAIT + 1s) == Act::kNothing);
	f.Hear(Filled(0x10, 1, { Ref(0, 0x50) }), job, T0 + 20s);
	CHECK(f.Due(0x10, job, T0 + 20s) == Act::kStart);
	CHECK(f.PhaseOf(0x10) == Phase::kStarting);
	CHECK(f.Hold(0x10));
	CHECK(f.CountsOf(0x10).retries == 1);
	f.StartMade(0x10, { .refs = 1, .forces = { { 0, 0x50 } } });
	job.running = true;
	CHECK(f.Due(0x10, job, T0 + 21s) == Act::kStarted);
	CHECK(f.Ours(0x10));
	CHECK(f.TakeLate(0x10));  // its stages dropped while it was given up are made again
	CHECK(!f.TakeLate(0x10));
	// a run started in time holds its stages meanwhile: nothing to make again
	QuestRunRules::Follower t;
	auto                    quick = RunsHere(true);
	t.Hear(Filled(0x11, 1, { Ref(0, 0x50) }), quick, T0);
	CHECK(t.Due(0x11, quick, T0) == Act::kStart);
	t.StartMade(0x11, {});
	quick.running = true;
	CHECK(t.Due(0x11, quick, T0 + 1s) == Act::kStarted);
	CHECK(!t.TakeLate(0x11));

	// (b) our start runs only after START_WAIT: given up meanwhile, then the run's (kStarted, its dropped stages made again)
	QuestRunRules::Follower g;
	auto                    here = RunsHere(true);
	g.Hear(Filled(0x20, 1, { Ref(0, 0x50) }), here, T0);
	CHECK(g.Due(0x20, here, T0) == Act::kStart);
	g.StartMade(0x20, {});
	here.starting = true;
	CHECK(g.Due(0x20, here, T0 + QuestRunRules::START_WAIT) == Act::kGiveUp);
	CHECK(!g.Ours(0x20));
	CHECK(!g.Hold(0x20));
	CHECK(g.Due(0x20, here, T0 + QuestRunRules::START_WAIT + QuestRunRules::RETRY_EVERY) == Act::kNothing);  // still starting: not again
	here.starting = false;
	here.running = true;
	CHECK(g.Due(0x20, here, T0 + 15s) == Act::kStarted);
	CHECK(g.Ours(0x20));
	CHECK(g.TakeLate(0x20));
	CHECK(!g.TakeLate(0x20));  // once

	// ... and the host's stop of it stops it here, even while it was given up (it may run yet)
	QuestRunRules::Follower h;
	auto                    late = RunsHere(true);
	h.Hear(Filled(0x30, 4, { Ref(0, 0x50) }), late, T0);
	CHECK(h.Due(0x30, late, T0) == Act::kStart);
	h.StartMade(0x30, {});
	late.starting = true;
	CHECK(h.Due(0x30, late, T0 + QuestRunRules::START_WAIT) == Act::kGiveUp);
	CHECK(!h.Hear(Stop(0x30, 4), late, T0 + 11s));  // its word forgotten once stopped
	CHECK(h.Due(0x30, late, T0 + 11s) == Act::kStop);

	// a run given up that nothing of this game's started is not stopped here (a friend's own job stays its own)
	QuestRunRules::Follower k;
	auto                    none = RunsHere(true);
	k.Hear(Start(0x40, 1, true), none, T0);
	CHECK(k.Due(0x40, none, T0 + QuestRunRules::START_WAIT) == Act::kGiveUp);
	none.running = true;
	CHECK(k.Hear(Stop(0x40, 1), none, T0 + 11s));
	CHECK(k.Due(0x40, none, T0 + 11s) == Act::kNothing);
}

TEST("quest follower: a start the game would not make is given up at once and tried again; one that cannot start waits for new fills")
{
	using QuestRunRules::Act;
	using QuestRunRules::Phase;
	QuestRunRules::Follower f;
	auto                    here = RunsHere(true);
	f.Hear(Filled(0x10, 1, { Ref(0, 0x50) }), here, T0);
	CHECK(f.Due(0x10, here, T0) == Act::kStart);
	f.StartFailed(0x10, T0, true);
	CHECK(f.PhaseOf(0x10) == Phase::kGaveUp);
	CHECK(!f.Hold(0x10));  // nothing runs: its stages are not held for START_WAIT
	CHECK(f.GaveUp() == 1);
	CHECK(f.Due(0x10, here, T0 + QuestRunRules::RETRY_EVERY - 1s) == Act::kNothing);
	CHECK(f.Due(0x10, here, T0 + QuestRunRules::RETRY_EVERY) == Act::kStart);
	CHECK(f.Retried() == 1);

	// cannot start (an alias only the event fills had nothing forced): not tried again until the host's fills change
	f.StartFailed(0x10, T0 + 30s, false);
	CHECK(f.Due(0x10, here, T0 + 10min) == Act::kNothing);
	f.Hear(Filled(0x10, 1, { Ref(0, 0x50), Loc(1, 0x60) }), here, T0 + 11min);
	CHECK(f.Due(0x10, here, T0 + 11min) == Act::kStart);
	// StartFailed only answers a start under way
	f.StartMade(0x10, {});
	here.running = true;
	CHECK(f.Due(0x10, here, T0 + 11min + 1s) == Act::kStarted);
	f.StartFailed(0x10, T0 + 12min, true);
	CHECK(f.Ours(0x10));
}

TEST("quest follower: a radiant job taken with another location is stopped and started again once; forcings, missing references and locations apart said once")
{
	using QuestRunRules::Act;
	using QuestRunRules::Force;
	using QuestRunRules::Phase;
	QuestRunRules::Follower f;
	auto                    job = RunsHere(true);
	job.running = true;
	f.Hear(Filled(0x10, 1, { Ref(0, 0x50), Loc(1, 0x60) }), job, T0);
	CHECK(f.Due(0x10, job, T0) == Act::kAdopt);
	CHECK(f.Restart(0x10, T0));
	CHECK(!f.Restart(0x10, T0));  // once a run
	CHECK(f.PhaseOf(0x10) == Phase::kRestarting);
	CHECK(f.Hold(0x10));
	CHECK(!f.Ours(0x10));
	CHECK(f.Due(0x10, job, T0 + 1s) == Act::kNothing);  // its stop not made yet
	f.StoppedHere(0x10);                                 // the stop's event: the restart's, not an end
	CHECK(f.PhaseOf(0x10) == Phase::kRestarting);
	job.running = false;
	CHECK(f.Due(0x10, job, T0 + 2s) == Act::kStart);
	f.StartMade(0x10, { .refs = 1, .locs = 1, .forces = { { 0, 0x50 } } });
	job.running = true;
	CHECK(f.Due(0x10, job, T0 + 3s) == Act::kStarted);
	CHECK(f.TakeLate(0x10));  // what its stop undid is made again
	CHECK(f.CountsOf(0x10).restarted);
	CHECK(f.Restarts() == 1);
	CHECK(!f.Restart(0x10, T0 + 4s));

	// a restart whose stop never comes is given up after START_WAIT
	QuestRunRules::Follower s;
	auto                    stuck = RunsHere(true);
	stuck.running = true;
	s.Hear(Start(0x20, 1, true), stuck, T0);
	CHECK(s.Due(0x20, stuck, T0) == Act::kAdopt);
	CHECK(s.Restart(0x20, T0));
	CHECK(s.Due(0x20, stuck, T0 + QuestRunRules::START_WAIT) == Act::kGiveUp);

	// said once each per run: a forcing (missed when the start gave it, else later), a reference not loaded, a location apart
	CHECK(f.Forced(0x10, Force{ 0, 0x50 }));
	CHECK(!f.Forced(0x10, Force{ 0, 0x50 }));
	CHECK(f.Forced(0x10, Force{ 2, 0x52 }));
	CHECK(f.CountsOf(0x10).missed == 1);
	CHECK(f.CountsOf(0x10).later == 1);
	CHECK(f.NotLoaded(0x10, Force{ 3, 0x53 }));
	CHECK(!f.NotLoaded(0x10, Force{ 3, 0x53 }));
	CHECK(f.NotLoaded(0x10, Force{ 4, 0x53 }));
	CHECK(f.CountsOf(0x10).notLoaded == 2);
	CHECK(f.NotLoadedCount() == 2);
	CHECK(f.LocApart(0x10, Force{ 1, 0x61 }));
	CHECK(!f.LocApart(0x10, Force{ 1, 0x61 }));
	CHECK(f.LocApartCount() == 1);
	// a new run says them afresh
	f.Hear(Start(0x10, 2, true), job, T0 + 5s);
	CHECK(f.NotLoaded(0x10, Force{ 3, 0x53 }));
	CHECK(f.CountsOf(0x10).notLoaded == 1);
	CHECK(f.CountsOf(0x10).missed == 0);
}

TEST("quest start plan: the host's references and locations forced, optional empties made empty, the rest this game's own; an event's alias left unfilled cannot start")
{
	using QuestRunRules::AliasRecord;
	using QuestRunRules::FillType;
	using QuestRunRules::PlanStep;
	using QuestRunRules::Way;
	const std::vector<AliasRecord> record{
		{ 0, true, FillType::kFromEvent, false },    // the camp the bounty names (the Story Manager's event)
		{ 1, false, FillType::kFromAlias, false },   // its leader (from the camp)
		{ 2, false, FillType::kConditions, true },   // an optional witness
		{ 3, false, FillType::kCreated, false },     // a courier the quest creates
		{ 4, false, FillType::kConditions, false },  // a reference the friend has not loaded
		{ 5, true, FillType::kConditions, true },    // an optional location the host left empty
	};
	const std::vector<AliasFill> fills{ Loc(0, 0x60), Ref(1, 0x51), Empty(2), Runtime(3), Ref(4, 0x54), Empty(5) };
	const auto have = [](std::uint32_t a_form, bool) { return a_form != 0x54; };
	const auto plan = QuestRunRules::PlanStart(record, &fills, true, have);
	CHECK(plan.steps == (std::vector<PlanStep>{ { 0, true, Way::kLoc, 0x60 }, { 1, false, Way::kRef, 0x51 }, { 2, false, Way::kNone, 0 }, { 3, false, Way::kOwn, 0 },
						   { 4, false, Way::kOwn, 0 }, { 5, true, Way::kNone, 0 } }));
	CHECK(plan.note.refs == 1);
	CHECK(plan.note.locs == 1);
	CHECK(plan.note.nones == 2);
	CHECK(plan.note.own == 2);
	CHECK(plan.note.notLoaded == 1);
	CHECK(plan.note.forces == (std::vector<QuestRunRules::Force>{ { 1, 0x51 } }));
	CHECK(plan.CanStart());

	// no location can be written in this game: the camp stays its own, and only the event fills it: it cannot start
	const auto noLoc = QuestRunRules::PlanStart(record, &fills, false, have);
	CHECK(noLoc.steps[0].way == Way::kOwn);
	CHECK(noLoc.steps[5].way == Way::kOwn);
	CHECK(noLoc.locsKept == 2);
	CHECK(noLoc.needsEvent == (std::vector<std::uint32_t>{ 0 }));
	CHECK(!noLoc.CanStart());
	// the host's location unknown here (a plugin this game lacks, or not read there): the same
	const std::vector<AliasFill> unknown{ Unknown(0), Ref(1, 0x51) };
	CHECK(!QuestRunRules::PlanStart(record, &unknown, true, have).CanStart());
	// no fills at all: every alias its own, and the event's alias cannot be filled
	CHECK(!QuestRunRules::PlanStart(record, nullptr, true, have).CanStart());
	// an optional alias the event fills may stay empty: it starts
	const std::vector<AliasRecord> optional{ { 0, true, FillType::kFromEvent, true }, { 1, false, FillType::kConditions, false } };
	CHECK(QuestRunRules::PlanStart(optional, nullptr, true, have).CanStart());
	// a kind that is not the alias's (a location for a reference alias) is never forced
	const std::vector<AliasFill> crossed{ Ref(0, 0x60), Loc(1, 0x51) };
	const auto x = QuestRunRules::PlanStart(record, &crossed, true, have);
	CHECK(x.steps[0].way == Way::kOwn);
	CHECK(x.steps[1].way == Way::kOwn);
}

TEST("quest locations: the record's slot is the one every forced alias agrees on; the map is trusted only when it holds the forced locations")
{
	QuestRunRules::LocProbe probe;
	for (std::size_t i = 0; i + 1 < QuestRunRules::LOC_PROBE_MIN; ++i) {
		probe.Sample(0b000001);
	}
	CHECK(!probe.Slot());  // too few
	probe.Sample(0b000101);
	CHECK(probe.Slot() == std::optional<std::size_t>{ 0 });
	probe.Sample(0b000100);  // one that disagrees: no slot agreed by all
	CHECK(!probe.Slot());
	QuestRunRules::LocProbe two;
	for (std::size_t i = 0; i < QuestRunRules::LOC_PROBE_MIN; ++i) {
		two.Sample(0b000011);
	}
	CHECK(!two.Slot());  // two slots agree: not known which

	QuestRunRules::LocCheck good;
	for (std::size_t i = 0; i < QuestRunRules::LOC_CHECK_MIN; ++i) {
		CHECK(good.Now() == QuestRunRules::LocCheck::State::kUnread);
		good.Sample(i != 0 || QuestRunRules::LOC_CHECK_MIN < 5);  // one of five off (a script moved it): still verified
	}
	CHECK(good.Now() == QuestRunRules::LocCheck::State::kVerified);
	good.Sample(false);
	CHECK(good.Now() == QuestRunRules::LocCheck::State::kVerified);  // decided for good
	QuestRunRules::LocCheck bad;
	for (std::size_t i = 0; i < QuestRunRules::LOC_CHECK_MIN; ++i) {
		bad.Sample(i % 2 == 0);
	}
	CHECK(bad.Now() == QuestRunRules::LocCheck::State::kBad);
	CHECK(QuestRunRules::LocStateName(bad.Now()) == "bad");

	CHECK(QuestRunRules::LocFillOf(2, 0x60, true) == Loc(2, 0x60));
	CHECK(QuestRunRules::LocFillOf(2, 0, true) == Empty(2));
	CHECK(QuestRunRules::LocFillOf(2, 0x60, false) == Unknown(2));
	CHECK(QuestRunRules::LocFillOf(2, 0xFF000060, true) == Runtime(2));
	const std::vector<AliasFill> fills{ Ref(0, 0x50), Loc(1, 0x60), Ref(2, 0x52), Empty(3) };
	CHECK(QuestRunRules::CountOf(fills, AliasFill::kRef) == 2);
	CHECK(QuestRunRules::CountOf(fills, AliasFill::kLoc) == 1);
	CHECK(QuestRunRules::Describe(fills) == "0:00000050,1:L00000060,2:00000052,3:-");

	// a location apart is not forced (given only at a start), and the run is not the same until it holds
	const auto c = QuestRunRules::Against({ Loc(1, 0x60), Ref(0, 0x50) }, { Loc(1, 0x61), Ref(0, 0x50) });
	CHECK(c.force.empty());
	CHECK(c.locs == (std::vector<QuestRunRules::Force>{ { 1, 0x60 } }));
	CHECK(!c.Same());
	CHECK(!c.LocsSame());
	CHECK(QuestRunRules::Against({ Loc(1, 0x60) }, { Loc(1, 0x60) }).Same());
	CHECK(!QuestRunRules::Against({ Loc(1, 0x60) }, { Unknown(1) }).LocsSame());  // not read here: not known to be the same

	// a location this game cannot read (its map not checked) is unread: never apart (no restart, no locApart), never
	// forced, and the references are judged on their own (the plain legs' refSame)
	const auto u = QuestRunRules::Against({ Loc(1, 0x60), Ref(0, 0x50), Loc(2, 0x61) }, { Unknown(1), Ref(0, 0x50), Unknown(2) });
	CHECK(u.locs.empty());
	CHECK(u.force.empty());
	CHECK(u.unread == 2);
	CHECK(u.unknown == 0);
	CHECK(u.same == 1);
	CHECK(u.RefsSame());
	CHECK(!u.LocsSame());
	CHECK(!u.Same());
	// ... and a reference apart beside it is still forced
	const auto v = QuestRunRules::Against({ Loc(1, 0x60), Ref(0, 0x50) }, { Unknown(1), Ref(0, 0x51) });
	CHECK(v.locs.empty());
	CHECK(v.force == (std::vector<QuestRunRules::Force>{ { 0, 0x50 } }));
	CHECK(!v.RefsSame());
	// a location both games read and that differs stays apart, beside the same references
	const auto w = QuestRunRules::Against({ Loc(1, 0x60), Ref(0, 0x50) }, { Loc(1, 0x61), Ref(0, 0x50) });
	CHECK(w.RefsSame());
	CHECK(!w.LocsSame());
	CHECK(w.unread == 0);
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

TEST("server: a radiant job's last stage and its end, said just ahead of its stop, reach a friend before the stop")
{
	HostedSession s;
	auto&         f = s.f;
	REQUIRE(s.host);
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));
	f.fake->Deliver(1, Protocol::Encode(Start(0x50, 1, true)));
	f.fake->Deliver(1, Report({ Stage(0x50, 10) }));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kRemoteQuests)));
	REQUIRE(f.fake->Settle());
	f.fake->Take(2);
	// the host's frame: the job's last stage and its end (EndRun), then its stop, in that order
	const QuestEntry completed{ 0x50, QuestEntry::kCompleted, 0, 0 };
	f.fake->Deliver(1, Report({ Stage(0x50, 200), completed }));
	f.fake->Deliver(1, Protocol::Encode(Stop(0x50, 1)));
	const auto packets = f.fake->TakeThrough(2, Type(MT::kRemoteQuestRun));
	const auto entries = EntriesIn(packets);
	REQUIRE(entries.size() == 2);
	CHECK(entries[0].second == Stage(0x50, 200));
	CHECK(entries[1].second == completed);
	const auto lastEntry = std::ranges::find_if(packets, [](const auto& a_p) { return Protocol::PeekType(a_p) == MT::kRemoteQuests; });
	const auto stop = std::ranges::find_if(packets, [](const auto& a_p) { return Protocol::PeekType(a_p) == MT::kRemoteQuestRun; });
	REQUIRE(stop != packets.end());
	CHECK(lastEntry < stop);
	const auto runs = RunsIn(packets);
	REQUIRE(runs.size() == 1);
	CHECK(runs[0].flags == QuestRun::kStopped);
	REQUIRE(f.fake->Settle());
	// none of the host's word on the run was refused
	const auto answers = EntriesIn(f.fake->Take(1), MT::kQuestAnswer);
	CHECK(std::ranges::none_of(answers, [](const auto& a_a) { return a_a.second.value == QuestEntry::kRefused; }));
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
