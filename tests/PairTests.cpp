#include "Test.h"

#include "FormTranslate.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "game/PairRules.h"
#include "game/StandInRules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Kill moves and paired animations (docs/DESIGN.md, "Kill moves and paired animations"): the pairs on the wire, the
// server passing them on, and the rules a game says its own by and plays and holds a friend's by.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Pair = Protocol::Pair;
	using Side = Protocol::PairSide;
	using Steady = std::chrono::steady_clock;
	using PairRules::Role;
	using PairRules::Shown;
	using PairRules::Verdict;

	constexpr Protocol::NpcKey BANDIT{ 0x00012345, 0 };
	constexpr Protocol::NpcKey WOLF_OF_1{ 0xFF000801, 1 };  // a spawn, the key its spawner's game gave it
	constexpr Protocol::NpcKey DRAGON{ 0x0001CA05, 0 };

	Side PlayerSide(std::uint32_t a_player)
	{
		return Side{ .player = a_player };
	}

	Side NpcSide(Protocol::NpcKey a_npc)
	{
		return Side{ .npc = a_npc };
	}

	Pair KillMove(Side a_attacker, Side a_victim)
	{
		Pair pair;
		pair.attacker = a_attacker;
		pair.victim = a_victim;
		pair.sentMs = 1000;
		pair.idle = 0x000E1E6D;
		pair.event = "pa_KillMove1HMDecap";
		pair.flags = Protocol::kPairKill;
		pair.x = 100.0f;
		pair.y = -200.0f;
		pair.z = 50.0f;
		pair.heading = 1.5f;
		return pair;
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

	std::vector<Protocol::Pairs> PairsTo(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<Protocol::Pairs> out;
		for (auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kPairs) {
				CHECK(sent.reliable);
				if (auto msg = Protocol::DecodePairs(sent.data)) {
					out.push_back(std::move(*msg));
				}
			}
		}
		return out;
	}

	std::size_t CountOf(const std::vector<Protocol::Pairs>& a_msgs)
	{
		std::size_t n = 0;
		for (const auto& msg : a_msgs) {
			n += msg.pairs.size();
		}
		return n;
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

	// Facts of a pair that plays: a stand-in's kill move on a copy, near.
	PairRules::Facts Playable()
	{
		return PairRules::Facts{ .attacker = Shown::kStandIn, .victim = Shown::kCopy, .apart = 80.0f, .drift = 20.0f, .kill = true, .idleFound = true };
	}
}

// ---- the message ----

TEST("pair protocol: pairs read back as they were written, as message 76 of protocol 24, and carry forms")
{
	static_assert(Protocol::VERSION >= 24);
	static_assert(static_cast<std::uint8_t>(MT::kPairs) == 76);
	Pair sneak = KillMove(PlayerSide(2), NpcSide(BANDIT));
	sneak.flags = Protocol::kPairKill | Protocol::kPairSneak;
	Pair feed = KillMove(PlayerSide(2), NpcSide(WOLF_OF_1));
	feed.event = "pa_VampireFeedFront";
	feed.flags = 0;
	feed.idle = 0;
	const Pair dragon = KillMove(NpcSide(DRAGON), PlayerSide(2));
	const Protocol::Pairs pairs{ .sender = 2, .pairs = { sneak, feed, dragon } };
	CHECK(Protocol::DecodePairs(Protocol::Encode(pairs)) == pairs);
	CHECK(Protocol::PeekType(Protocol::Encode(pairs)) == MT::kPairs);
	CHECK(Protocol::DecodePairs(Protocol::Encode(Protocol::Pairs{})) == Protocol::Pairs{});
	CHECK(FormTranslate::CarriesForms(MT::kPairs));
}

