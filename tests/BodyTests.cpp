#include "Test.h"

#include "BodyBook.h"
#include "FormTranslate.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "game/BodyRules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

// Bodies of the dead (docs/DESIGN.md, "Bodies"): falls and rests on the wire, the server's book of them, and the game's
// rules: bone names, the bones sent, settling, laying a copy, and what waits.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Body = Protocol::Body;
	using Bone = Protocol::Bone;
	using Kind = Protocol::BodyKind;
	using Steady = std::chrono::steady_clock;
	using BodyBook::Verdict;

	constexpr Protocol::NpcKey SOLDIER{ 0x00012345, 0 };
	constexpr Protocol::NpcKey INNKEEPER{ 0x00012346, 0 };
	constexpr std::uint32_t    CELL = 0x0001C0DE;
	constexpr std::uint32_t    TAMRIEL = 0x0000003C;

	Bone BoneAt(std::string_view a_name, float a_x, float a_y = -2000.0f, float a_z = 64.0f)
	{
		Bone bone;
		bone.name = BodyRules::NameHash(a_name);
		bone.x = a_x;
		bone.y = a_y;
		bone.z = a_z;
		const auto s = std::sin(0.2f);
		bone.rot = { 0.0f, 0.0f, s, std::cos(0.2f) };
		return bone;
	}

	std::vector<Bone> Skeleton(float a_x = 1000.0f)
	{
		return { BoneAt("NPC Pelvis [Pelv]", a_x), BoneAt("NPC Spine [Spn0]", a_x + 10.0f), BoneAt("NPC Head [Head]", a_x + 40.0f),
			BoneAt("NPC L Thigh [LThg]", a_x - 10.0f, -1990.0f), BoneAt("NPC R Thigh [RThg]", a_x - 10.0f, -2010.0f) };
	}

	Body Rest(Protocol::NpcKey a_key = SOLDIER, float a_x = 1000.0f)
	{
		auto body = BodyRules::RestOf(a_key, CELL, 0, { a_x, -2000.0f, 60.0f }, Skeleton(a_x));
		REQUIRE(body);
		return *body;
	}

	Body Fall(Protocol::NpcKey a_key = SOLDIER)
	{
		return BodyRules::FallOf(a_key, CELL, 0, { 1000.0f, -2000.0f, 60.0f }, { 200.0f, -50.0f, 300.0f });
	}

	using Names = std::vector<std::string>;

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

	std::vector<Protocol::Bodies> BodiesTo(Fixture& a_f, PeerId a_peer)
	{
		std::vector<Protocol::Bodies> out;
		for (auto& sent : a_f.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kBodies) {
				CHECK(sent.reliable);
				if (auto msg = Protocol::DecodeBodies(sent.data)) {
					out.push_back(std::move(*msg));
				}
			}
		}
		return out;
	}
}

// ---- the message ----

TEST("body protocol: falls and rests read back as written, as message 68 of protocol 18, and carry forms")
{
	static_assert(static_cast<int>(MT::kBodies) == 68);
	static_assert(Protocol::VERSION >= 18);
	auto outside = Rest(INNKEEPER);
	outside.cell = 0;
	outside.worldspace = TAMRIEL;
	const Protocol::Bodies bodies{ .sender = 2, .full = true, .bodies = { Fall(), Rest(), outside } };
	CHECK(Protocol::DecodeBodies(Protocol::Encode(bodies)) == bodies);
	CHECK(Protocol::PeekType(Protocol::Encode(bodies)) == MT::kBodies);
	CHECK(Protocol::DecodeBodies(Protocol::Encode(Protocol::Bodies{})) == Protocol::Bodies{});
	CHECK(FormTranslate::CarriesForms(MT::kBodies));

	// A fall's bones and a rest's velocity, left in by the caller, are not written.
	auto fallWithBones = Fall();
	fallWithBones.bones = Skeleton();
	auto restMoving = Rest();
	restMoving.vx = 10.0f;
	const auto read = Protocol::DecodeBodies(Protocol::Encode(Protocol::Bodies{ .bodies = { fallWithBones, restMoving } }));
	REQUIRE(read);
	CHECK(read->bodies[0].bones.empty());
	CHECK(read->bodies[0].vx == 200.0f);
	CHECK(read->bodies[1].vx == 0.0f);
	CHECK(read->bodies[1].bones == Skeleton());
}

