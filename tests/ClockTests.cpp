#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"
#include "game/ClockRules.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// The session's time of day and weather (docs/DESIGN.md, "Time and weather"): the message (common/Protocol.h), its forms
// on the wire (common/FormTranslate.h), the server's choice of whose clock is the session's (common/Server.cpp), and each
// game's rules (src/game/ClockRules.h). What reads and sets the game itself (src/game/WorldClock.cpp) is tested in a game
// only: tools/rig/steps/clock.sh.

using Protocol::ClockState;
using namespace ClockRules;

namespace
{
	using MT = Protocol::MessageType;
	using namespace std::chrono_literals;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	bool Near(double a_a, double a_b, double a_within = 1e-4) { return std::fabs(a_a - a_b) <= a_within; }

	ClockState Owner(float a_hour, float a_days, std::uint32_t a_player = 1, float a_scale = 20.0f)
	{
		return ClockState{ .playerId = a_player, .gameHour = a_hour, .daysPassed = a_days, .timeScale = a_scale };
	}

	ClockState Outside(std::uint32_t a_weather, std::uint32_t a_worldspace = 0x3C)
	{
		ClockState clock = Owner(12.0f, 10.0f);
		clock.worldspace = a_worldspace;
		clock.weather = a_weather;
		return clock;
	}

	std::size_t CountOf(const std::vector<std::vector<std::uint8_t>>& a_packets, MT a_type)
	{
		return static_cast<std::size_t>(std::ranges::count_if(a_packets, [&](const auto& a_packet) { return Protocol::PeekType(a_packet) == a_type; }));
	}

	std::vector<ClockState> ClocksIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<ClockState> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == MT::kRemoteClock) {
				if (const auto clock = Protocol::DecodeClockState(packet)) {
					out.push_back(*clock);
				}
			}
		}
		return out;
	}

	// A follower lined up with player 1's clock at a_hour, its own day count a_days (the owner's is 10 days on).
	Follower LinedUp(float a_hour, double a_days)
	{
		Follower follower;
		const auto set = follower.Follow(Owner(a_hour, static_cast<float>(a_days + 10.0)), a_hour, a_days);
		CHECK(!set);  // the same hour: nothing to move
		CHECK(follower.lineUps == 1u);
		return follower;
	}
}

// ---- the message ------------------------------------------------------------------------------------------------

TEST("clock: a game's clock goes as it is said, the owner named only from the server")
{
	const ClockState ours{ .playerId = 7, .gameHour = 13.5f, .daysPassed = 42.5625f, .timeScale = 20.0f, .worldspace = 0x3C, .weather = 0x10A241 };
	const auto       sent = Protocol::Encode(ours);
	REQUIRE(Protocol::PeekType(sent) == MT::kClock);
	const auto read = Protocol::DecodeClockState(sent);
	REQUIRE(read);
	CHECK(read->playerId == 0u);  // a game does not say who it is: the server knows
	CHECK(read->gameHour == 13.5f);
	CHECK(read->daysPassed == 42.5625f);
	CHECK(read->timeScale == 20.0f);
	CHECK(read->worldspace == 0x3Cu);
	CHECK(read->weather == 0x10A241u);

	const auto passed = Protocol::DecodeClockState(Protocol::Encode(ours, MT::kRemoteClock));
	REQUIRE(passed);
	CHECK(*passed == ours);
	CHECK(Protocol::VERSION == 32);
}

TEST("clock: a clock no game could have is no message")
{
	const auto bad = [](ClockState a_clock) { return !Protocol::DecodeClockState(Protocol::Encode(a_clock)); };
	const auto nan = std::numeric_limits<float>::quiet_NaN();
	CHECK(bad(Owner(24.0f, 1.0f)));
	CHECK(bad(Owner(-0.01f, 1.0f)));
	CHECK(bad(Owner(nan, 1.0f)));
	CHECK(bad(Owner(12.0f, -1.0f)));
	CHECK(bad(Owner(12.0f, 2.0e6f)));
	CHECK(bad(Owner(12.0f, std::numeric_limits<float>::infinity())));
	CHECK(bad(Owner(12.0f, 1.0f, 1, -1.0f)));
	CHECK(bad(Owner(12.0f, 1.0f, 1, nan)));
	CHECK(bad(Owner(12.0f, 1.0f, 1, 200000.0f)));
	CHECK(!bad(Owner(0.0f, 0.0f, 1, 0.0f)));  // midnight of the first day, time standing still: a game can have it
	CHECK(!bad(Owner(23.999f, 1.0e6f, 1, 100000.0f)));

	auto longer = Protocol::Encode(Owner(12.0f, 1.0f));
	longer.push_back(0);
	CHECK(!Protocol::DecodeClockState(longer));
	auto shorter = Protocol::Encode(Owner(12.0f, 1.0f));
	shorter.pop_back();
	CHECK(!Protocol::DecodeClockState(shorter));
	CHECK(!Protocol::DecodeClockState(Protocol::Encode(Protocol::QuestReset{}, false)));
}

TEST("clock: its worldspace and weather cross by plugin name; a mod's the other game lacks is none, and a weather goes with its worldspace")
{
	using FormIds::Mapper;
	using FormTranslate::Result;
	using FormTranslate::Way;
	CHECK(FormTranslate::CarriesForms(MT::kClock));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteClock));

	// The host has Weathers.esp before Shared.esp; the guest has Shared.esp only. The session's table is the host's
	// order, so Shared.esp is 02 there and 01 in the guest's game.
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Weathers.esp", "Shared.esp" }, {});
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Shared.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const Mapper guestEdge(guest, session);
	const auto   read = [&](const ClockState& a_clock, Result& a_result) {
		const auto packet = Protocol::Encode(a_clock, MT::kRemoteClock);
		a_result = FormTranslate::Translate(guestEdge, Way::kToLocal, packet);
		return Protocol::DecodeClockState(a_result.kind == Result::Kind::kChanged ? a_result.packet : packet);
	};

	Result result;
	// Tamriel and a base game weather: as they are.
	auto clock = Outside(0x0010A241);
	auto in = read(clock, result);
	REQUIRE(in);
	CHECK(in->worldspace == 0x3Cu);
	CHECK(in->weather == 0x0010A241u);
	CHECK(!result.Skipped());

	// A weather of Shared.esp: the guest's own ID for it.
	clock.weather = 0x02000801;
	in = read(clock, result);
	REQUIRE(in);
	CHECK(in->weather == 0x01000801u);
	CHECK(in->playerId == 1u);
	CHECK(in->gameHour == 12.0f);

	// A weather of Weathers.esp, which the guest lacks: none, so nothing is forced; the clock still comes.
	clock.weather = 0x01000801;
	in = read(clock, result);
	REQUIRE(in);
	CHECK(in->weather == 0u);
	CHECK(in->worldspace == 0x3Cu);
	CHECK(result.fields == 1u);

	// A worldspace of Weathers.esp: no place here, and its weather (a base game one) says nothing of our skies either.
	clock = Outside(0x0010A241, 0x01000D62);
	in = read(clock, result);
	REQUIRE(in);
	CHECK(in->worldspace == 0u);
	CHECK(in->weather == 0u);
	CHECK(result.fields == 2u);

	// A runtime weather (FF, a mod's script made it) is no game's but the sender's, both ways.
	const Mapper same(host, session);
	Result       out;
	clock = Outside(0xFF000812);
	out = FormTranslate::Translate(same, Way::kToSession, Protocol::Encode(clock));
	REQUIRE(out.kind == Result::Kind::kChanged);
	const auto sent = Protocol::DecodeClockState(out.packet);
	REQUIRE(sent);
	CHECK(sent->weather == 0u);
	CHECK(sent->worldspace == 0x3Cu);
	CHECK(out.runtime == 1u);
}

// ---- whose clock is the session's: the server ----------------------------------------------------------------------

