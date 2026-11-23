#include "Test.h"

#include "FormTranslate.h"
#include "RefBook.h"
#include "ServerFixture.h"
#include "game/RefRules.h"

#include <string>
#include <vector>

// Doors, locks and mechanisms (docs/DESIGN.md, "Doors, locks and mechanisms"): the messages (common/Protocol.h), their
// form IDs on the wire (common/FormTranslate.h), the server's book (common/RefBook.h) and passing on (common/Server.cpp),
// and each game's rules (src/game/RefRules.h). What reads and changes the game itself (src/game/WorldObjects.cpp) is
// tested in a game only: tools/rig/steps/doors.sh.

using Protocol::Activation;
using Protocol::RefState;

namespace
{
	using MT = Protocol::MessageType;
	using Clock = std::chrono::steady_clock;

	RefState Door(std::uint32_t a_ref, std::uint8_t a_open, std::uint8_t a_locked = RefState::kUnknown, std::uint8_t a_level = RefState::LEVEL_UNKNOWN,
		std::uint32_t a_player = 0)
	{
		return RefState{ .refId = a_ref, .open = a_open, .locked = a_locked, .lockLevel = a_level, .playerId = a_player };
	}

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	std::vector<RefState> RefStatesIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<RefState> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == MT::kRemoteRefState) {
				if (const auto state = Protocol::DecodeRefState(packet)) {
					out.push_back(*state);
				}
			}
		}
		return out;
	}

	std::size_t CountOf(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_packets, [&](const auto& a_packet) { return Protocol::PeekType(a_packet) == a_type; }));
	}
}

// ---- the messages -------------------------------------------------------------------------------------------------

TEST("refs: a door's state goes to the server and back to every game, its reporter named on the way back")
{
	const auto said = Door(0x0001A2B3, RefState::kYes, RefState::kNo, 2);
	const auto packet = Protocol::Encode(said);
	CHECK(Protocol::PeekType(packet) == MT::kRefState);
	CHECK(packet.size() == 1 + 4 + 3);
	const auto back = Protocol::DecodeRefState(packet);
	REQUIRE(back);
	CHECK(*back == said);

	auto remote = said;
	remote.playerId = 3;
	const auto passed = Protocol::Encode(remote, MT::kRemoteRefState);
	CHECK(Protocol::PeekType(passed) == MT::kRemoteRefState);
	const auto heard = Protocol::DecodeRefState(passed);
	REQUIRE(heard);
	CHECK(*heard == remote);
	CHECK(heard->playerId == 3);
}

TEST("refs: a state with a part out of range, cut short or too long is not read")
{
	auto packet = Protocol::Encode(Door(0x10, 3));
	CHECK(!Protocol::DecodeRefState(packet));  // open past kUnknown
	packet = Protocol::Encode(Door(0x10, RefState::kYes, 7));
	CHECK(!Protocol::DecodeRefState(packet));  // locked past kUnknown
	packet = Protocol::Encode(Door(0x10, RefState::kYes, RefState::kYes, 6));
	CHECK(!Protocol::DecodeRefState(packet));  // a level past the game's
	packet = Protocol::Encode(Door(0x10, RefState::kYes, RefState::kYes, RefState::LEVEL_MAX));
	CHECK(Protocol::DecodeRefState(packet));
	packet.pop_back();
	CHECK(!Protocol::DecodeRefState(packet));
	packet = Protocol::Encode(Door(0x10, RefState::kYes));
	packet.push_back(0);
	CHECK(!Protocol::DecodeRefState(packet));
	// Another message's bytes are no state.
	CHECK(!Protocol::DecodeRefState(Protocol::Encode(Activation{ .refId = 0x10 })));
}