TEST("pair protocol: the most fit; more, a side of nobody or of both, one actor twice, no event, a bad flag, place or heading, or a byte too many are refused")
{
	Protocol::Pairs most{ .sender = 1 };
	for (std::size_t i = 0; i < Protocol::MAX_PAIRS; ++i) {
		Pair pair = KillMove(PlayerSide(1), NpcSide(BANDIT));
		pair.event = std::string(Protocol::MAX_EVENT_NAME, 'k');
		most.pairs.push_back(pair);
	}
	auto bytes = Protocol::Encode(most);
	CHECK(bytes.size() == Protocol::MAX_PAIRS_SIZE);
	CHECK(Protocol::DecodePairs(bytes) == most);
	bytes[5] = static_cast<std::uint8_t>(Protocol::MAX_PAIRS + 1);
	CHECK(!Protocol::DecodePairs(bytes));
	most.pairs.push_back(KillMove(PlayerSide(1), NpcSide(BANDIT)));
	CHECK(Protocol::DecodePairs(Protocol::Encode(most))->pairs.size() == Protocol::MAX_PAIRS);

	const auto refused = [](Pair a_pair) {
		return !Protocol::DecodePairs(Protocol::Encode(Protocol::Pairs{ .sender = 1, .pairs = { KillMove(PlayerSide(1), NpcSide(BANDIT)), a_pair } }));
	};
	CHECK(refused(KillMove(Side{}, NpcSide(BANDIT))));            // an attacker of nobody
	CHECK(refused(KillMove(PlayerSide(1), Side{})));              // a victim of nobody
	CHECK(refused(KillMove(NpcSide({ 0, 3 }), NpcSide(BANDIT))));  // an NPC of id 0
	CHECK(refused(KillMove(NpcSide(BANDIT), NpcSide(BANDIT))));   // one actor on both sides
	CHECK(refused(KillMove(PlayerSide(1), PlayerSide(1))));
	Pair bad = KillMove(PlayerSide(1), NpcSide(BANDIT));
	bad.event.clear();
	CHECK(refused(bad));
	bad = KillMove(PlayerSide(1), NpcSide(BANDIT));
	bad.event = "pa_\x01kill";
	CHECK(!Protocol::PairHolds(bad));  // a control character: no game writes one
	const auto cleaned = Protocol::DecodePairs(Protocol::Encode(Protocol::Pairs{ .pairs = { bad } }));
	REQUIRE(cleaned);  // ... and the reader drops it (Reader::Str), as from every string on the wire
	CHECK(cleaned->pairs[0].event == "pa_kill");
	bad.event = "\x01\x02";  // nothing left: no event
	CHECK(refused(bad));
	bad = KillMove(PlayerSide(1), NpcSide(BANDIT));
	bad.flags = 4;
	CHECK(refused(bad));
	bad = KillMove(PlayerSide(1), NpcSide(BANDIT));
	bad.x = std::nanf("");
	CHECK(refused(bad));
	bad = KillMove(PlayerSide(1), NpcSide(BANDIT));
	bad.z = 1e9f;
	CHECK(refused(bad));
	bad = KillMove(PlayerSide(1), NpcSide(BANDIT));
	bad.heading = -7.5f;
	CHECK(refused(bad));
	// An event past MAX_EVENT_NAME is cut to it on the wire (the writer's Str), and so holds.
	Pair longer = KillMove(PlayerSide(1), NpcSide(BANDIT));
	longer.event = std::string(Protocol::MAX_EVENT_NAME + 5, 'a');
	const auto cut = Protocol::DecodePairs(Protocol::Encode(Protocol::Pairs{ .pairs = { longer } }));
	if (cut) {
		CHECK(cut->pairs[0].event.size() <= Protocol::MAX_EVENT_NAME);
	}
	// A player's side that names an NPC too is read as the player's alone (the key is not written for it).
	Side both = PlayerSide(1);
	both.npc = BANDIT;
	const auto read = Protocol::DecodePairs(Protocol::Encode(Protocol::Pairs{ .pairs = { KillMove(both, NpcSide(WOLF_OF_1)) } }));
	REQUIRE(read);
	CHECK((read->pairs[0].attacker == PlayerSide(1)));
	// A byte too many, or one too few.
	auto extra = Protocol::Encode(Protocol::Pairs{ .pairs = { KillMove(PlayerSide(1), NpcSide(BANDIT)) } });
	extra.push_back(0);
	CHECK(!Protocol::DecodePairs(extra));
	auto shorter = Protocol::Encode(Protocol::Pairs{ .pairs = { KillMove(PlayerSide(1), NpcSide(BANDIT)) } });
	shorter.pop_back();
	CHECK(!Protocol::DecodePairs(shorter));
}

