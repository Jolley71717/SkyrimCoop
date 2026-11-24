#include "Test.h"

#include "FormTranslate.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "game/NpcRules.h"
#include "game/ReactionRules.h"
#include "game/StandInRules.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// Melee hit reactions in every game (docs/DESIGN.md, "Combat"): the messages, the server sending each copy's reaction
// to its NPC's runner, and the rules: what a reaction is, a stagger's size and way, which of a copy's reactions are our
// player's, the runner's echo, and the runner's gate.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Why = Protocol::ClaimWhy;
	using Kind = ReactionRules::Kind;
	using Steady = std::chrono::steady_clock;

	constexpr Protocol::NpcKey GUARD{ 0x00012345, 0 };
	constexpr Protocol::NpcKey WOLF{ 0x00023456, 0 };

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

	void Forget(Fixture& a_f, std::initializer_list<PeerId> a_peers)
	{
		REQUIRE(a_f.fake->Settle());
		for (const auto peer : a_peers) {
			(void)a_f.fake->Take(peer);
		}
	}

	ReactionRules::Heard HeardOn(std::uint32_t a_id, std::string a_event, Steady::time_point a_at)
	{
		return ReactionRules::Heard{ .id = a_id, .event = std::move(a_event), .stagger = std::nullopt, .at = a_at };
	}
}

// ---- the messages ----

TEST("reaction protocol: reactions read back as written, as message 79 of protocol 28, and carry forms")
{
	static_assert(Protocol::VERSION >= 28);
	const Protocol::NpcReactions msg{ .player = 2,
		.reactions = { { GUARD, "staggerStart", Protocol::Stagger{ 128, 64 } }, { WOLF, "recoilLargeStart", std::nullopt } } };
	const auto bytes = Protocol::Encode(msg);
	CHECK(Protocol::PeekType(bytes) == MT::kNpcReactions);
	CHECK(static_cast<int>(MT::kNpcReactions) == 79);
	CHECK(Protocol::DecodeNpcReactions(bytes) == msg);
	CHECK(FormTranslate::CarriesForms(MT::kNpcReactions));
	// An empty list reads (a translation may leave every reaction out).
	CHECK(Protocol::DecodeNpcReactions(Protocol::Encode(Protocol::NpcReactions{})));

	// The longest fits its size.
	Protocol::NpcReactions many;
	for (std::uint32_t i = 0; i < Protocol::MAX_NPC_REACTIONS; ++i) {
		many.reactions.push_back({ { 0x00100000 + i, 7 }, std::string(Protocol::MAX_EVENT_NAME, 's'), Protocol::Stagger{ 255, 255 } });
	}
	const auto longest = Protocol::Encode(many);
	CHECK(longest.size() <= Protocol::MAX_NPC_REACTIONS_SIZE);
	CHECK(Protocol::DecodeNpcReactions(longest) == many);
}

TEST("reaction protocol: one past the most, an NPC of id 0, no event, a stagger mark past 1, cut short or a byte too many are refused")
{
	const auto refused = [](const Protocol::NpcReactions& a_msg) { return !Protocol::DecodeNpcReactions(Protocol::Encode(a_msg)); };
	CHECK(refused(Protocol::NpcReactions{ .reactions = { { Protocol::NpcKey{ 0, 0 }, "staggerStart", std::nullopt } } }));
	CHECK(refused(Protocol::NpcReactions{ .reactions = { { GUARD, "", std::nullopt } } }));

	auto good = Protocol::Encode(Protocol::NpcReactions{ .reactions = { { GUARD, "staggerStart", Protocol::Stagger{ 1, 2 } } } });
	REQUIRE(Protocol::DecodeNpcReactions(good));
	auto mark = good;
	mark[mark.size() - 3] = 2;  // the stagger's mark
	CHECK(!Protocol::DecodeNpcReactions(mark));
	auto longer = good;
	longer.push_back(0);
	CHECK(!Protocol::DecodeNpcReactions(longer));
	auto shorter = good;
	shorter.pop_back();
	CHECK(!Protocol::DecodeNpcReactions(shorter));

	// A count past the most.
	Protocol::Writer w{ MT::kNpcReactions };
	w.U32(0);
	w.U8(static_cast<std::uint8_t>(Protocol::MAX_NPC_REACTIONS + 1));
	CHECK(!Protocol::DecodeNpcReactions(w.Data()));
}