TEST("body protocol: the longest fits; no NPC, a bad kind, no bones, a bone unnamed or far off, too many, a fall too fast or a bad count are refused")
{
	Protocol::Bodies many;
	for (std::uint32_t i = 0; i < Protocol::MAX_BODIES; ++i) {
		auto body = Rest({ 0x00100000 + i, 0 });
		body.bones.clear();
		for (std::uint32_t b = 0; b < Protocol::MAX_BONES; ++b) {
			body.bones.push_back(BoneAt("bone " + std::to_string(b), 1000.0f + static_cast<float>(b)));
		}
		many.bodies.push_back(body);
	}
	CHECK(Protocol::Encode(many).size() == Protocol::MAX_BODIES_SIZE);
	CHECK(Protocol::DecodeBodies(Protocol::Encode(many)) == many);

	auto noNpc = Rest();
	noNpc.key = {};
	CHECK(!Protocol::BodyHolds(noNpc));
	auto noBones = Rest();
	noBones.bones.clear();
	CHECK(!Protocol::BodyHolds(noBones));
	auto unnamed = Rest();
	unnamed.bones[1].name = 0;
	CHECK(!Protocol::BodyHolds(unnamed));
	auto farBone = Rest();
	farBone.bones[2].x += Protocol::MAX_BONE_REACH + 100.0f;
	CHECK(!Protocol::BodyHolds(farBone));
	CHECK(!Protocol::DecodeBodies(Protocol::Encode(Protocol::Bodies{ .bodies = { farBone } })));
	auto badTurn = Rest();
	badTurn.bones[0].rot = { 0.0f, 0.0f, 0.0f, 0.0f };
	CHECK(!Protocol::DecodeBodies(Protocol::Encode(Protocol::Bodies{ .bodies = { badTurn } })));
	auto tooFast = Fall();
	tooFast.vz = Protocol::MAX_OBJECT_SPEED * 2.0f;
	CHECK(!Protocol::DecodeBodies(Protocol::Encode(Protocol::Bodies{ .bodies = { tooFast } })));
	CHECK(Protocol::BodyHolds(BodyRules::FallOf(SOLDIER, CELL, 0, {}, { 0.0f, 0.0f, Protocol::MAX_OBJECT_SPEED * 2.0f })));  // the builder caps it

	constexpr std::size_t FIRST = 1 + 4 + 1 + 1;  // type, sender, full, count
	auto badKind = Protocol::Encode(Protocol::Bodies{ .bodies = { Fall() } });
	badKind[FIRST + 8] = Protocol::BODY_KIND_COUNT;
	CHECK(!Protocol::DecodeBodies(badKind));
	auto badFull = Protocol::Encode(Protocol::Bodies{ .bodies = { Fall() } });
	badFull[5] = 2;
	CHECK(!Protocol::DecodeBodies(badFull));
	auto tooMany = Protocol::Encode(Protocol::Bodies{});
	tooMany[6] = static_cast<std::uint8_t>(Protocol::MAX_BODIES + 1);
	CHECK(!Protocol::DecodeBodies(tooMany));
	auto tooManyBones = Protocol::Encode(Protocol::Bodies{ .bodies = { Fall() } });
	tooManyBones.back() = static_cast<std::uint8_t>(Protocol::MAX_BONES + 1);
	CHECK(!Protocol::DecodeBodies(tooManyBones));
	auto shortOne = Protocol::Encode(Protocol::Bodies{ .bodies = { Rest() } });
	shortOne.pop_back();
	CHECK(!Protocol::DecodeBodies(shortOne));
	auto longOne = Protocol::Encode(Protocol::Bodies{ .bodies = { Rest() } });
	longOne.push_back(0);
	CHECK(!Protocol::DecodeBodies(longOne));
}