TEST("pair form translate: a pair whose NPC the reader lacks is left out; an idle it lacks becomes none, the pair kept to be found by its event")
{
	const Edges            e;
	const Protocol::NpcKey ofExtra{ 0x04000ABC, 0 };  // Extra.esp's: the guest lacks it
	const Protocol::NpcKey ofB{ 0x03000ABC, 0 };      // B.esp's reference in the host's numbering
	const Protocol::NpcKey inGuest{ 0x01000ABC, 0 };

	Pair bIdle = KillMove(PlayerSide(1), NpcSide(ofB));
	bIdle.idle = 0x03000123;                                        // B.esp's kill move: kept, renumbered
	Pair extraIdle = KillMove(NpcSide(BANDIT), PlayerSide(1));
	extraIdle.idle = 0x04000456;                                    // Extra.esp's: none, the pair kept
	const Pair extraVictim = KillMove(PlayerSide(1), NpcSide(ofExtra));  // no copy to play it on
	const Pair extraAttacker = KillMove(NpcSide(ofExtra), PlayerSide(1));
	const Pair spawn = KillMove(NpcSide(WOLF_OF_1), NpcSide(BANDIT));

	const auto pairs = Protocol::DecodePairs(
		e.HostToGuest(Protocol::Encode(Protocol::Pairs{ .sender = 1, .pairs = { bIdle, extraIdle, extraVictim, extraAttacker, spawn } })));
	REQUIRE(pairs);
	CHECK(pairs->sender == 1u);
	REQUIRE(pairs->pairs.size() == 3u);
	CHECK((pairs->pairs[0].victim.npc == inGuest));
	CHECK(pairs->pairs[0].idle == 0x01000123u);
	CHECK(pairs->pairs[1].idle == 0u);
	CHECK(pairs->pairs[1].event == "pa_KillMove1HMDecap");
	CHECK((pairs->pairs[1].attacker.npc == BANDIT));  // Skyrim.esm's, the same everywhere
	CHECK((pairs->pairs[2].attacker.npc == WOLF_OF_1));
}

// ---- the server ----

TEST("pair server: a player's own pairs and those of the NPCs its game runs go to the others with the sender named; any other is dropped")
{
	Fixture f;
	AllIn(f, 3);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { BANDIT, Protocol::ClaimWhy::kHave } } }));
	REQUIRE(f.fake->Settle());
	for (PeerId peer = 1; peer <= 3; ++peer) {
		(void)f.fake->Take(peer);
	}

	// Bo says his own on the bandit, Ada's, the bandit's (Ada runs it), and his own on Ada's player: only the first goes.
	const Pair bos = KillMove(PlayerSide(2), NpcSide(BANDIT));
	f.fake->Deliver(2, Protocol::Encode(Protocol::Pairs{ .sender = 99,
		.pairs = { bos, KillMove(PlayerSide(1), NpcSide(BANDIT)), KillMove(NpcSide(BANDIT), PlayerSide(2)), KillMove(PlayerSide(2), PlayerSide(1)) } }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 3 } }) {
		const auto got = PairsTo(f, peer);
		REQUIRE(got.size() == 1u);
		CHECK(got[0].sender == 2u);
		CHECK(got[0].pairs == std::vector<Pair>({ bos }));
	}
	CHECK(PairsTo(f, 2).empty());  // not back to its sender

	// Ada's bandit's, on Ada's own player and on another NPC: both go.
	const Pair onAda = KillMove(NpcSide(BANDIT), PlayerSide(1));
	const Pair onWolf = KillMove(NpcSide(BANDIT), NpcSide(WOLF_OF_1));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Pairs{ .pairs = { onAda, onWolf } }));
	REQUIRE(f.fake->Settle());
	const auto got = PairsTo(f, 2);
	REQUIRE(got.size() == 1u);
	CHECK(got[0].sender == 1u);
	CHECK(got[0].pairs == std::vector<Pair>({ onAda, onWolf }));

	// None of the sender's: nothing goes.
	f.fake->Deliver(3, Protocol::Encode(Protocol::Pairs{ .pairs = { KillMove(NpcSide(BANDIT), PlayerSide(3)) } }));
	REQUIRE(f.fake->Settle());
	CHECK(PairsTo(f, 1).empty());
	CHECK(PairsTo(f, 2).empty());
}

TEST("pair server: past a burst of 16 a player's pairs are dropped, and the log says so once")
{
	Fixture f;
	AllIn(f, 2);
	Protocol::Pairs full{};
	for (std::size_t i = 0; i < Protocol::MAX_PAIRS; ++i) {
		full.pairs.push_back(KillMove(PlayerSide(1), NpcSide(BANDIT)));
	}
	for (int i = 0; i < 4; ++i) {
		f.fake->Deliver(1, Protocol::Encode(full));
	}
	REQUIRE(f.fake->Settle());
	const auto passed = CountOf(PairsTo(f, 2));
	CHECK(passed >= 16u);
	CHECK(passed < 4u * Protocol::MAX_PAIRS);
	CHECK(f.LoggedSoon("sends paired animations faster than they are passed on"));
	const auto lines = f.LogLines();
	CHECK(std::ranges::count_if(lines, [](const std::string& a_line) { return a_line.find("sends paired animations faster") != std::string::npos; }) == 1);
}

