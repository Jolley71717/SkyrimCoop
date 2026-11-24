#include "Test.h"

#include "FormTranslate.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "game/KnockRules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Knockdowns (docs/DESIGN.md, "Knockdowns"): the knocks on the wire, the server passing them on and asking runners, and
// the rules a game says its own by and plays, holds and keeps down a friend's by.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Knock = Protocol::Knock;
	using Side = Protocol::PairSide;
	using Phase = KnockRules::Phase;
	using KPhase = Protocol::KnockPhase;
	using Kind = Protocol::KnockKind;
	using KnockRules::Shown;
	using KnockRules::Verdict;
	using KnockRules::AskVerdict;
	using TimePoint = KnockRules::Clock::time_point;

	constexpr Protocol::NpcKey BANDIT{ 0x00012345, 0 };
	constexpr Protocol::NpcKey WOLF_OF_1{ 0xFF000801, 1 };  // a spawn, the key its spawner's game gave it
	constexpr Protocol::NpcKey DRAGON{ 0x0001CA05, 0 };
	constexpr Protocol::NpcKey HORSE{ 0x00097E1E, 0 };

	const TimePoint T0 = TimePoint{} + 1000s;

	Side PlayerSide(std::uint32_t a_player)
	{
		return Side{ .player = a_player };
	}

	Side NpcSide(Protocol::NpcKey a_npc)
	{
		return Side{ .npc = a_npc };
	}

	Knock Down(Side a_actor, Kind a_kind = Kind::kPush)
	{
		Knock knock{ .actor = a_actor, .sentMs = 1000, .phase = KPhase::kDown, .kind = a_kind, .x = 100.0f, .y = -200.0f, .z = 50.0f };
		if (a_kind == Kind::kPush) {
			knock.vx = 300.0f;
			knock.vy = 400.0f;
			knock.vz = 100.0f;
		}
		return knock;
	}

	Knock Up(Side a_actor)
	{
		return Knock{ .actor = a_actor, .sentMs = 4000, .phase = KPhase::kUp, .x = 400.0f, .y = 200.0f, .z = 40.0f };
	}

	Knock Ask(Protocol::NpcKey a_npc, Kind a_kind = Kind::kPush)
	{
		Knock knock = Down(NpcSide(a_npc), a_kind);
		knock.phase = KPhase::kAsk;
		return knock;
	}

	Knock AskUp(Protocol::NpcKey a_npc)
	{
		return KnockRules::AskUpOf(Ask(a_npc, Kind::kParalysis), 5000);
	}

	using Names = std::vector<std::string>;

	// The host's and a guest's load orders: B.esp is 03 in the host's game and 01 in the guest's; Extra.esp is the
	// host's alone.
	struct Edges
	{
		Names           host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm", "A.esp", "B.esp", "Extra.esp" }, {});
		Names           guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "B.esp", "Update.esm", "A.esp" }, {});
		Names           session;
		FormIds::Mapper hostEdge;
		FormIds::Mapper guestEdge;

		Edges()
		{
			FormIds::Extend(session, host);
			FormIds::Extend(session, guest);
			hostEdge = FormIds::Mapper(host, session);
			guestEdge = FormIds::Mapper(guest, session);
		}

		std::vector<std::uint8_t> HostToGuest(const std::vector<std::uint8_t>& a_packet) const
		{
			auto       out = FormTranslate::Translate(hostEdge, FormTranslate::Way::kToSession, a_packet);
			const auto wire = out.kind == FormTranslate::Result::Kind::kChanged ? out.packet : a_packet;
			auto       in = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, wire);
			return in.kind == FormTranslate::Result::Kind::kChanged ? in.packet : wire;
		}
	};

	std::vector<Protocol::Knocks> KnocksTo(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<Protocol::Knocks> out;
		for (auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kKnocks) {
				CHECK(sent.reliable);
				if (auto msg = Protocol::DecodeKnocks(sent.data)) {
					out.push_back(std::move(*msg));
				}
			}
		}
		return out;
	}

	std::vector<Knock> AllOf(const std::vector<Protocol::Knocks>& a_msgs)
	{
		std::vector<Knock> out;
		for (const auto& msg : a_msgs) {
			out.insert(out.end(), msg.knocks.begin(), msg.knocks.end());
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

	// Ada (player 1) runs the bandit, the dragon and the horse.
	void AdaRuns(Fixture& a_f, PeerId a_count)
	{
		a_f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { BANDIT, Protocol::ClaimWhy::kHave }, { DRAGON, Protocol::ClaimWhy::kHave },
			{ HORSE, Protocol::ClaimWhy::kHave } } }));
		REQUIRE(a_f.fake->Settle());
		for (PeerId peer = 1; peer <= a_count; ++peer) {
			(void)a_f.fake->Take(peer);
		}
	}

	// Facts of a down that plays: a friend's stand-in or a copy, alive, standing, near.
	KnockRules::Facts Playable(Shown a_shown = Shown::kCopy)
	{
		return KnockRules::Facts{ .shown = a_shown, .drift = 40.0f };
	}

	bool Near(float a_a, float a_b, float a_within = 0.01f)
	{
		return std::fabs(a_a - a_b) <= a_within;
	}
}

// ---- the message ----

TEST("knock protocol: knocks read back as they were written, as message 85 of protocol 32, and carry forms")
{
	static_assert(Protocol::VERSION >= 32);
	static_assert(static_cast<std::uint8_t>(MT::kKnocks) == 85);
	const Protocol::Knocks knocks{ .sender = 2,
		.knocks = { Down(PlayerSide(2)), Down(NpcSide(BANDIT), Kind::kParalysis), Down(NpcSide(WOLF_OF_1)), Up(NpcSide(DRAGON)), Ask(HORSE),
			Ask(BANDIT, Kind::kParalysis), AskUp(BANDIT) } };
	CHECK(Protocol::DecodeKnocks(Protocol::Encode(knocks)) == knocks);
	CHECK(Protocol::PeekType(Protocol::Encode(knocks)) == MT::kKnocks);
	CHECK(Protocol::DecodeKnocks(Protocol::Encode(Protocol::Knocks{})) == Protocol::Knocks{});
	CHECK(FormTranslate::CarriesForms(MT::kKnocks));
	CHECK(Protocol::Encode(knocks).size() == 1 + 4 + 1 + 7 * Protocol::KNOCK_BYTES);
	CHECK(Protocol::KnockToRunner(KPhase::kAsk));
	CHECK(Protocol::KnockToRunner(KPhase::kAskUp));
	CHECK(!Protocol::KnockToRunner(KPhase::kDown));
	CHECK(!Protocol::KnockToRunner(KPhase::kUp));
}