TEST("body form translate: an NPC, cell or worldspace the reader lacks leaves the body out; bone names are kept")
{
	const Edges         e;
	const std::uint32_t ofB = 0x03000ABC;
	const std::uint32_t ofExtra = 0x04000ABC;
	const std::uint32_t inGuest = 0x01000ABC;

	auto inBCell = Rest({ ofB, 0 });
	inBCell.cell = ofB;
	auto inExtraCell = Rest();
	inExtraCell.cell = ofExtra;
	const auto read = Protocol::DecodeBodies(e.HostToGuest(Protocol::Encode(Protocol::Bodies{ .sender = 1, .bodies = { inBCell, Rest({ ofExtra, 0 }), inExtraCell, Fall() } })));
	REQUIRE(read);
	auto expected = Rest({ inGuest, 0 });
	expected.cell = inGuest;
	CHECK(read->bodies == std::vector<Body>({ expected, Fall() }));
}

// ---- the server's book ----

TEST("body book: only the runner's word (or anyone's while nobody runs it), only of the dead; a fall passes, a rest is kept; the raised are forgotten; the oldest go past MAX_KEPT")
{
	BodyBook::Book book;
	const auto     t = Steady::now();
	CHECK(book.Take(0, Rest(), true, true, t) == Verdict::kRefused);
	auto noNpc = Rest();
	noNpc.key = {};
	CHECK(book.Take(1, noNpc, true, true, t) == Verdict::kRefused);
	CHECK(book.Take(2, Rest(), false, true, t) == Verdict::kNotRun);
	CHECK(book.Take(1, Rest(), true, false, t) == Verdict::kAlive);
	CHECK(book.Take(1, Fall(), true, true, t) == Verdict::kPass);
	CHECK(book.Size() == 0u);
	CHECK(book.Take(1, Rest(), true, true, t) == Verdict::kPass);
	CHECK(book.Take(1, Rest(SOLDIER, 1200.0f), true, true, t + 1s) == Verdict::kPass);  // laid again: the newest stands
	CHECK(book.Take(2, Rest(INNKEEPER), true, true, t + 2s) == Verdict::kPass);
	CHECK(book.Size() == 2u);
	const auto all = book.All([](const Protocol::NpcKey&) { return true; });
	REQUIRE(all.size() == 2u);
	CHECK(all[0] == Rest(SOLDIER, 1200.0f));
	CHECK(all[1] == Rest(INNKEEPER));
	// The innkeeper is raised: never sent again.
	CHECK(book.All([](const Protocol::NpcKey& a_key) { return a_key == SOLDIER; }) == std::vector<Body>({ Rest(SOLDIER, 1200.0f) }));
	CHECK(book.Size() == 1u);

	for (std::uint32_t i = 0; i < BodyBook::MAX_KEPT; ++i) {
		(void)book.Take(1, Rest({ 0x00200000 + i, 0 }), true, true, t + 10s + std::chrono::milliseconds(i));
	}
	CHECK(book.Size() == BodyBook::MAX_KEPT);
	CHECK(book.counts.forgotten == 1u);
	CHECK(book.All([](const Protocol::NpcKey& a_key) { return a_key == SOLDIER; }).empty());  // the soldier went first
	book.Clear();
	CHECK(book.Size() == 0u);
}

TEST("body server: a runner's fall and rest go to the others with its sender named; another game's, or an alive NPC's, do not; joiners and askers get every body")
{
	Fixture f;
	AllIn(f, 2);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { SOLDIER, Protocol::ClaimWhy::kHave }, { INNKEEPER, Protocol::ClaimWhy::kHave } } }));
	REQUIRE(f.fake->Settle());
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcDeaths{ .deaths = { { SOLDIER, 0, 0 } } }));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	// Bo does not run the soldier; nobody has the innkeeper dead.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Bodies{ .bodies = { Fall(), Rest() } }));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Bodies{ .bodies = { Rest(INNKEEPER) } }));
	REQUIRE(f.fake->Settle());
	CHECK(BodiesTo(f, 1).empty());
	CHECK(BodiesTo(f, 2).empty());

	f.fake->Deliver(1, Protocol::Encode(Protocol::Bodies{ .sender = 99, .bodies = { Fall() } }));
	f.fake->Deliver(1, Protocol::Encode(Protocol::Bodies{ .bodies = { Rest() } }));
	REQUIRE(f.fake->Settle());
	CHECK(BodiesTo(f, 1).empty());
	const auto toBo = BodiesTo(f, 2);
	REQUIRE(toBo.size() == 2u);
	CHECK(toBo[0].sender == 1u);
	CHECK(toBo[0].bodies == std::vector<Body>({ Fall() }));
	CHECK(toBo[1].bodies == std::vector<Body>({ Rest() }));

	// A full message from a game is never passed on.
	f.fake->Deliver(1, Protocol::Encode(Protocol::Bodies{ .full = true, .bodies = { Rest() } }));
	REQUIRE(f.fake->Settle());
	CHECK(BodiesTo(f, 2).empty());

	// Cy joins: the soldier's body, as the session's full list; asked again after a load, the same.
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto toCy = BodiesTo(f, 3);
	REQUIRE(toCy.size() == 1u);
	CHECK(toCy[0].full);
	CHECK(toCy[0].sender == 0u);
	CHECK(toCy[0].bodies == std::vector<Body>({ Rest() }));
	f.fake->Deliver(3, Protocol::EncodeAskObjectRests());
	REQUIRE(f.fake->Settle());
	const auto again = BodiesTo(f, 3);
	REQUIRE(again.size() == 1u);
	CHECK(again[0].full);
	CHECK(again[0].bodies.size() == 1u);
}