TEST("clock server: the host's clock is the session's: passed on to the friends and kept for a joiner; a friend's is not")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };
	f.fake->SetLocal(1);
	const auto host = f.Join(1, Fixture::MakeHello("Host", 0x4057));
	REQUIRE(host);
	REQUIRE(host->hostId == host->playerId);
	const auto friendWelcome = f.Join(2, Fixture::MakeHello("Friend", 0x2222));
	REQUIRE(friendWelcome);

	// The friend's (it waited three hours): read and dropped, said once in the log.
	f.fake->Deliver(2, Protocol::Encode(Owner(17.0f, 3.0f)));
	f.fake->Deliver(2, Protocol::Encode(Owner(17.1f, 3.0f)));
	REQUIRE(f.LoggedSoon(std::format("player {} does not keep the session's clock (player {} does)", friendWelcome->playerId, host->playerId)));
	REQUIRE(f.fake->Settle());
	CHECK(CountOf(f.fake->Take(1), MT::kRemoteClock) == 0);
	CHECK(std::ranges::count_if(f.LogLines(), [](const std::string& a_line) { return a_line.find("does not keep the session's clock") != std::string::npos; }) == 1);

	// The host's: to the friend, with whose it is; not back to the host.
	f.fake->Deliver(1, Protocol::Encode(ClockState{ .gameHour = 14.0f, .daysPassed = 10.5f, .timeScale = 20.0f, .worldspace = 0x3C, .weather = 0x10A241 }));
	const auto heard = f.fake->WaitFor(2, Type(MT::kRemoteClock));
	REQUIRE(heard);
	const auto clock = Protocol::DecodeClockState(*heard);
	REQUIRE(clock);
	CHECK(clock->playerId == host->playerId);
	CHECK(clock->gameHour == 14.0f);
	CHECK(clock->weather == 0x10A241u);
	REQUIRE(f.fake->Settle());
	CHECK(CountOf(f.fake->Take(1), MT::kRemoteClock) == 0);

	// Someone joining later hears it with the session's other words, before the players' joins are done with.
	const auto later = f.Join(3, Fixture::MakeHello("Later", 0x3333));
	REQUIRE(later);
	const auto toLater = ClocksIn(f.fake->TakeThrough(3, Type(MT::kRemoteClock)));
	REQUIRE(toLater.size() == 1);
	CHECK(toLater[0].playerId == host->playerId);
	CHECK(toLater[0].gameHour == 14.0f);

	// A clock no game could have is not passed on; the next good one is.
	f.fake->Deliver(1, Protocol::Encode(Owner(24.5f, 10.5f)));
	f.fake->Deliver(1, Protocol::Encode(Owner(14.5f, 10.52f)));
	const auto next = ClocksIn(f.fake->TakeThrough(2, Type(MT::kRemoteClock)));
	REQUIRE(next.size() == 1);
	CHECK(next[0].gameHour == 14.5f);
}

TEST("clock server: with no host the player there longest keeps the clock, and the next one when they leave")
{
	Fixture f;  // a dedicated server: no game hosts it
	const auto a = f.Join(1, Fixture::MakeHello("A", 0x1111));
	const auto b = f.Join(2, Fixture::MakeHello("B", 0x2222));
	const auto c = f.Join(3, Fixture::MakeHello("C", 0x3333));
	REQUIRE(a);
	REQUIRE(b);
	REQUIRE(c);
	REQUIRE(a->hostId == 0u);

	f.fake->Deliver(2, Protocol::Encode(Owner(9.0f, 2.0f)));  // B's: not the session's
	f.fake->Deliver(1, Protocol::Encode(Owner(20.0f, 5.0f)));
	auto toC = ClocksIn(f.fake->TakeThrough(3, Type(MT::kRemoteClock)));
	REQUIRE(toC.size() == 1);
	CHECK(toC[0].playerId == a->playerId);
	CHECK(toC[0].gameHour == 20.0f);
	REQUIRE(f.fake->Settle());
	CHECK(ClocksIn(f.fake->Take(2)).size() == 1);  // B hears A's
	CHECK(ClocksIn(f.fake->Take(1)).empty());

	// A leaves: B is there longest now. A joiner meanwhile hears no clock of A's (a word of nobody's in the session).
	f.fake->Drop(1);
	REQUIRE(f.LoggedSoon(std::format("player {} left", a->playerId)));
	const auto d = f.Join(4, Fixture::MakeHello("D", 0x4444));
	REQUIRE(d);
	REQUIRE(f.fake->Settle());
	CHECK(ClocksIn(f.fake->Take(4)).empty());
	f.fake->Deliver(2, Protocol::Encode(Owner(9.5f, 2.0f)));
	toC = ClocksIn(f.fake->TakeThrough(3, Type(MT::kRemoteClock)));
	REQUIRE(toC.size() == 1);
	CHECK(toC[0].playerId == b->playerId);
	CHECK(toC[0].gameHour == 9.5f);
	CHECK(ClocksIn(f.fake->TakeThrough(4, Type(MT::kRemoteClock))).size() == 1);

	// ... and C's is still not the session's.
	f.fake->Deliver(3, Protocol::Encode(Owner(1.0f, 1.0f)));
	REQUIRE(f.LoggedSoon(std::format("player {} does not keep the session's clock (player {} does)", c->playerId, b->playerId)));
}

TEST("clock server: past the budget the owner's newest clock is owed and passed on, never lost")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };
	f.fake->SetLocal(1);
	REQUIRE(f.Join(1, Fixture::MakeHello("Host", 0x4057)));
	REQUIRE(f.Join(2, Fixture::MakeHello("Friend", 0x2222)));
	for (int i = 0; i < 10; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Owner(10.0f + 0.5f * static_cast<float>(i), 3.0f)));
	}
	// A burst of 4 goes at once; the newest (14.5) follows within about a second.
	std::vector<ClockState> heard;
	const auto              deadline = std::chrono::steady_clock::now() + 10s;
	while (std::chrono::steady_clock::now() < deadline && (heard.empty() || heard.back().gameHour != 14.5f)) {
		const auto more = ClocksIn(f.fake->Take(2));
		heard.insert(heard.end(), more.begin(), more.end());
		std::this_thread::sleep_for(20ms);
	}
	REQUIRE(!heard.empty());
	CHECK(heard.back().gameHour == 14.5f);
	CHECK(heard.size() <= 6u);
}

// ---- the rules each game follows -------------------------------------------------------------------------------------

TEST("clock rules: the hours line up forward, or back within the day, never back across midnight")
{
	CHECK(Near(HourGap(14.0, 10.0), 4.0));
	CHECK(Near(HourGap(1.0, 23.0), 2.0));
	CHECK(Near(HourGap(23.0, 1.0), -2.0));
	CHECK(Near(HourGap(12.0, 0.0), 12.0));
	CHECK(Near(LineUp(14.0, 10.0), 4.0));
	CHECK(Near(LineUp(10.0, 14.0), -4.0));  // back within the day
	CHECK(Near(LineUp(1.0, 23.0), 2.0));    // forward through midnight
	CHECK(Near(LineUp(23.0, 1.0), 22.0));   // not back across midnight: forward to 23:00 of our next day
	CHECK(Near(LineUp(6.0, 6.0), 0.0));
}

TEST("clock rules: the first report lines the hours up, moving the day count with them")
{
	Follower follower;
	const auto set = follower.Follow(Owner(14.0f, 10.58f), 10.0f, 3.42);
	REQUIRE(set);
	CHECK(Near(set->hour, 14.0));
	CHECK(Near(set->days, 4.0 / 24.0));
	CHECK(follower.lineUps == 1u);
	CHECK(follower.hourSets == 1u);
	CHECK(!follower.Moving());
	REQUIRE(follower.Offset());
	CHECK(Near(*follower.Offset(), 3.42 + 4.0 / 24.0 - 10.58, 1e-3));

	// Within the drift: nothing moves (a report a few seconds old).
	CHECK(!follower.Follow(Owner(14.1f, 10.584f), 14.0f, 3.42 + 4.0 / 24.0));
	CHECK(follower.lineUps == 1u);
}