// ---- the rules ----

TEST("pair rules: an event's part in a pair, in any case: the idle's own, the victim's, the end's; any other none")
{
	const std::vector<std::pair<std::string_view, Role>> cases{
		{ "pa_KillMove1HMDecap", Role::kAttacker },
		{ "PA_KILLMOVEBEARMAUL", Role::kAttacker },
		{ "pa_VampireFeedFront", Role::kAttacker },  // a feeding (kills no one)
		{ "pa_DragonKillMoveBite", Role::kAttacker },
		{ "KillMoveDualWieldXCross", Role::kAttacker },
		{ "PairedKillTarget", Role::kVictim },
		{ "pairedkilltarget", Role::kVictim },
		{ "PairedTargetStart", Role::kVictim },
		{ "PairEnd", Role::kEnd },
		{ "PairedStop", Role::kEnd },
		{ "KillMoveEnd", Role::kEnd },
		{ "weaponSwing", Role::kNone },
		{ "IdleChairSitting", Role::kNone },
		{ "pa", Role::kNone },
		{ "", Role::kNone },
	};
	for (const auto& [event, role] : cases) {
		if (PairRules::RoleOf(event) != role) {
			Test::Fail(__FILE__, __LINE__, std::format("{} read as role {}, not {}", event, static_cast<int>(PairRules::RoleOf(event)), static_cast<int>(role)));
		}
	}
	CHECK(PairRules::RoleOf(std::string(Protocol::MAX_EVENT_NAME + 1, 'k') + "killmove") == Role::kNone);  // too long to be one
	CHECK(PairRules::PairEvent("PairEnd"));
	CHECK(!PairRules::PairEvent("attackStart"));
	// And the stand-ins' act of each: a paired idle's own event is a kill move's, never replayed as an attack.
	CHECK(StandInRules::ActOf("pa_KillMove1HMDecap") == StandInRules::Act::kKillMove);
	CHECK(StandInRules::ActOf("PA_VampireFeedFront") == StandInRules::Act::kKillMove);
	CHECK(StandInRules::ActOf("pa_") != StandInRules::Act::kKillMove);  // nothing after the mark
	CHECK(StandInRules::ActOf("pace") != StandInRules::Act::kKillMove);
}

TEST("pair rules: an actor's events stay here while its kill move flag is up, for EVENT_HOLD after a pair's event, and while held here")
{
	const auto now = Steady::now();
	CHECK(!PairRules::EventsStayHere(false, std::nullopt, false, now));
	CHECK(PairRules::EventsStayHere(true, std::nullopt, false, now));
	CHECK(PairRules::EventsStayHere(false, std::nullopt, true, now));
	CHECK(PairRules::EventsStayHere(false, now - 1s, false, now));
	CHECK(PairRules::EventsStayHere(false, now - PairRules::EVENT_HOLD, false, now));
	CHECK(!PairRules::EventsStayHere(false, now - PairRules::EVENT_HOLD - 1ms, false, now));
}

TEST("pair rules: kinds by flags (a kill, a sneak kill, one that kills no one), counted and named")
{
	CHECK(PairRules::KindOf(Protocol::kPairKill) == PairRules::Kind::kKill);
	CHECK(PairRules::KindOf(Protocol::kPairKill | Protocol::kPairSneak) == PairRules::Kind::kSneak);
	CHECK(PairRules::KindOf(0) == PairRules::Kind::kOther);
	CHECK(PairRules::KindOf(Protocol::kPairSneak) == PairRules::Kind::kOther);  // a sneaking feeding kills no one
	PairRules::KindCounts counts;
	CHECK(counts.Line() == "none");
	counts.Add(PairRules::Kind::kKill);
	counts.Add(PairRules::Kind::kKill);
	counts.Add(PairRules::Kind::kOther);
	CHECK(counts.Line() == "kill:2,other:1");
	CHECK(counts.Total() == 3u);
	CHECK(PairRules::NameOf(PairRules::Kind::kSneak) == "sneak");
	CHECK(PairRules::NameOf(PairRules::Kind::kCount) == "?");
}