TEST("reaction protocol: a stagger said as a player's or an NPC's action reads back with its size and way; a mark past 1 is refused")
{
	const Protocol::NpcActions npc{ .runner = 1,
		.actions = { { GUARD, 10, "attackStart", std::nullopt }, { GUARD, 20, "staggerStart", Protocol::Stagger{ 200, 32 } } } };
	CHECK(Protocol::DecodeNpcActions(Protocol::Encode(npc)) == npc);

	const Protocol::PlayerActions player{ .actions = { { 5, "staggerStart", Protocol::Stagger{ 10, 250 } }, { 6, "blockStart", std::nullopt } } };
	const auto playerBytes = Protocol::Encode(player);
	const auto decoded = Protocol::DecodePlayerActions(playerBytes);
	REQUIRE(decoded);
	CHECK(decoded->actions == player.actions);

	// The last action has no stagger: its mark is the last byte.
	auto mark = Protocol::Encode(Protocol::NpcActions{ .actions = { { GUARD, 1, "x", std::nullopt } } });
	REQUIRE(mark.back() == 0);
	mark.back() = 2;
	CHECK(!Protocol::DecodeNpcActions(mark));
	auto playerMark = Protocol::Encode(Protocol::PlayerActions{ .actions = { { 1, "x", std::nullopt } } });
	REQUIRE(playerMark.back() == 0);
	playerMark.back() = 3;
	CHECK(!Protocol::DecodePlayerActions(playerMark));

	// The longest of each still fits.
	Protocol::NpcActions manyNpc;
	for (std::uint32_t i = 0; i < Protocol::MAX_NPC_ACTIONS; ++i) {
		manyNpc.actions.push_back({ { 0x00100000 + i, 7 }, i, std::string(Protocol::MAX_EVENT_NAME, 'a'), Protocol::Stagger{ 1, 1 } });
	}
	CHECK(Protocol::Encode(manyNpc).size() <= Protocol::MAX_NPC_ACTIONS_SIZE);
	Protocol::PlayerActions manyPlayer;
	for (std::uint32_t i = 0; i < Protocol::MAX_PLAYER_ACTIONS; ++i) {
		manyPlayer.actions.push_back({ i, std::string(Protocol::MAX_EVENT_NAME, 'a'), Protocol::Stagger{ 1, 1 } });
	}
	CHECK(Protocol::Encode(manyPlayer).size() <= Protocol::MAX_PLAYER_ACTIONS_SIZE);
}

// ---- the server ----

TEST("reaction server: a copy's reaction goes to its NPC's runner alone, the player named; none of an NPC nobody runs, the sender's own or a dead one")
{
	Fixture f;
	AllIn(f, 3);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { GUARD, Why::kHave } } }));
	Forget(f, { 1, 2, 3 });

	const Protocol::NpcReaction stagger{ GUARD, "staggerStart", Protocol::Stagger{ 100, 10 } };
	f.fake->Deliver(2, Protocol::Encode(Protocol::NpcReactions{ .player = 99, .reactions = { stagger, { WOLF, "recoilStart", std::nullopt } } }));
	REQUIRE(f.fake->Settle());
	const auto toRunner = SentTo<Protocol::NpcReactions>(f, 1, MT::kNpcReactions, Protocol::DecodeNpcReactions);
	REQUIRE(toRunner.size() == 1u);
	CHECK(toRunner[0].player == 2u);
	CHECK(toRunner[0].reactions == std::vector<Protocol::NpcReaction>({ stagger }));  // the wolf: nobody runs it
	CHECK(f.fake->Take(2).empty());
	CHECK(f.fake->Take(3).empty());

	// Ada's own NPC: its reactions go as its actions, never to herself.
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcReactions{ .reactions = { stagger } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());

	// Dead for the session: nowhere.
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcDeaths{ .deaths = { { GUARD, 2, 0 } } }));
	Forget(f, { 1, 2, 3 });
	f.fake->Deliver(3, Protocol::Encode(Protocol::NpcReactions{ .reactions = { stagger } }));
	REQUIRE(f.fake->Settle());
	CHECK(SentTo<Protocol::NpcReactions>(f, 1, MT::kNpcReactions, Protocol::DecodeNpcReactions).empty());
}

TEST("reaction server: a flood past the budget is dropped and said once")
{
	Fixture f;
	AllIn(f, 2);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { GUARD, Why::kHave } } }));
	Forget(f, { 1, 2 });
	for (int round = 0; round < 4; ++round) {
		Protocol::NpcReactions many;
		for (std::size_t i = 0; i < Protocol::MAX_NPC_REACTIONS; ++i) {
			many.reactions.push_back({ GUARD, "staggerStart", std::nullopt });
		}
		f.fake->Deliver(2, Protocol::Encode(many));
	}
	REQUIRE(f.fake->Settle());
	std::size_t passed = 0;
	for (const auto& msg : SentTo<Protocol::NpcReactions>(f, 1, MT::kNpcReactions, Protocol::DecodeNpcReactions)) {
		passed += msg.reactions.size();
	}
	CHECK(passed >= 32u);
	CHECK(passed < 4 * Protocol::MAX_NPC_REACTIONS);
	CHECK(f.LoggedSoon("server: player 2 sends hit reactions faster than they are passed on: the rest are dropped (said once)"));
}

