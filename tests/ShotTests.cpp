#include "Test.h"

#include "FormTranslate.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "game/ShotRules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

// Projectiles (docs/DESIGN.md, "Projectiles"): the shots on the wire, the server passing them on, and the rules a game
// says its own by and launches and ends a friend's by.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Shot = Protocol::Shot;
	using Kind = Protocol::ShotKind;
	using Steady = std::chrono::steady_clock;

	constexpr Protocol::NpcKey SOLDIER{ 0x00012345, 0 };
	constexpr Protocol::NpcKey SPAWN_OF_1{ 0xFF000801, 1 };
	constexpr float            PI = std::numbers::pi_v<float>;

	Shot PlayerShot(std::uint32_t a_player, Kind a_kind = Kind::kArrow)
	{
		Shot shot;
		shot.player = a_player;
		shot.sentMs = 1000;
		shot.projectile = 0x0003BE11;
		shot.weapon = 0x00012EB7;
		shot.ammo = 0x0001397D;
		shot.kind = a_kind;
		shot.x = 100.0f;
		shot.y = -200.0f;
		shot.z = 50.0f;
		shot.pitch = 0.1f;
		shot.heading = 1.5f;
		return shot;
	}

	Shot NpcShot(Protocol::NpcKey a_npc, Kind a_kind = Kind::kMissile)
	{
		Shot shot = PlayerShot(0, a_kind);
		shot.npc = a_npc;
		shot.weapon = 0;
		shot.ammo = 0;
		shot.spell = 0x00012FCD;
		shot.source = 1;
		return shot;
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

	std::vector<Protocol::Shots> ShotsTo(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<Protocol::Shots> out;
		for (auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kShots) {
				CHECK(sent.reliable);
				if (auto msg = Protocol::DecodeShots(sent.data)) {
					out.push_back(std::move(*msg));
				}
			}
		}
		return out;
	}

	std::size_t CountOf(const std::vector<Protocol::Shots>& a_msgs)
	{
		std::size_t n = 0;
		for (const auto& msg : a_msgs) {
			n += msg.shots.size();
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

	bool Near(float a_a, float a_b, float a_tolerance = 1e-3f)
	{
		return std::fabs(a_a - a_b) <= a_tolerance;
	}
}

// ---- the message ----

TEST("shot protocol: shots read back as they were written, as message 57 of protocol 13, and carry forms")
{
	static_assert(Protocol::VERSION >= 13);
	static_assert(static_cast<std::uint8_t>(MT::kShots) == 57);
	Shot held = NpcShot(SPAWN_OF_1, Kind::kFlame);
	held.flags = Protocol::kShotHeld | Protocol::kShotDual;
	held.power = 2.0f;
	held.scale = 1.5f;
	Shot arrow = PlayerShot(2);
	arrow.enchant = 0x0004B0BA;
	arrow.poison = 0x0003A5A4;
	const Protocol::Shots shots{ .sender = 2, .shots = { arrow, NpcShot(SOLDIER), held } };
	CHECK(Protocol::DecodeShots(Protocol::Encode(shots)) == shots);
	CHECK(Protocol::PeekType(Protocol::Encode(shots)) == MT::kShots);
	CHECK(Protocol::DecodeShots(Protocol::Encode(Protocol::Shots{})) == Protocol::Shots{});
	CHECK(FormTranslate::CarriesForms(MT::kShots));
	// Every kind and hand there is.
	for (std::uint8_t kind = 0; kind < Protocol::SHOT_KIND_COUNT; ++kind) {
		for (std::uint8_t source = 0; source < Protocol::SHOT_SOURCE_COUNT; ++source) {
			Shot shot = PlayerShot(1, static_cast<Kind>(kind));
			shot.source = source;
			CHECK(Protocol::ShotHolds(shot));
		}
	}
}

TEST("shot protocol: the most fit; more, a shot of nobody or of two, no record, a bad kind, hand, flag, place, angle, power or scale, or a byte too many are refused")
{
	Protocol::Shots most{ .sender = 1 };
	for (std::size_t i = 0; i < Protocol::MAX_SHOTS; ++i) {
		most.shots.push_back(PlayerShot(1));
	}
	auto bytes = Protocol::Encode(most);
	CHECK(bytes.size() == Protocol::MAX_SHOTS_SIZE);
	CHECK(Protocol::DecodeShots(bytes) == most);
	// A count past MAX_SHOTS (the encoder writes no more than it; the byte after the sender says how many).
	bytes[5] = static_cast<std::uint8_t>(Protocol::MAX_SHOTS + 1);
	CHECK(!Protocol::DecodeShots(bytes));
	// More than MAX_SHOTS given: the first MAX_SHOTS are written.
	most.shots.push_back(PlayerShot(1));
	CHECK(Protocol::DecodeShots(Protocol::Encode(most))->shots.size() == Protocol::MAX_SHOTS);

	const auto refused = [](Shot a_shot) { return !Protocol::DecodeShots(Protocol::Encode(Protocol::Shots{ .sender = 1, .shots = { PlayerShot(1), a_shot } })); };
	CHECK(refused(PlayerShot(0)));  // nobody
	Shot bad = NpcShot({ 0, 0 });
	CHECK(refused(bad));  // an NPC of id 0
	bad = PlayerShot(1);
	bad.projectile = 0;
	CHECK(refused(bad));
	bad = PlayerShot(1);
	bad.kind = static_cast<Kind>(Protocol::SHOT_KIND_COUNT);
	CHECK(refused(bad));
	bad = PlayerShot(1);
	bad.source = Protocol::SHOT_SOURCE_COUNT;
	CHECK(refused(bad));
	bad = PlayerShot(1);
	bad.flags = 4;
	CHECK(refused(bad));
	bad = PlayerShot(1);
	bad.x = std::nanf("");
	CHECK(refused(bad));
	bad = PlayerShot(1);
	bad.z = 1e9f;
	CHECK(refused(bad));
	bad = PlayerShot(1);
	bad.pitch = 8.0f;
	CHECK(refused(bad));
	bad = PlayerShot(1);
	bad.heading = -7.5f;
	CHECK(refused(bad));
	bad = PlayerShot(1);
	bad.power = 11.0f;
	CHECK(refused(bad));
	bad = PlayerShot(1);
	bad.scale = -0.5f;
	CHECK(refused(bad));
	// A player's shot that names an NPC too is read as the player's alone (the key is not written for it).
	Shot both = PlayerShot(1);
	both.npc = SOLDIER;
	const auto read = Protocol::DecodeShots(Protocol::Encode(Protocol::Shots{ .shots = { both } }));
	REQUIRE(read);
	CHECK((read->shots[0].npc == Protocol::NpcKey{}));
	// A byte too many, or one too few.
	auto extra = Protocol::Encode(Protocol::Shots{ .shots = { PlayerShot(1) } });
	extra.push_back(0);
	CHECK(!Protocol::DecodeShots(extra));
	auto shorter = Protocol::Encode(Protocol::Shots{ .shots = { PlayerShot(1) } });
	shorter.pop_back();
	CHECK(!Protocol::DecodeShots(shorter));
}

TEST("shot form translate: a shot whose record or NPC the reader lacks is left out; a source it lacks becomes none, the shot kept")
{
	const Edges            e;
	const Protocol::NpcKey ofExtra{ 0x04000ABC, 0 };  // Extra.esp's: the guest lacks it
	const Protocol::NpcKey ofB{ 0x03000ABC, 0 };      // B.esp's reference in the host's numbering
	const Protocol::NpcKey inGuest{ 0x01000ABC, 0 };

	Shot ofBRecord = PlayerShot(1);
	ofBRecord.projectile = 0x03000123;
	ofBRecord.weapon = 0x04000456;   // Extra.esp's bow: none
	ofBRecord.poison = 0x03000777;   // B.esp's poison: kept
	Shot extraRecord = PlayerShot(1);
	extraRecord.projectile = 0x04000123;
	Shot extraNpc = NpcShot(ofExtra);
	Shot bNpc = NpcShot(ofB);
	Shot spawn = NpcShot(SPAWN_OF_1);
	spawn.spell = 0x04000999;  // Extra.esp's spell: none

	const auto shots = Protocol::DecodeShots(e.HostToGuest(Protocol::Encode(Protocol::Shots{ .sender = 1, .shots = { ofBRecord, extraRecord, extraNpc, bNpc, spawn } })));
	REQUIRE(shots);
	CHECK(shots->sender == 1u);
	REQUIRE(shots->shots.size() == 3u);
	CHECK(shots->shots[0].projectile == 0x01000123u);
	CHECK(shots->shots[0].weapon == 0u);
	CHECK(shots->shots[0].poison == 0x01000777u);
	CHECK(shots->shots[0].ammo == 0x0001397Du);  // Skyrim.esm's, the same everywhere
	CHECK((shots->shots[1].npc == inGuest));
	CHECK((shots->shots[2].npc == SPAWN_OF_1));
	CHECK(shots->shots[2].spell == 0u);
}

// ---- the server ----

TEST("shot server: a player's own shots and those of the NPCs its game runs go to the others with the sender named; any other is dropped")
{
	Fixture f;
	AllIn(f, 3);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { SOLDIER, Protocol::ClaimWhy::kHave } } }));
	REQUIRE(f.fake->Settle());
	for (PeerId peer = 1; peer <= 3; ++peer) {
		(void)f.fake->Take(peer);
	}

	// Bo says his own, Ada's and the soldier's (Ada runs it): only his goes on, named as his.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Shots{ .sender = 99, .shots = { PlayerShot(2), PlayerShot(1), NpcShot(SOLDIER) } }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 1 }, PeerId{ 3 } }) {
		const auto got = ShotsTo(f, peer);
		REQUIRE(got.size() == 1u);
		CHECK(got[0].sender == 2u);
		CHECK(got[0].shots == std::vector<Shot>({ PlayerShot(2) }));
	}
	CHECK(ShotsTo(f, 2).empty());  // not back to its sender

	// Ada's soldier's goes on.
	f.fake->Deliver(1, Protocol::Encode(Protocol::Shots{ .shots = { NpcShot(SOLDIER) } }));
	REQUIRE(f.fake->Settle());
	const auto got = ShotsTo(f, 2);
	REQUIRE(got.size() == 1u);
	CHECK(got[0].sender == 1u);
	CHECK(got[0].shots == std::vector<Shot>({ NpcShot(SOLDIER) }));

	// None of the sender's: nothing goes.
	f.fake->Deliver(3, Protocol::Encode(Protocol::Shots{ .shots = { NpcShot(SOLDIER) } }));
	REQUIRE(f.fake->Settle());
	CHECK(ShotsTo(f, 1).empty());
	CHECK(ShotsTo(f, 2).empty());
}