// ---- the game's rules ----

TEST("body names: a hash of the node's name whatever its case, never 0; different bones differ")
{
	static_assert(BodyRules::NameHash("NPC Pelvis [Pelv]") == BodyRules::NameHash("npc pelvis [pelv]"));
	CHECK(BodyRules::NameHash("NPC Pelvis [Pelv]") != BodyRules::NameHash("NPC Spine [Spn0]"));
	CHECK(BodyRules::NameHash("") != 0u);
	CHECK(BodyRules::NameHash("NPC Head [Head]") != 0u);
}

TEST("body bones: each name once, finite, at most MAX_BONES, the pelvis kept first when there are too many")
{
	std::vector<Bone> bones;
	for (std::uint32_t b = 0; b < Protocol::MAX_BONES + 8; ++b) {
		bones.push_back(BoneAt("bone " + std::to_string(b), 1000.0f));
	}
	bones.push_back(BoneAt("NPC Pelvis [Pelv]", 1000.0f));
	bones.push_back(BoneAt("bone 3", 1500.0f));  // a second of a name
	auto nan = BoneAt("bad", 1000.0f);
	nan.y = std::nanf("");
	bones.insert(bones.begin(), nan);
	const auto picked = BodyRules::Pick(bones, BodyRules::NameHash("NPC Pelvis [Pelv]"));
	REQUIRE(picked.size() == Protocol::MAX_BONES);
	CHECK(picked.front().name == BodyRules::NameHash("NPC Pelvis [Pelv]"));
	CHECK(picked[1].name == BodyRules::NameHash("bone 0"));
	CHECK(std::count_if(picked.begin(), picked.end(), [](const Bone& a_bone) { return a_bone.name == BodyRules::NameHash("bone 3"); }) == 1);
	CHECK(std::none_of(picked.begin(), picked.end(), [](const Bone& a_bone) { return a_bone.name == BodyRules::NameHash("bad"); }));

	CHECK(!BodyRules::RestOf(SOLDIER, CELL, 0, { 1000.0f, 0.0f, 0.0f }, {}));  // no bones: nothing said
	CHECK(!BodyRules::RestOf(SOLDIER, CELL, 0, { 1000.0f + 2.0f * Protocol::MAX_BONE_REACH, -2000.0f, 0.0f }, Skeleton()));
	CHECK(BodyRules::RestOf(SOLDIER, CELL, 0, { 1000.0f, -2000.0f, 0.0f }, Skeleton()));
}

TEST("body laying: a copy more than CORPSE_SLACK off is laid; a closer one, or one with no bone named in both, is left")
{
	const auto there = Skeleton(1000.0f);
	CHECK(!BodyRules::NeedsLaying(Skeleton(1000.0f + BodyRules::CORPSE_SLACK - 1.0f), there));
	CHECK(BodyRules::NeedsLaying(Skeleton(1000.0f + BodyRules::CORPSE_SLACK + 1.0f), there));
	auto oneLimb = Skeleton(1000.0f);
	oneLimb[4].z += 200.0f;  // a leg left up a slope
	CHECK(BodyRules::NeedsLaying(oneLimb, there));
	CHECK(!BodyRules::NeedsLaying({ BoneAt("other", 5000.0f) }, there));
	CHECK(!BodyRules::Farthest({}, there));
	CHECK(*BodyRules::Farthest(Skeleton(1010.0f), there) == 10.0f);
}