TEST("clock rules: the owner's sleep is followed by the day count, through midnight one day at a time")
{
	// Lined up at 20:00; the owner sleeps eight hours, to 04:00 of its next day.
	auto       follower = LinedUp(20.0f, 3.8333);
	const auto first = follower.Follow(Owner(4.0f, static_cast<float>(13.8333 + 8.0 / 24.0)), 20.0f, 3.8333);
	REQUIRE(first);
	CHECK(Near(first->hour, BEFORE_MIDNIGHT));
	CHECK(Near(first->days, (BEFORE_MIDNIGHT - 20.0) / 24.0));
	CHECK(follower.Moving());
	CHECK(follower.midnightSteps == 1u);
	CHECK(follower.lineUps == 1u);  // followed, not lined up again

	// The game has not rolled the date over yet (paused in a menu): nothing; a report meanwhile waits for the move.
	CHECK(!follower.Advance(BEFORE_MIDNIGHT));
	CHECK(!follower.Follow(Owner(4.1f, 14.2f), BEFORE_MIDNIGHT, 3.8333 + 3.99 / 24.0));
	// It rolled over (and ran a moment): the rest, to 04:00.
	const auto rest = follower.Advance(0.002f);
	REQUIRE(rest);
	CHECK(Near(rest->hour, 4.002, 1e-3));
	CHECK(Near(rest->days, 4.0 / 24.0, 1e-3));
	CHECK(!follower.Moving());
	CHECK(follower.hourSets == 1u);
	// The whole move: the eight hours, less the hundredth the game passed itself.
	CHECK(Near(first->days + rest->days, 7.99 / 24.0, 1e-3));
}

TEST("clock rules: a wait of two days and six hours crosses two midnights, one at a time")
{
	auto follower = LinedUp(10.0f, 5.0);
	auto set = follower.Follow(Owner(16.0f, static_cast<float>(15.0 + 54.0 / 24.0)), 10.0f, 5.0);
	REQUIRE(set);
	CHECK(Near(set->hour, BEFORE_MIDNIGHT));
	CHECK(!follower.Advance(BEFORE_MIDNIGHT));
	set = follower.Advance(0.0f);
	REQUIRE(set);
	CHECK(Near(set->hour, BEFORE_MIDNIGHT));  // a whole day: to the next midnight
	CHECK(Near(set->days, BEFORE_MIDNIGHT / 24.0));
	set = follower.Advance(0.0f);
	REQUIRE(set);
	CHECK(Near(set->hour, 16.0, 1e-3));
	CHECK(follower.midnightSteps == 2u);
	CHECK(!follower.Moving());
}

TEST("clock rules: a friend ahead of the owner (its own wait or sleep) is brought back within the day")
{
	auto       follower = LinedUp(14.0f, 3.5);
	const auto set = follower.Follow(Owner(14.05f, 13.502f), 17.0f, 3.5 + 3.0 / 24.0);
	REQUIRE(set);
	CHECK(Near(set->hour, 14.05, 0.01));
	CHECK(set->days < 0.0);
	CHECK(Near(set->days, -2.95 / 24.0, 1e-3));
	CHECK(follower.backSets == 1u);
}

TEST("clock rules: a friend ahead across midnight goes forward to the owner's hour on its next day")
{
	// Lined up at 22:00; the friend fast-travels three hours, to 01:00 of its next day; the owner reads 23:00.
	auto       follower = LinedUp(22.0f, 4.9167);
	const auto set = follower.Follow(Owner(23.0f, static_cast<float>(14.9167 + 1.0 / 24.0)), 1.0f, 4.9167 + 3.0 / 24.0);
	REQUIRE(set);
	CHECK(Near(set->hour, 23.0, 1e-3));  // forward 22 hours, as waiting would; never back across midnight
	CHECK(Near(set->days, 22.0 / 24.0, 1e-3));
	CHECK(follower.lineUps == 2u);
	CHECK(follower.backSets == 0u);
}

TEST("clock rules: the owner loading another save lines up again rather than following days of difference")
{
	// Two days back, the same hour: nothing moves, and the owner's next sleep is followed.
	auto follower = LinedUp(12.0f, 6.5);
	CHECK(!follower.Follow(Owner(12.0f, 14.5f), 12.0f, 6.5));
	CHECK(follower.lineUps == 2u);
	const auto sleep = follower.Follow(Owner(20.0f, static_cast<float>(14.5 + 8.0 / 24.0)), 12.0f, 6.5);
	REQUIRE(sleep);
	CHECK(Near(sleep->hour, 20.0, 1e-3));
	CHECK(follower.lineUps == 2u);

	// A hundred hours on (another save, much later): the time of day, not a hundred hours of waiting.
	auto       later = LinedUp(12.0f, 6.5);
	const auto jump = later.Follow(Owner(16.0f, static_cast<float>(16.5 + 100.0 / 24.0)), 12.0f, 6.5);
	REQUIRE(jump);
	CHECK(Near(jump->hour, 16.0, 1e-3));
	CHECK(Near(jump->days, 4.0 / 24.0, 1e-3));
	CHECK(later.lineUps == 2u);
}

TEST("clock rules: a clock set by hand (the day count not moved) lines up by the hours, and a new owner lines up afresh")
{
	// Our hour set to 19:00 by a console, the day count left: the hours count, five back.
	auto       follower = LinedUp(14.0f, 3.5);
	const auto set = follower.Follow(Owner(14.0f, 13.5f), 19.0f, 3.5);
	REQUIRE(set);
	CHECK(Near(set->hour, 14.0, 1e-3));
	CHECK(follower.lineUps == 2u);
	CHECK(follower.backSets == 1u);

	// Another player keeps the clock now (the host left a dedicated server's session): lined up with theirs.
	const auto other = follower.Follow(Owner(16.0f, 99.0f, 2), 14.0f, 3.5);
	REQUIRE(other);
	CHECK(Near(other->hour, 16.0, 1e-3));
	CHECK(follower.Owner() == 2u);
	CHECK(follower.lineUps == 3u);

	// A load here: lined up again, the move under way dropped.
	follower.Rebaseline();
	CHECK(!follower.Offset());
	CHECK(!follower.Follow(Owner(16.0f, 99.0f, 2), 16.0f, 7.0));
	CHECK(follower.lineUps == 4u);
}

TEST("clock rules: a game says its clock every five seconds, and within a second when it jumps or its sky changes")
{
	using Clock = std::chrono::steady_clock;
	const auto t0 = Clock::time_point{} + 1000s;
	Reporter   reporter;
	ClockState ours = Owner(10.0f, 5.0f);
	ours.worldspace = 0x3C;
	ours.weather = 0x10A241;
	CHECK(reporter.Due(t0, ours));
	reporter.Sent(t0, ours);

	// Running on as time does: not before five seconds.
	const auto ran = [&](ClockState a_clock, double a_seconds) {
		const double hours = a_seconds * 20.0 / 3600.0;
		a_clock.gameHour += static_cast<float>(hours);
		a_clock.daysPassed += static_cast<float>(hours / 24.0);
		return a_clock;
	};
	CHECK(!reporter.Due(t0 + 500ms, ran(ours, 0.5)));
	CHECK(!reporter.Due(t0 + 1200ms, ran(ours, 1.2)));
	CHECK(!reporter.Due(t0 + 2500ms, ran(ours, 2.5)));
	CHECK(reporter.Due(t0 + 5s, ran(ours, 5.0)));

	// A sleep: the hour and the day count jumped.
	auto slept = ran(ours, 1.5);
	slept.gameHour += 8.0f;
	slept.daysPassed += 8.0f / 24.0f;
	Reporter r1;
	r1.Sent(t0, ours);
	CHECK(r1.Due(t0 + 1500ms, slept));
	// ... but looked at most once a second: just after a look, not yet.
	Reporter r2;
	r2.Sent(t0, ours);
	CHECK(!r2.Due(t0 + 1100ms, ran(ours, 1.1)));
	CHECK(!r2.Due(t0 + 1500ms, slept));
	CHECK(r2.Due(t0 + 2200ms, slept));

	// A console moved the hour alone; a day passed at the same hour (a wait of 24 hours); the weather turned; the
	// timescale changed; the player stepped inside.
	const auto check = [&](ClockState a_changed) {
		Reporter r;
		r.Sent(t0, ours);
		return r.Due(t0 + 1500ms, a_changed);
	};
	auto hourAlone = ran(ours, 1.5);
	hourAlone.gameHour += 3.0f;
	CHECK(check(hourAlone));
	auto wholeDay = ran(ours, 1.5);
	wholeDay.daysPassed += 1.0f;
	CHECK(check(wholeDay));
	auto turned = ran(ours, 1.5);
	turned.weather = 0x10E1F2;
	CHECK(check(turned));
	auto faster = ran(ours, 1.5);
	faster.timeScale = 40.0f;
	CHECK(check(faster));
	auto inside = ran(ours, 1.5);
	inside.worldspace = 0;
	inside.weather = 0;
	CHECK(check(inside));
	CHECK(!check(ran(ours, 1.5)));
}