TEST("refs: a mechanism's use goes as its reference, and comes back naming who used it")
{
	const auto packet = Protocol::Encode(Activation{ .refId = 0x00051234 });
	CHECK(Protocol::PeekType(packet) == MT::kActivate);
	const auto back = Protocol::DecodeActivation(packet);
	REQUIRE(back);
	CHECK(back->refId == 0x00051234);
	CHECK(back->playerId == 0);

	const auto passed = Protocol::Encode(Activation{ .refId = 0x00051234, .playerId = 2 }, MT::kRemoteActivate);
	const auto heard = Protocol::DecodeActivation(passed);
	REQUIRE(heard);
	CHECK(heard->playerId == 2);
	auto cut = passed;
	cut.pop_back();
	CHECK(!Protocol::DecodeActivation(cut));
	CHECK(!Protocol::DecodeActivation(Protocol::Encode(Door(1, RefState::kYes))));
}

TEST("refs: a report laid over what is known changes only the parts it says")
{
	const auto known = Door(0x20, RefState::kYes, RefState::kYes, 3, 1);
	const auto merged = Protocol::MergeRefState(known, Door(0x20, RefState::kUnknown, RefState::kNo, RefState::LEVEL_UNKNOWN, 2));
	CHECK(merged.open == RefState::kYes);
	CHECK(merged.locked == RefState::kNo);
	CHECK(merged.lockLevel == 3);
	CHECK(merged.playerId == 2);
	CHECK(!Door(0x20, RefState::kUnknown).Says());
	CHECK(Door(0x20, RefState::kUnknown, RefState::kUnknown, 1).Says());
}

// ---- form IDs on the wire -----------------------------------------------------------------------------------------

TEST("refs: a door is named on the wire by its plugin, and one of a plugin the other game lacks reaches it as none")
{
	using Names = std::vector<std::string>;
	const Names host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "A.esp" }, {});
	const Names guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "B.esp", "A.esp" }, {});
	Names       session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const FormIds::Mapper hostEdge(host, session);
	const FormIds::Mapper guestEdge(guest, session);
	CHECK(FormTranslate::CarriesForms(MT::kRefState));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteRefState));
	CHECK(FormTranslate::CarriesForms(MT::kActivate));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteActivate));
	CHECK(!FormTranslate::CarriesForms(MT::kAskRefStates));

	const auto through = [](const FormIds::Mapper& a_edge, FormTranslate::Way a_way, const std::vector<std::uint8_t>& a_packet) {
		auto result = FormTranslate::Translate(a_edge, a_way, a_packet);
		return result.kind == FormTranslate::Result::Kind::kChanged ? result.packet : a_packet;
	};
	// A.esp's door: 01 in the host's game and the session's, 02 in the guest's.
	const auto out = through(guestEdge, FormTranslate::Way::kToSession, Protocol::Encode(Door(0x02000ABC, RefState::kYes, RefState::kNo, 1)));
	const auto onWire = Protocol::DecodeRefState(out);
	REQUIRE(onWire);
	CHECK(onWire->refId == 0x01000ABC);
	CHECK(onWire->open == RefState::kYes);
	CHECK(onWire->lockLevel == 1);
	auto passed = *onWire;
	passed.playerId = 2;
	const auto atHost = Protocol::DecodeRefState(through(hostEdge, FormTranslate::Way::kToLocal, Protocol::Encode(passed, MT::kRemoteRefState)));
	REQUIRE(atHost);
	CHECK(atHost->refId == 0x01000ABC);
	CHECK(atHost->playerId == 2);
	// B.esp's lever (session 02) is none in the host's game; the use still reads, as reference 0, which nothing applies.
	const auto use = Protocol::DecodeActivation(through(hostEdge, FormTranslate::Way::kToLocal, Protocol::Encode(Activation{ 0x02000DEF, 2 }, MT::kRemoteActivate)));
	REQUIRE(use);
	CHECK(use->refId == 0);
	CHECK(use->playerId == 2);
	// A runtime reference (FF) is no other game's: it leaves as none.
	const auto runtime = Protocol::DecodeRefState(through(hostEdge, FormTranslate::Way::kToSession, Protocol::Encode(Door(0xFF000812, RefState::kYes))));
	REQUIRE(runtime);
	CHECK(runtime->refId == 0);
	CHECK(!RefRules::Shareable(0xFF000812));
	CHECK(!RefRules::Shareable(0));
	CHECK(RefRules::Shareable(0x0001A2B3));
}