TEST("pair rules: outbox: an attacker's pair on a victim said once in SAY_AGAIN; on another victim said again; split in messages of MAX_PAIRS")
{
	PairRules::Outbox outbox;
	const auto        now = Steady::now();
	const Pair        first = KillMove(PlayerSide(1), NpcSide(BANDIT));
	CHECK(outbox.Say(first, now));
	CHECK(!outbox.Say(first, now + 100ms));  // its event and its flag, a frame apart
	CHECK(outbox.coalesced == 1u);
	CHECK(outbox.SaidLately(PlayerSide(1), now + 1s));
	CHECK(!outbox.SaidLately(NpcSide(BANDIT), now + 1s));
	CHECK(outbox.VictimLately(NpcSide(BANDIT), now + 1s));
	CHECK(!outbox.VictimLately(PlayerSide(1), now + 1s));
	CHECK(!outbox.VictimLately(NpcSide(BANDIT), now + PairRules::SAY_AGAIN));
	CHECK(outbox.Say(KillMove(PlayerSide(1), NpcSide(WOLF_OF_1)), now + 200ms));  // another victim: another pair
	CHECK(outbox.Say(first, now + PairRules::SAY_AGAIN));                         // again, later: another pair
	CHECK(outbox.Say(KillMove(NpcSide(DRAGON), PlayerSide(1)), now));             // another attacker
	CHECK(outbox.said == 4u);
	const auto msgs = outbox.Take();
	REQUIRE(msgs.size() == 1u);
	CHECK(msgs[0].pairs.size() == 4u);
	CHECK(msgs[0].pairs[0] == first);
	CHECK(outbox.Take().empty());

	// Many attackers in one tick: messages of at most MAX_PAIRS; past the queue's room dropped.
	PairRules::Outbox many;
	for (std::uint32_t i = 1; i <= 70; ++i) {
		(void)many.Say(KillMove(NpcSide({ 0x00100000u + i, 0 }), PlayerSide(1)), now);
	}
	CHECK(many.said == 64u);
	CHECK(many.dropped == 6u);
	const auto split = many.Take();
	REQUIRE(split.size() == 4u);
	CHECK(std::ranges::all_of(split, [](const Protocol::Pairs& a_msg) { return a_msg.pairs.size() == Protocol::MAX_PAIRS; }));

	// Pruned past SAY_AGAIN, cleared at once.
	outbox.Prune(now + 10s);
	CHECK(!outbox.SaidLately(PlayerSide(1), now + 10s));
	CHECK(outbox.Say(first, now + 10s));
	outbox.Clear();
	CHECK(outbox.Take().empty());
	CHECK(!outbox.SaidLately(PlayerSide(1), now + 10s));
}

namespace
{
	using PairRules::ByFlag;
	using PairRules::FlagSide;

	constexpr std::uint32_t DECAP = 0x000E1E6D;      // a kill move's idle
	constexpr std::uint32_t DRAUGR_KM = 0x000F2A10;  // a draugr's kill move's idle

	// One of ours in a kill move, its idle fresh (changed since its flag was last down) or not.
	FlagSide Ours(std::uint32_t a_idle, std::string_view a_event, bool a_fresh, bool a_inKillMove = true)
	{
		return FlagSide{ .idle = a_idle, .event = a_event, .inKillMove = a_inKillMove, .fresh = a_fresh };
	}

	// An actor that is not ours (a copy, a stand-in): its idle's freshness is not known.
	FlagSide Theirs(std::uint32_t a_idle, std::string_view a_event, bool a_inKillMove = true)
	{
		return FlagSide{ .idle = a_idle, .event = a_event, .inKillMove = a_inKillMove };
	}
}

TEST("pair rules: by its flag alone an actor is said only when its last idle is a fresh pair's and the other's is not the same")
{
	CHECK(PairRules::JudgeByFlag(Ours(DECAP, "pa_KillMove1HMDecap", true), FlagSide{}) == ByFlag::kSay);
	CHECK(PairRules::JudgeByFlag(Ours(DECAP, "pa_KillMove1HMDecap", true), Theirs(0x00012345, "IdleStop")) == ByFlag::kSay);
	CHECK(PairRules::JudgeByFlag(Ours(DECAP, "pa_KillMove1HMDecap", true), Theirs(DECAP, "pa_KillMove1HMDecap")) == ByFlag::kAmbiguous);
	CHECK(PairRules::JudgeByFlag(Ours(0, "", true), Theirs(DECAP, "pa_KillMove1HMDecap")) == ByFlag::kNotPaired);  // the victim: no idle of its own
	CHECK(PairRules::JudgeByFlag(Ours(0x00011111, "IdleStop", true), FlagSide{}) == ByFlag::kNotPaired);         // an idle that is no pair's
	CHECK(PairRules::JudgeByFlag(Ours(0x00011111, "PairedKillTarget", true), FlagSide{}) == ByFlag::kNotPaired);
	// The other in no kill move with a pair's old idle is no rival (it is not in this pair as its attacker).
	CHECK(PairRules::JudgeByFlag(Ours(DECAP, "pa_KillMove1HMDecap", true), Theirs(DRAUGR_KM, "pa_KillMoveDraugrA", false)) == ByFlag::kSay);
}

