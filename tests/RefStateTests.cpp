#include "Test.h"

#include "FormTranslate.h"
#include "RefBook.h"
#include "ServerFixture.h"
#include "game/RefRules.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Doors, locks and mechanisms (docs/DESIGN.md, "Doors, locks and mechanisms"): the messages (common/Protocol.h), their
// form IDs on the wire (common/FormTranslate.h), the server's book (common/RefBook.h) and passing on (common/Server.cpp),
// and each game's rules (src/game/RefRules.h). What reads and changes the game itself (src/game/WorldObjects.cpp) is
// tested in a game only: tools/rig/steps/doors.sh.

using Protocol::Activation;
using Protocol::RefRefusal;
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
	// (each comparison in parentheses: a braced list's comma would split the CHECK macro's argument)
	CHECK((Of(Facts{ .base = Base::kDoor }) == Shares{ .open = true, .lock = true }));
	CHECK((Of(Facts{ .base = Base::kDoor, .loadDoor = true }) == Shares{ .lock = true }));   // a house door: its lock only
	CHECK((Of(Facts{ .base = Base::kDoor, .automatic = true }) == Shares{ .lock = true }));  // each game opens it for whoever comes near
	CHECK((Of(Facts{ .base = Base::kContainer }) == Shares{ .lock = true }));
	// An activator: its open state where it has one (a portcullis, a gate, a drawbridge built as an activator, named or
	// not), and its use where a player can use it (a lever, a chain, a pillar).
	CHECK((Of(Facts{ .base = Base::kActivator, .named = true }) == Shares{ .open = true, .use = true }));
	CHECK((Of(Facts{ .base = Base::kActivator }) == Shares{ .open = true }));
	// ... and one with no open state (a trigger, a pillar) says none: what it reads is cut to nothing.
	CHECK(!RefRules::Shared(Door(1, RefState::kUnknown, RefState::kUnknown), Of(Facts{ .base = Base::kActivator })).Says());
	CHECK(!Of(Facts{ .base = Base::kDoor, .marker = true }).Any());
	CHECK(!Of(Facts{}).Any());
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kDoor }) } == "door");
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kDoor, .loadDoor = true }) } == "loaddoor");
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kDoor, .automatic = true }) } == "autodoor");
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kActivator, .named = true }) } == "mechanism");
	CHECK(std::string_view{ RefRules::KindName(Facts{ .base = Base::kActivator }) } == "activator");
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

TEST("refs: a lock the session's word locks is at the word's level, also where this game reads no level")
{
	// No lock here (it reads unlocked, no level): a lock is added, at the word's level, not the game's very easy.
	auto change = RefRules::Plan(Door(1, RefState::kUnknown, RefState::kYes, 2), Door(1, RefState::kUnknown, RefState::kNo));
	CHECK(change.locked == std::optional{ true });
	CHECK(change.level == std::optional<std::uint8_t>{ 2 });
	// A leveled lock, unlocked here (its level depends on our player: none read): locked at the word's level.
	change = RefRules::Plan(Door(1, RefState::kUnknown, RefState::kYes, 5), Door(1, RefState::kUnknown, RefState::kNo, RefState::LEVEL_UNKNOWN));
	CHECK(change.level == std::optional<std::uint8_t>{ 5 });
	// Locked here already, at a level the game scales (none read): the word's level is written.
	change = RefRules::Plan(Door(1, RefState::kUnknown, RefState::kYes, 3), Door(1, RefState::kUnknown, RefState::kYes, RefState::LEVEL_UNKNOWN));
	CHECK(!change.locked);
	CHECK(change.level == std::optional<std::uint8_t>{ 3 });
	// Unlocked by the word: its own number follows where it reads one, and nothing is added where it reads none.
	change = RefRules::Plan(Door(1, RefState::kUnknown, RefState::kNo, 1), Door(1, RefState::kUnknown, RefState::kYes, 4));
	CHECK(change.locked == std::optional{ false });
	CHECK(change.level == std::optional<std::uint8_t>{ 1 });
	change = RefRules::Plan(Door(1, RefState::kUnknown, RefState::kNo, 1), Door(1, RefState::kUnknown, RefState::kNo, RefState::LEVEL_UNKNOWN));
	CHECK(!change.Any());
	// A reference that shares no lock here (its lock reads none: Shared) is not given one.
	change = RefRules::Plan(Door(1, RefState::kUnknown, RefState::kYes, 2), Door(1, RefState::kNo, RefState::kUnknown, RefState::LEVEL_UNKNOWN));
	CHECK(!change.Any());
	// The same level: nothing.
	change = RefRules::Plan(Door(1, RefState::kUnknown, RefState::kYes, 2), Door(1, RefState::kUnknown, RefState::kYes, 2));
	CHECK(!change.Any());
}