// ---- the server's book --------------------------------------------------------------------------------------------

TEST("refs: the server's book keeps the last word on each reference, and passes on only what changes it")
{
	RefBook  book;
	RefState out;
	CHECK(book.Report(Door(0x30, RefState::kYes, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 1), out) == RefBook::Take::kNews);
	CHECK(out.open == RefState::kYes);
	CHECK(book.Report(Door(0x30, RefState::kYes, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 2), out) == RefBook::Take::kSame);
	CHECK(out.playerId == 2);
	CHECK(book.Report(Door(0x30, RefState::kUnknown, RefState::kYes, 4, 2), out) == RefBook::Take::kNews);
	CHECK(out.open == RefState::kYes);  // what the lock report left unsaid stays
	CHECK(out.locked == RefState::kYes);
	CHECK(out.lockLevel == 4);
	CHECK(book.Report(Door(0x31, RefState::kNo), out) == RefBook::Take::kNews);
	CHECK(book.Report(Door(0, RefState::kYes), out) == RefBook::Take::kRefused);
	CHECK(book.Report(Door(0xFF000001, RefState::kYes), out) == RefBook::Take::kRefused);
	CHECK(book.Report(Door(0x32, RefState::kUnknown), out) == RefBook::Take::kRefused);
	const auto all = book.All();
	REQUIRE(all.size() == 2);
	CHECK(all[0].refId == 0x30);  // in the order first reported
	CHECK(all[0].playerId == 0);  // the session's word, sent again
	CHECK(all[0].locked == RefState::kYes);
	CHECK(all[1].refId == 0x31);
	book.Clear();
	CHECK(book.Size() == 0);
}

TEST("refs: a full book keeps what it has and refuses new references")
{
	RefBook  book(2);
	RefState out;
	CHECK(book.Report(Door(1, RefState::kYes), out) == RefBook::Take::kNews);
	CHECK(book.Report(Door(2, RefState::kYes), out) == RefBook::Take::kNews);
	CHECK(book.Report(Door(3, RefState::kYes), out) == RefBook::Take::kFull);
	CHECK(book.Report(Door(1, RefState::kNo), out) == RefBook::Take::kNews);  // a known one still changes
	CHECK(book.Size() == 2);
}

// ---- each game's rules --------------------------------------------------------------------------------------------

TEST("refs: what is shared of each kind, by what the engine keeps and never a script's name")
{
	using RefRules::Base;
	using RefRules::Facts;
	using RefRules::Of;
	using RefRules::Shares;
	CHECK(Of(Facts{ .base = Base::kDoor }) == Shares{ .open = true, .lock = true });
	CHECK(Of(Facts{ .base = Base::kDoor, .loadDoor = true }) == Shares{ .lock = true });   // a house door: its lock only
	CHECK(Of(Facts{ .base = Base::kDoor, .automatic = true }) == Shares{ .lock = true });  // each game opens it for whoever comes near
	CHECK(Of(Facts{ .base = Base::kContainer }) == Shares{ .lock = true });
	CHECK(Of(Facts{ .base = Base::kActivator, .named = true }) == Shares{ .use = true });  // a lever, a chain, a pillar
	CHECK(!Of(Facts{ .base = Base::kActivator }).Any());                                   // a trigger, an unnamed trap part
	CHECK(!Of(Facts{ .base = Base::kDoor, .marker = true }).Any());
	CHECK(!Of(Facts{}).Any());
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kDoor }) } == "door");
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kDoor, .loadDoor = true }) } == "loaddoor");
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kDoor, .automatic = true }) } == "autodoor");
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kActivator, .named = true }) } == "mechanism");
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kContainer }) } == "container");
}