TEST("shot server: past a burst of 96 a player's shots are dropped, and the log says so once")
{
	Fixture f;
	AllIn(f, 2);
	Protocol::Shots full{};
	for (std::size_t i = 0; i < Protocol::MAX_SHOTS; ++i) {
		full.shots.push_back(PlayerShot(1));
	}
	for (int i = 0; i < 6; ++i) {
		f.fake->Deliver(1, Protocol::Encode(full));
	}
	REQUIRE(f.fake->Settle());
	const auto passed = CountOf(ShotsTo(f, 2));
	CHECK(passed >= 96u);
	CHECK(passed < 6u * Protocol::MAX_SHOTS);
	CHECK(f.LoggedSoon("sends shots faster than they are passed on"));
	const auto lines = f.LogLines();
	CHECK(std::ranges::count_if(lines, [](const std::string& a_line) { return a_line.find("sends shots faster") != std::string::npos; }) == 1);
}

// ---- the rules ----

TEST("shot rules: a record's kind by its type bits; names both ways")
{
	CHECK(ShotRules::KindOf(ShotRules::TYPE_ARROW) == Kind::kArrow);
	CHECK(ShotRules::KindOf(ShotRules::TYPE_MISSILE) == Kind::kMissile);
	CHECK(ShotRules::KindOf(ShotRules::TYPE_LOBBER) == Kind::kLobber);
	CHECK(ShotRules::KindOf(ShotRules::TYPE_BEAM) == Kind::kBeam);
	CHECK(ShotRules::KindOf(ShotRules::TYPE_FLAME) == Kind::kFlame);
	CHECK(ShotRules::KindOf(ShotRules::TYPE_CONE) == Kind::kCone);
	CHECK(ShotRules::KindOf(ShotRules::TYPE_BARRIER) == Kind::kBarrier);
	CHECK(ShotRules::KindOf(0) == Kind::kOther);
	CHECK(ShotRules::KindOf(ShotRules::TYPE_MISSILE | ShotRules::TYPE_ARROW) == Kind::kArrow);
	CHECK(ShotRules::KindOf(ShotRules::TYPE_MISSILE | ShotRules::TYPE_LOBBER) == Kind::kLobber);
	for (std::uint8_t i = 0; i < Protocol::SHOT_KIND_COUNT; ++i) {
		const auto kind = static_cast<Kind>(i);
		CHECK(ShotRules::KindNamed(ShotRules::NameOf(kind)) == kind);
	}
	CHECK(!ShotRules::KindNamed("bolt"));
	ShotRules::KindCounts counts;
	CHECK(counts.Line() == "none");
	counts.Add(Kind::kArrow);
	counts.Add(Kind::kArrow);
	counts.Add(Kind::kFlame);
	CHECK(counts.Line() == "arrow:2,flame:1");
	CHECK(counts.Total() == 3u);
	CHECK(counts.Of(Kind::kArrow) == 2u);
}

