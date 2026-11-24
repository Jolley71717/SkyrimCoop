#include "Test.h"

#include "FormTranslate.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "game/DownRules.h"

#include <chrono>
#include <optional>
#include <random>
#include <vector>

// A player brought down and helped up (docs/DESIGN.md, "Downed and helped up"): the messages, the server passing them
// on, and the rules of our own down, of helping a friend up and of a downed friend's stand-in.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using DownRules::Phase;
	using DownRules::Said;
	using Steady = std::chrono::steady_clock;

	// Our player in a session, standing, our flag on.
	DownRules::Facts Standing()
	{
		return DownRules::Facts{ .friends = true, .ours = true };
	}

	DownRules::Facts Down()
	{
		auto facts = Standing();
		facts.down = true;
		return facts;
	}

	template <class Msg>
	std::vector<Msg> SentTo(Fixture& a_f, PeerId a_peer, MT a_type, std::optional<Msg> (*a_decode)(std::span<const std::uint8_t>))
	{
		std::vector<Msg> out;
		for (auto& sent : a_f.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == a_type) {
				CHECK(sent.reliable);
				if (auto msg = a_decode(sent.data)) {
					out.push_back(*msg);
				}
			}
		}
		return out;
	}

	void AllIn(Fixture& a_f, PeerId a_count)
	{
		const char* names[] = { "Ada", "Bo", "Cy", "Di" };
		for (PeerId peer = 1; peer <= a_count; ++peer) {
			REQUIRE(a_f.Join(peer, Fixture::MakeHello(names[peer - 1])));
		}
		REQUIRE(a_f.fake->Settle());
		for (PeerId peer = 1; peer <= a_count; ++peer) {
			(void)a_f.fake->Take(peer);
		}
	}
}

// ---- the messages ----

TEST("downed protocol: downs and helps read back as written, as messages 77 and 78 of protocol 25, and carry no forms")
{
	static_assert(Protocol::VERSION >= 25);
	static_assert(static_cast<std::uint8_t>(MT::kDowned) == 77);
	static_assert(static_cast<std::uint8_t>(MT::kHelpUp) == 78);
	static_assert(Protocol::DOWNED_STATES == DownRules::SAID_COUNT);
	for (const auto& down : { Protocol::Downed{ 2, 1, 45 }, Protocol::Downed{ 0, 2, 0 }, Protocol::Downed{ 3, 0, 0 } }) {
		CHECK(Protocol::DecodeDowned(Protocol::Encode(down)) == down);
		CHECK(Protocol::PeekType(Protocol::Encode(down)) == MT::kDowned);
	}
	CHECK(Protocol::DecodeHelpUp(Protocol::Encode(Protocol::HelpUp{ 4 })) == Protocol::HelpUp{ 4 });
	CHECK(Protocol::PeekType(Protocol::Encode(Protocol::HelpUp{ 4 })) == MT::kHelpUp);
	CHECK(!FormTranslate::CarriesForms(MT::kDowned));
	CHECK(!FormTranslate::CarriesForms(MT::kHelpUp));
}

TEST("downed protocol: a state past the last, seconds left while not waiting, a help of nobody, or a byte too many are refused")
{
	CHECK(!Protocol::DecodeDowned(Protocol::Encode(Protocol::Downed{ 1, 3, 0 })));
	CHECK(!Protocol::DecodeDowned(Protocol::Encode(Protocol::Downed{ 1, 0, 5 })));
	CHECK(!Protocol::DecodeDowned(Protocol::Encode(Protocol::Downed{ 1, 2, 5 })));
	CHECK(!Protocol::DecodeHelpUp(Protocol::Encode(Protocol::HelpUp{ 0 })));
	auto down = Protocol::Encode(Protocol::Downed{ 1, 1, 10 });
	down.push_back(0);
	CHECK(!Protocol::DecodeDowned(down));
	auto help = Protocol::Encode(Protocol::HelpUp{ 1 });
	help.pop_back();
	CHECK(!Protocol::DecodeHelpUp(help));
}

// ---- the server ----