TEST("pair rules: by its flag, an old pair's idle is never said: the victim of an NPC's kill move who did one earlier is not the attacker")
{
	// Our player did a kill move minutes ago: its last idle is still that one. Now a draugr (a copy here, run by the other
	// game) kill-moves it. The player's flag is up and no attacker event of its was heard.
	const auto player = Ours(DECAP, "pa_KillMove1HMDecap", false);
	CHECK(PairRules::JudgeByFlag(player, Theirs(DRAUGR_KM, "pa_KillMoveDraugrA")) == ByFlag::kStale);
	CHECK(PairRules::JudgeByFlag(player, Theirs(0, "")) == ByFlag::kStale);  // a copy whose process played no idle
	// Its freshness not known (never seen with its flag down): not said either.
	CHECK(PairRules::JudgeByFlag(FlagSide{ .idle = DECAP, .event = "pa_KillMove1HMDecap", .inKillMove = true }, FlagSide{}) == ByFlag::kStale);
	// Fresh, but the other is in it with a pair's attacker idle that may be fresh: either could be the attacker.
	CHECK(PairRules::JudgeByFlag(Ours(DECAP, "pa_KillMove1HMDecap", true), Theirs(DRAUGR_KM, "pa_KillMoveDraugrA")) == ByFlag::kAmbiguous);
	CHECK(PairRules::JudgeByFlag(Ours(DECAP, "pa_KillMove1HMDecap", true), Ours(DRAUGR_KM, "pa_KillMoveDraugrA", true)) == ByFlag::kAmbiguous);
	// One held by a pair played here (an NPC we run, the victim of a friend's pair) is never said back, whatever its idle.
	auto held = Ours(DECAP, "pa_KillMove1HMDecap", true);
	held.held = true;
	CHECK(PairRules::JudgeByFlag(held, FlagSide{}) == ByFlag::kHeld);
}

TEST("pair rules: by its flag, the order two of ours are judged in never changes who is said")
{
	// Two NPCs we run: the attacker's idle fresh, the victim's an old pair's (it did a kill move earlier).
	const auto attacker = Ours(DRAUGR_KM, "pa_KillMoveDraugrA", true);
	const auto victim = Ours(DECAP, "pa_KillMove1HMDecap", false);
	const auto judge = [](const FlagSide& a_first, const FlagSide& a_second) {
		return std::pair{ PairRules::JudgeByFlag(a_first, a_second), PairRules::JudgeByFlag(a_second, a_first) };
	};
	CHECK((judge(attacker, victim) == std::pair{ ByFlag::kSay, ByFlag::kStale }));
	CHECK((judge(victim, attacker) == std::pair{ ByFlag::kStale, ByFlag::kSay }));
	// Both fresh with different pairs' idles: neither can be told the attacker, in either order.
	const auto other = Ours(DECAP, "pa_KillMove1HMDecap", true);
	CHECK((judge(attacker, other) == std::pair{ ByFlag::kAmbiguous, ByFlag::kAmbiguous }));
	CHECK((judge(other, attacker) == std::pair{ ByFlag::kAmbiguous, ByFlag::kAmbiguous }));
}