TEST("shot rules: aim: a way from its angles and back, the error of a launch off it, and the velocity it is turned to")
{
	const auto north = ShotRules::Direction(0.0f, 0.0f);
	CHECK(Near(north[0], 0.0f));
	CHECK(Near(north[1], 1.0f));
	CHECK(Near(north[2], 0.0f));
	const auto east = ShotRules::Direction(0.0f, PI / 2.0f);
	CHECK(Near(east[0], 1.0f));
	CHECK(Near(east[1], 0.0f));
	const auto down = ShotRules::Direction(PI / 4.0f, 0.0f);  // a pitch above 0 looks down
	CHECK(down[2] < 0.0f);

	for (const auto [pitch, heading] : { std::pair{ 0.2f, 0.5f }, std::pair{ -0.4f, 3.0f }, std::pair{ 0.0f, 5.5f } }) {
		const auto way = ShotRules::Direction(pitch, heading);
		const auto angles = ShotRules::AnglesOf(way[0] * 3000.0f, way[1] * 3000.0f, way[2] * 3000.0f);
		REQUIRE(angles);
		CHECK(Near(angles->first, pitch));
		CHECK(Near(angles->second, heading));
		const auto error = ShotRules::AimError(way[0] * 3000.0f, way[1] * 3000.0f, way[2] * 3000.0f, pitch, heading);
		REQUIRE(error);
		CHECK(*error < 0.1f);
		CHECK(!ShotRules::Reaimed(way[0] * 3000.0f, way[1] * 3000.0f, way[2] * 3000.0f, pitch, heading));
	}
	CHECK(!ShotRules::AnglesOf(0.0f, 0.0f, 0.0f));
	CHECK(!ShotRules::AimError(0.0f, 0.0f, 0.0f, 0.0f, 0.0f));  // a beam: no velocity to read

	// The engine aimed it east; the shooter aimed north: 90 degrees off, turned north at the same speed.
	const auto error = ShotRules::AimError(2000.0f, 0.0f, 0.0f, 0.0f, 0.0f);
	REQUIRE(error);
	CHECK(Near(*error, 90.0f, 0.01f));
	const auto turned = ShotRules::Reaimed(2000.0f, 0.0f, 0.0f, 0.0f, 0.0f);
	REQUIRE(turned);
	CHECK(Near((*turned)[0], 0.0f, 0.01f));
	CHECK(Near((*turned)[1], 2000.0f, 0.01f));
	// Within AIM_FIX: left as it flies.
	const auto slight = ShotRules::Direction(0.0f, 0.01f);  // about 0.6 degrees
	CHECK(!ShotRules::Reaimed(slight[0] * 1000.0f, slight[1] * 1000.0f, slight[2] * 1000.0f, 0.0f, 0.0f));
}