TEST("downed server: a down goes to the others with the sender named, a waiting one is kept for a joiner, an up ends it")
{
	Fixture f;
	AllIn(f, 2);
	f.fake->Deliver(2, Protocol::Encode(Protocol::Downed{ .player = 99, .state = 1, .secondsLeft = 45 }));
	REQUIRE(f.fake->Settle());
	auto got = SentTo<Protocol::Downed>(f, 1, MT::kDowned, Protocol::DecodeDowned);
	REQUIRE(got.size() == 1u);
	CHECK((got[0] == Protocol::Downed{ 2, 1, 45 }));
	CHECK(SentTo<Protocol::Downed>(f, 2, MT::kDowned, Protocol::DecodeDowned).empty());  // not back to its sender
	CHECK(f.LoggedSoon("server: player 2 is down, waiting for help"));

	// A joiner hears Bo is down.
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	got = SentTo<Protocol::Downed>(f, 3, MT::kDowned, Protocol::DecodeDowned);
	REQUIRE(got.size() == 1u);
	CHECK(got[0].player == 2u);

	// Bo is up: everyone hears, and the next joiner hears nothing.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Downed{ .state = 0 }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 3 } }) {
		got = SentTo<Protocol::Downed>(f, peer, MT::kDowned, Protocol::DecodeDowned);
		REQUIRE(got.size() == 1u);
		CHECK((got[0] == Protocol::Downed{ 2, 0, 0 }));
	}
	REQUIRE(f.Join(4, Fixture::MakeHello("Di")));
	REQUIRE(f.fake->Settle());
	CHECK(SentTo<Protocol::Downed>(f, 4, MT::kDowned, Protocol::DecodeDowned).empty());
}

TEST("downed server: a help goes to the downed player alone, the helper named; never one's own, nor to one not waiting")
{
	Fixture f;
	AllIn(f, 3);
	// Nobody is down: Ada's help for Bo goes nowhere.
	f.fake->Deliver(1, Protocol::Encode(Protocol::HelpUp{ 2 }));
	REQUIRE(f.fake->Settle());
	CHECK(SentTo<Protocol::HelpUp>(f, 2, MT::kHelpUp, Protocol::DecodeHelpUp).empty());
	CHECK(f.LoggedSoon("server: player 1's help for player 2 dropped (not waiting for help)"));

	f.fake->Deliver(2, Protocol::Encode(Protocol::Downed{ .state = 1, .secondsLeft = 40 }));
	REQUIRE(f.fake->Settle());
	for (PeerId peer = 1; peer <= 3; ++peer) {
		(void)f.fake->Take(peer);
	}
	// Bo helping himself, and a help for a player who is not there: nothing.
	f.fake->Deliver(2, Protocol::Encode(Protocol::HelpUp{ 2 }));
	f.fake->Deliver(1, Protocol::Encode(Protocol::HelpUp{ 7 }));
	REQUIRE(f.fake->Settle());
	CHECK(SentTo<Protocol::HelpUp>(f, 2, MT::kHelpUp, Protocol::DecodeHelpUp).empty());
	CHECK(f.LoggedSoon("server: player 2's help for player 2 dropped (itself)"));
	CHECK(f.LoggedSoon("server: player 1's help for player 7 dropped (no such player)"));

	// Ada helps Bo: Bo alone hears it, Ada named.
	f.fake->Deliver(1, Protocol::Encode(Protocol::HelpUp{ 2 }));
	REQUIRE(f.fake->Settle());
	const auto got = SentTo<Protocol::HelpUp>(f, 2, MT::kHelpUp, Protocol::DecodeHelpUp);
	REQUIRE(got.size() == 1u);
	CHECK(got[0].player == 1u);
	CHECK(SentTo<Protocol::HelpUp>(f, 3, MT::kHelpUp, Protocol::DecodeHelpUp).empty());

	// Bo getting up: Cy's late help goes nowhere.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Downed{ .state = 2 }));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(2);
	f.fake->Deliver(3, Protocol::Encode(Protocol::HelpUp{ 2 }));
	REQUIRE(f.fake->Settle());
	CHECK(SentTo<Protocol::HelpUp>(f, 2, MT::kHelpUp, Protocol::DecodeHelpUp).empty());
}

// ---- our own down ----

TEST("downed rules: our flag is on while a friend is here and nothing else makes us essential, off alone or when dead")
{
	DownRules::Self self;
	const auto      t = Steady::now();
	auto            facts = DownRules::Facts{ .friends = true };
	CHECK(self.Frame(facts, t).essential == std::optional<bool>{ true });
	facts.ours = true;
	CHECK(!self.Frame(facts, t + 10ms).essential);
	facts.friends = false;  // alone again
	CHECK(self.Frame(facts, t + 20ms).essential == std::optional<bool>{ false });
	facts = DownRules::Facts{ .friends = true, .elsewise = true };  // a quest's essential: never ours too
	CHECK(!self.Frame(facts, t + 30ms).essential);
	facts = DownRules::Facts{ .friends = true, .ours = true, .elsewise = true };
	CHECK(self.Frame(facts, t + 40ms).essential == std::optional<bool>{ false });
	facts = DownRules::Facts{ .friends = true, .dead = true };
	CHECK(!self.Frame(facts, t + 50ms).essential);
	CHECK(self.Now() == Phase::kUp);
}