TEST("knock protocol: an ask-up is the ask's actor and place, its kind, phase ask-up, no launch; one of a player or one that moves is refused")
{
	const Knock ask = Ask(BANDIT, Kind::kParalysis);
	const Knock up = KnockRules::AskUpOf(ask, 7777);
	CHECK(up.phase == KPhase::kAskUp);
	CHECK(up.actor == ask.actor);
	CHECK(up.kind == Kind::kParalysis);
	CHECK(up.sentMs == 7777u);
	CHECK(up.x == ask.x);
	CHECK(KnockRules::Length({ up.vx, up.vy, up.vz }) == 0.0f);
	CHECK(Protocol::KnockHolds(up));
	// From a push's ask too (its launch dropped).
	CHECK(Protocol::KnockHolds(KnockRules::AskUpOf(Ask(BANDIT), 1)));

	const auto refused = [](Knock a_knock) { return !Protocol::DecodeKnocks(Protocol::Encode(Protocol::Knocks{ .knocks = { a_knock } })); };
	Knock ofPlayer = up;
	ofPlayer.actor = PlayerSide(2);
	CHECK(refused(ofPlayer));
	Knock moving = up;
	moving.vx = 10.0f;
	CHECK(refused(moving));
	CHECK(!refused(up));
}

TEST("knock protocol: the most fit; more, a side of nobody, an unknown phase or kind, an ask of a player, a bad place, a launch too fast or not finite, an up that moves, or a byte too many are refused")
{
	Protocol::Knocks full{};
	for (std::size_t i = 0; i < Protocol::MAX_KNOCKS; ++i) {
		full.knocks.push_back(Down(NpcSide(BANDIT)));
	}
	CHECK(Protocol::DecodeKnocks(Protocol::Encode(full)) == full);
	CHECK(Protocol::Encode(full).size() <= Protocol::MAX_KNOCKS_SIZE);

	// One more than MAX_KNOCKS: Encode writes the most, so the count is forged.
	auto bytes = Protocol::Encode(full);
	bytes[5] = static_cast<std::uint8_t>(Protocol::MAX_KNOCKS + 1);
	CHECK(!Protocol::DecodeKnocks(bytes));

	const auto refused = [](Knock a_knock) { return !Protocol::DecodeKnocks(Protocol::Encode(Protocol::Knocks{ .knocks = { a_knock } })); };
	CHECK(refused(Down(Side{})));
	CHECK(!refused(Down(Side{ .player = 2, .npc = BANDIT })));  // a player's side is written with no NPC: it reads back as the player's
	Knock phase = Down(PlayerSide(2));
	phase.phase = static_cast<KPhase>(0);
	CHECK(refused(phase));
	phase.phase = static_cast<KPhase>(5);
	CHECK(refused(phase));
	Knock kind = Down(PlayerSide(2));
	kind.kind = static_cast<Kind>(2);
	CHECK(refused(kind));
	Knock askPlayer = Down(PlayerSide(2));
	askPlayer.phase = KPhase::kAsk;
	CHECK(refused(askPlayer));
	Knock place = Down(PlayerSide(2));
	place.y = std::numeric_limits<float>::quiet_NaN();
	CHECK(refused(place));
	place.y = 1e9f;
	CHECK(refused(place));
	Knock fast = Down(PlayerSide(2));
	fast.vx = Protocol::MAX_KNOCK_SPEED;
	fast.vy = 200.0f;
	CHECK(refused(fast));
	fast.vx = std::numeric_limits<float>::infinity();
	CHECK(refused(fast));
	Knock movingUp = Up(PlayerSide(2));
	movingUp.vz = 1.0f;
	CHECK(refused(movingUp));
	Knock limit = Down(PlayerSide(2));
	limit.vx = Protocol::MAX_KNOCK_SPEED;
	limit.vy = 0.0f;
	limit.vz = 0.0f;
	CHECK(!refused(limit));  // exactly the most is fine

	auto extra = Protocol::Encode(Protocol::Knocks{ .knocks = { Down(PlayerSide(2)) } });
	extra.push_back(0);
	CHECK(!Protocol::DecodeKnocks(extra));
	auto shortOne = Protocol::Encode(Protocol::Knocks{ .knocks = { Down(PlayerSide(2)) } });
	shortOne.pop_back();
	CHECK(!Protocol::DecodeKnocks(shortOne));
}

TEST("knock form translate: a knock of an NPC the reader lacks is left out; a player's, a spawn's and one of a plugin both have are kept, renumbered")
{
	const Edges            e;
	const Protocol::NpcKey ofExtra{ 0x04000ABC, 0 };  // Extra.esp's: the guest lacks it
	const Protocol::NpcKey ofB{ 0x03000ABC, 0 };      // B.esp's reference in the host's numbering
	const Protocol::NpcKey inGuest{ 0x01000ABC, 0 };

	const auto knocks = Protocol::DecodeKnocks(e.HostToGuest(Protocol::Encode(
		Protocol::Knocks{ .sender = 1, .knocks = { Down(NpcSide(ofB)), Down(NpcSide(ofExtra)), Up(PlayerSide(1)), Down(NpcSide(WOLF_OF_1)), Ask(ofExtra) } })));
	REQUIRE(knocks);
	CHECK(knocks->sender == 1u);
	REQUIRE(knocks->knocks.size() == 3u);
	CHECK((knocks->knocks[0].actor.npc == inGuest));
	CHECK(knocks->knocks[1].actor.player == 1u);
	CHECK(knocks->knocks[1].phase == KPhase::kUp);
	CHECK((knocks->knocks[2].actor.npc == WOLF_OF_1));
}

// ---- the server ----

