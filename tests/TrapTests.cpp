#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"
#include "TrapBook.h"
#include "game/RefRules.h"
#include "game/TrapRules.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// World traps (docs/DESIGN.md, "Traps"): the messages (common/Protocol.h), their form IDs on the wire
// (common/FormTranslate.h), the server's book (common/TrapBook.h) and its handling (common/Server.cpp), and each game's
// rules (src/game/TrapRules.h). What reads and changes the game itself (src/game/Traps.cpp) is tested in a game only:
// tools/rig/steps/traps.sh.

using Protocol::Trap;
using Protocol::TrapBatch;
using Protocol::TrapKind;

namespace
{
	using MT = Protocol::MessageType;
	using namespace std::chrono_literals;
	using Clock = TrapRules::Clock;
	using TrapRules::Look;
	using TrapRules::Verdict;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	constexpr std::uint32_t TRIPWIRE = 0x0006A4F5;
	constexpr std::uint32_t BEAR = 0x0001F2E1;
	constexpr std::uint32_t HINGE = 0x000B7A1C;

	Trap Word(std::uint32_t a_ref, TrapKind a_kind = TrapKind::kTripwire, std::uint8_t a_phase = Trap::kFired, std::uint32_t a_base = TRIPWIRE)
	{
		return Trap{ .ref = a_ref, .base = a_base, .kind = a_kind, .phase = a_phase };
	}

	Trap Bear(std::uint32_t a_ref, std::uint8_t a_phase)
	{
		return Word(a_ref, TrapKind::kBearTrap, a_phase, BEAR);
	}

	std::vector<std::uint8_t> Batch(std::vector<Trap> a_entries)
	{
		TrapBatch batch;
		batch.entries = std::move(a_entries);
		return Protocol::Encode(batch);
	}

	std::vector<std::uint8_t> Ask()
	{
		return Protocol::Writer{ MT::kAskTraps }.Data();
	}

	std::vector<TrapBatch> Heard(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<TrapBatch> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == MT::kRemoteTraps) {
				if (const auto batch = Protocol::DecodeTrapBatch(packet)) {
					out.push_back(*batch);
				}
			}
		}
		return out;
	}

	std::vector<Trap> Entries(const std::vector<TrapBatch>& a_batches)
	{
		std::vector<Trap> out;
		for (const auto& batch : a_batches) {
			out.insert(out.end(), batch.entries.begin(), batch.entries.end());
		}
		return out;
	}

	const Clock::time_point T0 = Clock::time_point{} + 1h;

	// A loaded, settled trap of the word's base and kind, reading a_look, our player a_distance from it.
	TrapRules::Facts Here(const Trap& a_word, Look a_look, float a_distance = 300.0f)
	{
		return TrapRules::Facts{ .loaded = true, .settled = true, .base = a_word.base, .kind = a_word.kind, .look = a_look, .fresh = true, .playerDistance = a_distance };
	}
}

// ---- the messages -------------------------------------------------------------------------------------------------

TEST("traps: a game's batch and the server's go there and back as they were; the server's field never leaves a game")
{
	CHECK(Protocol::VERSION >= 32);
	TrapBatch ours;
	ours.entries = { Word(0x0001A2B3), Bear(0x0003C001, Trap::kArmed), Word(0x02000D10, TrapKind::kHinge, Trap::kDisarmed, HINGE) };
	const auto packet = Protocol::Encode(ours);
	CHECK(Protocol::PeekType(packet) == MT::kTraps);
	CHECK(packet.size() == 2 + 3 * 10);
	CHECK(Protocol::DecodeTrapBatch(packet) == ours);

	auto told = ours;
	told.entries[0].playerId = 7;
	CHECK(Protocol::DecodeTrapBatch(Protocol::Encode(told)) == ours);

	TrapBatch theirs;
	theirs.catchUp = true;
	theirs.entries = { Trap{ .ref = 0x0001A2B3, .base = TRIPWIRE, .kind = TrapKind::kTripwire, .phase = Trap::kFired, .playerId = 2 },
		Trap{ .ref = 0x0003C001, .base = BEAR, .kind = TrapKind::kBearTrap, .phase = Trap::kArmed, .playerId = 3 } };
	const auto remote = Protocol::Encode(theirs, MT::kRemoteTraps);
	CHECK(Protocol::PeekType(remote) == MT::kRemoteTraps);
	CHECK(Protocol::DecodeTrapBatch(remote) == theirs);
	theirs.catchUp = false;
	CHECK(Protocol::DecodeTrapBatch(Protocol::Encode(theirs, MT::kRemoteTraps))->catchUp == false);

	const auto ask = Ask();
	CHECK(ask.size() == 1);
	CHECK(Protocol::PeekType(ask) == MT::kAskTraps);
	CHECK(!Protocol::DecodeTrapBatch(ask));
}