TEST("refs: the game's open states and lock levels, as the wire has them")
{
	CHECK(RefRules::OpenOf(1) == RefState::kYes);
	CHECK(RefRules::OpenOf(2) == RefState::kYes);  // opening: where it is going
	CHECK(RefRules::OpenOf(3) == RefState::kNo);
	CHECK(RefRules::OpenOf(4) == RefState::kNo);
	CHECK(RefRules::OpenOf(0) == RefState::kUnknown);
	CHECK(RefRules::OpenOf(9) == RefState::kUnknown);
	CHECK(RefRules::LevelOf(-1) == RefState::LEVEL_UNKNOWN);
	CHECK(RefRules::LevelOf(0) == 0);
	CHECK(RefRules::LevelOf(5) == 5);
	CHECK(RefRules::LevelOf(6) == RefState::LEVEL_UNKNOWN);
	// a lock's own number (the record's 1/25/50/75/100/255) is not the game's level enum
	CHECK(RefRules::LevelOfBase(1) == 0);
	CHECK(RefRules::LevelOfBase(0) == 0);
	CHECK(RefRules::LevelOfBase(25) == 1);
	CHECK(RefRules::LevelOfBase(49) == 1);
	CHECK(RefRules::LevelOfBase(50) == 2);
	CHECK(RefRules::LevelOfBase(75) == 3);
	CHECK(RefRules::LevelOfBase(100) == 4);
	CHECK(RefRules::LevelOfBase(127) == 4);
	CHECK(RefRules::LevelOfBase(-1) == 5);  // 255, needs a key, in a signed byte
	CHECK(RefRules::LevelOfBase(-5) == RefState::LEVEL_UNKNOWN);
	for (std::uint8_t level = 0; level <= RefState::LEVEL_MAX; ++level) {
		CHECK(RefRules::LevelOfBase(RefRules::BaseOfLevel(level)) == level);
	}
	CHECK(RefRules::BaseOfLevel(2) == 50);
	CHECK(RefRules::BaseOfLevel(5) == -1);
	const auto shared = RefRules::Shared(Door(1, RefState::kYes, RefState::kYes, 2), RefRules::Shares{ .lock = true });
	CHECK(shared.open == RefState::kUnknown);
	CHECK(shared.locked == RefState::kYes);
	const auto none = RefRules::Shared(Door(1, RefState::kYes, RefState::kYes, 2), RefRules::Shares{ .use = true });
	CHECK(!none.Says());
}

TEST("refs: a change is reported when our player made it or is near it, and a friend's use is made only by their stand-in near it")
{
	CHECK(RefRules::Reports(true, -1.0f));
	CHECK(RefRules::Reports(false, 500.0f));
	CHECK(RefRules::Reports(false, RefRules::REPORT_REACH));
	CHECK(!RefRules::Reports(false, RefRules::REPORT_REACH + 1.0f));
	CHECK(!RefRules::Reports(false, -1.0f));  // another cell or worldspace

	using RefRules::JudgeUse;
	using RefRules::UseVerdict;
	CHECK(JudgeUse({ .loaded = true, .mechanism = true, .standIn = true, .standInDistance = 150.0f }) == UseVerdict::kUse);
	CHECK(JudgeUse({ .loaded = false, .mechanism = true, .standIn = true, .standInDistance = 150.0f }) == UseVerdict::kNotLoaded);
	CHECK(JudgeUse({ .loaded = true, .mechanism = false, .standIn = true, .standInDistance = 150.0f }) == UseVerdict::kNotMechanism);
	CHECK(JudgeUse({ .loaded = true, .mechanism = true, .standIn = false }) == UseVerdict::kNoStandIn);
	CHECK(JudgeUse({ .loaded = true, .mechanism = true, .standIn = true, .standInDistance = RefRules::USE_REACH + 1.0f }) == UseVerdict::kTooFar);
	CHECK(JudgeUse({ .loaded = true, .mechanism = true, .standIn = true, .standInDistance = -1.0f }) == UseVerdict::kTooFar);
	CHECK(std::string_view{ RefRules::UseVerdictName(UseVerdict::kNoStandIn) } == "no stand-in");
}