// ---- the rules ----

TEST("reaction rules: a stagger, a recoil and a blocked hit by their names in any case; their ends and everything else are none")
{
	using ReactionRules::KindOf;
	CHECK(KindOf("staggerStart") == Kind::kStagger);
	CHECK(KindOf("STAGGERSTART") == Kind::kStagger);
	CHECK(KindOf("recoilStart") == Kind::kRecoil);
	CHECK(KindOf("recoilLargeStart") == Kind::kRecoil);
	CHECK(KindOf("blockHitStart") == Kind::kBlockHit);
	CHECK(KindOf("staggerStop") == Kind::kNone);
	CHECK(KindOf("recoilStop") == Kind::kNone);
	CHECK(KindOf("staggerEnd") == Kind::kNone);
	CHECK(KindOf("staggerExit") == Kind::kNone);
	CHECK(KindOf("attackStart") == Kind::kNone);
	CHECK(KindOf("blockStart") == Kind::kNone);
	CHECK(KindOf("bashStart") == Kind::kNone);
	CHECK(KindOf("") == Kind::kNone);
	CHECK(ReactionRules::KindName(Kind::kRecoil) == "recoil");
	CHECK(!ReactionRules::IsReaction("moveStart"));
	// Every reaction is one a copy plays as its runner's action.
	CHECK(NpcRules::Replayed("staggerStart"));
	CHECK(NpcRules::Replayed("recoilLargeStart"));
	CHECK(NpcRules::Replayed("blockHitStart"));
}

TEST("reaction rules: a stagger's size and way in a byte each, taken round a turn, clamped, a non-number as 0")
{
	using ReactionRules::Pack;
	using ReactionRules::Unpack;
	CHECK((Pack(0.0f, 0.0f) == Protocol::Stagger{ 0, 0 }));
	CHECK(Pack(Protocol::STAGGER_MOST, 0.0f).magnitude == 255);
	CHECK(Pack(99.0f, 0.0f).magnitude == 255);
	CHECK(Pack(-1.0f, 0.0f).magnitude == 0);
	CHECK(Pack(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()) == Protocol::Stagger{});
	CHECK(Pack(0.0f, 0.25f).direction == 64);
	CHECK(Pack(0.0f, 1.25f).direction == 64);
	CHECK(Pack(0.0f, -0.25f).direction == 192);
	CHECK(Pack(0.0f, 0.9999f).direction == 0);  // a whole turn round is no turn
	for (const float magnitude : { 0.0f, 0.25f, 0.5f, 1.0f, 1.7f }) {
		for (const float direction : { 0.0f, 0.1f, 0.5f, 0.75f }) {
			const auto back = Unpack(Pack(magnitude, direction));
			CHECK(std::fabs(back.magnitude - magnitude) <= Protocol::STAGGER_MOST / 255.0f);
			CHECK(std::fabs(back.direction - direction) <= 1.0f / 256.0f);
		}
	}
}

TEST("reaction rules: a copy's reaction near our player's hit, before or after it, is ours; one with none is judged not ours after the grace")
{
	ReactionRules::Judge judge;
	const auto           t = Steady::now();

	// Hit, then the stagger.
	judge.Touched(1, t);
	judge.Heard(HeardOn(1, "staggerStart", t + 50ms));
	auto ours = judge.Ours(t + 60ms);
	REQUIRE(ours.size() == 1u);
	CHECK(ours[0].event == "staggerStart");

	// The stagger heard first, its hit later in the frame: waits, then ours.
	judge.Heard(HeardOn(2, "recoilStart", t + 100ms));
	CHECK(judge.Ours(t + 110ms).empty());
	CHECK(judge.Waiting() == 1u);
	judge.Touched(2, t + 120ms);
	CHECK(judge.Ours(t + 130ms).size() == 1u);

	// No hit: not ours once the grace is over.
	judge.Heard(HeardOn(3, "staggerStart", t + 200ms));
	CHECK(judge.Ours(t + 200ms + ReactionRules::GRACE - 1ms).empty());
	CHECK(judge.Ours(t + 200ms + ReactionRules::GRACE).empty());
	CHECK(judge.Waiting() == 0u);

	// A hit too long before.
	judge.Touched(4, t);
	judge.Heard(HeardOn(4, "staggerStart", t + ReactionRules::REACH + 1ms));
	CHECK(judge.Ours(t + ReactionRules::REACH + ReactionRules::GRACE + 2ms).empty());
	CHECK(judge.ours == 2u);
	CHECK(judge.notOurs == 2u);

	// Hits long gone are forgotten; a flood past the most is dropped and counted.
	CHECK(judge.Ours(t + 10s).empty());
	CHECK(judge.Touching() == 0u);
	for (std::size_t i = 0; i < ReactionRules::MAX_HEARD + 3; ++i) {
		judge.Heard(HeardOn(5, "staggerStart", t));
	}
	CHECK(judge.Waiting() == ReactionRules::MAX_HEARD);
	CHECK(judge.dropped == 3u);
	judge.Clear();
	CHECK(judge.Waiting() == 0u);
}