TEST("traps: a batch with no entry, an entry of no reference or base, a kind or phase not known, a one-shot trap set again, or bytes left over is not read")
{
	CHECK(!Protocol::DecodeTrapBatch(Batch({})));
	CHECK(!Protocol::DecodeTrapBatch(Batch({ Word(0) })));
	CHECK(!Protocol::DecodeTrapBatch(Batch({ Word(0x1A2B3, TrapKind::kTripwire, Trap::kFired, 0) })));
	CHECK(!Protocol::DecodeTrapBatch(Batch({ Word(0x1A2B3, TrapKind::kNone) })));
	CHECK(!Protocol::DecodeTrapBatch(Batch({ Word(0x1A2B3, static_cast<TrapKind>(Protocol::TRAP_KIND_MAX + 1)) })));
	CHECK(!Protocol::DecodeTrapBatch(Batch({ Word(0x1A2B3, TrapKind::kTripwire, 0) })));
	CHECK(!Protocol::DecodeTrapBatch(Batch({ Word(0x1A2B3, TrapKind::kTripwire, Trap::PHASE_MAX + 1) })));
	CHECK(!Protocol::DecodeTrapBatch(Batch({ Word(0x1A2B3, TrapKind::kTripwire, Trap::kArmed) })));  // only a toggle is set again
	CHECK(Protocol::DecodeTrapBatch(Batch({ Bear(0x1A2B3, Trap::kArmed) })).has_value());

	auto longer = Batch({ Word(0x1A2B3) });
	longer.push_back(0);
	CHECK(!Protocol::DecodeTrapBatch(longer));
	auto shorter = Batch({ Word(0x1A2B3) });
	shorter.pop_back();
	CHECK(!Protocol::DecodeTrapBatch(shorter));
	auto badFlags = Protocol::Encode(TrapBatch{ .catchUp = true, .entries = { Word(0x1A2B3) } }, MT::kRemoteTraps);
	badFlags[1] = 2;
	CHECK(!Protocol::DecodeTrapBatch(badFlags));
}

TEST("traps: a long list goes in batches of MAX_TRAP_BATCH, each marked a catch-up when asked; none for none")
{
	std::vector<Trap> many;
	for (std::uint32_t i = 1; i <= 130; ++i) {
		many.push_back(Word(0x00100000 + i));
	}
	const auto packets = Protocol::TrapBatches(many, MT::kRemoteTraps, true);
	REQUIRE(packets.size() == 3);
	std::vector<Trap> back;
	for (const auto& packet : packets) {
		const auto batch = Protocol::DecodeTrapBatch(packet);
		REQUIRE(batch);
		CHECK(batch->catchUp);
		CHECK(batch->entries.size() <= Protocol::MAX_TRAP_BATCH);
		back.insert(back.end(), batch->entries.begin(), batch->entries.end());
	}
	CHECK(back == many);
	CHECK(Protocol::TrapBatches({}, MT::kTraps).empty());
}

TEST("traps: a one-shot trap keeps its first word; a bear trap's word of the other side replaces the one held")
{
	using Protocol::TrapSupersedes;
	CHECK(TrapSupersedes(TrapKind::kTripwire, Trap::kFired, std::nullopt));
	CHECK(!TrapSupersedes(TrapKind::kTripwire, Trap::kFired, Trap::kFired));
	CHECK(!TrapSupersedes(TrapKind::kHinge, Trap::kFired, Trap::kDisarmed));
	CHECK(!TrapSupersedes(TrapKind::kHinge, Trap::kDisarmed, Trap::kFired));
	CHECK(TrapSupersedes(TrapKind::kBearTrap, Trap::kArmed, Trap::kFired));
	CHECK(TrapSupersedes(TrapKind::kBearTrap, Trap::kFired, Trap::kArmed));
	CHECK(TrapSupersedes(TrapKind::kBearTrap, Trap::kDisarmed, Trap::kArmed));
	CHECK(!TrapSupersedes(TrapKind::kBearTrap, Trap::kDisarmed, Trap::kFired));  // shut either way
	CHECK(!TrapSupersedes(TrapKind::kBearTrap, Trap::kArmed, Trap::kArmed));
}

TEST("traps: references and bases are named by their plugin on the wire; an entry a game lacks is left out, and a batch left with none is not sent")
{
	using FormTranslate::Result;
	using FormTranslate::Way;
	using Names = std::vector<std::string>;
	const Names host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm", "A.esp", "B.esp" }, {});
	const Names guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "B.esp", "Update.esm", "A.esp", "Extra.esp" }, {});
	Names       session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const FormIds::Mapper hostEdge(host, session);
	const FormIds::Mapper guestEdge(guest, session);

	CHECK(FormTranslate::CarriesForms(MT::kTraps));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteTraps));
	CHECK(!FormTranslate::CarriesForms(MT::kAskTraps));

	// The guest set off a tripwire of B.esp (its 01) placed in Skyrim.esm's cell, and one of Extra.esp (its 04).
	const auto out = FormTranslate::Translate(guestEdge, Way::kToSession, Batch({ Word(0x00012345, TrapKind::kTripwire, Trap::kFired, 0x01000800), Word(0x04000D11) }));
	REQUIRE(out.kind == Result::Kind::kChanged);
	const auto told = Protocol::DecodeTrapBatch(out.packet);
	REQUIRE(told);
	REQUIRE(told->entries.size() == 2);
	CHECK(told->entries[0].base == 0x03000800);  // B.esp is the session's 03
	CHECK(told->entries[1].ref == 0x04000D11);   // Extra.esp its 04

	TrapBatch passed = *told;
	for (auto& entry : passed.entries) {
		entry.playerId = 2;
	}
	const auto atHost = FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(passed, MT::kRemoteTraps));
	REQUIRE(atHost.kind == Result::Kind::kChanged);
	CHECK(atHost.entries == 1);
	const auto heard = Protocol::DecodeTrapBatch(atHost.packet);
	REQUIRE(heard);
	REQUIRE(heard->entries.size() == 1);
	CHECK(heard->entries[0].base == 0x03000800);
	CHECK(heard->entries[0].playerId == 2);
	CHECK(heard->entries[0].kind == TrapKind::kTripwire);

	TrapBatch extraOnly;
	extraOnly.entries = { Trap{ .ref = 0x04000D11, .base = TRIPWIRE, .kind = TrapKind::kTripwire, .phase = Trap::kFired, .playerId = 2 } };
	CHECK(FormTranslate::Translate(hostEdge, Way::kToLocal, Protocol::Encode(extraOnly, MT::kRemoteTraps)).kind == Result::Kind::kDrop);

	const auto runtime = FormTranslate::Translate(guestEdge, Way::kToSession, Batch({ Word(0xFF000123), Word(0x00012345) }));
	REQUIRE(runtime.kind == Result::Kind::kChanged);
	const auto kept = Protocol::DecodeTrapBatch(runtime.packet);
	REQUIRE(kept);
	REQUIRE(kept->entries.size() == 1);
	CHECK(kept->entries[0].ref == 0x00012345);
}