TEST("refs: the session's word changes only what it says and the reference reads otherwise")
{
	auto change = RefRules::Plan(Door(1, RefState::kYes, RefState::kNo), Door(1, RefState::kNo, RefState::kYes, 2));
	CHECK(change.open == std::optional{ true });
	CHECK(change.locked == std::optional{ false });
	CHECK(!change.level);
	change = RefRules::Plan(Door(1, RefState::kYes), Door(1, RefState::kYes, RefState::kNo));
	CHECK(!change.Any());
	// A part the reference has none of here (an open state on a load door) is left alone.
	change = RefRules::Plan(Door(1, RefState::kYes, RefState::kYes), Door(1, RefState::kUnknown, RefState::kNo));
	CHECK(!change.open);
	CHECK(change.locked == std::optional{ true });
	change = RefRules::Plan(Door(1, RefState::kUnknown, RefState::kYes, 5), Door(1, RefState::kUnknown, RefState::kYes, 2));
	CHECK(change.level == std::optional<std::uint8_t>{ 5 });
}

TEST("refs: our own word on a door is not sent back, and a friend's applied here reads as known")
{
	RefRules::Book book;
	CHECK(book.News(Door(7, RefState::kYes)));  // the session has said nothing of it
	book.Sent(Door(7, RefState::kYes));
	CHECK(!book.News(Door(7, RefState::kYes)));
	CHECK(book.News(Door(7, RefState::kNo)));
	CHECK(book.Heard(Door(7, RefState::kYes, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 1), 1));  // our echo, nothing else out
	CHECK(book.Outstanding() == 0);
	// A friend's word: applied, and the open state it leaves here (an event of our own) is no news.
	CHECK(book.Heard(Door(7, RefState::kNo, RefState::kYes, 1, 2), 1));
	CHECK(!book.News(Door(7, RefState::kNo, RefState::kYes, 1)));
	CHECK(!book.News(Door(7, RefState::kUnknown)));  // says nothing
	book.Clear();
	CHECK(book.Size() == 0);
}

TEST("refs: two players at one door at once end on the same state, the one the server heard last")
{
	// A opens while B closes; the server hears A's first, passes both on to both, in that order.
	RefRules::Book a;
	RefRules::Book b;
	auto           doorA = RefState::kYes;
	auto           doorB = RefState::kNo;
	a.Sent(Door(9, doorA));
	b.Sent(Door(9, doorB));
	const auto first = Door(9, RefState::kYes, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 1);
	const auto second = Door(9, RefState::kNo, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 2);
	const auto hear = [](RefRules::Book& a_book, std::uint8_t& a_door, const RefState& a_word, std::uint32_t a_us) {
		if (a_book.Heard(a_word, a_us)) {
			if (const auto change = RefRules::Plan(*a_book.Known(9), Door(9, a_door)); change.open) {
				a_door = *change.open ? RefState::kYes : RefState::kNo;
			}
		}
	};
	hear(a, doorA, first, 1);
	hear(a, doorA, second, 1);
	hear(b, doorB, first, 2);
	hear(b, doorB, second, 2);
	CHECK(doorA == RefState::kNo);
	CHECK(doorB == RefState::kNo);
}

TEST("refs: our own word that comes back after a later report of ours is old, and not applied")
{
	RefRules::Book book;
	book.Sent(Door(5, RefState::kYes));
	book.Sent(Door(5, RefState::kNo));  // opened and shut again before the server answered
	CHECK(!book.Heard(Door(5, RefState::kYes, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 1), 1));
	CHECK(book.Heard(Door(5, RefState::kNo, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 1), 1));
	CHECK(book.Known(5)->open == RefState::kNo);
	// The session's word sent again (a load, a join) is always applied where it reads otherwise.
	CHECK(book.Heard(Door(5, RefState::kYes), 1));
}