TEST("knock server: a player's own knocks and those of the NPCs its game runs go to the others with the sender named; any other is dropped")
{
	Fixture f;
	AllIn(f, 3);
	AdaRuns(f, 3);

	// Bo says his own down, Ada's, the bandit's (Ada runs it) and an NPC no one runs: only the first goes.
	const Knock bos = Down(PlayerSide(2));
	f.fake->Deliver(2, Protocol::Encode(Protocol::Knocks{ .sender = 99, .knocks = { bos, Down(PlayerSide(1)), Down(NpcSide(BANDIT)), Down(NpcSide(WOLF_OF_1)) } }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 3 } }) {
		const auto got = KnocksTo(f, peer);
		REQUIRE(got.size() == 1u);
		CHECK(got[0].sender == 2u);
		CHECK(got[0].knocks == std::vector<Knock>({ bos }));
	}
	CHECK(KnocksTo(f, 2).empty());  // not back to its sender

	// Ada's: her player's up, the bandit's paralysis, the dragon's down, the horse's up: all go, to both.
	const std::vector<Knock> adas{ Up(PlayerSide(1)), Down(NpcSide(BANDIT), Kind::kParalysis), Down(NpcSide(DRAGON)), Up(NpcSide(HORSE)) };
	f.fake->Deliver(1, Protocol::Encode(Protocol::Knocks{ .knocks = adas }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 2 }, PeerId{ 3 } }) {
		const auto got = KnocksTo(f, peer);
		REQUIRE(got.size() == 1u);
		CHECK(got[0].sender == 1u);
		CHECK(got[0].knocks == adas);
	}
	CHECK(KnocksTo(f, 1).empty());
}

TEST("knock server: an ask goes to the NPC's runner alone, the asker named; one of an NPC no one runs, or the asker's own, is dropped")
{
	Fixture f;
	AllIn(f, 3);
	AdaRuns(f, 3);

	// Bo asks for the bandit and the dragon (Ada's), the wolf (no one's), and says his own down in the same message.
	const Knock bos = Down(PlayerSide(2));
	f.fake->Deliver(2, Protocol::Encode(Protocol::Knocks{ .knocks = { Ask(BANDIT), Ask(WOLF_OF_1), bos, Ask(DRAGON) } }));
	REQUIRE(f.fake->Settle());
	const auto toAda = AllOf(KnocksTo(f, 1));
	CHECK(toAda == std::vector<Knock>({ bos, Ask(BANDIT), Ask(DRAGON) }));
	const auto toCy = AllOf(KnocksTo(f, 3));
	CHECK(toCy == std::vector<Knock>({ bos }));  // the asks are Ada's alone
	CHECK(KnocksTo(f, 2).empty());

	// Each message names its sender: the asks name Bo.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Knocks{ .knocks = { Ask(HORSE) } }));
	REQUIRE(f.fake->Settle());
	const auto asked = KnocksTo(f, 1);
	REQUIRE(asked.size() == 1u);
	CHECK(asked[0].sender == 2u);
	CHECK(asked[0].knocks == std::vector<Knock>({ Ask(HORSE) }));
	CHECK(KnocksTo(f, 3).empty());

	// Ada asking for her own bandit: dropped (her game knocks it itself).
	f.fake->Deliver(1, Protocol::Encode(Protocol::Knocks{ .knocks = { Ask(BANDIT) } }));
	REQUIRE(f.fake->Settle());
	CHECK(KnocksTo(f, 1).empty());
	CHECK(KnocksTo(f, 2).empty());
	CHECK(KnocksTo(f, 3).empty());
}

TEST("knock server: a paralysis asked and its ask-up go to the NPC's runner alone, the asker named; Ada's own ask-up is dropped")
{
	Fixture f;
	AllIn(f, 3);
	AdaRuns(f, 3);

	const std::vector<Knock> bos{ Ask(BANDIT, Kind::kParalysis), AskUp(BANDIT), AskUp(WOLF_OF_1) };
	f.fake->Deliver(2, Protocol::Encode(Protocol::Knocks{ .knocks = bos }));
	REQUIRE(f.fake->Settle());
	const auto toAda = KnocksTo(f, 1);
	REQUIRE(toAda.size() == 1u);
	CHECK(toAda[0].sender == 2u);
	CHECK(toAda[0].knocks == std::vector<Knock>({ Ask(BANDIT, Kind::kParalysis), AskUp(BANDIT) }));  // the wolf is no one's
	CHECK(KnocksTo(f, 3).empty());
	CHECK(KnocksTo(f, 2).empty());

	f.fake->Deliver(1, Protocol::Encode(Protocol::Knocks{ .knocks = { AskUp(BANDIT) } }));
	REQUIRE(f.fake->Settle());
	CHECK(KnocksTo(f, 1).empty());
	CHECK(KnocksTo(f, 2).empty());
	CHECK(KnocksTo(f, 3).empty());
}

TEST("knock server: an ask-up is never dropped, past the budget too (the runner's NPC is held down until it comes)")
{
	Fixture f;
	AllIn(f, 2);
	AdaRuns(f, 2);
	Protocol::Knocks asks{};
	Protocol::Knocks ups{};
	for (std::size_t i = 0; i < Protocol::MAX_KNOCKS; ++i) {
		asks.knocks.push_back(Ask(BANDIT, Kind::kParalysis));
		ups.knocks.push_back(AskUp(BANDIT));
	}
	for (int i = 0; i < 4; ++i) {
		f.fake->Deliver(2, Protocol::Encode(asks));
	}
	REQUIRE(f.fake->Settle());
	const auto passed = AllOf(KnocksTo(f, 1)).size();
	CHECK(passed >= 64u);
	CHECK(passed < 4u * Protocol::MAX_KNOCKS);  // the budget is spent

	for (int i = 0; i < 4; ++i) {
		f.fake->Deliver(2, Protocol::Encode(ups));
	}
	REQUIRE(f.fake->Settle());
	CHECK(AllOf(KnocksTo(f, 1)).size() == 4u * Protocol::MAX_KNOCKS);
}