TEST("refs: a change reports only the part it is about, never the rest of what the reporting game reads")
{
	using RefRules::Part;
	using RefRules::Shares;
	using RefRules::Within;
	const Shares door{ .open = true, .lock = true };
	// The friend's door reads open and unlocked (picked long ago in their save); they only opened it: the lock is not said.
	const auto read = Door(7, RefState::kYes, RefState::kNo, 2);
	const auto opened = RefRules::Shared(read, Within(door, Part::kOpen));
	CHECK(opened.open == RefState::kYes);
	CHECK(opened.locked == RefState::kUnknown);
	CHECK(opened.lockLevel == RefState::LEVEL_UNKNOWN);
	// A lock event says the lock and its level, not the open state.
	const auto locked = RefRules::Shared(read, Within(door, Part::kLock));
	CHECK(locked.open == RefState::kUnknown);
	CHECK(locked.locked == RefState::kNo);
	CHECK(locked.lockLevel == 2);
	// A chest opened shares nothing (its open state is not shared; its lock was not the change).
	CHECK(!RefRules::Shared(read, Within(Shares{ .lock = true }, Part::kOpen)).Says());
	CHECK(!Within(door, Part::kOpen).use);
	// A reference the session knows nothing of is news only in the part the change says.
	RefRules::Book book;
	CHECK(book.News(opened));
	book.Sent(opened);
	CHECK(book.Known(7)->Says());
	CHECK(book.Known(7)->locked == RefState::kUnknown);  // the book holds no lock it was never told
	// A held or refused report read again says only what it was about.
	const auto again = RefRules::Only(Door(7, RefState::kNo, RefState::kYes, 4), opened);
	CHECK(again.open == RefState::kNo);
	CHECK(again.locked == RefState::kUnknown);
	CHECK(again.lockLevel == RefState::LEVEL_UNKNOWN);
	const auto againLock = RefRules::Only(Door(7, RefState::kNo, RefState::kYes, 4), locked);
	CHECK(againLock.open == RefState::kUnknown);
	CHECK(againLock.locked == RefState::kYes);
	CHECK(againLock.lockLevel == 4);
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
	const auto hear = [](RefRules::Book& a_book, auto& a_door, const RefState& a_word, std::uint32_t a_us) {
		if (a_book.Heard(a_word, a_us)) {
			if (const auto change = RefRules::Plan(*a_book.Known(9), Door(9, a_door)); change.open) {
				a_door = static_cast<std::remove_reference_t<decltype(a_door)>>(*change.open ? RefState::kYes : RefState::kNo);
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

TEST("refs: a report the server did not take is answered, no longer out, and news again")
{
	RefRules::Book book;
	book.Sent(Door(8, RefState::kYes));
	CHECK(book.Outstanding() == 1);
	CHECK(!book.News(Door(8, RefState::kYes)));
	// Refused (busy), the session having no word on it: not out any more, and the door's state is news again.
	book.Refused(8, RefState{});
	CHECK(book.Outstanding() == 0);
	CHECK(!book.Known(8));
	CHECK(book.News(Door(8, RefState::kYes)));
	// Refused while the session holds another word: that word is what it says, and ours differs from it.
	book.Sent(Door(8, RefState::kYes));
	book.Refused(8, Door(8, RefState::kNo));
	CHECK(book.Known(8)->open == RefState::kNo);
	CHECK(book.News(Door(8, RefState::kYes)));
	// Two out, the first refused: the echo of the second is ours and the newest, and is applied.
	book.Sent(Door(8, RefState::kYes));
	book.Sent(Door(8, RefState::kNo));
	book.Refused(8, RefState{});
	CHECK(book.Heard(Door(8, RefState::kNo, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 1), 1));
	CHECK(book.Outstanding() == 0);
}

TEST("refs: a report dropped at one door does not leave two players on different states")
{
	// The review's case: player 1 and player 2 at one door. The server hears 1's report past 1's budget and answers it
	// busy; then 2's, which it takes and passes on. Each game applies what it hears and reports again what is news.
	RefRules::Book a;
	RefRules::Book b;
	RefRules::Again again;
	auto           doorA = RefState::kYes;  // 1 opened it
	auto           doorB = RefState::kNo;   // 2 closed it
	const auto     t0 = Clock::now();
	a.Sent(Door(9, doorA));
	b.Sent(Door(9, doorB));
	// 1 hears its refusal (the session has no word yet): not out, sent again in a moment.
	a.Refused(9, RefState{});
	again.Add(Door(9, RefState::kYes), t0);
	// 2's report is the session's word, to both.
	const auto word = Door(9, RefState::kNo, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 2);
	REQUIRE(a.Heard(word, 1));
	if (const auto change = RefRules::Plan(*a.Known(9), Door(9, doorA)); change.open) {
		doorA = *change.open ? RefState::kYes : RefState::kNo;
	}
	REQUIRE(b.Heard(word, 2));
	CHECK(b.Outstanding() == 0);
	// 1's report goes again: it reads the session's word now, which is no news.
	const auto due = again.Due(t0 + RefRules::AGAIN_AFTER);
	REQUIRE(due.size() == 1);
	CHECK(!a.News(RefRules::Only(Door(9, doorA), due[0])));
	CHECK(doorA == RefState::kNo);
	CHECK(doorB == RefState::kNo);
	CHECK(a.Outstanding() == 0);  // no echo count left one too high
}

TEST("refs: a refused report goes again after a moment, its newest parts once")
{
	RefRules::Again again;
	const auto      t0 = Clock::now();
	again.Add(Door(3, RefState::kYes), t0);
	again.Add(Door(3, RefState::kUnknown, RefState::kYes, 2), t0 + std::chrono::milliseconds(100));
	CHECK(again.Due(t0 + std::chrono::milliseconds(900)).empty());
	const auto due = again.Due(t0 + std::chrono::milliseconds(100) + RefRules::AGAIN_AFTER);
	REQUIRE(due.size() == 1);
	CHECK(due[0].open == RefState::kYes);
	CHECK(due[0].locked == RefState::kYes);
	CHECK(again.Size() == 0);
}

TEST("refs: a lever whose script flips its gate ends with the gate as the user's game has it, in every game")
{
	// The user's game (A) pulled the lever: its script opened the gate, and A reported the use and then the gate. In
	// game B the use is made by A's stand-in (B's lever script will flip B's gate a moment later), and the gate's word
	// arrives in the same frame. B's script flips what it finds.
	const auto run = [](std::uint8_t a_gateB, bool a_settle) {
		RefRules::Book    book;
		RefRules::Pending pending;
		RefRules::Settle  settle;
		const auto        t0 = Clock::now();
		auto              gate = a_gateB;
		const auto        apply = [&](const RefState& a_want) {
			if (const auto change = RefRules::Plan(a_want, Door(20, gate)); change.open) {
				gate = *change.open ? RefState::kYes : RefState::kNo;
			}
		};
		if (a_settle) {
			settle.Used(t0);  // the stand-in's use is made
		}
		const auto word = Door(20, RefState::kYes, RefState::kUnknown, RefState::LEVEL_UNKNOWN, 1);
		REQUIRE(book.Heard(word, 2));
		if (settle.Active(t0)) {
			pending.Add(*book.Known(20));
		} else {
			apply(*book.Known(20));
		}
		// The lever's script runs (Papyrus, a few frames later): it flips the gate.
		gate = gate == RefState::kYes ? RefState::kNo : RefState::kYes;
		// B's change is the replayed script's: not reported while settling (the book would call it news otherwise).
		const bool reports = !(settle.Active(t0 + std::chrono::milliseconds(200))) && book.News(Door(20, gate));
		// The words that waited are applied once the use has settled.
		CHECK(!settle.Active(t0 + RefRules::SETTLE));
		for (const auto& want : pending.All()) {
			apply(want);
		}
		return std::pair{ gate, reports };
	};
	// Closed in B as in A before: the script opens it, the word finds it open.
	CHECK((run(RefState::kNo, true) == std::pair{ std::uint8_t{ RefState::kYes }, false }));
	// Already open in B (B's save): the script closes it, and the word opens it again after.
	CHECK((run(RefState::kYes, true) == std::pair{ std::uint8_t{ RefState::kYes }, false }));
	// Without settling, the word first and the flip after: the gate ends closed, and B would report it closed to all.
	CHECK((run(RefState::kNo, false) == std::pair{ std::uint8_t{ RefState::kNo }, true }));
}

TEST("refs: an open state that changes with no event is found between two reads, and one first seen is no change")
{
	RefRules::OpenWatch watch;
	watch.BeginScan();
	CHECK(!watch.Seen(1, RefState::kNo));  // first seen: this game's as it is
	CHECK(!watch.Seen(2, RefState::kYes));
	watch.EndScan();
	watch.BeginScan();
	CHECK(watch.Seen(1, RefState::kYes));  // a portcullis a script raised
	CHECK(!watch.Seen(2, RefState::kYes));
	watch.EndScan();
	watch.BeginScan();
	CHECK(!watch.Seen(1, RefState::kYes));
	watch.EndScan();  // 2 not read: out of reach, forgotten
	CHECK(watch.Size() == 1);
	watch.BeginScan();
	CHECK(!watch.Seen(2, RefState::kNo));  // back in reach, closed: first seen again, no change
	CHECK(!watch.Seen(1, RefState::kUnknown));
	watch.EndScan();
	watch.BeginScan();
	CHECK(!watch.Seen(1, RefState::kYes));  // from none to open is no change either
	watch.EndScan();
}

TEST("refs: a refusal goes as its reference, why and the session's word, and a bad one is not read")
{
	const auto session = Door(0x0001A2B3, RefState::kNo, RefState::kYes, 5);
	const auto refusal = RefRefusal::Of(Door(0x0001A2B3, RefState::kYes), RefRefusal::kBusy, &session);
	const auto packet = Protocol::Encode(refusal);
	CHECK(Protocol::PeekType(packet) == MT::kRefRefused);
	CHECK(packet.size() == 1 + 4 + 7);
	const auto back = Protocol::DecodeRefRefusal(packet);
	REQUIRE(back);
	CHECK(*back == refusal);
	CHECK(back->Said().open == RefState::kYes);
	CHECK(back->Said().locked == RefState::kUnknown);
	CHECK(back->Session().locked == RefState::kYes);
	CHECK(back->Session().lockLevel == 5);
	CHECK(back->Session().refId == 0x0001A2B3);
	CHECK(!RefRefusal::Of(Door(1, RefState::kYes), RefRefusal::kFull, nullptr).Session().Says());
	auto bad = packet;
	bad[5] = 3;  // no such reason
	CHECK(!Protocol::DecodeRefRefusal(bad));
	bad = packet;
	bad[6] = 3;  // open past kUnknown
	CHECK(!Protocol::DecodeRefRefusal(bad));
	bad = packet;
	bad.pop_back();
	CHECK(!Protocol::DecodeRefRefusal(bad));
	CHECK(!Protocol::DecodeRefRefusal(Protocol::Encode(Door(1, RefState::kYes))));
	CHECK(FormTranslate::CarriesForms(MT::kRefRefused));
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

TEST("server: every report is answered: past the budget the reporter hears busy, with the session's word, and nothing goes to the others")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	// The session's word on 0x100 first, from player 2.
	f.fake->Deliver(2, Protocol::Encode(Door(0x100, RefState::kYes)));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteRefState)));
	REQUIRE(f.fake->Settle());
	f.fake->Take(1);
	f.fake->Take(2);
	// A script near player 1 changes 40 references in one frame, 0x100 among them (its last).
	constexpr int SENT = 40;
	for (int i = 1; i < SENT; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Door(0x200 + i, RefState::kYes)));
	}
	f.fake->Deliver(1, Protocol::Encode(Door(0x100, RefState::kNo)));
	REQUIRE(f.fake->Settle());
	const auto toA = f.fake->Take(1);
	const auto taken = RefStatesIn(toA);
	std::vector<RefRefusal> refused;
	for (const auto& packet : toA) {
		if (const auto refusal = Protocol::DecodeRefRefusal(packet)) {
			refused.push_back(*refusal);
		}
	}
	CHECK(taken.size() + refused.size() == SENT);  // each one answered, once
	REQUIRE(!refused.empty());                     // 32 at once, then 8 a second: some were past it
	for (const auto& refusal : refused) {
		CHECK(refusal.reason == RefRefusal::kBusy);
		CHECK(refusal.saidOpen != RefState::kUnknown);
	}
	// The last, on 0x100, was past the budget: the answer carries the session's word (player 2's open), not 1's closed.
	CHECK(refused.back().refId == 0x100);
	CHECK(refused.back().Said().open == RefState::kNo);
	CHECK(refused.back().Session().open == RefState::kYes);
	// Player 2 heard only what was taken, and no refusal.
	const auto toB = f.fake->Take(2);
	CHECK(RefStatesIn(toB).size() == taken.size());
	CHECK(CountOf(toB, MT::kRefRefused) == 0);
}