TEST("refs: a door flipped over and over is reported eight times at once, then once every 2 s, its newest state")
{
	RefRules::Outbox outbox;
	const auto       t0 = Clock::now();
	int              now = 0;
	for (int i = 0; i < 8; ++i) {
		now += outbox.Offer(Door(4, i % 2 ? RefState::kNo : RefState::kYes), t0) ? 1 : 0;
	}
	CHECK(now == 8);
	CHECK(!outbox.Offer(Door(4, RefState::kYes), t0));
	CHECK(!outbox.Offer(Door(4, RefState::kNo), t0 + std::chrono::milliseconds(500)));
	CHECK(outbox.Held() == 1);
	CHECK(outbox.Due(t0 + std::chrono::milliseconds(1000)).empty());
	const auto due = outbox.Due(t0 + RefRules::EVERY);
	REQUIRE(due.size() == 1);
	CHECK(due[0].open == RefState::kNo);  // the newest
	CHECK(outbox.Held() == 0);
	// Another door has its own pace.
	CHECK(outbox.Offer(Door(6, RefState::kYes), t0));
}

TEST("refs: the session's words wait for their reference to load, the newest of each kept, and a full list drops new ones")
{
	RefRules::Pending pending;
	CHECK(pending.Add(Door(1, RefState::kYes)));
	CHECK(pending.Add(Door(1, RefState::kUnknown, RefState::kYes)));
	REQUIRE(pending.Size() == 1);
	CHECK(pending.All()[0].open == RefState::kYes);
	CHECK(pending.All()[0].locked == RefState::kYes);
	pending.Remove(1);
	CHECK(pending.Size() == 0);
	for (std::uint32_t i = 1; i <= RefRules::MAX_PENDING; ++i) {
		CHECK(pending.Add(Door(i, RefState::kYes)));
	}
	CHECK(!pending.Add(Door(0x9999, RefState::kYes)));
	CHECK(pending.Dropped() == 1);
	CHECK(pending.Add(Door(2, RefState::kNo)));  // one waiting already still takes a newer word
}

// ---- the server ---------------------------------------------------------------------------------------------------

TEST("server: a door's state goes to every game, its reporter's too, and a joiner hears every door kept")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	f.fake->Deliver(2, Protocol::Encode(Door(0x0001A2B3, RefState::kYes, RefState::kNo, 1)));
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 2 } }) {
		const auto packet = f.fake->WaitFor(peer, Type(MT::kRemoteRefState));
		REQUIRE(packet);
		const auto state = Protocol::DecodeRefState(*packet);
		REQUIRE(state);
		CHECK(state->refId == 0x0001A2B3);
		CHECK(state->open == RefState::kYes);
		CHECK(state->playerId == 2);
	}
	f.fake->Deliver(1, Protocol::Encode(Door(0x0001A2B3, RefState::kUnknown, RefState::kYes, 3)));
	REQUIRE(f.fake->WaitFor(2, Type(MT::kRemoteRefState)));

	// A third player comes: after the welcome, the session's word on each door, whole, from nobody.
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	const auto packets = f.fake->TakeThrough(3, Type(MT::kRemoteRefState));
	const auto states = RefStatesIn(packets);
	REQUIRE(states.size() == 1);
	CHECK(states[0].refId == 0x0001A2B3);
	CHECK(states[0].open == RefState::kYes);
	CHECK(states[0].locked == RefState::kYes);
	CHECK(states[0].lockLevel == 3);
	CHECK(states[0].playerId == 0);
	CHECK(CountOf(packets, MT::kPlayerJoined) == 2);  // after the welcome and who is there
	CHECK(Protocol::PeekType(packets.front()) == MT::kPlayerJoined);
}