TEST("shot rules: a friend's shot is launched from the actor that shows its shooter only near where it left; a big actor's reach is wider")
{
	CHECK(ShotRules::Launchable(0.0f, 128.0f));
	CHECK(ShotRules::Launchable(ShotRules::REACH, 0.0f));
	CHECK(!ShotRules::Launchable(ShotRules::REACH + 1.0f, 0.0f));
	CHECK(ShotRules::Launchable(ShotRules::REACH + 500.0f, 400.0f));  // a dragon's breath leaves its mouth
	CHECK(!ShotRules::Launchable(std::nanf(""), 128.0f));
	CHECK(!ShotRules::Launchable(ShotRules::REACH + 1.0f, -50.0f));
}

TEST("shot rules: outbox: each shot is said; a held stream at most once per gap per shooter and hand; past the budget dropped; at most MAX_SHOTS a message")
{
	ShotRules::Outbox outbox;
	const auto        t0 = Steady::now();
	Shot              held = PlayerShot(1, Kind::kFlame);
	held.flags = Protocol::kShotHeld;
	held.source = 0;
	CHECK(outbox.Seen(held, t0));
	CHECK(!outbox.Seen(held, t0 + 20ms));  // the same stream, too soon
	Shot otherHand = held;
	otherHand.source = 1;
	CHECK(outbox.Seen(otherHand, t0 + 20ms));  // the other hand is its own stream
	Shot npcHeld = NpcShot(SOLDIER, Kind::kFlame);
	npcHeld.flags = Protocol::kShotHeld;
	CHECK(outbox.Seen(npcHeld, t0 + 20ms));  // another shooter
	CHECK(outbox.Seen(held, t0 + ShotRules::HELD_GAP + 1ms));
	CHECK(outbox.Seen(PlayerShot(1), t0 + 21ms));  // fire and forget: never coalesced
	CHECK(outbox.Seen(PlayerShot(1), t0 + 21ms));
	CHECK(outbox.coalesced == 1u);
	CHECK(outbox.byKind.Of(Kind::kFlame) == 4u);
	CHECK(outbox.byKind.Of(Kind::kArrow) == 2u);
	auto taken = outbox.Take();
	CHECK(taken.size() == 6u);
	CHECK(outbox.sent == 6u);
	CHECK(outbox.Take().empty());

	// The budget: a burst, then dropped.
	ShotRules::Outbox flood;
	std::size_t       kept = 0;
	for (int i = 0; i < 200; ++i) {
		kept += flood.Seen(PlayerShot(1), t0) ? 1 : 0;
	}
	CHECK(kept == static_cast<std::size_t>(ShotRules::Outbox::BURST));
	CHECK(flood.dropped == 200u - kept);
	CHECK(flood.Take().size() == Protocol::MAX_SHOTS);  // the rest wait for the next message
	CHECK(flood.Waiting() == kept - Protocol::MAX_SHOTS);
	CHECK(flood.Seen(PlayerShot(1), t0 + 1s));  // the budget fills again
	flood.Clear();
	CHECK(flood.Waiting() == 0u);
}