// ---- the server's book --------------------------------------------------------------------------------------------

TEST("trap book: a one-shot trap is kept by its first word; a bear trap by its last of the other side; nobody is sent their own")
{
	TrapBook book;
	Trap     out;
	CHECK(book.Report(Word(0x1A2B3), 1, out) == TrapBook::Take::kNews);
	CHECK(out.playerId == 1);
	CHECK(book.Report(Word(0x1A2B3), 2, out) == TrapBook::Take::kSame);
	CHECK(book.Report(Word(0x1A2B3, TrapKind::kTripwire, Trap::kDisarmed), 2, out) == TrapBook::Take::kSame);
	CHECK(book.Find(0x1A2B3)->playerId == 1);

	CHECK(book.Report(Bear(0x3C001, Trap::kFired), 2, out) == TrapBook::Take::kNews);
	CHECK(book.Report(Bear(0x3C001, Trap::kDisarmed), 1, out) == TrapBook::Take::kSame);
	CHECK(book.Report(Bear(0x3C001, Trap::kArmed), 1, out) == TrapBook::Take::kNews);
	CHECK(out.playerId == 1);
	CHECK(out.phase == Trap::kArmed);
	// Another kind on a reference held: the first kind is kept.
	CHECK(book.Report(Word(0x3C001, TrapKind::kTripwire, Trap::kFired), 2, out) == TrapBook::Take::kSame);

	// Player 1 holds both now; player 2 hears both, player 3 both, player 1 none.
	CHECK(book.For(1).empty());
	const auto two = book.For(2);
	REQUIRE(two.size() == 2);
	CHECK(two[0].ref == 0x1A2B3);
	CHECK(two[1].ref == 0x3C001);  // said again last: last
	CHECK(book.For(3).size() == 2);
}

TEST("trap book: no reference, a runtime one, no base or a word that can't be held is refused; a full book keeps no new one")
{
	TrapBook book(2);
	Trap     out;
	CHECK(book.Report(Word(0), 1, out) == TrapBook::Take::kRefused);
	CHECK(book.Report(Word(0xFF000123), 1, out) == TrapBook::Take::kRefused);
	CHECK(book.Report(Word(0x1A2B3, TrapKind::kTripwire, Trap::kFired, 0), 1, out) == TrapBook::Take::kRefused);
	CHECK(book.Report(Word(0x1A2B3, TrapKind::kTripwire, Trap::kArmed), 1, out) == TrapBook::Take::kRefused);
	CHECK(book.Report(Word(0x1A2B3, TrapKind::kNone), 1, out) == TrapBook::Take::kRefused);
	CHECK(book.Size() == 0);
	CHECK(book.Report(Word(0x1), 1, out) == TrapBook::Take::kNews);
	CHECK(book.Report(Word(0x2), 1, out) == TrapBook::Take::kNews);
	CHECK(book.Report(Word(0x3), 1, out) == TrapBook::Take::kFull);
	CHECK(book.Report(Word(0x1), 2, out) == TrapBook::Take::kSame);  // a held one is still read
	book.Clear();
	CHECK(book.Size() == 0);
}

TEST("server: a trap's word goes to the others under its reporter's number, never back to it; the same again is not news")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	const auto cy = f.Join(3, Fixture::MakeHello("Cy"));
	REQUIRE(ada);
	REQUIRE(bo);
	REQUIRE(cy);
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);
	(void)f.fake->Take(3);

	f.fake->Deliver(1, Batch({ Word(0x1A2B3), Bear(0x3C001, Trap::kFired) }));
	for (const PeerId peer : { PeerId{ 2 }, PeerId{ 3 } }) {
		const auto heard = Heard(f.fake->TakeThrough(peer, Type(MT::kRemoteTraps)));
		REQUIRE(heard.size() == 1);
		CHECK(!heard[0].catchUp);
		REQUIRE(heard[0].entries.size() == 2);
		CHECK(heard[0].entries[0].ref == 0x1A2B3);
		CHECK(heard[0].entries[0].playerId == ada->playerId);
		CHECK(heard[0].entries[1].kind == TrapKind::kBearTrap);
	}
	REQUIRE(f.fake->Settle());
	CHECK(Heard(f.fake->Take(1)).empty());

	// Said again by Bo (its stand-in of Ada set it off there too): nothing new. A runtime reference: kept by none.
	f.fake->Deliver(2, Batch({ Word(0x1A2B3), Bear(0x3C001, Trap::kFired), Word(0xFF000801) }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 2 }, PeerId{ 3 } }) {
		CHECK(Heard(f.fake->Take(peer)).empty());
	}
	// Bo sets the bear trap again: news to Ada and Cy, Bo's now.
	f.fake->Deliver(2, Batch({ Bear(0x3C001, Trap::kArmed) }));
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 3 } }) {
		const auto news = Entries(Heard(f.fake->TakeThrough(peer, Type(MT::kRemoteTraps))));
		REQUIRE(news.size() == 1);
		CHECK(news[0].phase == Trap::kArmed);
		CHECK(news[0].playerId == bo->playerId);
	}
}