TEST("server: a report that changes nothing goes back to its reporter alone")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	f.fake->Deliver(1, Protocol::Encode(Door(0x40, RefState::kYes)));
	f.fake->Deliver(2, Protocol::Encode(Door(0x40, RefState::kYes)));  // the same: B's door opened as A's word came
	f.fake->Deliver(1, Protocol::Encode(Door(0x41, RefState::kNo)));   // news again, after it
	const auto toA = RefStatesIn(f.fake->TakeThrough(1, Type(MT::kRemoteRefState)));
	const auto atA = RefStatesIn(f.fake->TakeThrough(1, Type(MT::kRemoteRefState)));
	CHECK(toA.size() == 1);
	CHECK(atA.size() == 1);
	CHECK(atA[0].refId == 0x41);  // A never heard B's same word
	auto toB = RefStatesIn(f.fake->TakeThrough(2, Type(MT::kRemoteRefState)));
	REQUIRE(toB.size() == 1);
	CHECK(toB[0].refId == 0x40);
	toB = RefStatesIn(f.fake->TakeThrough(2, Type(MT::kRemoteRefState)));
	REQUIRE(toB.size() == 1);
	CHECK(toB[0].refId == 0x40);  // B's own, answered
	CHECK(toB[0].playerId == 2);
	toB = RefStatesIn(f.fake->TakeThrough(2, Type(MT::kRemoteRefState)));
	REQUIRE(toB.size() == 1);
	CHECK(toB[0].refId == 0x41);
}

TEST("server: a mechanism's use goes to the others, not back to its user, and is not kept for a joiner")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	f.fake->Deliver(1, Protocol::Encode(Activation{ .refId = 0x00051234 }));
	const auto packet = f.fake->WaitFor(2, Type(MT::kRemoteActivate));
	REQUIRE(packet);
	const auto use = Protocol::DecodeActivation(*packet);
	REQUIRE(use);
	CHECK(use->refId == 0x00051234);
	CHECK(use->playerId == 1);
	f.fake->Deliver(1, Protocol::Encode(Activation{ .refId = 0 }));           // no reference
	f.fake->Deliver(1, Protocol::Encode(Activation{ .refId = 0xFF000044 }));  // a runtime one
	f.fake->Deliver(1, Protocol::Encode(Door(0x50, RefState::kYes)));         // then a door, to all
	CHECK(CountOf(f.fake->TakeThrough(1, Type(MT::kRemoteRefState)), MT::kRemoteActivate) == 0);
	CHECK(CountOf(f.fake->TakeThrough(2, Type(MT::kRemoteRefState)), MT::kRemoteActivate) == 0);
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	CHECK(CountOf(f.fake->TakeThrough(3, Type(MT::kRemoteRefState)), MT::kRemoteActivate) == 0);
}

TEST("server: a game that loaded a save asks for every door again and hears them, the others nothing")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	f.fake->Deliver(1, Protocol::Encode(Door(0x60, RefState::kYes)));
	f.fake->Deliver(1, Protocol::Encode(Door(0x61, RefState::kNo, RefState::kYes, 5)));
	REQUIRE(RefStatesIn(f.fake->TakeThrough(2, Type(MT::kRemoteRefState))).size() == 1);
	REQUIRE(RefStatesIn(f.fake->TakeThrough(2, Type(MT::kRemoteRefState))).size() == 1);
	f.fake->Deliver(2, Protocol::Writer{ MT::kAskRefStates }.Data());
	std::vector<RefState> again;
	for (int i = 0; i < 2; ++i) {
		const auto packet = f.fake->WaitFor(2, Type(MT::kRemoteRefState));
		REQUIRE(packet);
		again.push_back(*Protocol::DecodeRefState(*packet));
	}
	CHECK(again[0].refId == 0x60);
	CHECK(again[1].refId == 0x61);
	CHECK(again[1].lockLevel == 5);
	CHECK(again[0].playerId == 0);
	REQUIRE(f.fake->Settle());
	CHECK(RefStatesIn(f.fake->Take(1)).size() == 2);  // A heard its own two reports, and nothing of B's ask
}

TEST("server: a new session forgets the last one's doors")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	f.fake->Deliver(1, Protocol::Encode(Door(0x70, RefState::kYes)));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteRefState)));
	f.fake->Drop(1);
	REQUIRE(f.fake->Settle());
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));  // the first player in again: a new session
	f.fake->Deliver(2, Protocol::Encode(Door(0x71, RefState::kYes)));
	const auto states = RefStatesIn(f.fake->TakeThrough(2, Type(MT::kRemoteRefState)));
	REQUIRE(states.size() == 1);
	CHECK(states[0].refId == 0x71);
}