TEST("knock server: past a burst of 64 a player's downs are dropped, and the log says so once; an up is never dropped")
{
	Fixture f;
	AllIn(f, 2);
	Protocol::Knocks downs{};
	Protocol::Knocks ups{};
	for (std::size_t i = 0; i < Protocol::MAX_KNOCKS; ++i) {
		downs.knocks.push_back(Down(PlayerSide(1)));
		ups.knocks.push_back(Up(PlayerSide(1)));
	}
	for (int i = 0; i < 4; ++i) {
		f.fake->Deliver(1, Protocol::Encode(downs));
	}
	REQUIRE(f.fake->Settle());
	const auto passed = AllOf(KnocksTo(f, 2)).size();
	CHECK(passed >= 64u);
	CHECK(passed < 4u * Protocol::MAX_KNOCKS);
	CHECK(f.LoggedSoon("sends knockdowns faster than they are passed on"));

	for (int i = 0; i < 4; ++i) {
		f.fake->Deliver(1, Protocol::Encode(ups));
	}
	REQUIRE(f.fake->Settle());
	CHECK(AllOf(KnocksTo(f, 2)).size() == 4u * Protocol::MAX_KNOCKS);

	for (int i = 0; i < 4; ++i) {
		f.fake->Deliver(1, Protocol::Encode(downs));
	}
	REQUIRE(f.fake->Settle());
	const auto lines = f.LogLines();
	CHECK(std::ranges::count_if(lines, [](const std::string& a_line) { return a_line.find("sends knockdowns faster") != std::string::npos; }) == 1);
}

// ---- the rules: the knock state ----

TEST("knock rules: the knock state: normal is standing, get-up getting up, every other state down; out and down are resting")
{
	CHECK(KnockRules::PhaseOf(0) == Phase::kStanding);
	for (const std::uint32_t state : { 1u, 2u, 3u, 4u, 5u, 7u }) {
		CHECK(KnockRules::PhaseOf(state) == Phase::kDown);
	}
	CHECK(KnockRules::PhaseOf(6) == Phase::kGettingUp);
	CHECK(KnockRules::Resting(3));
	CHECK(KnockRules::Resting(7));
	for (const std::uint32_t state : { 0u, 1u, 2u, 4u, 5u, 6u }) {
		CHECK(!KnockRules::Resting(state));
	}
	CHECK(KnockRules::NameOf(Phase::kGettingUp) == "gettingUp");
	CHECK(KnockRules::NameOf(Kind::kParalysis) == "paralysis");
}

// ---- the rules: saying ours ----

TEST("knock rules: watch: a push is said down SAMPLE after it began, with where it began and its launch; up as it gets up, with where it rests")
{
	KnockRules::Watch watch;
	CHECK(watch.Step(7, Phase::kStanding, { 0, 0, 0 }, false, T0).empty());
	CHECK(watch.Size() == 0u);  // a standing actor is not kept
	CHECK(watch.Step(7, Phase::kDown, { 0, 0, 0 }, false, T0).empty());
	CHECK(watch.Down(7));
	CHECK(watch.Step(7, Phase::kDown, { 20, 0, 5 }, false, T0 + 50ms).empty());
	const auto down = watch.Step(7, Phase::kDown, { 40, 30, 10 }, false, T0 + 100ms);
	REQUIRE(down.size() == 1u);
	CHECK(down[0].phase == KPhase::kDown);
	CHECK(down[0].kind == Kind::kPush);
	CHECK(down[0].at.x == 0.0f);
	CHECK(down[0].when == T0);
	CHECK(Near(down[0].launch.x, 400.0f, 1.0f));
	CHECK(Near(down[0].launch.y, 300.0f, 1.0f));
	CHECK(Near(down[0].launch.z, 100.0f, 1.0f));
	CHECK(watch.Step(7, Phase::kDown, { 200, 150, 0 }, false, T0 + 2s).empty());  // said once
	const auto up = watch.Step(7, Phase::kGettingUp, { 210, 160, 0 }, false, T0 + 4s);
	REQUIRE(up.size() == 1u);
	CHECK(up[0].phase == KPhase::kUp);
	CHECK(up[0].at.x == 210.0f);
	CHECK(up[0].when == T0 + 4s);
	CHECK(KnockRules::Length(up[0].launch) == 0.0f);
	CHECK(watch.Step(7, Phase::kGettingUp, { 210, 160, 0 }, false, T0 + 5s).empty());
	CHECK(watch.Step(7, Phase::kStanding, { 210, 160, 0 }, false, T0 + 6s).empty());
	CHECK(watch.Size() == 0u);
}

TEST("knock rules: watch: a knock over within SAMPLE is said down and up at once; one that stands with no get-up is said up; down again while getting up is a new knock")
{
	KnockRules::Watch watch;
	(void)watch.Step(1, Phase::kDown, { 0, 0, 0 }, false, T0);
	const auto both = watch.Step(1, Phase::kStanding, { 5, 0, 0 }, false, T0 + 50ms);
	REQUIRE(both.size() == 2u);
	CHECK(both[0].phase == KPhase::kDown);
	CHECK(both[1].phase == KPhase::kUp);
	CHECK(watch.Size() == 0u);

	(void)watch.Step(2, Phase::kDown, { 0, 0, 0 }, false, T0);
	CHECK(watch.Step(2, Phase::kDown, { 0, 0, 0 }, false, T0 + 200ms).size() == 1u);
	const auto stood = watch.Step(2, Phase::kStanding, { 1, 1, 0 }, false, T0 + 3s);
	REQUIRE(stood.size() == 1u);
	CHECK(stood[0].phase == KPhase::kUp);

	(void)watch.Step(3, Phase::kDown, { 0, 0, 0 }, false, T0);
	(void)watch.Step(3, Phase::kDown, { 0, 0, 0 }, false, T0 + 200ms);
	CHECK(watch.Step(3, Phase::kGettingUp, { 0, 0, 0 }, false, T0 + 3s).size() == 1u);
	CHECK(watch.Step(3, Phase::kDown, { 50, 0, 0 }, false, T0 + 3200ms).empty());  // knocked again as it got up
	const auto again = watch.Step(3, Phase::kDown, { 50, 0, 0 }, false, T0 + 3300ms);
	REQUIRE(again.size() == 1u);
	CHECK(again[0].at.x == 50.0f);
	CHECK(again[0].when == T0 + 3200ms);
}