TEST("server: a joiner hears every kept trap but its own as a catch-up; a game that loaded asks and hears them again")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	REQUIRE(ada);
	REQUIRE(bo);
	std::vector<Trap> adas;
	for (std::uint32_t i = 1; i <= 70; ++i) {
		adas.push_back(Word(0x00100000 + i));
	}
	for (const auto& packet : Protocol::TrapBatches(adas, MT::kTraps)) {
		f.fake->Deliver(1, packet);
	}
	f.fake->Deliver(2, Batch({ Bear(0x3C001, Trap::kFired) }));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	const auto cy = f.Join(3, Fixture::MakeHello("Cy"));
	REQUIRE(cy);
	REQUIRE(f.fake->Settle());
	const auto caught = Heard(f.fake->Take(3));
	REQUIRE(caught.size() == 2);
	CHECK(std::ranges::all_of(caught, &TrapBatch::catchUp));
	const auto all = Entries(caught);
	REQUIRE(all.size() == 71);
	CHECK(all[0].ref == 0x00100001);
	CHECK(all[70].ref == 0x3C001);
	CHECK(all[70].playerId == bo->playerId);
	CHECK(Heard(f.fake->Take(1)).empty());

	f.fake->Deliver(2, Ask());
	REQUIRE(f.fake->Settle());
	const auto again = Entries(Heard(f.fake->Take(2)));
	REQUIRE(again.size() == 70);
	CHECK(std::ranges::none_of(again, [](const Trap& a_t) { return a_t.ref == 0x3C001; }));
	auto odd = Ask();
	odd.push_back(0);
	f.fake->Deliver(2, odd);
	REQUIRE(f.fake->Settle());
	CHECK(Heard(f.fake->Take(2)).empty());
}

TEST("server: past its budget a player's trap batches are dropped, said in the log once; a new session starts with none kept")
{
	Fixture    f;
	const auto ada = f.Join(1, Fixture::MakeHello("Ada"));
	const auto bo = f.Join(2, Fixture::MakeHello("Bo"));
	REQUIRE(ada);
	REQUIRE(bo);
	constexpr std::uint32_t BURST = 40;  // the budget holds 16 at once, and 4 more a second
	for (std::uint32_t i = 1; i <= BURST; ++i) {
		f.fake->Deliver(1, Batch({ Word(0x00200000 + i) }));
	}
	REQUIRE(f.fake->Settle());
	const auto passed = Heard(f.fake->Take(2)).size();
	CHECK(passed >= 16);
	CHECK(passed < BURST);
	CHECK(f.LoggedSoon("what became of its traps faster than the server passes it on"));
	const auto lines = f.LogLines();
	CHECK(std::ranges::count_if(lines, [](const std::string& a_l) { return a_l.find("what became of its traps") != std::string::npos; }) == 1);

	f.fake->Drop(1);
	f.fake->Drop(2);
	REQUIRE(f.fake->Settle());
	const auto cy = f.Join(3, Fixture::MakeHello("Cy"));
	REQUIRE(cy);
	f.fake->Deliver(3, Ask());
	REQUIRE(f.fake->Settle());
	CHECK(Heard(f.fake->Take(3)).empty());
}

// ---- each game's rules --------------------------------------------------------------------------------------------

TEST("trap rules: a trap is told by its scripts' chain, a mod's that extends one too; repeatable traps and other scripts are none")
{
	using TrapRules::Classify;
	using V = std::vector<std::string_view>;
	CHECK(Classify(V{ "Tripwire", "TrapTriggerBase", "ObjectReference" }) == TrapKind::kTripwire);
	CHECK(Classify(V{ "tripwire" }) == TrapKind::kTripwire);  // Papyrus names are not case-sensitive
	CHECK(Classify(V{ "MyModTripwire", "Tripwire", "TrapTriggerBase" }) == TrapKind::kTripwire);
	CHECK(Classify(V{ "TrapTriggerHinge", "TrapTriggerBase" }) == TrapKind::kHinge);
	CHECK(Classify(V{ "TrapRiggedRockfall", "TrapBase" }) == TrapKind::kRockfall);
	CHECK(Classify(V{ "TrapBreakAwayBase" }) == TrapKind::kBreakaway);
	CHECK(Classify(V{ "TrapBear" }) == TrapKind::kBearTrap);
	CHECK(Classify(V{ "trapSoulGemController" }) == TrapKind::kSoulGem);
	CHECK(Classify(V{ "TrapBase", "ObjectReference" }) == TrapKind::kNone);         // blades, darts: they set themselves again
	CHECK(Classify(V{ "PressurePlate", "TrapTriggerBase" }) == TrapKind::kNone);    // each game's own
	CHECK(Classify(V{ "TrapTriggerBase" }) == TrapKind::kNone);
	CHECK(Classify(V{ "defaultActivateSelf" }) == TrapKind::kNone);
	CHECK(Classify(V{}) == TrapKind::kNone);
}