TEST("shot rules: first sight: new only at a projectile's first update while it is young; a new one in a freed one's memory is new")
{
	ShotRules::FirstSight sight;
	const auto            t0 = Steady::now();
	CHECK(sight.New(0x1000, 0xFF000001, 0.0f, t0));
	CHECK(!sight.New(0x1000, 0xFF000001, 0.016f, t0));
	CHECK(sight.New(0x1000, 0xFF000002, 0.0f, t0));  // the same memory, another projectile
	CHECK(!sight.New(0x2000, 0xFF000003, 5.0f, t0));  // an arrow in a wall, seen for the first time: not new
	CHECK(!sight.New(0x2000, 0xFF000003, 0.0f, t0));  // and never after
	CHECK(!sight.New(0x3000, 0xFF000004, std::nanf(""), t0));
	// Full: the oldest go first.
	ShotRules::FirstSight full;
	for (std::uintptr_t i = 0; i < ShotRules::FirstSight::MAX + 10; ++i) {
		(void)full.New(0x10000 + i, 0xFF000000 + static_cast<std::uint32_t>(i), 0.0f, t0 + std::chrono::milliseconds(i));
	}
	CHECK(full.Size() <= ShotRules::FirstSight::MAX);
	CHECK(full.New(0x10000, 0xFF000000, 0.0f, t0 + 2s));  // the first, forgotten
	full.Clear();
	CHECK(full.Size() == 0u);
}