TEST("pair rules: idle memory: an idle is fresh when it changed since the flag last fell; a pair's idle taken while it is down is not at rest")
{
	PairRules::IdleMemory memory;
	constexpr std::uint32_t ME = 0x14;
	constexpr std::uint32_t WALK = 0x00011111;
	CHECK(!memory.Fresh(ME, DECAP));  // never seen: not known fresh
	// First seen at rest with an old kill move's idle.
	memory.Note(ME, false, DECAP, true);
	CHECK(!memory.Fresh(ME, DECAP));  // the same idle: an old pair's (the victim's case)
	CHECK(memory.Fresh(ME, DRAUGR_KM));
	// Its process takes the next pair's idle a tick before its flag rises: still fresh once the flag is up.
	memory.Note(ME, false, DRAUGR_KM, true);
	CHECK(memory.Fresh(ME, DRAUGR_KM));
	memory.Note(ME, true, DRAUGR_KM, true);
	CHECK(memory.Fresh(ME, DRAUGR_KM));
	// Its flag falls: that idle is at rest now, an old one.
	memory.Note(ME, false, DRAUGR_KM, true);
	CHECK(!memory.Fresh(ME, DRAUGR_KM));
	// An idle that is no pair's taken at rest is at rest: the same kill move again later is fresh.
	memory.Note(ME, false, WALK, false);
	CHECK(memory.Fresh(ME, DRAUGR_KM));
	CHECK(!memory.Fresh(ME, WALK));
	// First seen already in a kill move: not known.
	memory.Note(0x00012345, true, DECAP, true);
	CHECK(!memory.Fresh(0x00012345, DECAP));
	CHECK(memory.Size() == 2u);
	memory.Forget([](std::uint32_t a_id) { return a_id != ME; });
	CHECK(memory.Size() == 1u);
	memory.Clear();
	CHECK(memory.Size() == 0u);
}

TEST("pair rules: event index: the lowest idle of an event, in any case; none for another")
{
	PairRules::EventIndex index;
	index.Add("pa_KillMove1HMDecap", 0x000E1E6D);
	index.Add("PA_KILLMOVE1HMDECAP", 0x000E1000);  // a lower one with the same event
	index.Add("pa_KillMove1HMDecap", 0x000E2000);
	index.Add("", 0x00012345);
	index.Add("pa_VampireFeedFront", 0);
	CHECK(index.Find("pa_killmove1hmdecap") == std::optional<std::uint32_t>{ 0x000E1000u });
	CHECK(!index.Find("pa_VampireFeedFront"));
	CHECK(!index.Find("pa_Other"));
	CHECK(index.Size() == 1u);
	index.Clear();
	CHECK(index.Size() == 0u);
}

TEST("pair rules: inbox: a friend's pair is due when its actors are drawn at its moment, in order; one with no moment goes now; a stale one is dropped")
{
	PairRules::Inbox inbox;
	const auto       now = Steady::now();
	const Pair       a = KillMove(PlayerSide(2), NpcSide(BANDIT));
	const Pair       b = KillMove(NpcSide(DRAGON), PlayerSide(2));
	inbox.Push(b, 2, now + 200ms, now);
	inbox.Push(a, 2, now + 100ms, now);
	inbox.Push(KillMove(PlayerSide(3), NpcSide(WOLF_OF_1)), 3, std::nullopt, now);
	CHECK(inbox.Size() == 3u);
	auto due = inbox.Take(now);
	REQUIRE(due.size() == 1u);  // no moment: due at once
	CHECK(due[0].sender == 3u);
	CHECK(inbox.Take(now + 50ms).empty());
	due = inbox.Take(now + 250ms);
	REQUIRE(due.size() == 2u);
	CHECK(due[0].pair == a);
	CHECK(due[1].pair == b);

	// Waited past STALE: dropped and counted.
	inbox.Push(a, 2, now, now);
	CHECK(inbox.Take(now + PairRules::STALE + 1ms).empty());
	CHECK(inbox.late == 1u);
	// A moment read far ahead never holds the queue up longer than STALE.
	inbox.Push(a, 2, now + 1h, now);
	CHECK(inbox.Take(now + PairRules::STALE).size() == 1u);
	// A player left: theirs go.
	inbox.Push(a, 2, now, now);
	inbox.Push(KillMove(PlayerSide(3), NpcSide(BANDIT)), 3, now, now);
	inbox.Forget(2);
	CHECK(inbox.Size() == 1u);
	inbox.Clear();
	CHECK(inbox.Size() == 0u);
	// At most MAX waiting.
	for (std::size_t i = 0; i < PairRules::Inbox::MAX + 3; ++i) {
		inbox.Push(a, 2, now, now);
	}
	CHECK(inbox.Size() == PairRules::Inbox::MAX);
	CHECK(inbox.dropped == 3u);
}