TEST("trap rules: each kind's states read armed, going, spent, disarmed or between; a state its kind does not name is unknown")
{
	using TrapRules::LookOf;
	CHECK(LookOf(TrapKind::kTripwire, "") == Look::kArmed);
	CHECK(LookOf(TrapKind::kTripwire, "inactive") == Look::kArmed);
	CHECK(LookOf(TrapKind::kTripwire, "Active") == Look::kGoing);
	CHECK(LookOf(TrapKind::kTripwire, "DoNothing") == Look::kFired);
	CHECK(LookOf(TrapKind::kHinge, "Active") == Look::kArmed);
	CHECK(LookOf(TrapKind::kHinge, "Triggered") == Look::kFired);
	CHECK(LookOf(TrapKind::kHinge, "Disarmed") == Look::kDisarmed);
	CHECK(LookOf(TrapKind::kRockfall, "Idle") == Look::kArmed);
	CHECK(LookOf(TrapKind::kRockfall, "DoNothing") == Look::kFired);
	CHECK(LookOf(TrapKind::kBreakaway, "UnBroken") == Look::kArmed);
	CHECK(LookOf(TrapKind::kBreakaway, "Destroyed") == Look::kFired);
	CHECK(LookOf(TrapKind::kBearTrap, "Open") == Look::kArmed);
	CHECK(LookOf(TrapKind::kBearTrap, "busy") == Look::kBusy);
	CHECK(LookOf(TrapKind::kBearTrap, "Closed") == Look::kFired);
	CHECK(LookOf(TrapKind::kBearTrap, "") == Look::kUnknown);  // its OnLoad picks open or closed
	CHECK(LookOf(TrapKind::kSoulGem, "inmagictrap") == Look::kArmed);
	CHECK(LookOf(TrapKind::kSoulGem, "disarmed") == Look::kDisarmed);
	CHECK(LookOf(TrapKind::kTripwire, "SomeModState") == Look::kUnknown);
	CHECK(LookOf(TrapKind::kNone, "") == Look::kUnknown);
	CHECK(TrapRules::PhaseOf(Look::kGoing) == Trap::kFired);
	CHECK(TrapRules::PhaseOf(Look::kDisarmed) == Trap::kDisarmed);
	CHECK(TrapRules::PhaseOf(Look::kArmed) == Trap::kArmed);
}

TEST("trap rules: a one-shot trap seen set, then spent, is told once; one spent as its cell loads, or flickering, never is")
{
	TrapRules::Watch watch;
	const auto       k = TrapKind::kTripwire;
	CHECK(!watch.Read(1, k, Look::kArmed, 500.0f, T0));
	CHECK(!watch.Read(1, k, Look::kArmed, 500.0f, T0 + 3s));
	const auto told = watch.Read(1, k, Look::kGoing, 5000.0f, T0 + 4s);  // our player far: a one-shot is told anyway
	REQUIRE(told);
	CHECK(*told == Trap::kFired);
	CHECK(!watch.Read(1, k, Look::kFired, 500.0f, T0 + 5s));  // going to spent: the same side
	CHECK(!watch.Read(1, k, Look::kFired, 500.0f, T0 + 20s));

	// Spent from its first read (the save's): never told. Set again by a cell reset and spent again: told (the book keeps the first).
	CHECK(!watch.Read(2, k, Look::kFired, 500.0f, T0));
	CHECK(!watch.Read(2, k, Look::kFired, 500.0f, T0 + 10s));
	CHECK(!watch.Read(2, k, Look::kArmed, 500.0f, T0 + 11s));  // a one-shot set again: never told
	CHECK(!watch.Read(2, k, Look::kArmed, 500.0f, T0 + 14s));
	CHECK(watch.Read(2, k, Look::kFired, 500.0f, T0 + 15s).has_value());

	// Read armed for less than SETTLE as its cell loads, then spent: a load's flicker.
	CHECK(!watch.Read(3, k, Look::kArmed, 500.0f, T0));
	CHECK(!watch.Read(3, k, Look::kFired, 500.0f, T0 + 500ms));
	CHECK(!watch.Read(3, k, Look::kFired, 500.0f, T0 + 10s));

	// Between looks and unknown states are read again, never a side.
	CHECK(!watch.Read(4, TrapKind::kBearTrap, Look::kUnknown, 500.0f, T0));
	CHECK(!watch.Read(4, TrapKind::kBearTrap, Look::kBusy, 500.0f, T0 + 1s));
	CHECK(!watch.Read(4, TrapKind::kBearTrap, Look::kArmed, 500.0f, T0 + 2s));
	CHECK(!watch.Settled(5, T0));
	CHECK(watch.Settled(4, T0 + 2s));
	CHECK(watch.Size() == 4);
	watch.Forget(4);
	CHECK(!watch.Has(4));
}

TEST("trap rules: a bear trap is told snapped and set again only with our player near; never one this game just made from a word")
{
	TrapRules::Watch watch;
	const auto       k = TrapKind::kBearTrap;
	CHECK(!watch.Read(1, k, Look::kArmed, 200.0f, T0));
	CHECK(!watch.Read(1, k, Look::kArmed, 200.0f, T0 + 3s));
	CHECK(!watch.Read(1, k, Look::kBusy, 200.0f, T0 + 4s));
	const auto snapped = watch.Read(1, k, Look::kFired, 200.0f, T0 + 5s);
	REQUIRE(snapped);
	CHECK(*snapped == Trap::kFired);
	CHECK(!watch.Read(1, k, Look::kFired, 200.0f, T0 + 8s));
	const auto set = watch.Read(1, k, Look::kArmed, 200.0f, T0 + 9s);
	REQUIRE(set);
	CHECK(*set == Trap::kArmed);

	// Snapped with nobody of ours near (a friend's stand-in, an NPC): told by the game where it happened, not here.
	CHECK(!watch.Read(2, k, Look::kArmed, 3000.0f, T0));
	CHECK(!watch.Read(2, k, Look::kArmed, 3000.0f, T0 + 3s));
	CHECK(!watch.Read(2, k, Look::kFired, 3000.0f, T0 + 4s));
	CHECK(!watch.Read(2, k, Look::kFired, -1.0f, T0 + 8s));
	CHECK(watch.FarToggles() == 1);

	// Made here from the session's word: not told back, within APPLY_QUIET; a change after it is ours again.
	CHECK(!watch.Read(3, k, Look::kArmed, 200.0f, T0));
	CHECK(!watch.Read(3, k, Look::kArmed, 200.0f, T0 + 3s));
	watch.Applied(3, T0 + 3s);
	CHECK(watch.RecentlyApplied(3, T0 + 4s));
	CHECK(!watch.Read(3, k, Look::kFired, 200.0f, T0 + 4s));
	CHECK(watch.Quieted() == 1);
	CHECK(!watch.RecentlyApplied(3, T0 + 9s));
	CHECK(watch.Read(3, k, Look::kArmed, 200.0f, T0 + 12s).has_value());
}