TEST("clock rules: weather is forced on arriving under the owner's sky, eased in when it turns, and a script's left alone")
{
	using Clock = std::chrono::steady_clock;
	const auto      t0 = Clock::time_point{} + 1000s;
	WeatherFollower weather;
	const auto      owner = Outside(0xAAAA);
	const Sky       ourSky{ .outside = true, .worldspace = 0x3C, .weather = 0xBBBB };

	// Not under the owner's sky: inside, another worldspace, or the owner inside (no weather said).
	CHECK(weather.Decide(owner, Sky{ .outside = false, .worldspace = 0, .weather = 0xBBBB }, t0) == WeatherMove::kNone);
	CHECK(weather.Decide(owner, Sky{ .outside = true, .worldspace = 0x1234, .weather = 0xBBBB }, t0) == WeatherMove::kNone);
	CHECK(weather.Decide(Outside(0, 0), ourSky, t0) == WeatherMove::kNone);
	CHECK(weather.Decide(Outside(0), ourSky, t0) == WeatherMove::kNone);

	// Arriving (a join, stepping out) under another weather: forced at once; then the same.
	CHECK(weather.Decide(owner, ourSky, t0) == WeatherMove::kForce);
	CHECK(weather.Decide(owner, Sky{ .outside = true, .worldspace = 0x3C, .weather = 0xAAAA, .pinned = true, .overrideWeather = 0xAAAA }, t0 + 1s) == WeatherMove::kNone);

	// The owner's weather turns: eased in; not asked again while it eases; forced after WEATHER_PATIENCE.
	const auto turned = Outside(0xCCCC);
	const Sky  stillOld{ .outside = true, .worldspace = 0x3C, .weather = 0xAAAA };
	CHECK(weather.Decide(turned, stillOld, t0 + 10s) == WeatherMove::kEase);
	CHECK(weather.Decide(turned, stillOld, t0 + 20s) == WeatherMove::kNone);
	CHECK(weather.Decide(turned, stillOld, t0 + 10s + WEATHER_PATIENCE) == WeatherMove::kForce);
	CHECK(weather.Decide(turned, Sky{ .outside = true, .worldspace = 0x3C, .weather = 0xCCCC, .pinned = true, .overrideWeather = 0xCCCC }, t0 + 41s) == WeatherMove::kNone);

	// Our own game rolls another weather: eased back, not snapped.
	CHECK(weather.Decide(turned, Sky{ .outside = true, .worldspace = 0x3C, .weather = 0xDDDD }, t0 + 60s) == WeatherMove::kEase);

	// A script of ours forced a weather (a quest's storm): left alone, and counted.
	CHECK(weather.Decide(turned, Sky{ .outside = true, .worldspace = 0x3C, .weather = 0xEEEE, .overridden = true }, t0 + 70s) == WeatherMove::kHeld);
	CHECK(weather.held == 1u);

	// Inside and out again under another weather: an arrival, forced.
	CHECK(weather.Decide(turned, Sky{}, t0 + 80s) == WeatherMove::kNone);
	CHECK(weather.Decide(turned, ourSky, t0 + 90s) == WeatherMove::kForce);
	CHECK(weather.forced == 3u);
	CHECK(weather.eased == 2u);

	// After a load (Reset) the first difference is an arrival again.
	weather.Reset();
	CHECK(weather.Decide(owner, ourSky, t0 + 100s) == WeatherMove::kForce);
}

TEST("clock rules: the owner's timescale while following, ours kept for the save and put back at the end")
{
	ScaleKeeper scale;
	CHECK(!scale.Kept());
	const auto first = scale.Follow(30.0f, 20.0f);
	REQUIRE(first);
	CHECK(*first == 30.0f);
	REQUIRE(scale.Kept());
	CHECK(*scale.Kept() == 20.0f);
	CHECK(!scale.Follow(30.0f, 30.0f));
	CHECK(!scale.Follow(30.0f, 30.005f));  // the same, within SAME_SCALE

	// A save: ours goes into it; the next tick follows again, ours still kept.
	const auto own = scale.Own(30.0f);
	REQUIRE(own);
	CHECK(*own == 20.0f);
	CHECK(scale.Follow(30.0f, 20.0f));
	CHECK(*scale.Kept() == 20.0f);

	// The session ends: ours back, nothing kept.
	const auto back = scale.Restore(30.0f);
	REQUIRE(back);
	CHECK(*back == 20.0f);
	CHECK(!scale.Kept());
	CHECK(!scale.Restore(20.0f));

	// Never followed, or the same as the owner's all along: nothing to put back.
	ScaleKeeper same;
	CHECK(!same.Follow(20.0f, 20.0f));
	CHECK(!same.Own(20.0f));
	CHECK(!same.Restore(20.0f));

	// A load: the loaded save's timescale is ours from then on.
	ScaleKeeper loaded;
	CHECK(loaded.Follow(30.0f, 20.0f));
	loaded.Forget();
	CHECK(loaded.Follow(30.0f, 10.0f));
	CHECK(*loaded.Kept() == 10.0f);
	CHECK(loaded.sets == 2u);
}