TEST("downed rules: brought down by ours, we wait; a help gets us up with the engine's own get-up, said each step")
{
	DownRules::Self self;
	const auto      t = Steady::now();
	(void)self.Frame(Standing(), t);
	auto step = self.Frame(Down(), t + 100ms);
	CHECK(step.wentDown);
	CHECK(step.noRecovery);
	CHECK(step.say);
	CHECK(self.Says() == Said::kWaiting);
	CHECK(self.WaitsForHelp());
	CHECK(self.SecondsLeft(t + 100ms) == 45);
	CHECK(self.SecondsLeft(t + 10100ms) == 35);

	// A friend's help: taken once, a second does nothing.
	CHECK(self.Help(t + 5s));
	CHECK(!self.Help(t + 5100ms));
	CHECK(self.counts.helpsRefused == 1u);
	CHECK(self.Says() == Said::kGettingUp);
	CHECK(self.TakeHelpPending());
	CHECK(!self.TakeHelpPending());
	// Still down for a moment: nothing by hand before GET_UP_WAIT.
	step = self.Frame(Down(), t + 5100ms);
	CHECK(!step.standUp);
	CHECK(!step.kill);
	// Up: said, the engine's recovery let be again.
	step = self.Frame(Standing(), t + 5600ms);
	CHECK(step.gotUp);
	CHECK(step.recovery);
	CHECK(step.say);
	CHECK(self.Says() == Said::kUp);
	CHECK(self.counts.gotUp == 1u);
	CHECK(self.counts.downs == 1u);
}

TEST("downed rules: a get-up the engine does not finish is done by hand, again each second, and never leaves us down")
{
	DownRules::Self self;
	auto            t = Steady::now();
	(void)self.Frame(Standing(), t);
	(void)self.Frame(Down(), t);
	CHECK(self.Help(t));
	std::size_t byHand = 0;
	bool        bled = false;
	for (int i = 1; i <= 2200 && !bled; ++i) {
		t += 10ms;
		const auto step = self.Frame(Down(), t);
		byHand += step.standUp ? 1 : 0;
		if (step.standUp) {
			CHECK(step.health);
		}
		bled = step.kill;
	}
	// First by hand at 1.5 s of play, then once a second until 20 s: about 19, then the bleed-out's death.
	CHECK(byHand >= 17u);
	CHECK(byHand <= 20u);
	CHECK(bled);
	CHECK(self.Now() == Phase::kBledOut);
	CHECK(self.counts.neverUp == 1u);
}

TEST("downed rules: menus stop the get-up's time, not the wait's (the friends' games go on)")
{
	DownRules::Self self;
	auto            t = Steady::now();
	(void)self.Frame(Standing(), t);
	(void)self.Frame(Down(), t);
	auto paused = Down();
	paused.paused = true;
	// 50 s in a menu while waiting: bled out all the same.
	bool killed = false;
	for (int i = 0; i < 50 && !killed; ++i) {
		t += 1s;
		killed = self.Frame(paused, t).kill;
	}
	CHECK(killed);

	DownRules::Self other;
	t = Steady::now();
	(void)other.Frame(Standing(), t);
	(void)other.Frame(Down(), t);
	CHECK(other.Help(t));
	// A minute in a menu while getting up: no stand-up by hand, no death.
	for (int i = 0; i < 60; ++i) {
		t += 1s;
		const auto step = other.Frame(paused, t);
		CHECK(!step.kill);
		CHECK(!step.standUp);
	}
	CHECK(other.Now() == Phase::kGettingUp);
}