TEST("trap rules: a change with a friend's stand-in nearer to the trap than our player is that friend's, told by their game, never here")
{
	using TrapRules::Watch;
	// Who is nearer: a stand-in in the trap's cell or space and nearer than our player, or our player out of it.
	CHECK(Watch::StandInNearer(900.0f, 120.0f));
	CHECK(Watch::StandInNearer(-1.0f, 120.0f));
	CHECK(!Watch::StandInNearer(120.0f, 900.0f));
	CHECK(!Watch::StandInNearer(120.0f, TrapRules::NO_STAND_IN));
	CHECK(!Watch::StandInNearer(-1.0f, TrapRules::NO_STAND_IN));
	CHECK(!Watch::StandInNearer(300.0f, 300.0f));  // a tie: ours (each game tells its own; the session keeps the first)

	// How near someone came between two reads: a runner 120 units past a tripwire by the read crossed it (0), nearer than a
	// friend standing 100 units off; a jump (a load door) counts only where it ends; standing still, where it stands.
	using TrapRules::Closest;
	using TrapRules::Point;
	const Point trap{ 0.0f, 0.0f, 0.0f };
	CHECK(Closest(trap, Point{ -60.0f, 0.0f, 0.0f }, Point{ 120.0f, 0.0f, 0.0f }) < 0.01f);
	CHECK(Closest(trap, Point{ -60.0f, 30.0f, 0.0f }, Point{ 120.0f, 30.0f, 0.0f }) > 29.9f);
	CHECK(Closest(trap, Point{ -60.0f, 30.0f, 0.0f }, Point{ 120.0f, 30.0f, 0.0f }) < 30.1f);
	CHECK(Closest(trap, Point{ 100.0f, 0.0f, 0.0f }, Point{ 100.0f, 0.0f, 0.0f }) > 99.9f);
	CHECK(Closest(trap, Point{ 200.0f, 0.0f, 0.0f }, Point{ 400.0f, 0.0f, 0.0f }) < 200.1f);  // walking away: where it was
	CHECK(Closest(trap, Point{ 200.0f, 0.0f, 0.0f }, Point{ 400.0f, 0.0f, 0.0f }) > 199.9f);
	CHECK(Closest(trap, Point{ -5000.0f, 0.0f, 0.0f }, Point{ 3000.0f, 0.0f, 0.0f }) > 2999.0f);  // a jump: where it ended

	// A tripwire the friend's stand-in walked through here, a moment after the friend did in their game: not told here,
	// so the session holds the friend's word (theirs on a load of their own save; never a coin flip).
	Watch      watch;
	const auto k = TrapKind::kTripwire;
	CHECK(!watch.Read(1, k, Look::kArmed, 800.0f, T0, 2000.0f));
	CHECK(!watch.Read(1, k, Look::kArmed, 800.0f, T0 + 3s, 1500.0f));
	CHECK(!watch.Read(1, k, Look::kGoing, 800.0f, T0 + 4s, 60.0f));
	CHECK(watch.StandInsNearer() == 1);
	CHECK(!watch.Read(1, k, Look::kFired, 800.0f, T0 + 5s, 300.0f));  // spent since: the same side, never told late

	// Our player walked through it, the friend's stand-in further off: ours, told.
	CHECK(!watch.Read(2, k, Look::kArmed, 900.0f, T0, 700.0f));
	CHECK(!watch.Read(2, k, Look::kArmed, 300.0f, T0 + 3s, 700.0f));
	const auto ours = watch.Read(2, k, Look::kGoing, 80.0f, T0 + 4s, 700.0f);
	REQUIRE(ours);
	CHECK(*ours == Trap::kFired);

	// One an NPC set off with no stand-in in its cell or space: told, wherever our player is.
	CHECK(!watch.Read(3, k, Look::kArmed, 3000.0f, T0));
	CHECK(!watch.Read(3, k, Look::kArmed, 3000.0f, T0 + 3s));
	CHECK(watch.Read(3, k, Look::kFired, 3000.0f, T0 + 4s, TrapRules::NO_STAND_IN).has_value());

	// A bear trap the friend's stand-in stands at, our player within TOGGLE_REACH but further off: theirs.
	const auto b = TrapKind::kBearTrap;
	CHECK(!watch.Read(4, b, Look::kArmed, 600.0f, T0, 90.0f));
	CHECK(!watch.Read(4, b, Look::kArmed, 600.0f, T0 + 3s, 90.0f));
	CHECK(!watch.Read(4, b, Look::kFired, 600.0f, T0 + 4s, 90.0f));
	CHECK(watch.StandInsNearer() == 2);
	CHECK(watch.FarToggles() == 0);
	// ... and set open again by our player, who stands at it now: ours.
	CHECK(!watch.Read(4, b, Look::kFired, 80.0f, T0 + 7s, 600.0f));
	CHECK(watch.Read(4, b, Look::kArmed, 80.0f, T0 + 8s, 600.0f).has_value());
}