TEST("knock rules: watch: a paralysis (its value up at any time while down) is said with no launch; a launch too fast is cut to the most; forgotten actors say nothing more")
{
	KnockRules::Watch watch;
	(void)watch.Step(4, Phase::kDown, { 0, 0, 0 }, false, T0);
	const auto para = watch.Step(4, Phase::kDown, { 30, 0, 0 }, true, T0 + 100ms);
	REQUIRE(para.size() == 1u);
	CHECK(para[0].kind == Kind::kParalysis);
	CHECK(KnockRules::Length(para[0].launch) == 0.0f);
	const auto up = watch.Step(4, Phase::kStanding, { 30, 0, 0 }, false, T0 + 10s);
	REQUIRE(up.size() == 1u);
	CHECK(up[0].kind == Kind::kParalysis);

	(void)watch.Step(5, Phase::kDown, { 0, 0, 0 }, false, T0);
	const auto fast = watch.Step(5, Phase::kDown, { 5000, 0, 0 }, false, T0 + 100ms);  // 50000 u/s
	REQUIRE(fast.size() == 1u);
	CHECK(KnockRules::Length(fast[0].launch) <= Protocol::MAX_KNOCK_SPEED);
	CHECK(KnockRules::Length(fast[0].launch) > Protocol::MAX_KNOCK_SPEED * 0.99f);
	CHECK(Protocol::KnockHolds(KnockRules::ToWire(PlayerSide(1), fast[0], 0)));

	(void)watch.Step(6, Phase::kDown, { 0, 0, 0 }, false, T0);
	(void)watch.Step(8, Phase::kDown, { 0, 0, 0 }, false, T0);
	watch.ForgetIf([](std::uint32_t a_id) { return a_id == 6; });
	CHECK(!watch.Down(6));
	CHECK(watch.Down(8));
	watch.Forget(8);
	CHECK(watch.Size() == 1u);  // 5
	watch.Clear();
	CHECK(watch.Size() == 0u);
}

TEST("knock rules: to the wire: a down carries its launch, an up none, an ask the down's launch as an ask; every one holds")
{
	const KnockRules::Said down{ .phase = KPhase::kDown, .kind = Kind::kPush, .at = { 1, 2, 3 }, .launch = { 100, 0, 50 } };
	const auto             wire = KnockRules::ToWire(NpcSide(BANDIT), down, 77);
	CHECK(wire.phase == KPhase::kDown);
	CHECK(wire.sentMs == 77u);
	CHECK(wire.x == 1.0f);
	CHECK(wire.vx == 100.0f);
	CHECK(wire.vz == 50.0f);
	CHECK(Protocol::KnockHolds(wire));
	const auto ask = KnockRules::ToWire(NpcSide(BANDIT), down, 77, true);
	CHECK(ask.phase == KPhase::kAsk);
	CHECK(ask.vx == 100.0f);
	CHECK(Protocol::KnockHolds(ask));
	const KnockRules::Said up{ .phase = KPhase::kUp, .at = { 4, 5, 6 }, .launch = { 9, 9, 9 } };
	const auto             upWire = KnockRules::ToWire(PlayerSide(3), up, 5);
	CHECK(upWire.phase == KPhase::kUp);
	CHECK(upWire.vx == 0.0f);
	CHECK(Protocol::KnockHolds(upWire));

	std::vector<Knock> many(Protocol::MAX_KNOCKS * 2 + 3, wire);
	const auto         batches = KnockRules::Batch(many);
	REQUIRE(batches.size() == 3u);
	CHECK(batches[0].knocks.size() == Protocol::MAX_KNOCKS);
	CHECK(batches[2].knocks.size() == 3u);
	CHECK(KnockRules::Batch({}).empty());
}

// ---- the rules: knocking an actor here ----

TEST("knock rules: push: from BEHIND the launch's way, as hard as its speed over this game's speed per magnitude, within the least and the most; a slow or straight-up launch falls in place")
{
	const auto push = KnockRules::PushFor({ 100, 100, 0 }, { 600, 0, 200 }, 120.0f);
	CHECK(!push.inPlace);
	CHECK(Near(push.from.x, 100.0f - KnockRules::BEHIND));
	CHECK(Near(push.from.y, 100.0f));
	CHECK(Near(push.from.z, 0.0f));
	CHECK(Near(push.magnitude, std::sqrt(600.0f * 600.0f + 200.0f * 200.0f) / 120.0f, 0.01f));

	const auto diagonal = KnockRules::PushFor({ 0, 0, 0 }, { -300, -400, 0 }, 100.0f);
	CHECK(Near(diagonal.from.x, 60.0f));  // 3/5 of BEHIND the other way
	CHECK(Near(diagonal.from.y, 80.0f));
	CHECK(Near(diagonal.magnitude, 5.0f));

	CHECK(Near(KnockRules::PushFor({ 0, 0, 0 }, { 7000, 0, 0 }, 10.0f).magnitude, KnockRules::MAX_MAGNITUDE));
	CHECK(Near(KnockRules::PushFor({ 0, 0, 0 }, { 40, 0, 0 }, 1000.0f).magnitude, KnockRules::MIN_MAGNITUDE));
	// A per-magnitude out of reason is clamped: never a division by nothing.
	CHECK(std::isfinite(KnockRules::PushFor({ 0, 0, 0 }, { 500, 0, 0 }, 0.0f).magnitude));
	CHECK(Near(KnockRules::PushFor({ 0, 0, 0 }, { 500, 0, 0 }, 0.0f).magnitude, 500.0f / KnockRules::MIN_SPEED_PER_MAGNITUDE));

	for (const auto launch : { KnockRules::Vec{ 10, 0, 0 }, KnockRules::Vec{ 0, 0, 800 }, KnockRules::Vec{ 0, 0, 0 },
			 KnockRules::Vec{ std::numeric_limits<float>::quiet_NaN(), 0, 0 } }) {
		const auto fall = KnockRules::PushFor({ 5, 6, 7 }, launch, 120.0f);
		CHECK(fall.inPlace);
		CHECK(fall.magnitude == KnockRules::MIN_MAGNITUDE);
		CHECK(Near(fall.from.x, 5.0f));
		CHECK(Near(fall.from.z, 7.0f - KnockRules::BEHIND));
	}
}