TEST("downed rules: no help in time: our flag off and the kill, tried again every 2 s until it takes; friends leaving end nothing")
{
	DownRules::Self self;
	auto            t = Steady::now();
	(void)self.Frame(Standing(), t);
	(void)self.Frame(Down(), t);
	auto alone = Down();
	alone.friends = false;  // they left: the down goes on its clock (one may come back)
	for (int i = 0; i < 44; ++i) {
		t += 1s;
		const auto step = self.Frame(alone, t);
		CHECK(!step.kill);
		CHECK(!step.essential || *step.essential);
	}
	CHECK(self.SecondsLeft(t) == 1);
	t += 1s;
	auto step = self.Frame(Down(), t);
	CHECK(step.kill);
	CHECK(step.essential == std::optional<bool>{ false });
	CHECK(step.say);
	CHECK(self.Says() == Said::kUp);
	CHECK(self.counts.bledOut == 1u);
	// The kill did not take; our flag is off (no "ours" any more): tried again after 2 s, never ours again.
	auto notTaken = Down();
	notTaken.ours = false;
	CHECK(!self.Frame(notTaken, t + 1s).kill);
	step = self.Frame(notTaken, t + 2s);
	CHECK(step.kill);
	CHECK(!step.essential || !*step.essential);
	// Dead: nothing more (the game loads the save).
	auto dead = notTaken;
	dead.dead = true;
	dead.down = false;
	step = self.Frame(dead, t + 10s);
	CHECK(!step.kill);
	CHECK(!step.essential);
	// The load: up, and our flag judged again.
	self.Forget();
	CHECK(self.Frame(DownRules::Facts{ .friends = true }, t + 20s).essential == std::optional<bool>{ true });
}

TEST("downed rules: reminders of the seconds left at 30 and 10, once each, and the down said again every 5 s")
{
	DownRules::Self self;
	auto            t = Steady::now();
	(void)self.Frame(Standing(), t);
	(void)self.Frame(Down(), t);
	std::vector<int> reminded;
	std::size_t      said = 0;
	for (int i = 0; i < 440; ++i) {
		t += 100ms;
		const auto step = self.Frame(Down(), t);
		if (step.remind != 0) {
			reminded.push_back(step.remind);
		}
		said += step.say ? 1 : 0;
	}
	CHECK(reminded == std::vector<int>({ 30, 10 }));
	CHECK(said >= 8u);
	CHECK(said <= 9u);
}

TEST("downed rules: stood up by another path while down is up; a save that took our flag off has it back; a script's kill ends it")
{
	DownRules::Self self;
	auto            t = Steady::now();
	(void)self.Frame(Standing(), t);
	(void)self.Frame(Down(), t);
	auto saved = Down();
	saved.ours = false;  // BeforeSave took it off
	CHECK(self.Frame(saved, t + 1s).essential == std::optional<bool>{ true });
	auto step = self.Frame(Standing(), t + 2s);  // a console's resurrect
	CHECK(step.gotUp);
	CHECK(self.counts.upElsewise == 1u);
	CHECK(self.Now() == Phase::kUp);

	(void)self.Frame(Down(), t + 3s);
	auto dead = Down();
	dead.dead = true;
	step = self.Frame(dead, t + 4s);
	CHECK(self.Now() == Phase::kBledOut);
	CHECK(step.say);
	// A down that another flag began (a quest's essential) is the engine's own: never ours.
	DownRules::Self quest;
	auto            questDown = DownRules::Facts{ .friends = true, .elsewise = true, .down = true };
	step = quest.Frame(questDown, t);
	CHECK(!step.wentDown);
	CHECK(quest.Now() == Phase::kUp);
}

TEST("downed rules: a bleedout camera with nobody down is ended after a second, again every 2 s while it stays")
{
	DownRules::Self self;
	auto            t = Steady::now();
	auto            facts = Standing();
	facts.bleedCamera = true;
	CHECK(!self.Frame(facts, t).fixCamera);
	CHECK(!self.Frame(facts, t + 500ms).fixCamera);
	CHECK(self.Frame(facts, t + 1s).fixCamera);
	CHECK(!self.Frame(facts, t + 2s).fixCamera);
	CHECK(self.Frame(facts, t + 3s).fixCamera);
	// While down it is the down's own camera.
	auto down = Down();
	down.bleedCamera = true;
	CHECK(!self.Frame(down, t + 10s).fixCamera);
	CHECK(!self.Frame(down, t + 20s).fixCamera);
}