TEST("body settle: at rest once no bone moved MOVE_EPSILON for STILL_FOR, never before MIN_FALL; a body that slides for ever is given up on")
{
	const auto        t = Steady::now();
	BodyRules::Settle settle{ Skeleton(1000.0f), t };
	CHECK(!settle.Look(Skeleton(1040.0f), t + 250ms));
	CHECK(!settle.Look(Skeleton(1060.0f), t + 500ms));
	CHECK(!settle.Look(Skeleton(1061.0f), t + 1000ms));  // under MOVE_EPSILON
	CHECK(settle.Look(Skeleton(1061.5f), t + 1500ms));
	CHECK(!settle.GaveUp());
	CHECK(settle.Bones() == Skeleton(1060.0f));

	BodyRules::Settle lying{ Skeleton(), t };
	CHECK(!lying.Look(Skeleton(), t + 250ms));
	CHECK(lying.Look(Skeleton(), t + BodyRules::STILL_FOR));

	BodyRules::Settle sliding{ Skeleton(), t };
	bool rested = false;
	for (int i = 1; i <= 60 && !rested; ++i) {
		rested = sliding.Look(Skeleton(1000.0f + 5.0f * static_cast<float>(i)), t + std::chrono::milliseconds(250 * i));
	}
	CHECK(rested);
	CHECK(sliding.GaveUp());

	// A bone that comes or goes (its 3D changed) is a move.
	BodyRules::Settle changing{ Skeleton(), t };
	auto              fewer = Skeleton();
	fewer.pop_back();
	CHECK(!changing.Look(fewer, t + 900ms));
	CHECK(!changing.Look(fewer, t + 1500ms));
	CHECK(changing.Look(fewer, t + 1900ms));
}

TEST("body waiting: the newest rest of each body waits until laid; a fall waits FALL_WAIT for its death; at most MAX_WAITING rests")
{
	const auto         t = Steady::now();
	BodyRules::Waiting waiting;
	waiting.Rest(Rest());
	waiting.Rest(Rest(SOLDIER, 1300.0f));
	waiting.Rest(Rest(INNKEEPER));
	CHECK(waiting.Rests() == 2u);
	float      laidAt = 0.0f;
	const auto layOnlySoldier = [&](const Body& a_body) {
		if (a_body.key == SOLDIER) {
			laidAt = a_body.x;
			return true;
		}
		return false;
	};
	CHECK(waiting.TryRests(layOnlySoldier) == 1u);
	CHECK(laidAt == 1300.0f);
	CHECK(waiting.HasRest(INNKEEPER));

	waiting.Fall(Fall(), t);
	waiting.Fall(Fall(INNKEEPER), t);
	const auto pushOnlyInnkeeper = [](const Body& a_body) { return a_body.key == INNKEEPER; };
	CHECK(waiting.TryFalls(t + 1s, pushOnlyInnkeeper) == 1u);
	CHECK(waiting.Falls() == 1u);
	const auto pushNone = [](const Body&) { return false; };
	CHECK(waiting.TryFalls(t + BodyRules::FALL_WAIT, pushNone) == 0u);
	CHECK(waiting.Falls() == 0u);
	CHECK(waiting.late == 1u);

	waiting.Forget(INNKEEPER);
	CHECK(waiting.Rests() == 0u);
	for (std::uint32_t i = 0; i < BodyRules::MAX_WAITING + 2; ++i) {
		waiting.Rest(Rest({ 0x00300000 + i, 0 }));
	}
	CHECK(waiting.Rests() == BodyRules::MAX_WAITING);
	CHECK(waiting.refused == 2u);
	waiting.Clear();
	CHECK(waiting.Rests() == 0u);

	// A fall's velocity is capped and finite.
	const auto capped = BodyRules::Capped({ 0.0f, 0.0f, 3.0f * Protocol::MAX_OBJECT_SPEED });
	CHECK(std::fabs(capped.z - Protocol::MAX_OBJECT_SPEED) < 0.5f);
	CHECK(BodyRules::Capped({ std::nanf(""), 0.0f, 0.0f }) == BodyRules::Vec{});
}