TEST("shot rules: judge: a launch of ours flies; a stand-in's or a moved copy's own is ended; an actor's is for the main thread to say; the world's is left")
{
	using ShotRules::Fate;
	CHECK(ShotRules::Judge({ .actor = true, .standIn = true, .movedCopy = false, .launchedByUs = true }) == Fate::kOurs);
	CHECK(ShotRules::Judge({ .actor = true, .standIn = true, .movedCopy = false, .launchedByUs = false }) == Fate::kEnd);
	CHECK(ShotRules::Judge({ .actor = true, .standIn = false, .movedCopy = true, .launchedByUs = false }) == Fate::kEnd);
	CHECK(ShotRules::Judge({ .actor = true, .standIn = false, .movedCopy = false, .launchedByUs = false }) == Fate::kSay);
	CHECK(ShotRules::Judge({ .actor = false, .standIn = false, .movedCopy = false, .launchedByUs = false }) == Fate::kLeave);
	CHECK(ShotRules::Judge({ .actor = false, .standIn = false, .movedCopy = false, .launchedByUs = true }) == Fate::kOurs);
}

TEST("shot rules: pending: a launch of ours is told by its shooter, record and origin, once, for a short while; taken back when the launch returns")
{
	ShotRules::Pending pending;
	const auto         t0 = Steady::now();
	const auto         token = pending.Add(0xA000, 0x0003BE11, 10.0f, 20.0f, 30.0f, t0);
	CHECK(!pending.Match(0xB000, 0x0003BE11, 10.0f, 20.0f, 30.0f, t0));  // another shooter
	CHECK(!pending.Match(0xA000, 0x0003BE12, 10.0f, 20.0f, 30.0f, t0));  // another record
	CHECK(!pending.Match(0xA000, 0x0003BE11, 10.0f, 20.0f, 30.0f + ShotRules::PENDING_REACH + 1.0f, t0));  // moved off
	CHECK(pending.Match(0xA000, 0x0003BE11, 11.0f, 20.0f, 30.0f, t0 + 10ms));
	CHECK(!pending.Match(0xA000, 0x0003BE11, 10.0f, 20.0f, 30.0f, t0 + 10ms));  // once
	CHECK(!pending.Cancel(token));  // already matched

	const auto second = pending.Add(0xA000, 0x0003BE11, 10.0f, 20.0f, 30.0f, t0);
	CHECK(pending.Cancel(second));  // returned before its first update: taken back
	CHECK(!pending.Match(0xA000, 0x0003BE11, 10.0f, 20.0f, 30.0f, t0));  // and no native takes its place

	(void)pending.Add(0xA000, 0x0003BE11, 10.0f, 20.0f, 30.0f, t0);
	CHECK(!pending.Match(0xA000, 0x0003BE11, 10.0f, 20.0f, 30.0f, t0 + ShotRules::PENDING_FOR + 1ms));  // too late
	pending.Clear();
	CHECK(pending.Size() == 0u);
}