TEST("trap rules: a trap read once more as it leaves the world is told when that read is news: a magic trap's gem taken")
{
	// Taken by our player, the gem's controller goes "disarmed" and the gem is disabled in one call: the read that finds it
	// disabled reads "disarmed", the trap's spend.
	TrapRules::Watch watch;
	const auto       k = TrapKind::kSoulGem;
	CHECK(TrapRules::LookOf(k, "inmagictrap") == Look::kArmed);
	CHECK(TrapRules::LookOf(k, "disarmed") == Look::kDisarmed);
	CHECK(!watch.Read(1, k, TrapRules::LookOf(k, "inmagictrap"), 400.0f, T0));
	CHECK(!watch.Read(1, k, TrapRules::LookOf(k, "inmagictrap"), 90.0f, T0 + 3s));
	const auto taken = watch.Read(1, k, TrapRules::LookOf(k, "disarmed"), 90.0f, T0 + 3s + 250ms);
	REQUIRE(taken);
	CHECK(*taken == Trap::kDisarmed);
	CHECK(TrapRules::StepsFor(k, Verdict::kFireLive) == (std::vector<TrapRules::Step>{ { TrapRules::Step::kGoto, "disarmed" }, { TrapRules::Step::kDisable, {} } }));

	// Made here from a friend's word (its "disarmed" and its disable): the read as it leaves is never told back.
	CHECK(!watch.Read(2, k, Look::kArmed, 400.0f, T0));
	CHECK(!watch.Read(2, k, Look::kArmed, 400.0f, T0 + 3s));
	watch.Applied(2, T0 + 3s);
	CHECK(!watch.Read(2, k, Look::kDisarmed, 400.0f, T0 + 3s + 250ms));
	CHECK(watch.Quieted() == 1);

	// The word made on it is done once it left (its making ended in the disable); one never made waits for it to load.
	TrapRules::Waiting waiting;
	CHECK(waiting.Add(Word(0x2, k, Trap::kDisarmed), false, T0));
	CHECK(waiting.Add(Word(0x3, k, Trap::kDisarmed), true, T0));
	waiting.Find(0x2)->attempts = 1;
	CHECK(waiting.Left(0x2));
	CHECK(!waiting.Find(0x2));
	CHECK(!waiting.Left(0x3));
	CHECK(waiting.Find(0x3));
	CHECK(!waiting.Left(0x9));
}

TEST("trap rules: a word is made only on a loaded, settled trap of its base and kind; live when fresh and near, quiet otherwise")
{
	using TrapRules::Judge;
	const auto word = Word(0x1A2B3);
	CHECK(Judge(word, Here(word, Look::kArmed)) == Verdict::kFireLive);
	auto facts = Here(word, Look::kArmed, TrapRules::LIVE_REACH + 1.0f);
	CHECK(Judge(word, facts) == Verdict::kFireQuiet);  // our player far
	facts = Here(word, Look::kArmed, -1.0f);
	CHECK(Judge(word, facts) == Verdict::kFireQuiet);  // another cell
	facts = Here(word, Look::kArmed);
	facts.catchUp = true;
	CHECK(Judge(word, facts) == Verdict::kFireQuiet);  // a catch-up: spent long ago
	facts = Here(word, Look::kArmed);
	facts.fresh = false;
	CHECK(Judge(word, facts) == Verdict::kFireQuiet);  // waited for its cell

	CHECK(Judge(word, Here(word, Look::kGoing)) == Verdict::kNothing);
	CHECK(Judge(word, Here(word, Look::kFired)) == Verdict::kNothing);
	CHECK(Judge(word, Here(word, Look::kBusy)) == Verdict::kWait);
	CHECK(Judge(word, Here(word, Look::kUnknown)) == Verdict::kWait);
	CHECK(Judge(word, TrapRules::Facts{}) == Verdict::kWait);  // not loaded here
	facts = Here(word, Look::kArmed);
	facts.settled = false;
	CHECK(Judge(word, facts) == Verdict::kWait);
	facts = Here(word, Look::kArmed);
	facts.justMade = true;
	CHECK(Judge(word, facts) == Verdict::kWait);
	facts = Here(word, Look::kArmed);
	facts.base = 0x00099999;
	CHECK(Judge(word, facts) == Verdict::kSkip);  // another trap here
	facts = Here(word, Look::kArmed);
	facts.kind = TrapKind::kHinge;
	CHECK(Judge(word, facts) == Verdict::kSkip);
	facts = Here(word, Look::kArmed);
	facts.attempts = TrapRules::MAX_ATTEMPTS;
	CHECK(Judge(word, facts) == Verdict::kGiveUp);
	CHECK(Judge(Word(0x1A2B3, TrapKind::kTripwire, Trap::kArmed), Here(word, Look::kFired)) == Verdict::kSkip);  // a one-shot is never set again

	const auto disarmed = Word(0x2000, TrapKind::kHinge, Trap::kDisarmed, HINGE);
	CHECK(Judge(disarmed, Here(disarmed, Look::kArmed)) == Verdict::kDisarm);
	CHECK(Judge(disarmed, Here(disarmed, Look::kFired)) == Verdict::kNothing);  // it went off here first: spent either way

	const auto open = Bear(0x3000, Trap::kArmed);
	CHECK(Judge(open, Here(open, Look::kFired)) == Verdict::kRearm);
	CHECK(Judge(open, Here(open, Look::kArmed)) == Verdict::kNothing);
	const auto shut = Bear(0x3000, Trap::kFired);
	CHECK(Judge(shut, Here(shut, Look::kArmed)) == Verdict::kFireLive);
	CHECK(Judge(shut, Here(shut, Look::kBusy)) == Verdict::kWait);
}