TEST("clock rules: a walled city and the land round it are one sky: a world on its parent's climate is its parent's")
{
	// Tamriel (3C); Whiterun inside its walls (1A26F), a child world on Tamriel's climate; a mod's world on the city's;
	// a world on its own climate (Solstheim, a mod's land); two worlds whose parents loop (a broken mod).
	const auto parentSky = [](std::uint32_t a_world) -> std::uint32_t {
		switch (a_world) {
		case 0x1A26F:
			return 0x3C;
		case 0x50001:
			return 0x1A26F;
		case 0x60001:
			return 0x60002;
		case 0x60002:
			return 0x60001;
		default:
			return 0;
		}
	};
	CHECK(SkyWorld(std::uint32_t{ 0x3C }, parentSky) == 0x3Cu);
	CHECK(SkyWorld(std::uint32_t{ 0x1A26F }, parentSky) == 0x3Cu);
	CHECK(SkyWorld(std::uint32_t{ 0x50001 }, parentSky) == 0x3Cu);  // walked up two
	CHECK(SkyWorld(std::uint32_t{ 0x7777 }, parentSky) == 0x7777u);
	CHECK(SkyWorld(std::uint32_t{ 0 }, parentSky) == 0u);
	const auto looped = SkyWorld(std::uint32_t{ 0x60001 }, parentSky);  // stops
	CHECK(looped == 0x60001u || looped == 0x60002u);

	using Clock = std::chrono::steady_clock;
	const auto t0 = Clock::time_point{} + 1000s;
	const auto at = [&](std::uint32_t a_world, std::uint32_t a_weather) {
		// Following, the weather here is our pin (WeatherFollower: set as an override).
		return Sky{ .outside = true, .worldspace = SkyWorld(a_world, parentSky), .weather = a_weather, .pinned = true, .overrideWeather = a_weather };
	};
	const auto says = [&](std::uint32_t a_world, std::uint32_t a_weather) { return Outside(a_weather, SkyWorld(a_world, parentSky)); };

	// Compared by the raw worldspace, a friend outside never follows a host inside the walls (the gap this closes).
	WeatherFollower raw;
	CHECK(raw.Decide(Outside(0xAAAA, 0x1A26F), Sky{ .outside = true, .worldspace = 0x3C, .weather = 0xBBBB }, t0) == WeatherMove::kNone);

	// The host inside Whiterun's walls, a friend on the plains: one sky. Arriving: forced once; the host's weather turns:
	// eased in, as under one sky.
	WeatherFollower weather;
	CHECK(says(0x1A26F, 0xAAAA).worldspace == 0x3Cu);
	CHECK(weather.Decide(says(0x1A26F, 0xAAAA), at(0x3C, 0xBBBB), t0) == WeatherMove::kForce);
	CHECK(weather.Decide(says(0x1A26F, 0xAAAA), at(0x3C, 0xAAAA), t0 + 5s) == WeatherMove::kNone);
	CHECK(weather.Decide(says(0x1A26F, 0xCCCC), at(0x3C, 0xAAAA), t0 + 10s) == WeatherMove::kEase);
	CHECK(weather.Decide(says(0x1A26F, 0xCCCC), at(0x3C, 0xCCCC), t0 + 20s) == WeatherMove::kNone);
	// The host walks out of the gate: still one sky, no arrival snap; its next turn eases in.
	CHECK(weather.Decide(says(0x3C, 0xCCCC), at(0x3C, 0xCCCC), t0 + 30s) == WeatherMove::kNone);
	CHECK(weather.Decide(says(0x3C, 0xDDDD), at(0x3C, 0xCCCC), t0 + 40s) == WeatherMove::kEase);
	// The friend walks in through the gate: still one sky; the host's turn eases in there too.
	CHECK(weather.Decide(says(0x3C, 0xDDDD), at(0x1A26F, 0xDDDD), t0 + 50s) == WeatherMove::kNone);
	CHECK(weather.Decide(says(0x3C, 0xEEEE), at(0x1A26F, 0xDDDD), t0 + 60s) == WeatherMove::kEase);
	CHECK(weather.forced == 1u);
	CHECK(weather.eased == 3u);

	// A world on its own climate is another sky: not followed, and our pin let go there (its climate's weather from now).
	CHECK(weather.Decide(says(0x3C, 0xEEEE), at(0x7777, 0xDDDD), t0 + 70s) == WeatherMove::kRelease);
	CHECK(weather.Decide(says(0x3C, 0xEEEE), Sky{ .outside = true, .worldspace = 0x7777, .weather = 0xDDDD }, t0 + 71s) == WeatherMove::kNone);
	CHECK(weather.releases == 1u);
}

TEST("clock rules: nothing follows while this game's own Sleep/Wait runs; once it shuts, the owner's clock is lined up from there")
{
	WaitGate gate;
	CHECK(gate.Next(false) == WaitStep::kRun);
	CHECK(gate.Next(true) == WaitStep::kHold);
	CHECK(gate.Next(true) == WaitStep::kHold);
	CHECK(gate.Open());
	CHECK(gate.waits == 1u);
	CHECK(gate.Next(false) == WaitStep::kShut);
	CHECK(gate.Next(false) == WaitStep::kRun);
	CHECK(gate.Next(true) == WaitStep::kHold);
	CHECK(gate.waits == 2u);
	gate.Reset();  // a load
	CHECK(!gate.Open());
	CHECK(gate.Next(false) == WaitStep::kRun);

	// A friend lined up at 01:30 sleeps eleven hours, to 12:30; nothing is followed meanwhile. Once the menu shuts, the
	// owner's last report (01:36) brings it back within the day, the day count with it: one move back after the sleep.
	{
		auto       follower = LinedUp(1.5f, 3.0625);
		const auto set = follower.Follow(Owner(1.6f, static_cast<float>(13.0625 + 0.1 / 24.0)), 12.5f, 3.0625 + 11.0 / 24.0);
		REQUIRE(set);
		CHECK(Near(set->hour, 1.6, 0.01));
		CHECK(Near(set->days, -10.9 / 24.0, 1e-3));
		CHECK(follower.backSets == 1u);
		CHECK(follower.lineUps == 1u);  // by the day offset, not lined up again
	}

	// A move forward to midnight was under way (the owner slept) when the friend's own wait began: dropped when the wait
	// ends, and the owner's clock followed from where the wait left the friend (past midnight already): forward to it.
	{
		auto       follower = LinedUp(20.0f, 3.8333);
		const auto first = follower.Follow(Owner(4.0f, static_cast<float>(13.8333 + 8.0 / 24.0)), 20.0f, 3.8333);
		REQUIRE(first);
		CHECK(follower.Moving());
		follower.Drop();
		CHECK(!follower.Moving());
		CHECK(!follower.Advance(1.99f));
		// The friend's own wait took it from 23:59 to 01:59 of its next day.
		const auto set = follower.Follow(Owner(4.1f, static_cast<float>(13.8333 + 8.1 / 24.0)), 1.99f, 3.8333 + 5.99 / 24.0);
		REQUIRE(set);
		CHECK(Near(set->hour, 4.1, 0.01));
		CHECK(Near(set->days, 2.11 / 24.0, 1e-3));
		CHECK(follower.lineUps == 1u);
		CHECK(!follower.Moving());
	}
}

TEST("clock rules: the Helgen opening holds the clock only once it has begun: a stage set, or the player held still in the cart")
{
	CHECK(OpeningHolds(true, false, 0, false));    // the cart: the quest at its start, the player's movement taken
	CHECK(OpeningHolds(true, false, 10, true));    // a stage set: in Helgen, free to move
	CHECK(!OpeningHolds(true, false, 0, true));    // running untouched, free to move: coc from the main menu, an alternate start
	CHECK(!OpeningHolds(true, true, 1000, true));  // done
	CHECK(!OpeningHolds(false, false, 0, false));  // not running (a scene elsewhere holds the player)
	CHECK(!OpeningHolds(false, true, 1000, true));
}

// ---- a friend's weather magic (Clear Skies, Storm Call, the sun arrow, a mod's weather spell) ------------------------

namespace
{
	using Protocol::WeatherAsk;

	std::vector<WeatherAsk> AsksIn(const std::vector<std::vector<std::uint8_t>>& a_packets)
	{
		std::vector<WeatherAsk> out;
		for (const auto& packet : a_packets) {
			if (Protocol::PeekType(packet) == MT::kRemoteWeatherAsk) {
				if (const auto ask = Protocol::DecodeWeatherAsk(packet)) {
					out.push_back(*ask);
				}
			}
		}
		return out;
	}

	// Following player 1 under Tamriel's sky: its weather here, pinned (WeatherFollower).
	Sky Pinned(std::uint32_t a_weather)
	{
		return Sky{ .outside = true, .worldspace = 0x3C, .weather = a_weather, .pinned = true, .overrideWeather = a_weather };
	}

	// Under Tamriel's sky with a script's override (a spell's), or none.
	Sky Held(std::uint32_t a_weather, std::uint32_t a_override)
	{
		return Sky{ .outside = true, .worldspace = 0x3C, .weather = a_weather, .overridden = a_override != 0, .overrideWeather = a_override };
	}

	Sky Plain(std::uint32_t a_weather)
	{
		return Sky{ .outside = true, .worldspace = 0x3C, .weather = a_weather };
	}
}