TEST("shot rules: inbox: a friend's shot is due when its shooter is drawn at its moment, in order; one with no moment goes after the delay; a stale one is dropped")
{
	ShotRules::Inbox inbox;
	const auto       t0 = Steady::now();
	Shot             a = PlayerShot(1);
	a.sentMs = 1;
	Shot b = PlayerShot(1);
	b.sentMs = 2;
	Shot c = PlayerShot(1);
	c.sentMs = 3;
	inbox.Push(b, t0 + 20ms, t0);
	inbox.Push(a, t0 + 10ms, t0);
	inbox.Push(c, std::nullopt, t0 + 30ms);
	CHECK(inbox.Due(t0).empty());
	auto due = inbox.Due(t0 + 20ms);
	REQUIRE(due.size() == 2u);
	CHECK(due[0].sentMs == 1u);
	CHECK(due[1].sentMs == 2u);
	CHECK(inbox.Due(t0 + 30ms).size() == 1u);
	// Stale.
	inbox.Push(a, t0, t0);
	CHECK(inbox.Due(t0 + ShotRules::STALE + 1ms).empty());
	CHECK(inbox.late == 1u);
	// A moment read far in the future holds nothing up longer than STALE.
	inbox.Push(a, t0 + 1h, t0);
	CHECK(inbox.Due(t0 + ShotRules::STALE).size() == 1u);
	// Full: the oldest dropped.
	for (std::size_t i = 0; i < ShotRules::Inbox::MAX + 5; ++i) {
		inbox.Push(a, t0, t0);
	}
	CHECK(inbox.Size() == ShotRules::Inbox::MAX);
	CHECK(inbox.dropped == 5u);
	inbox.Clear();
	CHECK(inbox.Size() == 0u);
}

TEST("shot rules: live: an arrow of ours that stopped ends after the dwell; a rune after its life; any other after MAX_LIFE; at most MAX_LIVE; all at a save")
{
	ShotRules::Live live;
	const auto      t0 = Steady::now();
	live.Add(1, Kind::kArrow, 0.0f, t0);
	live.Add(2, Kind::kMissile, 0.0f, t0);
	live.Add(3, Kind::kLobber, 20.0f, t0);  // a rune whose record lasts 20 s
	live.Add(4, Kind::kLobber, 0.0f, t0);   // one whose record says none
	live.Add(5, Kind::kArrow, 0.0f, t0);
	CHECK(live.Count() == 5u);
	CHECK(live.Count(Kind::kArrow) == 2u);

	CHECK(!live.Step(1, false, false, t0 + 100ms));  // flying
	CHECK(!live.Step(1, false, true, t0 + 1s));      // stuck in a wall
	CHECK(!live.Step(1, false, true, t0 + 2s));
	CHECK(live.Step(1, false, true, t0 + 1s + ShotRules::DWELL));
	CHECK(live.endedStuck == 1u);
	CHECK(!live.Step(1, false, true, t0 + 10s));  // forgotten

	CHECK(!live.Step(5, true, false, t0 + 1s));  // it hit: the game ended it
	CHECK(live.gone == 1u);

	CHECK(!live.Step(2, false, true, t0 + 5s));  // a missile that stopped is no arrow: it lies until its life
	CHECK(live.Step(2, false, false, t0 + ShotRules::MAX_LIFE));
	CHECK(!live.Step(3, false, false, t0 + 19s));
	CHECK(live.Step(3, false, false, t0 + 20s));
	CHECK(!live.Step(4, false, false, t0 + 29s));
	CHECK(live.Step(4, false, false, t0 + ShotRules::LOBBER_LIFE));
	CHECK(live.endedOld == 3u);
	CHECK(live.Count() == 0u);

	// A rune whose record is shorter than MAX_LIFE still lies MAX_LIFE.
	live.Add(6, Kind::kLobber, 2.0f, t0);
	CHECK(!live.Step(6, false, false, t0 + 5s));
	CHECK(live.Step(6, false, false, t0 + ShotRules::MAX_LIFE));

	for (std::uint32_t id = 100; live.Room(); ++id) {
		live.Add(id, Kind::kMissile, 0.0f, t0);
	}
	CHECK(live.Count() == ShotRules::MAX_LIVE);
	CHECK(live.Ids().size() == ShotRules::MAX_LIVE);
	CHECK(live.TakeAll().size() == ShotRules::MAX_LIVE);
	CHECK(live.Count() == 0u);
	CHECK(live.Room());
}