TEST("trap rules: each kind is made its own script's way: live goes off, quiet and disarmed set the spent look, a bear trap snaps and opens")
{
	using TrapRules::Step;
	using TrapRules::StepsFor;
	using S = std::vector<Step>;
	CHECK(StepsFor(TrapKind::kTripwire, Verdict::kFireLive) == (S{ { Step::kGoto, "Active" } }));
	CHECK(StepsFor(TrapKind::kTripwire, Verdict::kFireQuiet) == (S{ { Step::kGoto, "DoNothing" } }));
	CHECK(StepsFor(TrapKind::kHinge, Verdict::kFireLive) == (S{ { Step::kGoto, "Triggered" } }));
	CHECK(StepsFor(TrapKind::kHinge, Verdict::kDisarm) == (S{ { Step::kGoto, "Disarmed" } }));
	CHECK(StepsFor(TrapKind::kRockfall, Verdict::kFireQuiet) == (S{ { Step::kGoto, "DoNothing" }, { Step::kAnim, "break" } }));
	CHECK(StepsFor(TrapKind::kBreakaway, Verdict::kFireLive) == (S{ { Step::kTriggerEnter, {} } }));
	CHECK(StepsFor(TrapKind::kBreakaway, Verdict::kFireQuiet) == (S{ { Step::kGoto, "Destroyed" }, { Step::kDisable, {} } }));
	CHECK(StepsFor(TrapKind::kBearTrap, Verdict::kFireLive) == (S{ { Step::kAnim, "Trigger01" }, { Step::kGoto, "Closed" } }));
	CHECK(StepsFor(TrapKind::kBearTrap, Verdict::kRearm) == (S{ { Step::kAnim, "Reset01" }, { Step::kGoto, "Open" } }));
	CHECK(StepsFor(TrapKind::kSoulGem, Verdict::kDisarm) == (S{ { Step::kGoto, "disarmed" }, { Step::kDisable, {} } }));
	CHECK(StepsFor(TrapKind::kTripwire, Verdict::kRearm).empty());  // a one-shot is never set again
	for (const auto verdict : { Verdict::kNothing, Verdict::kWait, Verdict::kSkip, Verdict::kGiveUp }) {
		CHECK(StepsFor(TrapKind::kBearTrap, verdict).empty());
	}
}

TEST("trap rules: our words go one per trap, the newest; at most MAX_TRAP_BATCH a batch, a batch every PACE; past MAX the oldest go")
{
	TrapRules::Outbox outbox;
	outbox.Add(Bear(0x1, Trap::kFired));
	outbox.Add(Word(0x2));
	auto again = Bear(0x1, Trap::kArmed);
	again.playerId = 9;  // never sent by a game
	outbox.Add(again);
	CHECK(outbox.Size() == 2);
	const auto first = outbox.Due(T0);
	REQUIRE(first);
	REQUIRE(first->size() == 2);
	CHECK((*first)[0].phase == Trap::kArmed);
	CHECK((*first)[0].playerId == 0);
	CHECK(!outbox.Due(T0 + 100ms));

	for (std::uint32_t i = 1; i <= TrapRules::Outbox::MAX + 10; ++i) {
		outbox.Add(Word(0x00100000 + i));
	}
	CHECK(outbox.Size() == TrapRules::Outbox::MAX);
	CHECK(outbox.Overflow() == 10);
	CHECK(!outbox.Due(T0 + 200ms));
	const auto next = outbox.Due(T0 + TrapRules::Outbox::PACE);
	REQUIRE(next);
	CHECK(next->size() == Protocol::MAX_TRAP_BATCH);
	CHECK(next->front().ref == 0x00100000 + 11);
	outbox.Clear();
	CHECK(outbox.Size() == 0);
}

TEST("trap rules: the session's words wait by reference, the newest on each tried afresh; a full list refuses new ones")
{
	TrapRules::Waiting waiting;
	CHECK(waiting.Add(Bear(0x1, Trap::kFired), true, T0));
	waiting.Find(0x1)->attempts = 2;
	CHECK(waiting.Add(Bear(0x1, Trap::kArmed), false, T0 + 1s));
	REQUIRE(waiting.Find(0x1));
	CHECK(waiting.Find(0x1)->word.phase == Trap::kArmed);
	CHECK(waiting.Find(0x1)->attempts == 0);
	CHECK(!waiting.Find(0x1)->catchUp);
	CHECK(waiting.Size() == 1);
	waiting.Remove(0x1);
	CHECK(!waiting.Find(0x1));

	for (std::uint32_t i = 1; i <= TrapRules::Waiting::MAX; ++i) {
		REQUIRE(waiting.Add(Word(0x00100000 + i), true, T0));
	}
	CHECK(!waiting.Add(Word(0x00900000), true, T0));
	CHECK(waiting.Refused() == 1);
	CHECK(waiting.Add(Word(0x00100001), false, T0));  // a word on one held is still taken
	waiting.Clear();
	CHECK(waiting.Size() == 0);
}

TEST("trap rules: a friend's stand-in never makes a use of a trap here (the trap's own word says what became of it)")
{
	using RefRules::JudgeUse;
	using RefRules::UseVerdict;
	CHECK(JudgeUse({ .loaded = true, .mechanism = true, .standIn = true, .standInDistance = 150.0f, .trap = true }) == UseVerdict::kTrap);
	CHECK(JudgeUse({ .loaded = true, .mechanism = false, .standIn = false, .trap = true }) == UseVerdict::kTrap);
	CHECK(JudgeUse({ .loaded = false, .mechanism = true, .standIn = true, .standInDistance = 150.0f, .trap = true }) == UseVerdict::kNotLoaded);
	CHECK(JudgeUse({ .loaded = true, .mechanism = true, .standIn = true, .standInDistance = 150.0f }) == UseVerdict::kUse);
	CHECK(std::string_view{ RefRules::UseVerdictName(UseVerdict::kTrap) } == "trap");
}