TEST("weather ask: a friend's weather magic goes as it is said, the asker named only from the server; a release names nothing")
{
	const WeatherAsk ask{ .playerId = 7, .worldspace = 0x3C, .weather = 0x10A241, .held = true };
	const auto       sent = Protocol::Encode(ask);
	REQUIRE(Protocol::PeekType(sent) == MT::kWeatherAsk);
	const auto read = Protocol::DecodeWeatherAsk(sent);
	REQUIRE(read);
	CHECK(read->playerId == 0u);  // a game does not say who it is: the server knows
	CHECK(read->worldspace == 0x3Cu);
	CHECK(read->weather == 0x10A241u);
	CHECK(read->held);
	CHECK(!read->release);
	const auto passed = Protocol::DecodeWeatherAsk(Protocol::Encode(ask, MT::kRemoteWeatherAsk));
	REQUIRE(passed);
	CHECK(*passed == ask);

	// A release: its forms and its hold are not written (only the asker's held one is let go).
	const WeatherAsk release{ .playerId = 7, .worldspace = 0x3C, .weather = 0x10A241, .held = true, .release = true };
	const auto       let = Protocol::DecodeWeatherAsk(Protocol::Encode(release, MT::kRemoteWeatherAsk));
	REQUIRE(let);
	CHECK(let->release);
	CHECK(!let->held);
	CHECK(let->worldspace == 0u);
	CHECK(let->weather == 0u);
	CHECK(let->playerId == 7u);
}

TEST("weather ask: an ask no game could make is no message")
{
	const WeatherAsk good{ .worldspace = 0x3C, .weather = 0x10A241 };
	CHECK(Protocol::DecodeWeatherAsk(Protocol::Encode(good)));
	CHECK(!Protocol::DecodeWeatherAsk(Protocol::Encode(WeatherAsk{ .worldspace = 0x3C })));   // no weather to set
	CHECK(!Protocol::DecodeWeatherAsk(Protocol::Encode(WeatherAsk{ .weather = 0x10A241 })));  // no sky to set it under

	auto flags = Protocol::Encode(good);
	flags.back() = 4;  // a flag no game knows
	CHECK(!Protocol::DecodeWeatherAsk(flags));
	flags.back() = 2;  // a release naming forms
	CHECK(!Protocol::DecodeWeatherAsk(flags));
	auto heldRelease = Protocol::Encode(WeatherAsk{ .release = true });
	REQUIRE(Protocol::DecodeWeatherAsk(heldRelease));
	heldRelease.back() = 3;  // a release that holds
	CHECK(!Protocol::DecodeWeatherAsk(heldRelease));

	auto longer = Protocol::Encode(good);
	longer.push_back(0);
	CHECK(!Protocol::DecodeWeatherAsk(longer));
	auto shorter = Protocol::Encode(good);
	shorter.pop_back();
	CHECK(!Protocol::DecodeWeatherAsk(shorter));
	CHECK(!Protocol::DecodeWeatherAsk(Protocol::Encode(Owner(12.0f, 1.0f))));
}

TEST("weather ask: its worldspace and weather cross by plugin name; one the owner's game lacks is not asked at all")
{
	using FormIds::Mapper;
	using FormTranslate::Result;
	using FormTranslate::Way;
	CHECK(FormTranslate::CarriesForms(MT::kWeatherAsk));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteWeatherAsk));

	// As in the clock's test: the host has Weathers.esp before Shared.esp; the guest (the owner here) Shared.esp only.
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Weathers.esp", "Shared.esp" }, {});
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Shared.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const Mapper guestEdge(guest, session);
	const auto   toGuest = [&](const WeatherAsk& a_ask) {
		return FormTranslate::Translate(guestEdge, Way::kToLocal, Protocol::Encode(a_ask, MT::kRemoteWeatherAsk));
	};

	// Tamriel and a base game weather: as they are.
	auto result = toGuest(WeatherAsk{ .playerId = 2, .worldspace = 0x3C, .weather = 0x0010A241, .held = true });
	CHECK(result.kind == Result::Kind::kSame);

	// A weather of Shared.esp: the guest's own ID for it, the rest as it was.
	result = toGuest(WeatherAsk{ .playerId = 2, .worldspace = 0x3C, .weather = 0x02000801, .held = true });
	REQUIRE(result.kind == Result::Kind::kChanged);
	const auto mapped = Protocol::DecodeWeatherAsk(result.packet);
	REQUIRE(mapped);
	CHECK(mapped->weather == 0x01000801u);
	CHECK(mapped->worldspace == 0x3Cu);
	CHECK(mapped->playerId == 2u);
	CHECK(mapped->held);

	// A weather, or a worldspace, of Weathers.esp, which the guest lacks: nothing it could set, so not read at all.
	CHECK(toGuest(WeatherAsk{ .playerId = 2, .worldspace = 0x3C, .weather = 0x01000801 }).kind == Result::Kind::kDrop);
	CHECK(toGuest(WeatherAsk{ .playerId = 2, .worldspace = 0x01000D62, .weather = 0x0010A241 }).kind == Result::Kind::kDrop);

	// A release names nothing: always read.
	CHECK(toGuest(WeatherAsk{ .playerId = 2, .release = true }).kind == Result::Kind::kSame);
}

TEST("weather ask server: a friend's goes to the clock owner alone, with whose it is; the owner's own is dropped; a burst is budgeted, a release never")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };
	f.fake->SetLocal(1);
	const auto host = f.Join(1, Fixture::MakeHello("Host", 0x4057));
	const auto a = f.Join(2, Fixture::MakeHello("A", 0x2222));
	const auto b = f.Join(3, Fixture::MakeHello("B", 0x3333));
	REQUIRE(host);
	REQUIRE(a);
	REQUIRE(b);
	REQUIRE(f.fake->Settle());
	f.fake->Take(1);
	f.fake->Take(2);
	f.fake->Take(3);

	// A's Clear Skies: to the host, named A's; not to B, not back to A.
	f.fake->Deliver(2, Protocol::Encode(WeatherAsk{ .worldspace = 0x3C, .weather = 0x10A241, .held = true }));
	const auto heard = f.fake->WaitFor(1, Type(MT::kRemoteWeatherAsk));
	REQUIRE(heard);
	const auto ask = Protocol::DecodeWeatherAsk(*heard);
	REQUIRE(ask);
	CHECK(ask->playerId == a->playerId);
	CHECK(ask->weather == 0x10A241u);
	CHECK(ask->held);
	REQUIRE(f.fake->Settle());
	CHECK(CountOf(f.fake->Take(2), MT::kRemoteWeatherAsk) == 0);
	CHECK(CountOf(f.fake->Take(3), MT::kRemoteWeatherAsk) == 0);

	// The host's own magic is its own sky, said by its clock: an ask from it goes nowhere, said once in the log.
	f.fake->Deliver(1, Protocol::Encode(WeatherAsk{ .worldspace = 0x3C, .weather = 0x10A241 }));
	f.fake->Deliver(1, Protocol::Encode(WeatherAsk{ .worldspace = 0x3C, .weather = 0x10A242 }));
	REQUIRE(f.LoggedSoon(std::format("player {} asked for a weather while keeping the session's clock itself: dropped", host->playerId)));
	REQUIRE(f.fake->Settle());
	CHECK(CountOf(f.fake->Take(1), MT::kRemoteWeatherAsk) == 0);
	CHECK(CountOf(f.fake->Take(2), MT::kRemoteWeatherAsk) == 0);
	CHECK(CountOf(f.fake->Take(3), MT::kRemoteWeatherAsk) == 0);
	CHECK(std::ranges::count_if(f.LogLines(), [](const std::string& a_line) { return a_line.find("asked for a weather while keeping") != std::string::npos; }) == 1);

	// B spams its shout: a burst of 4 goes, the rest are dropped; its release always goes.
	for (std::uint32_t i = 0; i < 12; ++i) {
		f.fake->Deliver(3, Protocol::Encode(WeatherAsk{ .worldspace = 0x3C, .weather = 0x10A241u + i, .held = true }));
	}
	f.fake->Deliver(3, Protocol::Encode(WeatherAsk{ .release = true }));
	std::vector<WeatherAsk> got;
	const auto              deadline = std::chrono::steady_clock::now() + 10s;
	while (std::chrono::steady_clock::now() < deadline && (got.empty() || !got.back().release)) {
		const auto more = AsksIn(f.fake->Take(1));
		got.insert(got.end(), more.begin(), more.end());
		std::this_thread::sleep_for(20ms);
	}
	REQUIRE(!got.empty());
	CHECK(got.back().release);
	CHECK(got.back().playerId == b->playerId);
	CHECK(got.size() >= 5u);
	CHECK(got.size() <= 7u);  // 4 at once and the release (one or two refilled at most, on a slow machine)
}