TEST("reaction rules: our reaction coming back from the runner is skipped once, of its kind, on its copy, within the echo")
{
	ReactionRules::Echoes echoes;
	const auto            t = Steady::now();
	echoes.Ours(GUARD, Kind::kStagger, t);
	CHECK(!echoes.Echo(GUARD, Kind::kRecoil, t + 100ms));  // another kind
	CHECK(!echoes.Echo(WOLF, Kind::kStagger, t + 100ms));   // another copy
	CHECK(echoes.Echo(GUARD, Kind::kStagger, t + 300ms));
	CHECK(!echoes.Echo(GUARD, Kind::kStagger, t + 400ms));  // the runner's own second stagger plays
	CHECK(echoes.echoes == 1u);

	echoes.Ours(GUARD, Kind::kStagger, t);
	CHECK(!echoes.Echo(GUARD, Kind::kStagger, t + ReactionRules::ECHO + 1ms));  // too late: the runner's own
	echoes.Ours(WOLF, Kind::kBlockHit, t);
	echoes.Forget(t + ReactionRules::ECHO + 1ms);
	CHECK(echoes.Size() == 0u);
	CHECK(!echoes.Echo(GUARD, Kind::kNone, t));
}

TEST("reaction rules: the runner plays a friend's reaction, not the same kind on that NPC again within the gap, and never a name that is no reaction")
{
	ReactionRules::Gate gate;
	using Play = ReactionRules::Play;
	const auto t = Steady::now();
	CHECK(gate.Judge(GUARD, "staggerStart", t) == Play::kPlay);
	CHECK(gate.Judge(GUARD, "staggerStart", t + 100ms) == Play::kTooSoon);
	CHECK(gate.Judge(GUARD, "recoilStart", t + 100ms) == Play::kPlay);
	CHECK(gate.Judge(WOLF, "staggerStart", t + 100ms) == Play::kPlay);
	CHECK(gate.Judge(GUARD, "staggerStart", t + ReactionRules::RUNNER_GAP) == Play::kPlay);
	CHECK(gate.Judge(GUARD, "attackStart", t) == Play::kNotReaction);
	CHECK(gate.Judge(GUARD, "IdleForceDefaultState", t) == Play::kNotReaction);
	gate.Forget(t + 10s);
	CHECK(gate.Size() == 0u);
}

TEST("reaction rules: the outbox sends a burst, then drops past the budget, in messages of at most the most")
{
	ReactionRules::Outbox outbox;
	const auto            t = Steady::now();
	std::size_t           added = 0;
	for (int i = 0; i < 40; ++i) {
		added += outbox.Add({ GUARD, "staggerStart", std::nullopt }, t) ? 1 : 0;
	}
	CHECK(added == 16u);
	CHECK(outbox.dropped == 24u);
	CHECK(outbox.Take().size() == 16u);
	CHECK(outbox.Take().empty());
	CHECK(outbox.Add({ GUARD, "staggerStart", std::nullopt }, t + 1s));  // the budget is back
}

TEST("reaction rules: a stagger's size and way go with the event through a copy's queue and our player's outbox")
{
	NpcRules::ActionQueue queue;
	const auto            t = Steady::now();
	queue.Push(NpcRules::Event{ "staggerStart", Protocol::Stagger{ 9, 8 } }, t, t);
	queue.Push("attackStart", t + 10ms, t);
	const auto due = queue.DueEvents(t + 20ms);
	REQUIRE(due.size() == 2u);
	CHECK((due[0] == NpcRules::Event{ "staggerStart", Protocol::Stagger{ 9, 8 } }));
	CHECK((due[1] == NpcRules::Event{ "attackStart", std::nullopt }));

	StandInRules::Outbox outbox;
	CHECK(outbox.Heard("staggerStart", 7, t, Protocol::Stagger{ 1, 2 }));
	CHECK(outbox.Heard("blockStart", 8, t));
	const auto taken = outbox.Take();
	REQUIRE(taken.size() == 2u);
	CHECK((taken[0].stagger == Protocol::Stagger{ 1, 2 }));
	CHECK(!taken[1].stagger);
}