TEST("pair rules: judge: a stand-in's or a copy's pair plays on a stand-in or a copy, or on an NPC we run when it kills; never ours, the dead, the busy or the far")
{
	CHECK(PairRules::Judge(Playable()) == Verdict::kPlay);
	auto facts = Playable();
	facts.attacker = Shown::kCopy;  // an NPC another game runs (a creature, a dragon, a summon)
	facts.victim = Shown::kStandIn;
	CHECK(PairRules::Judge(facts) == Verdict::kPlay);
	facts = Playable();
	facts.victim = Shown::kRunHere;  // the kill reaches this game as a friend's hit too
	CHECK(PairRules::Judge(facts) == Verdict::kPlay);
	facts.kill = false;  // a feeding on an NPC we run: its AI is ours
	CHECK(PairRules::Judge(facts) == Verdict::kVictimOurs);

	const auto with = [](auto a_change) {
		auto changed = Playable();
		a_change(changed);
		return PairRules::Judge(changed);
	};
	CHECK(with([](auto& f) { f.attacker = Shown::kNone; }) == Verdict::kNoAttacker);
	CHECK(with([](auto& f) { f.attacker = Shown::kOurPlayer; }) == Verdict::kAttackerOurs);
	CHECK(with([](auto& f) { f.attacker = Shown::kRunHere; }) == Verdict::kAttackerOurs);
	CHECK(with([](auto& f) { f.victim = Shown::kNone; }) == Verdict::kNoVictim);
	CHECK(with([](auto& f) { f.victim = Shown::kOurPlayer; }) == Verdict::kVictimOurPlayer);
	CHECK(with([](auto& f) { f.victimAlive = false; }) == Verdict::kDead);
	CHECK(with([](auto& f) { f.attackerAlive = false; }) == Verdict::kDead);
	CHECK(with([](auto& f) { f.attackerBusy = true; }) == Verdict::kBusy);
	CHECK(with([](auto& f) { f.victimBusy = true; }) == Verdict::kBusy);
	CHECK(with([](auto& f) { f.apart = PairRules::PAIR_REACH + 1.0f; }) == Verdict::kFar);
	CHECK(with([](auto& f) { f.drift = PairRules::REACH + 1.0f; }) == Verdict::kFar);
	CHECK(with([](auto& f) { f.drift = std::nanf(""); }) == Verdict::kFar);
	CHECK(with([](auto& f) { f.idleFound = false; }) == Verdict::kNoIdle);
	// No room to hold both (MAX_HELD): refused, never played unheld (its states would pull it apart).
	CHECK(with([](auto& f) { f.full = true; }) == Verdict::kFull);
	CHECK(PairRules::NameOf(Verdict::kFull) == "full");

	PairRules::VerdictCounts counts;
	CHECK(counts.Refused() == "none");
	counts.Add(Verdict::kPlay);
	counts.Add(Verdict::kFar);
	counts.Add(Verdict::kDead);
	counts.Add(Verdict::kDead);
	CHECK(counts.Refused() == "dead:2,far:1");
	CHECK(counts.Of(Verdict::kPlay) == 1u);
	CHECK(PairRules::NameOf(Verdict::kVictimOurPlayer) == "victimOurPlayer");
	CHECK(PairRules::NameOf(Verdict::kCount) == "?");
}

TEST("pair rules: hold: a kill held while in it (at least MIN_HOLD, at most MAX_HOLD), let go at once when it never took; one that kills no one FEED_HOLD")
{
	using PairRules::Hold;
	const auto since = Steady::now();
	// Not yet in it: held while it may still take.
	CHECK(PairRules::HoldStep(since, since + 100ms, true, false, false) == Hold::kHold);
	CHECK(PairRules::HoldStep(since, since + PairRules::TAKE_WITHIN, true, false, false) == Hold::kNoTake);
	// In it: held; out of it, ended once MIN_HOLD passed.
	CHECK(PairRules::HoldStep(since, since + 200ms, true, true, true) == Hold::kHold);
	CHECK(PairRules::HoldStep(since, since + 300ms, true, false, true) == Hold::kHold);
	CHECK(PairRules::HoldStep(since, since + PairRules::MIN_HOLD, true, false, true) == Hold::kEnded);
	CHECK(PairRules::HoldStep(since, since + 5s, true, true, true) == Hold::kHold);
	CHECK(PairRules::HoldStep(since, since + PairRules::MAX_HOLD, true, true, true) == Hold::kGaveUp);
	// One that kills no one (a feeding): no flag to end by.
	CHECK(PairRules::HoldStep(since, since + 5s, false, false, false) == Hold::kHold);
	CHECK(PairRules::HoldStep(since, since + PairRules::FEED_HOLD, false, false, false) == Hold::kEnded);
}