TEST("knock rules: scale: the median of the last KEEP replays' speed per magnitude, the default until one; a replay that hardly moved teaches nothing")
{
	KnockRules::Scale scale;
	CHECK(scale.SpeedPerMagnitude() == KnockRules::DEFAULT_SPEED_PER_MAGNITUDE);
	scale.Note(5.0f, 10.0f);  // hardly moved
	scale.Note(0.0f, 500.0f);
	scale.Note(5.0f, std::numeric_limits<float>::quiet_NaN());
	CHECK(scale.ignored == 3u);
	CHECK(scale.SpeedPerMagnitude() == KnockRules::DEFAULT_SPEED_PER_MAGNITUDE);
	scale.Note(10.0f, 1000.0f);  // 100
	CHECK(scale.SpeedPerMagnitude() == 100.0f);
	scale.Note(10.0f, 3000.0f);  // 300
	CHECK(scale.SpeedPerMagnitude() == 200.0f);
	scale.Note(1.0f, 1.0e6f);  // clamped to the most
	CHECK(scale.SpeedPerMagnitude() == 300.0f);
	for (int i = 0; i < 8; ++i) {
		scale.Note(2.0f, 100.0f);  // 50
	}
	CHECK(scale.SpeedPerMagnitude() == 50.0f);  // the older ones are gone
	CHECK(scale.learnt == 11u);
}

// ---- the rules: a friend's knocks ----

TEST("knock rules: inbox: a friend's knock is due when its actor is drawn at its moment, in order; one with no moment goes now; a stale down is dropped, an up never")
{
	KnockRules::Inbox inbox;
	inbox.Push(Down(NpcSide(BANDIT)), 1, T0 + 300ms, T0);
	inbox.Push(Down(PlayerSide(1)), 1, T0 + 100ms, T0);
	inbox.Push(Up(NpcSide(DRAGON)), 2, std::nullopt, T0);
	CHECK(inbox.Size() == 3u);
	auto due = inbox.Take(T0);
	REQUIRE(due.size() == 1u);  // the one with no moment
	CHECK(due[0].sender == 2u);
	due = inbox.Take(T0 + 300ms);
	REQUIRE(due.size() == 2u);
	CHECK(due[0].knock.actor.player == 1u);
	CHECK((due[1].knock.actor.npc == BANDIT));

	// A moment far ahead (a clock read wrong) is held to STALE.
	inbox.Push(Down(PlayerSide(1)), 1, T0 + 1h, T0);
	CHECK(inbox.Take(T0 + KnockRules::STALE).size() == 1u);

	// Stale: the down is dropped as late, the up is still played.
	inbox.Push(Down(NpcSide(BANDIT)), 1, T0, T0);
	inbox.Push(Up(NpcSide(BANDIT)), 1, T0, T0);
	due = inbox.Take(T0 + KnockRules::STALE + 1s);
	REQUIRE(due.size() == 1u);
	CHECK(due[0].knock.phase == KPhase::kUp);
	CHECK(inbox.late == 1u);

	// Full: the oldest down goes first, the ups stay.
	inbox.Push(Up(NpcSide(DRAGON)), 3, T0, T0);
	for (std::size_t i = 0; i < KnockRules::Inbox::MAX; ++i) {
		inbox.Push(Down(NpcSide(BANDIT)), 1, T0 + 1ms * static_cast<int>(i + 1), T0);
	}
	CHECK(inbox.Size() == KnockRules::Inbox::MAX);
	CHECK(inbox.dropped == 1u);
	due = inbox.Take(T0);
	REQUIRE(due.size() == 1u);
	CHECK(due[0].knock.phase == KPhase::kUp);

	inbox.Forget(1);
	CHECK(inbox.Size() == 0u);
	inbox.Push(Up(PlayerSide(1)), 1, T0, T0);
	inbox.Clear();
	CHECK(inbox.Size() == 0u);
}

TEST("knock rules: judge: a friend's down plays on a stand-in or a copy, alive, standing, near; never on ours; one knocked here already joins; refusals counted and named")
{
	CHECK(KnockRules::Judge(Playable()) == Verdict::kPlay);
	CHECK(KnockRules::Judge(Playable(Shown::kStandIn)) == Verdict::kPlay);
	CHECK(KnockRules::Judge(Playable(Shown::kNone)) == Verdict::kNoActor);
	CHECK(KnockRules::Judge(Playable(Shown::kOurPlayer)) == Verdict::kOurs);
	CHECK(KnockRules::Judge(Playable(Shown::kRunHere)) == Verdict::kOurs);
	auto facts = Playable();
	facts.alive = false;
	facts.knockedHere = true;
	CHECK(KnockRules::Judge(facts) == Verdict::kDead);  // a body is its death's
	facts = Playable();
	facts.knockedHere = true;
	facts.busy = true;
	CHECK(KnockRules::Judge(facts) == Verdict::kAlready);
	facts = Playable();
	facts.busy = true;
	CHECK(KnockRules::Judge(facts) == Verdict::kBusy);
	facts = Playable();
	facts.drift = KnockRules::REACH + 1.0f;
	CHECK(KnockRules::Judge(facts) == Verdict::kFar);
	facts.drift = std::numeric_limits<float>::quiet_NaN();
	CHECK(KnockRules::Judge(facts) == Verdict::kFar);
	facts = Playable();
	facts.full = true;
	CHECK(KnockRules::Judge(facts) == Verdict::kFull);

	KnockRules::VerdictCounts counts;
	CHECK(counts.Others() == "none");
	counts.Add(Verdict::kPlay);
	counts.Add(Verdict::kFar);
	counts.Add(Verdict::kDead);
	counts.Add(Verdict::kDead);
	CHECK(counts.Others() == "dead:2,far:1");
	CHECK(counts.Of(Verdict::kPlay) == 1u);
	CHECK(KnockRules::NameOf(Verdict::kAlready) == "already");
}

TEST("knock rules: ask: a paralysis asked of the NPC's runner is applied when it runs it, alive, free and near, down already too; not with Asked full")
{
	const KnockRules::AskFacts ok{ .ours = true, .drift = 30.0f, .kind = Kind::kParalysis };
	CHECK(KnockRules::JudgeAsk(ok) == AskVerdict::kApply);
	auto facts = ok;
	facts.down = true;  // a push's knock: the paralysis keeps it down for as long as the asker's copy is paralysed
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kApply);
	facts = ok;
	facts.ours = false;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kNotOurs);
	facts = ok;
	facts.alive = false;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kDead);
	facts = ok;
	facts.busy = true;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kBusy);
	facts = ok;
	facts.drift = KnockRules::REACH * 2.0f;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kFar);
	facts = ok;
	facts.full = true;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kFull);
	facts.kind = Kind::kPush;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kApply);  // a push keeps nothing: Asked's room is no matter
	CHECK(KnockRules::NameOf(AskVerdict::kFull) == "full");
}