TEST("weather follow: the owner's weather is pinned here while following, and let go on leaving its sky")
{
	using Clock = std::chrono::steady_clock;
	const auto      t0 = Clock::time_point{} + 1000s;
	WeatherFollower weather;
	const auto      owner = Outside(0xAAAA);

	// Already the owner's weather (a save just written, a script let its own go): pinned as it is, no transition.
	CHECK(weather.Decide(owner, Plain(0xAAAA), t0) == WeatherMove::kPin);
	CHECK(weather.Decide(owner, Pinned(0xAAAA), t0 + 1s) == WeatherMove::kNone);
	CHECK(weather.pins == 1u);

	// A script of ours holds its own override (a quest's storm): not pinned over it.
	CHECK(weather.Decide(owner, Held(0xAAAA, 0xAAAA), t0 + 2s) == WeatherMove::kNone);
	CHECK(weather.Decide(owner, Held(0xEEEE, 0xEEEE), t0 + 3s) == WeatherMove::kHeld);
	CHECK(weather.pins == 1u);

	// Inside (the pin is still the sky's override until let go): let go once; nothing after.
	Sky inside = Pinned(0xAAAA);
	inside.outside = false;
	inside.worldspace = 0;
	CHECK(weather.Decide(owner, inside, t0 + 4s) == WeatherMove::kRelease);
	CHECK(weather.Decide(owner, Sky{}, t0 + 5s) == WeatherMove::kNone);
	CHECK(weather.releases == 1u);

	// The owner went inside (no weather said): ours let go too, under the climate here.
	CHECK(weather.Decide(owner, Pinned(0xAAAA), t0 + 6s) == WeatherMove::kNone);
	CHECK(weather.Decide(Outside(0, 0), Pinned(0xAAAA), t0 + 7s) == WeatherMove::kRelease);
	CHECK(weather.releases == 2u);
}

TEST("weather magic: a held weather of our magic is asked of the owner, and let go there when it ends")
{
	using Clock = std::chrono::steady_clock;
	const auto t0 = Clock::time_point{} + 1000s;
	MagicWatch magic;

	// Nothing seen yet (the first tick): a cast opens nothing.
	magic.Cast(t0, 0x3C);
	CHECK(!magic.Holds());
	CHECK(magic.windows == 0u);

	// Following player 1's sky, AAAA pinned. Our Clear Skies takes hold: a window opens (our pin is taken off).
	magic.Seen(Pinned(0xAAAA));
	magic.Cast(t0, 0x3C);
	CHECK(magic.Holds());
	CHECK(magic.windows == 1u);
	CHECK(magic.Look(t0, Plain(0xAAAA), 0xAAAA).kind == MagicStep::Kind::kNone);

	// Its script holds a clear weather as an override: asked, held.
	const auto asked = magic.Look(t0 + 100ms, Held(0xAAAA, 0xCCCC), 0xAAAA);
	CHECK(asked.kind == MagicStep::Kind::kAsk);
	CHECK(asked.worldspace == 0x3Cu);
	CHECK(asked.weather == 0xCCCCu);
	CHECK(asked.held);
	CHECK(magic.Asked() == std::optional<std::uint32_t>{ 0xCCCC });
	CHECK(magic.asks == 1u);

	// While it lasts: nothing more, and the sky is the magic's (the owner's report carries it soon).
	CHECK(magic.Look(t0 + 10s, Held(0xCCCC, 0xCCCC), 0xCCCC).kind == MagicStep::Kind::kNone);
	CHECK(magic.Holds());

	// Another cast meanwhile (a potion) changes nothing of it.
	magic.Seen(Held(0xCCCC, 0xCCCC));
	magic.Cast(t0 + 20s, 0x3C);
	CHECK(magic.Look(t0 + 20s, Held(0xCCCC, 0xCCCC), 0xCCCC).kind == MagicStep::Kind::kNone);

	// The effect ends (its script lets the override go): the owner is told to let go.
	const auto ended = magic.Look(t0 + 60s, Plain(0xCCCC), 0xCCCC);
	CHECK(ended.kind == MagicStep::Kind::kRelease);
	CHECK(ended.weather == 0xCCCCu);
	CHECK(!magic.Holds());
	CHECK(!magic.Asked());
	CHECK(magic.releases == 1u);
}

TEST("weather magic: a weather our magic sets and does not hold is asked once; the window closes on its own")
{
	using Clock = std::chrono::steady_clock;
	const auto t0 = Clock::time_point{} + 1000s;

	// A Storm Call not held: the weather turns, no override. Asked, not held; left to the magic until the owner's
	// report carries it.
	{
		MagicWatch magic;
		magic.Seen(Pinned(0xAAAA));
		magic.Cast(t0, 0x3C);
		const auto asked = magic.Look(t0 + 1s, Plain(0xDDDD), 0xAAAA);
		CHECK(asked.kind == MagicStep::Kind::kAsk);
		CHECK(asked.weather == 0xDDDDu);
		CHECK(!asked.held);
		CHECK(!magic.Asked());  // nothing to let go later
		CHECK(magic.Look(t0 + 3s, Plain(0xDDDD), 0xAAAA).kind == MagicStep::Kind::kNone);
		CHECK(magic.Holds());
		CHECK(magic.Look(t0 + 4s, Plain(0xDDDD), 0xDDDD).kind == MagicStep::Kind::kNone);
		CHECK(!magic.Holds());  // the owner's sky has it: followed again
	}
	// ... and the owner never says it (its game went inside): given up after MAGIC_ANSWER.
	{
		MagicWatch magic;
		magic.Seen(Pinned(0xAAAA));
		magic.Cast(t0, 0x3C);
		CHECK(magic.Look(t0 + 1s, Plain(0xDDDD), 0xAAAA).kind == MagicStep::Kind::kAsk);
		CHECK(magic.Look(t0 + MAGIC_ANSWER, Plain(0xDDDD), 0).kind == MagicStep::Kind::kNone);
		CHECK(magic.Holds());
		CHECK(magic.Look(t0 + 1s + MAGIC_ANSWER, Plain(0xDDDD), 0).kind == MagicStep::Kind::kNone);
		CHECK(!magic.Holds());
	}
	// The sun arrow: it lets every override go, then sets its own, held: asked once, then asked again held.
	{
		MagicWatch magic;
		magic.Seen(Pinned(0xAAAA));
		magic.Cast(t0, 0x3C);
		CHECK(magic.Look(t0 + 1s, Plain(0xBBBB), 0xAAAA).kind == MagicStep::Kind::kAsk);
		const auto held = magic.Look(t0 + 2s, Held(0xBBBB, 0x5555), 0xAAAA);
		CHECK(held.kind == MagicStep::Kind::kAsk);
		CHECK(held.weather == 0x5555u);
		CHECK(held.held);
		CHECK(magic.asks == 2u);
	}
	// A spell that does nothing to the sky (a heal): the window closes, nothing asked.
	{
		MagicWatch magic;
		magic.Seen(Pinned(0xAAAA));
		magic.Cast(t0, 0x3C);
		CHECK(magic.Look(t0 + 1s, Plain(0xAAAA), 0xAAAA).kind == MagicStep::Kind::kNone);
		CHECK(magic.Holds());
		CHECK(magic.Look(t0 + MAGIC_WINDOW, Plain(0xAAAA), 0xAAAA).kind == MagicStep::Kind::kNone);
		CHECK(!magic.Holds());
		CHECK(magic.asks == 0u);
	}
	// Inside, or under a sky not the owner's (our own climate's): our magic's weather is our own, nothing opens.
	{
		MagicWatch magic;
		magic.Seen(Sky{});
		magic.Cast(t0, 0x3C);
		CHECK(!magic.Holds());
		magic.Seen(Sky{ .outside = true, .worldspace = 0x7777, .weather = 0xAAAA });
		magic.Cast(t0, 0x3C);
		CHECK(!magic.Holds());
		magic.Seen(Pinned(0xAAAA));
		magic.Cast(t0, 0);  // no owner's sky heard
		CHECK(!magic.Holds());
		CHECK(magic.windows == 0u);
	}
}