TEST("server: a report the session cannot take is answered so, and a full book's answer says it")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	f.fake->Deliver(1, Protocol::Encode(Door(0x300, RefState::kUnknown)));  // says nothing
	const auto packet = f.fake->WaitFor(1, Type(MT::kRefRefused));
	REQUIRE(packet);
	const auto refusal = Protocol::DecodeRefRefusal(*packet);
	REQUIRE(refusal);
	CHECK(refusal->reason == RefRefusal::kRefused);
	CHECK(refusal->refId == 0x300);
	CHECK(!refusal->Session().Says());
}

TEST("server: an ask for every door past its budget is answered later, not dropped")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	f.fake->Deliver(1, Protocol::Encode(Door(0x400, RefState::kYes)));
	REQUIRE(f.fake->WaitFor(1, Type(MT::kRemoteRefState)));
	// Three loads in a row: the budget holds two asks, then one each 5 s.
	for (int i = 0; i < 3; ++i) {
		f.fake->Deliver(1, Protocol::Writer{ MT::kAskRefStates }.Data());
	}
	REQUIRE(f.fake->Settle());
	CHECK(RefStatesIn(f.fake->Take(1)).size() == 2);
	// The third is owed, and answered from the server's loop once the budget has room (one in 5 s).
	const auto owed = f.fake->WaitFor(1, Type(MT::kRemoteRefState), std::chrono::seconds(8));
	REQUIRE(owed);
	CHECK(Protocol::DecodeRefState(*owed)->refId == 0x400);
}