TEST("downed rules: 2000 random frames never leave us down past the bleed-out with nobody acting, nor with our flag on alone and up")
{
	std::mt19937 rng{ 7 };
	for (int run = 0; run < 50; ++run) {
		DownRules::Self self;
		auto            t = Steady::now();
		bool            down = false;
		bool            ours = false;
		bool            dead = false;
		std::optional<Steady::time_point> downSince;
		for (int i = 0; i < 2000; ++i) {
			t += std::chrono::milliseconds(10 + rng() % 200);
			DownRules::Facts facts{ .friends = rng() % 4 != 0, .ours = ours, .down = down, .dead = dead, .paused = rng() % 10 == 0 };
			const auto       step = self.Frame(facts, t);
			if (step.essential) {
				ours = *step.essential;
			}
			if (rng() % 50 == 0 && self.Now() == Phase::kDown) {
				(void)self.Help(t);
			}
			// The world: a lethal blow now and then (down when ours, dead when not), the engine's get-up after help,
			// a kill that takes now and then, a load after a death.
			if (!down && !dead && rng() % 40 == 0) {
				(ours ? down : dead) = true;
			}
			if (step.kill && rng() % 2 == 0) {
				down = false;
				dead = true;
			}
			if ((step.standUp || self.Now() == Phase::kGettingUp) && down && rng() % 5 == 0) {
				down = false;
			}
			if (dead && rng() % 20 == 0) {
				dead = false;
				self.Forget();
			}
			if (down) {
				downSince = downSince.value_or(t);
				// A down never outlives the bleed-out (and a get-up's most) by more than the kill retries.
				CHECK(t - *downSince < DownRules::BLEED_OUT + DownRules::GET_UP_MAX + 2min);
			} else {
				downSince.reset();
			}
			if (self.Now() == Phase::kUp && !down && !dead && !facts.friends && step.essential) {
				CHECK(!*step.essential);
			}
		}
	}
}

// ---- helping a friend up ----

TEST("downed helper: near a friend who waits for 2 s is a help, not again for 3 s; away, unable or not waiting starts again")
{
	DownRules::Helper helper;
	const auto        t = Steady::now();
	CHECK(!helper.Frame(true, true, 200.0f, t));
	CHECK(!helper.Frame(true, true, 200.0f, t + 1s));
	CHECK(helper.Frame(true, true, 299.0f, t + 2s));
	CHECK(!helper.Frame(true, true, 200.0f, t + 3s));
	CHECK(helper.Frame(true, true, 200.0f, t + 5s));  // still waits: asked again
	CHECK(helper.sent == 2u);
	// Away: the time starts again.
	DownRules::Helper away;
	CHECK(!away.Frame(true, true, 200.0f, t));
	CHECK(!away.Frame(true, true, 400.0f, t + 1s));
	CHECK(!away.Frame(true, true, 200.0f, t + 2s));
	CHECK(!away.Frame(true, true, 200.0f, t + 3s));
	CHECK(away.Frame(true, true, 200.0f, t + 4s));
	// No stand-in here, unable (down ourselves, in a menu), or the friend not waiting: never.
	DownRules::Helper never;
	for (int i = 0; i < 10; ++i) {
		CHECK(!never.Frame(true, true, std::nullopt, t + i * 1s));
		CHECK(!never.Frame(true, false, 100.0f, t + i * 1s + 100ms));
		CHECK(!never.Frame(false, true, 100.0f, t + i * 1s + 200ms));
	}
	CHECK(never.sent == 0u);
}

// ---- a downed friend's stand-in ----

TEST("downed stand-in: kneels while its friend is down and gets up with them; a new stand-in is asked again; a refusal retried 5 times")
{
	using DownRules::Kneel;
	DownRules::StandInKneel kneel;
	const auto              t = Steady::now();
	CHECK(kneel.Frame(false, 0x101, t) == Kneel::kNothing);
	CHECK(kneel.Frame(true, 0x101, t) == Kneel::kStart);
	kneel.Took(true);
	CHECK(kneel.Kneeling());
	CHECK(kneel.Frame(true, 0x101, t + 2s) == Kneel::kNothing);
	CHECK(kneel.Frame(false, 0x101, t + 3s) == Kneel::kStop);
	kneel.Took(true);
	CHECK(kneel.Frame(false, 0x101, t + 4s) == Kneel::kNothing);
	// Placed again while down: the new one kneels too.
	CHECK(kneel.Frame(true, 0x101, t + 5s) == Kneel::kStart);
	kneel.Took(true);
	CHECK(kneel.Frame(true, 0x202, t + 6s) == Kneel::kStart);
	kneel.Took(false);
	// Turned away: again each second, five times at most.
	std::size_t asked = 1;
	for (int i = 1; i <= 20; ++i) {
		const auto what = kneel.Frame(true, 0x202, t + 6s + i * 500ms);
		if (what == Kneel::kStart) {
			++asked;
			kneel.Took(false);
		}
	}
	CHECK(asked == DownRules::KNEEL_TRIES);
	CHECK(kneel.turnedAway == DownRules::KNEEL_TRIES);
	// Gone: nothing, and a stand-in placed later while its friend is up does nothing.
	CHECK(kneel.Frame(true, 0, t + 30s) == Kneel::kNothing);
	CHECK(!kneel.Kneeling());
	CHECK(kneel.Frame(false, 0x303, t + 31s) == Kneel::kNothing);
}