TEST("knock rules: ask: a push asked of the NPC's runner is applied when it runs it, alive, standing, free and near")
{
	const KnockRules::AskFacts ok{ .ours = true, .drift = 30.0f };
	CHECK(KnockRules::JudgeAsk(ok) == AskVerdict::kApply);
	auto facts = ok;
	facts.ours = false;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kNotOurs);
	facts = ok;
	facts.alive = false;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kDead);
	facts = ok;
	facts.down = true;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kDown);
	facts = ok;
	facts.busy = true;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kBusy);
	facts = ok;
	facts.drift = KnockRules::REACH * 2.0f;
	CHECK(KnockRules::JudgeAsk(facts) == AskVerdict::kFar);

	KnockRules::AskCounts counts;
	counts.Add(AskVerdict::kApply);
	counts.Add(AskVerdict::kDown);
	CHECK(counts.Others() == "down:1");
}

// ---- the rules: a paralysis asked of the runner ----

TEST("knock rules: asked: the first ask raises, more askers share it; the last asker's ask-up lets it up; a stranger's ask-up does nothing")
{
	using Add = KnockRules::Asked::Add;
	KnockRules::Asked asked;
	CHECK(asked.Ask(BANDIT, 2, T0) == Add::kFirst);
	CHECK(asked.Has(BANDIT));
	CHECK(asked.Ask(BANDIT, 2, T0 + 1s) == Add::kMore);  // the same asker again
	CHECK(asked.Askers(BANDIT) == 1u);
	CHECK(asked.Ask(BANDIT, 3, T0 + 2s) == Add::kMore);
	CHECK(asked.Askers(BANDIT) == 2u);
	CHECK(!asked.Up(BANDIT, 4));     // never asked
	CHECK(!asked.Up(DRAGON, 2));     // not asked of it
	CHECK(!asked.Up(BANDIT, 2));     // player 3's copy is still paralysed
	CHECK(asked.Has(BANDIT));
	CHECK(!asked.Up(BANDIT, 2));     // said twice: once counts
	CHECK(asked.Up(BANDIT, 3));      // the last: let up
	CHECK(!asked.Has(BANDIT));
	CHECK(asked.Size() == 0u);
}

TEST("knock rules: asked: a leaving asker lets up what it alone asked; MAX_DOWN after the last ask it is let up with no ask-up; MAX NPCs at most")
{
	using Add = KnockRules::Asked::Add;
	KnockRules::Asked asked;
	(void)asked.Ask(BANDIT, 2, T0);
	(void)asked.Ask(DRAGON, 2, T0);
	(void)asked.Ask(DRAGON, 3, T0);
	const auto left = asked.Left(2);
	CHECK(left == std::vector<Protocol::NpcKey>({ BANDIT }));
	CHECK(asked.Has(DRAGON));
	CHECK(asked.Askers(DRAGON) == 1u);

	(void)asked.Ask(HORSE, 3, T0 + 20s);  // a newer ask
	CHECK(asked.Expired(T0 + KnockRules::MAX_DOWN - 1ms).empty());
	CHECK(asked.Expired(T0 + KnockRules::MAX_DOWN) == std::vector<Protocol::NpcKey>({ DRAGON }));
	CHECK(asked.Has(HORSE));
	(void)asked.Ask(HORSE, 3, T0 + 50s);  // asked again: its time starts again
	CHECK(asked.Expired(T0 + 20s + KnockRules::MAX_DOWN).empty());
	CHECK(asked.Forget(HORSE));
	CHECK(!asked.Forget(HORSE));

	for (std::uint32_t i = 0; i < KnockRules::Asked::MAX; ++i) {
		CHECK(asked.Ask(Protocol::NpcKey{ 0x00010000 + i, 0 }, 2, T0) == Add::kFirst);
	}
	CHECK(asked.Ask(BANDIT, 2, T0) == Add::kFull);
	CHECK(asked.Ask(Protocol::NpcKey{ 0x00010000, 0 }, 3, T0) == Add::kMore);  // one asked already still takes askers
	asked.Clear();
	CHECK(asked.Size() == 0u);
}

TEST("knock rules: asked: the copy's own paralysis is over when its value less what this game keeps it down by is none")
{
	CHECK(!KnockRules::OwnParalysisOver(1.0f, false));                    // the effect's, asked: not over
	CHECK(KnockRules::OwnParalysisOver(0.0f, false));                     // the effect ended
	CHECK(KnockRules::OwnParalysisOver(KnockRules::KEPT, true));          // only ours, kept while joined: over
	CHECK(!KnockRules::OwnParalysisOver(KnockRules::KEPT + 1.0f, true));  // the effect's and ours
	CHECK(KnockRules::OwnParalysisOver(-1.0f, false));                    // a value pulled below none
}

// ---- the rules: let go while down ----

TEST("knock rules: quiet: a hold given up while down is never held again as this world's, nor asked, until it has stood; then a new knock is")
{
	using KnockRules::Hold;
	using KnockRules::HoldStep;
	KnockRules::Quiet quiet;
	constexpr std::uint32_t COPY = 0xFF000801;
	// A friend's push played on the copy at T0, its up at T0 + 4 s; the copy is still lying UP_WAIT later: given up.
	const auto upAt = T0 + 4s;
	const auto gaveUpAt = upAt + KnockRules::UP_WAIT;
	REQUIRE(HoldStep(T0, gaveUpAt, Phase::kDown, true, upAt, std::nullopt) == Hold::kGaveUp);
	quiet.LetGo(COPY, true, gaveUpAt);
	// The same tick and for as long as it lies (and as it gets up): not this world's knock (no native hold, no ask).
	CHECK(!quiet.Native(COPY, Phase::kDown, gaveUpAt));
	CHECK(!quiet.Native(COPY, Phase::kDown, gaveUpAt + 10s));
	CHECK(!quiet.Native(COPY, Phase::kGettingUp, gaveUpAt + 11s));
	CHECK(!quiet.Native(COPY, Phase::kDown, gaveUpAt + 60s));  // however long (a stand-in's MAX_DOWN never cycles)
	CHECK(quiet.skipped == 4u);
	// It stands: no longer quiet; a knock after that is this world's.
	CHECK(quiet.Native(COPY, Phase::kStanding, gaveUpAt + 61s));
	CHECK(!quiet.Has(COPY));
	CHECK(quiet.Native(COPY, Phase::kDown, gaveUpAt + 62s));
	// Another actor was never quiet.
	CHECK(quiet.Native(0xFF000802, Phase::kDown, T0));
}