TEST("weather magic: a held one is let go on leaving its sky, after MAGIC_HOLD_MAX, and replaced by a second")
{
	using Clock = std::chrono::steady_clock;
	const auto t0 = Clock::time_point{} + 1000s;
	const auto ask = [&](MagicWatch& a_magic, std::uint32_t a_override) {
		a_magic.Seen(Pinned(0xAAAA));
		a_magic.Cast(t0, 0x3C);
		return a_magic.Look(t0, Held(0xAAAA, a_override), 0xAAAA);
	};
	{
		MagicWatch magic;
		REQUIRE(ask(magic, 0xCCCC).held);
		CHECK(magic.Look(t0 + 5s, Sky{}, 0xCCCC).kind == MagicStep::Kind::kRelease);  // went inside
	}
	{
		MagicWatch magic;
		REQUIRE(ask(magic, 0xCCCC).held);
		CHECK(magic.Look(t0 + MAGIC_HOLD_MAX - 1s, Held(0xCCCC, 0xCCCC), 0xCCCC).kind == MagicStep::Kind::kNone);
		CHECK(magic.Look(t0 + MAGIC_HOLD_MAX, Held(0xCCCC, 0xCCCC), 0xCCCC).kind == MagicStep::Kind::kRelease);
	}
	{
		MagicWatch magic;
		REQUIRE(ask(magic, 0xCCCC).held);
		const auto storm = magic.Look(t0 + 30s, Held(0xCCCC, 0xDDDD), 0xCCCC);  // Storm Call over Clear Skies
		CHECK(storm.kind == MagicStep::Kind::kAsk);
		CHECK(storm.weather == 0xDDDDu);
		CHECK(magic.Asked() == std::optional<std::uint32_t>{ 0xDDDD });
	}
	{
		MagicWatch magic;
		REQUIRE(ask(magic, 0xCCCC).held);
		magic.Reset();  // a load, the owner gone
		CHECK(!magic.Holds());
		CHECK(!magic.Asked());
	}
}

TEST("weather magic, the owner: a friend's is set under our sky, held until it ends, its friend leaves, or MAGIC_HOLD_MAX")
{
	using Clock = std::chrono::steady_clock;
	const auto t0 = Clock::time_point{} + 1000s;
	AskKeeper  keeper;
	const Sky  ours = Plain(0xAAAA);
	const Sky  held = Held(0xCCCC, 0xCCCC);
	const auto clearSkies = [](std::uint32_t a_player) { return WeatherAsk{ .playerId = a_player, .worldspace = 0x3C, .weather = 0xCCCC, .held = true }; };

	// Not under that sky (inside, or another worldspace): nothing set.
	CHECK(keeper.Heard(clearSkies(2), Sky{}, t0).kind == AskMove::Kind::kIgnore);
	CHECK(keeper.Heard(WeatherAsk{ .playerId = 2, .worldspace = 0x7777, .weather = 0xCCCC, .held = true }, ours, t0).kind == AskMove::Kind::kIgnore);
	CHECK(keeper.ignored == 2u);
	CHECK(keeper.Weather() == 0u);

	// Player 2's Clear Skies: set, held.
	auto move = keeper.Heard(clearSkies(2), ours, t0);
	CHECK(move.kind == AskMove::Kind::kSet);
	CHECK(move.weather == 0xCCCCu);
	CHECK(move.held);
	CHECK(keeper.Weather() == 0xCCCCu);
	CHECK(keeper.By() == 2u);
	CHECK(keeper.Tick(t0 + 10s, held).kind == AskMove::Kind::kNone);

	// Someone else's release is not 2's; 2's lets it go.
	CHECK(keeper.Heard(WeatherAsk{ .playerId = 3, .release = true }, held, t0 + 20s).kind == AskMove::Kind::kNone);
	CHECK(keeper.Weather() == 0xCCCCu);
	move = keeper.Heard(WeatherAsk{ .playerId = 2, .release = true }, held, t0 + 30s);
	CHECK(move.kind == AskMove::Kind::kRelease);
	CHECK(move.weather == 0xCCCCu);
	CHECK(keeper.Weather() == 0u);
	CHECK(keeper.releases == 1u);

	// 2 leaves while it holds: let go.
	REQUIRE(keeper.Heard(clearSkies(2), ours, t0 + 40s).kind == AskMove::Kind::kSet);
	CHECK(keeper.Left(3, held).kind == AskMove::Kind::kNone);
	CHECK(keeper.Left(2, held).kind == AskMove::Kind::kRelease);
	CHECK(keeper.Weather() == 0u);

	// Never said to end: let go after MAGIC_HOLD_MAX.
	REQUIRE(keeper.Heard(clearSkies(2), ours, t0 + 50s).kind == AskMove::Kind::kSet);
	CHECK(keeper.Tick(t0 + 50s + MAGIC_HOLD_MAX - 1s, held).kind == AskMove::Kind::kNone);
	move = keeper.Tick(t0 + 50s + MAGIC_HOLD_MAX, held);
	CHECK(move.kind == AskMove::Kind::kRelease);
	CHECK(keeper.Weather() == 0u);

	// A script of ours took the sky (a quest's storm) meanwhile: forgotten, and nothing of ours let go.
	REQUIRE(keeper.Heard(clearSkies(2), ours, t0 + 60s).kind == AskMove::Kind::kSet);
	CHECK(keeper.Tick(t0 + 61s, Held(0xEEEE, 0xEEEE)).kind == AskMove::Kind::kNone);
	CHECK(keeper.Weather() == 0u);
	CHECK(keeper.Heard(WeatherAsk{ .playerId = 2, .release = true }, Held(0xEEEE, 0xEEEE), t0 + 62s).kind == AskMove::Kind::kNone);

	// A weather not held (a Storm Call that lets the climate go on) replaces a held one: nothing kept.
	REQUIRE(keeper.Heard(clearSkies(2), ours, t0 + 70s).kind == AskMove::Kind::kSet);
	move = keeper.Heard(WeatherAsk{ .playerId = 3, .worldspace = 0x3C, .weather = 0xDDDD }, held, t0 + 71s);
	CHECK(move.kind == AskMove::Kind::kSet);
	CHECK(!move.held);
	CHECK(keeper.Weather() == 0u);
	CHECK(keeper.sets == 6u);
	CHECK(keeper.releases == 3u);
}

TEST("weather magic, the save: a friend's save keeps the session's day count, said against the one it joined with")
{
	SaveDays days;
	CHECK(!days.Since(12.0));
	days.Joined(10.25);
	days.Joined(11.0);  // the next tick: the first is kept
	REQUIRE(days.Since(12.75));
	CHECK(Near(*days.Since(12.75), 2.5));
	days.Reset();  // a load: its day count is the one it is in the session with
	CHECK(!days.Since(12.75));
	days.Joined(30.0);
	CHECK(Near(*days.Since(30.5), 0.5));
}