TEST("knock rules: quiet: a knock that never took and then takes within LATE_TAKE is that knock (quiet until it stands); after LATE_TAKE standing it is forgotten")
{
	using KnockRules::Hold;
	using KnockRules::HoldStep;
	KnockRules::Quiet quiet;
	constexpr std::uint32_t LATE = 0xFF000803;
	constexpr std::uint32_t NEVER = 0xFF000804;
	const auto noTakeAt = T0 + KnockRules::TAKE_WITHIN;
	REQUIRE(HoldStep(T0, noTakeAt, Phase::kStanding, false, std::nullopt, T0) == Hold::kNoTake);
	quiet.LetGo(LATE, false, noTakeAt);
	quiet.LetGo(NEVER, false, noTakeAt);
	CHECK(quiet.Native(LATE, Phase::kStanding, noTakeAt + 500ms));  // standing: nothing to hold, still quiet
	CHECK(quiet.Has(LATE));
	CHECK(!quiet.Native(LATE, Phase::kDown, noTakeAt + 1s));        // the late take
	CHECK(!quiet.Native(LATE, Phase::kDown, noTakeAt + 5s));        // quiet while it lies, past LATE_TAKE too
	CHECK(quiet.Native(LATE, Phase::kStanding, noTakeAt + 6s));
	CHECK(!quiet.Has(LATE));
	// Never took: forgotten once seen standing LATE_TAKE after.
	CHECK(quiet.Native(NEVER, Phase::kStanding, noTakeAt + KnockRules::LATE_TAKE));
	CHECK(!quiet.Has(NEVER));
	CHECK(quiet.Native(NEVER, Phase::kDown, noTakeAt + KnockRules::LATE_TAKE + 1s));  // a knock of this world's now
}

TEST("knock rules: quiet: at most MAX actors, the oldest forgotten first; cleared with the world")
{
	KnockRules::Quiet quiet;
	for (std::uint32_t i = 0; i < KnockRules::Quiet::MAX; ++i) {
		quiet.LetGo(i + 1, true, T0 + std::chrono::milliseconds(i));
	}
	CHECK(quiet.Size() == KnockRules::Quiet::MAX);
	quiet.LetGo(0xFFFF, true, T0 + 1h);
	CHECK(quiet.Size() == KnockRules::Quiet::MAX);
	CHECK(!quiet.Has(1));  // the oldest
	CHECK(quiet.Has(2));
	CHECK(quiet.Has(0xFFFF));
	quiet.LetGo(2, true, T0 + 2h);  // again: no room taken
	CHECK(quiet.Size() == KnockRules::Quiet::MAX);
	quiet.Forget(2);
	CHECK(!quiet.Has(2));
	quiet.Clear();
	CHECK(quiet.Size() == 0u);
}

// ---- the rules: holding and keeping down ----

TEST("knock rules: hold: held while knocked; let go STAND_GRACE after it stands; never taken by TAKE_WITHIN; still down UP_WAIT after the up or MAX_DOWN with none")
{
	using KnockRules::Hold;
	using KnockRules::HoldStep;
	// Waiting for the engine's knock to take.
	CHECK(HoldStep(T0, T0 + 100ms, Phase::kStanding, false, std::nullopt, T0) == Hold::kHold);
	CHECK(HoldStep(T0, T0 + KnockRules::TAKE_WITHIN, Phase::kStanding, false, std::nullopt, T0) == Hold::kNoTake);
	// Knocked: held, however long its friend's actor is down (to MAX_DOWN).
	CHECK(HoldStep(T0, T0 + 20s, Phase::kDown, true, std::nullopt, std::nullopt) == Hold::kHold);
	CHECK(HoldStep(T0, T0 + 20s, Phase::kGettingUp, true, std::nullopt, std::nullopt) == Hold::kHold);
	CHECK(HoldStep(T0, T0 + KnockRules::MAX_DOWN, Phase::kDown, true, std::nullopt, std::nullopt) == Hold::kGaveUp);
	// After its friend's up: UP_WAIT to stand.
	CHECK(HoldStep(T0, T0 + 5s, Phase::kDown, true, T0 + 4s, std::nullopt) == Hold::kHold);
	CHECK(HoldStep(T0, T0 + 4s + KnockRules::UP_WAIT, Phase::kDown, true, T0 + 4s, std::nullopt) == Hold::kGaveUp);
	// Standing again: STAND_GRACE, then let go.
	CHECK(HoldStep(T0, T0 + 6s, Phase::kStanding, true, T0 + 4s, T0 + 6s) == Hold::kHold);
	CHECK(HoldStep(T0, T0 + 6s + KnockRules::STAND_GRACE, Phase::kStanding, true, T0 + 4s, T0 + 6s) == Hold::kEnded);
	CHECK(KnockRules::NameOf(Hold::kNoTake) == "noTake");
}

TEST("knock rules: kept down: while its friend's actor is down and it lies here (a push once it lands, a paralysis at once), alive, once")
{
	CHECK(KnockRules::WantKeptDown(true, false, false, true, true));
	CHECK(!KnockRules::WantKeptDown(false, false, false, true, true));  // a native knock: no friend's down heard
	CHECK(!KnockRules::WantKeptDown(true, true, false, true, true));    // its up came: the engine gets it up
	CHECK(!KnockRules::WantKeptDown(true, false, true, true, true));    // kept already
	CHECK(!KnockRules::WantKeptDown(true, false, false, false, true));  // still flying
	CHECK(!KnockRules::WantKeptDown(true, false, false, true, false));  // dead
}
