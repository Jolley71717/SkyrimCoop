#include "Test.h"

#include "FormTranslate.h"
#include "ObjectBook.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "game/ObjectRules.h"

#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

// Loose objects (docs/DESIGN.md, "Loose objects"): holds and rests on the wire, the server's book of who moves what and
// where it lies, and the game's rules: the holder's frame, the class, whose move it is, settling, pacing, and what
// comes in.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Hold = Protocol::ObjectHold;
	using Rest = Protocol::ObjectRest;
	using RestKind = Protocol::RestKind;
	using Key = Protocol::ObjectKey;
	using Steady = std::chrono::steady_clock;
	using ObjectRules::Pose;
	using ObjectRules::Vec;
	using ObjectBook::Verdict;

	constexpr std::uint32_t TANKARD = 0x0001A2B3;
	constexpr std::uint32_t BUCKET = 0x0001A2B4;
	constexpr std::uint32_t CELL = 0x0001C0DE;
	constexpr std::uint32_t TAMRIEL = 0x0000003C;

	bool Near(float a_a, float a_b, float a_epsilon = 0.01f) { return std::fabs(a_a - a_b) <= a_epsilon; }
	bool Near(const Vec& a_a, const Vec& a_b, float a_epsilon = 0.01f) { return ObjectRules::Distance(a_a, a_b) <= a_epsilon; }
	bool Near(const Protocol::Quat& a_a, const Protocol::Quat& a_b, float a_epsilon = 0.001f) { return ObjectRules::Angle(a_a, a_b) <= a_epsilon; }

	// A turn of a_angle about the axis (normalised here).
	Protocol::Quat Turn(Vec a_axis, float a_angle)
	{
		const auto length = ObjectRules::Length(a_axis);
		a_axis = a_axis * (1.0f / length);
		const auto s = std::sin(a_angle * 0.5f);
		return { a_axis.x * s, a_axis.y * s, a_axis.z * s, std::cos(a_angle * 0.5f) };
	}

	Hold HoldOf(std::uint32_t a_id, float a_ahead = 80.0f)
	{
		Hold hold;
		hold.key = { Protocol::ObjectKind::kRef, a_id };
		hold.right = 10.0f;
		hold.ahead = a_ahead;
		hold.up = 40.0f;
		hold.rot = Turn({ 0, 0, 1 }, 0.5f);
		return hold;
	}

	Rest RestAt(std::uint32_t a_id, RestKind a_kind = RestKind::kRest, float a_x = 1000.0f)
	{
		return ObjectRules::RestOf(a_id, a_kind, CELL, 0, Pose{ { a_x, -2000.0f, 64.0f }, Turn({ 1, 0, 0 }, 0.3f) },
			a_kind == RestKind::kLetGo ? Vec{ 300.0f, 0.0f, 120.0f } : Vec{});
	}

	Rest Gone(std::uint32_t a_id)
	{
		Rest gone;
		gone.key = { Protocol::ObjectKind::kRef, a_id };
		gone.kind = RestKind::kGone;
		return gone;
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

	struct HoldsSent
	{
		Protocol::ObjectHolds msg;
		bool                  reliable = false;
	};
	std::vector<HoldsSent> HoldsTo(Fixture& a_f, PeerId a_peer)
	{
		std::vector<HoldsSent> out;
		for (auto& sent : a_f.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kObjectHolds) {
				if (auto msg = Protocol::DecodeObjectHolds(sent.data)) {
					out.push_back({ std::move(*msg), sent.reliable });
				}
			}
		}
		return out;
	}
	std::vector<Protocol::ObjectRests> RestsTo(Fixture& a_f, PeerId a_peer)
	{
		std::vector<Protocol::ObjectRests> out;
		for (auto& sent : a_f.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kObjectRests) {
				CHECK(sent.reliable);
				if (auto msg = Protocol::DecodeObjectRests(sent.data)) {
					out.push_back(std::move(*msg));
				}
			}
		}
		return out;
	}
}

// ---- the messages ----

TEST("object protocol: holds and rests read back as written, as messages 65 to 67 of protocol 17, and carry forms")
{
	static_assert(static_cast<int>(MT::kObjectHolds) == 65);
	static_assert(static_cast<int>(MT::kObjectRests) == 66);
	static_assert(static_cast<int>(MT::kAskObjectRests) == 67);
	static_assert(Protocol::VERSION >= 17);

	const Protocol::ObjectHolds holds{ .sender = 2, .holds = { HoldOf(TANKARD), HoldOf(BUCKET, 120.0f) } };
	CHECK(Protocol::DecodeObjectHolds(Protocol::Encode(holds)) == holds);
	CHECK(Protocol::PeekType(Protocol::Encode(holds)) == MT::kObjectHolds);
	CHECK(Protocol::DecodeObjectHolds(Protocol::Encode(Protocol::ObjectHolds{})) == Protocol::ObjectHolds{});

	auto outside = RestAt(BUCKET);
	outside.cell = 0;
	outside.worldspace = TAMRIEL;
	const Protocol::ObjectRests rests{ .sender = 3, .full = true, .rests = { RestAt(TANKARD, RestKind::kLetGo), RestAt(TANKARD), outside, Gone(BUCKET) } };
	CHECK(Protocol::DecodeObjectRests(Protocol::Encode(rests)) == rests);
	CHECK(Protocol::PeekType(Protocol::Encode(rests)) == MT::kObjectRests);

	CHECK(Protocol::EncodeAskObjectRests().size() == 1u);
	CHECK(Protocol::DecodeAskObjectRests(Protocol::EncodeAskObjectRests()));
	auto askLong = Protocol::EncodeAskObjectRests();
	askLong.push_back(0);
	CHECK(!Protocol::DecodeAskObjectRests(askLong));

	CHECK(FormTranslate::CarriesForms(MT::kObjectHolds));
	CHECK(FormTranslate::CarriesForms(MT::kObjectRests));
	CHECK(!FormTranslate::CarriesForms(MT::kAskObjectRests));
}

TEST("object protocol: the longest fit; a rest's velocity is written only for a let-go; bad keys, kinds, reaches, rotations, speeds and counts are refused")
{
	Protocol::ObjectHolds manyHolds;
	for (std::uint32_t i = 0; i < Protocol::MAX_OBJECT_HOLDS; ++i) {
		manyHolds.holds.push_back(HoldOf(0x00100000 + i));
	}
	CHECK(Protocol::Encode(manyHolds).size() == Protocol::MAX_OBJECT_HOLDS_SIZE);
	CHECK(Protocol::DecodeObjectHolds(Protocol::Encode(manyHolds)) == manyHolds);
	manyHolds.holds.push_back(HoldOf(TANKARD));
	CHECK(Protocol::DecodeObjectHolds(Protocol::Encode(manyHolds))->holds.size() == Protocol::MAX_OBJECT_HOLDS);  // cut by the writer

	Protocol::ObjectRests manyRests;
	for (std::uint32_t i = 0; i < Protocol::MAX_OBJECT_RESTS; ++i) {
		manyRests.rests.push_back(RestAt(0x00100000 + i, i % 2 ? RestKind::kRest : RestKind::kLetGo));
	}
	CHECK(Protocol::Encode(manyRests).size() == Protocol::MAX_OBJECT_RESTS_SIZE);
	CHECK(Protocol::DecodeObjectRests(Protocol::Encode(manyRests)) == manyRests);

	// A rest's velocity, left in by the caller, is not written.
	auto moving = RestAt(TANKARD);
	moving.vx = 50.0f;
	CHECK(!Protocol::ObjectRestHolds(moving));
	const auto read = Protocol::DecodeObjectRests(Protocol::Encode(Protocol::ObjectRests{ .rests = { moving } }));
	REQUIRE(read);
	CHECK(read->rests[0].vx == 0.0f);

	// Holds that do not hold.
	auto noId = HoldOf(0);
	CHECK(!Protocol::ObjectHoldHolds(noId));
	CHECK(!Protocol::DecodeObjectHolds(Protocol::Encode(Protocol::ObjectHolds{ .holds = { noId } })));
	CHECK(!Protocol::DecodeObjectHolds(Protocol::Encode(Protocol::ObjectHolds{ .holds = { HoldOf(TANKARD, Protocol::MAX_HOLD_REACH + 1.0f) } })));
	auto badTurn = HoldOf(TANKARD);
	badTurn.rot = { 0.0f, 0.0f, 0.0f, 2.0f };
	CHECK(!Protocol::DecodeObjectHolds(Protocol::Encode(Protocol::ObjectHolds{ .holds = { badTurn } })));
	auto nanHold = HoldOf(TANKARD);
	nanHold.up = std::nanf("");
	CHECK(!Protocol::DecodeObjectHolds(Protocol::Encode(Protocol::ObjectHolds{ .holds = { nanHold } })));
	constexpr std::size_t HOLDS_FIRST = 1 + 4 + 1;  // type, sender, count
	auto badKind = Protocol::Encode(Protocol::ObjectHolds{ .holds = { HoldOf(TANKARD) } });
	badKind[HOLDS_FIRST] = Protocol::OBJECT_KIND_COUNT;
	CHECK(!Protocol::DecodeObjectHolds(badKind));
	auto tooManyHolds = Protocol::Encode(Protocol::ObjectHolds{});
	tooManyHolds[5] = static_cast<std::uint8_t>(Protocol::MAX_OBJECT_HOLDS + 1);
	CHECK(!Protocol::DecodeObjectHolds(tooManyHolds));
	auto shortHold = Protocol::Encode(Protocol::ObjectHolds{ .holds = { HoldOf(TANKARD) } });
	shortHold.pop_back();
	CHECK(!Protocol::DecodeObjectHolds(shortHold));

	// Rests that do not hold.
	constexpr std::size_t RESTS_FIRST = 1 + 4 + 1 + 1;  // type, sender, full, count
	auto badRestKind = Protocol::Encode(Protocol::ObjectRests{ .rests = { RestAt(TANKARD) } });
	badRestKind[RESTS_FIRST + 5] = Protocol::REST_KIND_COUNT;
	CHECK(!Protocol::DecodeObjectRests(badRestKind));
	auto badFull = Protocol::Encode(Protocol::ObjectRests{ .rests = { RestAt(TANKARD) } });
	badFull[5] = 2;
	CHECK(!Protocol::DecodeObjectRests(badFull));
	auto tooManyRests = Protocol::Encode(Protocol::ObjectRests{});
	tooManyRests[6] = static_cast<std::uint8_t>(Protocol::MAX_OBJECT_RESTS + 1);
	CHECK(!Protocol::DecodeObjectRests(tooManyRests));
	auto tooFast = RestAt(TANKARD, RestKind::kLetGo);
	tooFast.vx = Protocol::MAX_OBJECT_SPEED + 1.0f;
	CHECK(!Protocol::DecodeObjectRests(Protocol::Encode(Protocol::ObjectRests{ .rests = { tooFast } })));
	auto offWorld = RestAt(TANKARD);
	offWorld.z = 1.0e30f;
	CHECK(!Protocol::DecodeObjectRests(Protocol::Encode(Protocol::ObjectRests{ .rests = { offWorld } })));
	auto longRest = Protocol::Encode(Protocol::ObjectRests{ .rests = { RestAt(TANKARD) } });
	longRest.push_back(0);
	CHECK(!Protocol::DecodeObjectRests(longRest));
}

TEST("object form translate: an object, cell or worldspace the reader lacks leaves the entry out; the rest are renumbered")
{
	const Edges         e;
	const std::uint32_t ofB = 0x03000ABC;      // B.esp's in the host's numbering
	const std::uint32_t ofExtra = 0x04000ABC;  // Extra.esp's: the guest lacks it
	const std::uint32_t inGuest = 0x01000ABC;
	const std::uint32_t ofSkyrim = 0x00013BBF;

	const auto holds = Protocol::DecodeObjectHolds(e.HostToGuest(Protocol::Encode(Protocol::ObjectHolds{ .sender = 1, .holds = { HoldOf(ofB), HoldOf(ofExtra), HoldOf(ofSkyrim) } })));
	REQUIRE(holds);
	CHECK(holds->sender == 1u);
	CHECK(holds->holds == std::vector<Hold>({ HoldOf(inGuest), HoldOf(ofSkyrim) }));

	auto inExtraCell = RestAt(ofSkyrim);
	inExtraCell.cell = ofExtra;
	auto inBCell = RestAt(ofB);
	inBCell.cell = ofB;
	auto outside = RestAt(ofSkyrim);
	outside.cell = 0;
	outside.worldspace = TAMRIEL;
	const auto rests = Protocol::DecodeObjectRests(
		e.HostToGuest(Protocol::Encode(Protocol::ObjectRests{ .sender = 1, .rests = { inBCell, RestAt(ofExtra), inExtraCell, outside, Gone(ofB) } })));
	REQUIRE(rests);
	auto inGuestCell = RestAt(inGuest);
	inGuestCell.cell = inGuest;
	CHECK(rests->rests == std::vector<Rest>({ inGuestCell, outside, Gone(inGuest) }));
}

// ---- the server's book ----

TEST("object book: one holder at a time, the first grab wins; a refused hold is answered with the holder's; a quiet holder holds no more")
{
	ObjectBook::Book book;
	const auto       t = Steady::now();
	CHECK(book.Hold(1, HoldOf(TANKARD), t).verdict == Verdict::kPass);
	CHECK(book.HolderOf({ Protocol::ObjectKind::kRef, TANKARD }, t) == 1u);
	const auto refused = book.Hold(2, HoldOf(TANKARD, 50.0f), t + 100ms);
	CHECK(refused.verdict == Verdict::kHeld);
	CHECK(!refused.Passes());
	CHECK(refused.holder == 1u);
	REQUIRE(refused.hold);
	CHECK(*refused.hold == HoldOf(TANKARD));
	CHECK(book.Hold(1, HoldOf(TANKARD, 90.0f), t + 200ms).Passes());  // the holder's next hold
	// A rest of what Ada holds, from Bo, is refused with no answer (her let-go is coming).
	const auto heldRest = book.Rest(2, RestAt(TANKARD), t + 300ms);
	CHECK(heldRest.verdict == Verdict::kHeld);
	CHECK(!heldRest.rest);
	// Ada goes quiet: past HOLD_TIMEOUT Bo may grab it.
	CHECK(book.HolderOf({ Protocol::ObjectKind::kRef, TANKARD }, t + 200ms + ObjectBook::HOLD_TIMEOUT) == 0u);
	CHECK(book.Hold(2, HoldOf(TANKARD), t + 200ms + ObjectBook::HOLD_TIMEOUT).Passes());
	// Nobody, a drop's key and a hold that does not hold are refused.
	CHECK(book.Hold(0, HoldOf(BUCKET), t).verdict == Verdict::kRefused);
	auto drop = HoldOf(BUCKET);
	drop.key.kind = Protocol::ObjectKind::kDrop;
	CHECK(book.Hold(1, drop, t).verdict == Verdict::kRefused);
	CHECK(book.Hold(1, HoldOf(BUCKET, 10000.0f), t).verdict == Verdict::kRefused);
	CHECK(book.counts.refused == 3u);
}

TEST("object book: a throw is its thrower's until its rest; the first rest stands for SETTLED and a second is answered with it; a pickup ends the place")
{
	ObjectBook::Book book;
	const auto       t = Steady::now();
	(void)book.Hold(1, HoldOf(TANKARD), t);
	CHECK(book.Rest(1, RestAt(TANKARD, RestKind::kLetGo), t + 1s).Passes());
	CHECK(book.HolderOf({ Protocol::ObjectKind::kRef, TANKARD }, t + 1s) == 0u);
	// Bo's rest while Ada's throw flies: refused, no answer.
	const auto thrown = book.Rest(2, RestAt(TANKARD, RestKind::kRest, 1500.0f), t + 2s);
	CHECK(thrown.verdict == Verdict::kThrown);
	CHECK(!thrown.rest);
	// Ada's rest stands (its velocity is never kept).
	CHECK(book.Rest(1, RestAt(TANKARD), t + 3s).Passes());
	REQUIRE(book.PlaceOf({ Protocol::ObjectKind::kRef, TANKARD }));
	CHECK(*book.PlaceOf({ Protocol::ObjectKind::kRef, TANKARD }) == RestAt(TANKARD));
	// Bo's rest within SETTLED: answered with Ada's.
	const auto settled = book.Rest(2, RestAt(TANKARD, RestKind::kRest, 1500.0f), t + 4s);
	CHECK(settled.verdict == Verdict::kSettled);
	REQUIRE(settled.rest);
	CHECK(*settled.rest == RestAt(TANKARD));
	// Ada's own next rest of it, at once, stands (her ball rolled on).
	CHECK(book.Rest(1, RestAt(TANKARD, RestKind::kRest, 1100.0f), t + 4s).Passes());
	// After SETTLED, Bo's move of it is his.
	CHECK(book.Rest(2, RestAt(TANKARD, RestKind::kRest, 1500.0f), t + 4s + ObjectBook::SETTLED).Passes());
	CHECK(book.PlaceOf({ Protocol::ObjectKind::kRef, TANKARD })->x == 1500.0f);
	// A thrower's flight ends after THROW_HOLD with no rest from them.
	CHECK(book.Rest(1, RestAt(BUCKET, RestKind::kLetGo), t).Passes());
	CHECK(book.Rest(2, RestAt(BUCKET), t + ObjectBook::THROW_HOLD).Passes());
	// Picked up: no place kept.
	CHECK(book.Rest(1, Gone(TANKARD), t + 20s).Passes());
	CHECK(!book.PlaceOf({ Protocol::ObjectKind::kRef, TANKARD }));
	CHECK(book.Places() == 1u);
	CHECK(book.All() == std::vector<Rest>({ RestAt(BUCKET) }));
}

TEST("object book: one player adds at most MAX_NEW_PLACES_PER_PLAYER objects; past MAX_PLACES the one moved longest ago goes; All is oldest first")
{
	ObjectBook::Book book;
	const auto       t = Steady::now();
	for (std::uint32_t i = 0; i < ObjectBook::MAX_NEW_PLACES_PER_PLAYER; ++i) {
		CHECK(book.Rest(5, RestAt(0x00200000 + i), t + std::chrono::milliseconds(i)).verdict == Verdict::kPass);
	}
	const auto full = book.Rest(5, RestAt(0x002FFFFF), t + 1s);
	CHECK(full.verdict == Verdict::kFull);
	CHECK(full.Passes());  // passed on, not kept
	CHECK(book.counts.notKept == 1u);
	CHECK(book.Rest(5, RestAt(0x00200000, RestKind::kRest, 900.0f), t + 4s).verdict == Verdict::kPass);  // a known one moves
	CHECK(book.Places() == ObjectBook::MAX_NEW_PLACES_PER_PLAYER);

	for (std::uint32_t player = 1; player <= 3; ++player) {
		for (std::uint32_t i = 0; i < ObjectBook::MAX_NEW_PLACES_PER_PLAYER; ++i) {
			(void)book.Rest(player, RestAt(0x00300000 + player * 0x1000 + i), t + 5s + std::chrono::milliseconds(i));
		}
	}
	CHECK(book.Places() == ObjectBook::MAX_PLACES);
	// The 2049th: 0x00200001, moved longest ago (0x00200000 moved again at 4 s), goes.
	CHECK(book.Rest(4, RestAt(TANKARD), t + 10s).Passes());
	CHECK(book.Places() == ObjectBook::MAX_PLACES);
	CHECK(!book.PlaceOf({ Protocol::ObjectKind::kRef, 0x00200001 }));
	CHECK(book.PlaceOf({ Protocol::ObjectKind::kRef, 0x00200000 }));
	CHECK(book.counts.forgotten == 1u);
	const auto all = book.All();
	REQUIRE(all.size() == ObjectBook::MAX_PLACES);
	CHECK(all.front().key.id == 0x00200002u);
	CHECK(all.back() == RestAt(TANKARD));
}

TEST("object book: owed rests come back in order, the oldest dropped past MAX_OWED; a leaver's holds and throws and owed end; Clear keeps nothing")
{
	ObjectBook::Book book;
	const auto       t = Steady::now();
	for (std::uint32_t i = 0; i < ObjectBook::MAX_OWED + 2; ++i) {
		book.Owe(1, RestAt(0x00400000 + i));
	}
	CHECK(book.counts.owedDropped == 2u);
	Rest next;
	REQUIRE(book.NextOwed(1, next));
	CHECK(next.key.id == 0x00400002u);
	CHECK(book.Owes(1));
	CHECK(!book.Owes(2));

	(void)book.Hold(1, HoldOf(TANKARD), t);
	(void)book.Rest(1, RestAt(BUCKET, RestKind::kLetGo), t);
	book.Leave(1);
	CHECK(!book.Owes(1));
	CHECK(book.HolderOf({ Protocol::ObjectKind::kRef, TANKARD }, t) == 0u);
	CHECK(book.Hold(2, HoldOf(TANKARD), t).Passes());
	CHECK(book.Rest(2, RestAt(BUCKET), t).Passes());

	book.Owe(3, RestAt(TANKARD));
	book.Clear();
	CHECK(!book.Owes(3));
	CHECK(book.Places() == 0u);
	CHECK(book.HolderOf({ Protocol::ObjectKind::kRef, TANKARD }, t) == 0u);
}

TEST("object server: a hold goes to the others unreliably with its holder named; a second holder hears the first's; rests pass, a late one is answered; joiners and askers get every place")
{
	Fixture f;
	AllIn(f, 2);

	// Ada holds the tankard: Bo sees it, on the state channel.
	f.fake->Deliver(1, Protocol::Encode(Protocol::ObjectHolds{ .sender = 99, .holds = { HoldOf(TANKARD) } }));
	REQUIRE(f.fake->Settle());
	CHECK(HoldsTo(f, 1).empty());
	const auto toBo = HoldsTo(f, 2);
	REQUIRE(toBo.size() == 1u);
	CHECK(!toBo[0].reliable);
	CHECK(toBo[0].msg.sender == 1u);
	CHECK(toBo[0].msg.holds == std::vector<Hold>({ HoldOf(TANKARD) }));

	// Bo grabs it too: Ada hears nothing, Bo hears Ada's hold (reliably) and lets go.
	f.fake->Deliver(2, Protocol::Encode(Protocol::ObjectHolds{ .holds = { HoldOf(TANKARD, 30.0f) } }));
	REQUIRE(f.fake->Settle());
	CHECK(HoldsTo(f, 1).empty());
	const auto answer = HoldsTo(f, 2);
	REQUIRE(answer.size() == 1u);
	CHECK(answer[0].reliable);
	CHECK(answer[0].msg.sender == 1u);
	CHECK(answer[0].msg.holds == std::vector<Hold>({ HoldOf(TANKARD) }));

	// Ada throws it and it rests: Bo hears both, with Ada named.
	f.fake->Deliver(1, Protocol::Encode(Protocol::ObjectRests{ .rests = { RestAt(TANKARD, RestKind::kLetGo), RestAt(TANKARD) } }));
	REQUIRE(f.fake->Settle());
	CHECK(RestsTo(f, 1).empty());
	const auto restsToBo = RestsTo(f, 2);
	REQUIRE(restsToBo.size() == 1u);
	CHECK(restsToBo[0].sender == 1u);
	CHECK(!restsToBo[0].full);
	CHECK(restsToBo[0].rests == std::vector<Rest>({ RestAt(TANKARD, RestKind::kLetGo), RestAt(TANKARD) }));

	// Bo's physics says it lies elsewhere, at once: Bo alone is told where it lies (the session's word).
	f.fake->Deliver(2, Protocol::Encode(Protocol::ObjectRests{ .rests = { RestAt(TANKARD, RestKind::kRest, 1300.0f) } }));
	REQUIRE(f.fake->Settle());
	CHECK(RestsTo(f, 1).empty());
	const auto told = RestsTo(f, 2);
	REQUIRE(told.size() == 1u);
	CHECK(told[0].sender == 0u);
	CHECK(told[0].rests == std::vector<Rest>({ RestAt(TANKARD) }));

	// A full message from a game is never passed on.
	f.fake->Deliver(1, Protocol::Encode(Protocol::ObjectRests{ .full = true, .rests = { RestAt(BUCKET) } }));
	REQUIRE(f.fake->Settle());
	CHECK(RestsTo(f, 2).empty());

	// Cy joins: every place, as the session's full list.
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto toCy = RestsTo(f, 3);
	REQUIRE(toCy.size() == 1u);
	CHECK(toCy[0].full);
	CHECK(toCy[0].sender == 0u);
	CHECK(toCy[0].rests == std::vector<Rest>({ RestAt(TANKARD) }));

	// Cy loads a save and asks again.
	f.fake->Deliver(3, Protocol::EncodeAskObjectRests());
	REQUIRE(f.fake->Settle());
	const auto again = RestsTo(f, 3);
	REQUIRE(again.size() == 1u);
	CHECK(again[0].full);
	CHECK(again[0].rests.size() == 1u);

	// Ada picks it up: the others hear it, and nobody joining later gets a place for it.
	f.fake->Deliver(1, Protocol::Encode(Protocol::ObjectRests{ .rests = { Gone(TANKARD) } }));
	REQUIRE(f.fake->Settle());
	CHECK(RestsTo(f, 2).size() == 1u);
	CHECK(RestsTo(f, 3).size() == 1u);
	f.fake->Deliver(3, Protocol::EncodeAskObjectRests());
	REQUIRE(f.fake->Settle());
	const auto none = RestsTo(f, 3);
	REQUIRE(none.size() == 1u);
	CHECK(none[0].full);
	CHECK(none[0].rests.empty());
}

TEST("object server: a blast's worth of rests past the budget is owed, never lost")
{
	Fixture f;
	AllIn(f, 2);
	Protocol::ObjectRests blast;
	for (std::uint32_t i = 0; i < 40; ++i) {
		blast.rests.push_back(RestAt(0x00500000 + i));
	}
	f.fake->Deliver(1, Protocol::Encode(blast));
	REQUIRE(f.fake->Settle());
	// The burst (24) at once, the rest 6 a second from the server's loop.
	std::size_t heard = 0;
	const auto  deadline = Steady::now() + 20s;
	while (heard < blast.rests.size() && Steady::now() < deadline) {
		for (const auto& msg : RestsTo(f, 2)) {
			heard += msg.rests.size();
		}
		std::this_thread::sleep_for(50ms);
	}
	CHECK(heard == blast.rests.size());
}

// ---- the game's rules ----

TEST("object frames: a hold in the holder's frame is drawn at the same place before the friend's stand-in, whatever way each faces")
{
	const Pose  object{ { 1100.0f, 2080.0f, 140.0f }, ObjectRules::Normalized(Turn({ 1, 2, 3 }, 0.7f)) };
	const Vec   holder{ 1000.0f, 2000.0f, 100.0f };
	const float heading = 1.1f;
	const auto  hold = ObjectRules::ToHolder({ Protocol::ObjectKind::kRef, TANKARD }, holder, heading, object);
	CHECK(Protocol::ObjectHoldHolds(hold));
	const auto back = ObjectRules::FromHolder(hold, holder, heading);
	CHECK(Near(back.at, object.at));
	CHECK(Near(back.rot, object.rot));

	// Facing north: ahead is +y; facing east (heading pi/2): ahead is +x, right is -y.
	const auto north = ObjectRules::ToHolder({}, {}, 0.0f, Pose{ { 0.0f, 100.0f, 20.0f }, {} });
	CHECK(Near(north.ahead, 100.0f));
	CHECK(Near(north.right, 0.0f));
	CHECK(Near(north.up, 20.0f));
	const auto east = ObjectRules::ToHolder({}, {}, std::numbers::pi_v<float> / 2, Pose{ { 100.0f, -30.0f, 0.0f }, {} });
	CHECK(Near(east.ahead, 100.0f));
	CHECK(Near(east.right, 30.0f));

	// The stand-in elsewhere, facing the other way: the object is before it as it was before the holder, turned with it.
	const Vec  standIn{ -500.0f, 300.0f, 0.0f };
	const auto drawn = ObjectRules::FromHolder(hold, standIn, heading + std::numbers::pi_v<float>);
	CHECK(Near(drawn.at - standIn, (object.at - holder) * -1.0f + Vec{ 0.0f, 0.0f, 2.0f * (object.at.z - holder.z) }));
	CHECK(Near(ObjectRules::Rotate(ObjectRules::Heading(heading), { 0.0f, 1.0f, 0.0f }), Vec{ std::sin(heading), std::cos(heading), 0.0f }));

	// Easing: none with no time, most of the way after a few time constants.
	const Pose from{ { 0.0f, 0.0f, 0.0f }, {} };
	const Pose to{ { 100.0f, 0.0f, 0.0f }, Turn({ 0, 0, 1 }, 1.0f) };
	CHECK(ObjectRules::Ease(from, to, 0.0f) == from);
	const auto eased = ObjectRules::Ease(from, to, ObjectRules::EASE_SECONDS * 5.0f);
	CHECK(eased.at.x > 99.0f);
	CHECK(ObjectRules::Angle(eased.rot, to.rot) < 0.02f);
	// A rotation and its negative are the same.
	const auto q = Turn({ 0, 0, 1 }, 0.4f);
	CHECK(Near(ObjectRules::Angle(q, Protocol::Quat{ -q.x, -q.y, -q.z, -q.w }), 0.0f));
}

TEST("object class: a loaded, enabled reference of the load order with one loose body; never an actor, a projectile, a runtime one, an alias's, many bodies or a keyframed one")
{
	ObjectRules::Facts clutter;
	clutter.has3D = true;
	clutter.bodies = 1;
	clutter.motion = ObjectRules::Motion::kDynamic;
	CHECK(ObjectRules::Movable(clutter));
	for (const auto motion : { ObjectRules::Motion::kSphereInertia, ObjectRules::Motion::kBoxInertia, ObjectRules::Motion::kThinBoxInertia }) {
		auto f = clutter;
		f.motion = motion;
		CHECK(ObjectRules::Movable(f));
	}
	for (const auto motion : { ObjectRules::Motion::kInvalid, ObjectRules::Motion::kKeyframed, ObjectRules::Motion::kFixed, ObjectRules::Motion::kCharacter }) {
		auto f = clutter;
		f.motion = motion;
		CHECK(!ObjectRules::Movable(f));
	}
	auto f = clutter;
	f.runtime = true;
	CHECK(!ObjectRules::Movable(f));
	f = clutter;
	f.actor = true;
	CHECK(!ObjectRules::Movable(f));
	f = clutter;
	f.transient = true;
	CHECK(!ObjectRules::Movable(f));
	f = clutter;
	f.enabled = false;
	CHECK(!ObjectRules::Movable(f));
	f = clutter;
	f.has3D = false;
	CHECK(!ObjectRules::Movable(f));
	f = clutter;
	f.inAlias = true;
	CHECK(!ObjectRules::Movable(f));
	f = clutter;
	f.bodies = 2;
	CHECK(!ObjectRules::Movable(f));
	f.bodies = 0;
	CHECK(!ObjectRules::Movable(f));
}

TEST("object causes: ours when our strongest cause that reached it is at least theirs; paths, blasts and single objects reach as they should; old causes count no more")
{
	using ObjectRules::Strength;
	const auto t = Steady::now();
	const Vec  here{ 0.0f, 0.0f, 0.0f };

	ObjectRules::Causes causes;
	CHECK(!causes.Judge(TANKARD, here, here, t).Ours());  // nothing reached it: nobody's

	// Our player and a friend's stand-in both stand by it: ours (a tie is ours, so one of the two games says it).
	causes.Add({ { 100.0f, 0.0f, 0.0f }, { 100.0f, 0.0f, 0.0f }, ObjectRules::PLAYER_REACH, Strength::kBody, true, t + 1s });
	causes.Add({ { -100.0f, 0.0f, 0.0f }, { -100.0f, 0.0f, 0.0f }, ObjectRules::PLAYER_REACH, Strength::kBody, false, t + 1s });
	CHECK(causes.Judge(TANKARD, here, here, t).Ours());

	// The friend's spell hit it: theirs.
	causes.Add({ {}, {}, 0.0f, Strength::kStrike, false, t + ObjectRules::HIT_HOLD, TANKARD });
	auto claim = causes.Judge(TANKARD, here, here, t);
	CHECK(!claim.Ours());
	CHECK(claim.theirs == Strength::kStrike);
	// The hit names the tankard alone: the bucket by it is still ours.
	CHECK(causes.Judge(BUCKET, here, here, t).Ours());

	// Our player grabbed it: ours again.
	causes.Add({ {}, {}, 0.0f, Strength::kGrab, true, t + 1s, TANKARD });
	CHECK(causes.Judge(TANKARD, here, here, t).Ours());

	// Old causes count no more; Prune drops them.
	CHECK(!causes.Judge(TANKARD, here, here, t + 5s).Ours());
	causes.Prune(t + 5s);
	CHECK(causes.Size() == 0u);

	// Our arrow's path passed 200 units from it: ours. A blast threw it past its reach: where it is now counts too.
	causes.Add({ { -1000.0f, 200.0f, 0.0f }, { 1000.0f, 200.0f, 0.0f }, ObjectRules::SHOT_REACH, Strength::kStrike, true, t + 2s });
	CHECK(causes.Judge(TANKARD, here, here, t).Ours());
	CHECK(!causes.Judge(TANKARD, { 0.0f, 600.0f, 0.0f }, { 0.0f, 700.0f, 0.0f }, t).Ours());
	causes.Clear();
	causes.Add({ { 5000.0f, 0.0f, 0.0f }, { 5000.0f, 0.0f, 0.0f }, 300.0f + ObjectRules::BLAST_MARGIN, Strength::kStrike, true, t + ObjectRules::BLAST_HOLD });
	CHECK(causes.Judge(TANKARD, { 5300.0f, 0.0f, 0.0f }, { 6500.0f, 0.0f, 0.0f }, t).Ours());
	CHECK(causes.Judge(TANKARD, { 7000.0f, 0.0f, 0.0f }, { 5100.0f, 0.0f, 0.0f }, t).Ours());
	CHECK(!causes.Judge(TANKARD, { 7000.0f, 0.0f, 0.0f }, { 7100.0f, 0.0f, 0.0f }, t).Ours());

	// No cause of no strength, and none that is not finite; at most MAX_CAUSES.
	causes.Clear();
	causes.Add({ {}, {}, 100.0f, Strength::kNone, true, t + 1s });
	causes.Add({ { std::nanf(""), 0.0f, 0.0f }, {}, 100.0f, Strength::kBody, true, t + 1s });
	CHECK(causes.Size() == 0u);
	for (std::size_t i = 0; i < ObjectRules::Causes::MAX_CAUSES + 5; ++i) {
		causes.Add({ {}, {}, 100.0f, Strength::kBody, true, t + 1s });
	}
	CHECK(causes.Size() == ObjectRules::Causes::MAX_CAUSES);
	CHECK(Near(ObjectRules::DistanceToPath({ 0.0f, 5.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }), 5.0f));
	CHECK(Near(ObjectRules::DistanceToPath({ 4.0f, 3.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }), std::sqrt(18.0f)));
}

TEST("object settle: at rest once still for STILL_FOR, never before MIN_FLIGHT; something that swings for ever is given up on at GIVE_UP")
{
	const auto t = Steady::now();
	const Pose start{ { 0.0f, 0.0f, 0.0f }, {} };
	ObjectRules::Settle settle{ start, t };
	CHECK(!settle.Look(Pose{ { 50.0f, 0.0f, 0.0f }, {} }, t + 100ms));
	CHECK(!settle.Look(Pose{ { 80.0f, 0.0f, 0.0f }, {} }, t + 200ms));
	CHECK(!settle.Look(Pose{ { 80.5f, 0.0f, 0.0f }, {} }, t + 500ms));  // under MOVE_EPSILON
	CHECK(settle.Look(Pose{ { 80.5f, 0.0f, 0.0f }, {} }, t + 800ms));
	CHECK(settle.Rested());
	CHECK(!settle.GaveUp());
	CHECK(Near(settle.Where().at, { 80.0f, 0.0f, 0.0f }));
	CHECK(settle.Look(Pose{ { 500.0f, 0.0f, 0.0f }, {} }, t + 900ms));  // rested stays rested

	// A turn is a move.
	ObjectRules::Settle turning{ start, t };
	CHECK(!turning.Look(Pose{ {}, Turn({ 0, 0, 1 }, 0.1f) }, t + 500ms));
	CHECK(!turning.Look(Pose{ {}, Turn({ 0, 0, 1 }, 0.1f) }, t + 1000ms));
	CHECK(turning.Look(Pose{ {}, Turn({ 0, 0, 1 }, 0.1f) }, t + 1100ms));

	// Never before MIN_FLIGHT, even when it never moved.
	ObjectRules::Settle still{ start, t };
	CHECK(!still.Look(start, t + 200ms));
	CHECK(still.Look(start, t + ObjectRules::STILL_FOR));

	ObjectRules::Settle swinging{ start, t };
	bool rested = false;
	for (int i = 1; i <= 160 && !rested; ++i) {
		rested = swinging.Look(Pose{ { i % 2 ? 10.0f : 0.0f, 0.0f, 0.0f }, {} }, t + std::chrono::milliseconds(100 * i));
	}
	CHECK(rested);
	CHECK(swinging.GaveUp());
}

TEST("object seen: the first look is a baseline; a move is reported with where it lay; Set moves the baseline; unseen ones are forgotten")
{
	const auto t = Steady::now();
	const Pose a{ { 0.0f, 0.0f, 0.0f }, {} };
	const Pose b{ { 30.0f, 0.0f, 0.0f }, {} };
	ObjectRules::Seen seen;
	CHECK(!seen.Look(TANKARD, a, t));
	CHECK(!seen.Look(TANKARD, a, t + 250ms));
	const auto moved = seen.Look(TANKARD, b, t + 500ms);
	REQUIRE(moved);
	CHECK(*moved == a);
	CHECK(!seen.Look(TANKARD, b, t + 750ms));
	seen.Set(TANKARD, a, t + 1s);
	CHECK(seen.Look(TANKARD, b, t + 1250ms));
	CHECK(seen.Size() == 1u);
	seen.Prune(t + 1250ms + ObjectRules::Seen::FORGET_AFTER + 1ms);
	CHECK(seen.Size() == 0u);
	seen.Set(BUCKET, a, t);
	seen.Forget(BUCKET);
	CHECK(seen.Size() == 0u);
}

TEST("object pacing: rests go BURST at once then PER_SECOND; a newer rest replaces a waiting one but never a let-go; old ones drop; holds 15 a second while moving, 2 while still")
{
	const auto         t = Steady::now();
	ObjectRules::Pacer pacer;
	for (std::uint32_t i = 0; i < 10; ++i) {
		pacer.Push(RestAt(0x00600000 + i), t);
	}
	CHECK(pacer.Due(t).size() == ObjectRules::BURST);
	CHECK(pacer.Waiting() == 2u);
	pacer.Push(RestAt(0x00600009, RestKind::kRest, 700.0f), t);  // replaces the waiting one
	CHECK(pacer.Waiting() == 2u);
	CHECK(pacer.replaced == 1u);
	const auto next = pacer.Due(t + 1s);
	REQUIRE(next.size() == 2u);
	CHECK(next[1].x == 700.0f);

	// A let-go waits; its rest does not replace it but follows it.
	for (std::uint32_t i = 0; i < 3; ++i) {
		pacer.Push(RestAt(0x00700000 + i), t + 1s);
	}
	pacer.Push(RestAt(TANKARD, RestKind::kLetGo), t + 1s);
	pacer.Push(RestAt(TANKARD), t + 1s);
	CHECK(pacer.Waiting() == 5u);
	// Unsent past WAIT_MAX: dropped.
	const auto late = pacer.Due(t + 1s + ObjectRules::WAIT_MAX + 1s);
	CHECK(late.empty());
	CHECK(pacer.dropped == 5u);
	pacer.Push(RestAt(TANKARD, RestKind::kLetGo), t + 30s);
	pacer.Push(RestAt(TANKARD), t + 30s);
	const auto both = pacer.Due(t + 30s);
	REQUIRE(both.size() == 2u);
	CHECK(both[0].kind == RestKind::kLetGo);
	CHECK(both[1].kind == RestKind::kRest);

	ObjectRules::HoldPace holds;
	const Pose            a{ { 0.0f, 0.0f, 0.0f }, {} };
	const Pose            b{ { 20.0f, 0.0f, 0.0f }, {} };
	CHECK(holds.Due(a, t));
	CHECK(!holds.Due(b, t + 30ms));
	CHECK(holds.Due(b, t + ObjectRules::HOLD_EVERY));
	CHECK(!holds.Due(b, t + 200ms));  // still: every STILL_EVERY
	CHECK(holds.Due(b, t + ObjectRules::HOLD_EVERY + ObjectRules::STILL_EVERY));
	holds.Reset();
	CHECK(holds.Due(b, t + 600ms));
}

TEST("object let-go: its velocity from two looks, capped at MAX_OBJECT_SPEED; none with no time; a rest's built with none")
{
	const auto t = Steady::now();
	const auto v = ObjectRules::Velocity({ 0.0f, 0.0f, 0.0f }, t, { 40.0f, 0.0f, 8.0f }, t + ObjectRules::LET_GO_AFTER);
	CHECK(Near(v, { 500.0f, 0.0f, 100.0f }, 0.5f));
	const auto fast = ObjectRules::Velocity({}, t, { 10000.0f, 0.0f, 0.0f }, t + 100ms);
	CHECK(Near(ObjectRules::Length(fast), Protocol::MAX_OBJECT_SPEED, 0.5f));
	CHECK(ObjectRules::Velocity({}, t, { 10.0f, 0.0f, 0.0f }, t) == Vec{});

	const Pose pose{ { 1.0f, 2.0f, 3.0f }, Turn({ 0, 1, 0 }, 0.2f) };
	const auto letGo = ObjectRules::RestOf(TANKARD, RestKind::kLetGo, CELL, 0, pose, { 100.0f, 0.0f, 0.0f });
	CHECK(letGo.vx == 100.0f);
	CHECK(Protocol::ObjectRestHolds(letGo));
	const auto rest = ObjectRules::RestOf(TANKARD, RestKind::kRest, CELL, 0, pose, { 100.0f, 0.0f, 0.0f });
	CHECK(rest.vx == 0.0f);
	CHECK(Protocol::ObjectRestHolds(rest));
	CHECK(ObjectRules::PoseOf(rest).at == pose.at);
}

TEST("object incoming: a place is written only near where it lies and when it is not there already; quiet after a friend's; echoes stop at ECHO_MAX")
{
	const auto t = Steady::now();
	const Pose there{ { 100.0f, 100.0f, 0.0f }, {} };
	CHECK(ObjectRules::Writable(there, { 0.0f, 0.0f, 0.0f }));
	CHECK(!ObjectRules::Writable(there, { 100.0f + ObjectRules::WRITE_JUMP + 1.0f, 100.0f, 0.0f }));
	CHECK(!ObjectRules::Writable(Pose{ { std::nanf(""), 0.0f, 0.0f }, {} }, {}));
	CHECK(!ObjectRules::Writable(Pose{ {}, { 0.0f, 0.0f, 0.0f, 0.0f } }, {}));
	CHECK(!ObjectRules::NeedsPut(there, Pose{ { 100.5f, 100.0f, 0.0f }, {} }));
	CHECK(ObjectRules::NeedsPut(there, Pose{ { 103.0f, 100.0f, 0.0f }, {} }));
	CHECK(ObjectRules::NeedsPut(there, Pose{ there.at, Turn({ 0, 0, 1 }, 0.1f) }));

	ObjectRules::Quiet quiet;
	quiet.Hush(TANKARD, t + ObjectRules::QUIET);
	quiet.Hush(TANKARD, t + 1s);  // never shortened
	CHECK(quiet.Hushed(TANKARD, t + 3s));
	CHECK(!quiet.Hushed(TANKARD, t + ObjectRules::QUIET));
	CHECK(!quiet.Hushed(BUCKET, t));
	quiet.End(TANKARD);
	CHECK(!quiet.Hushed(TANKARD, t));

	ObjectRules::Echo echo;
	for (int i = 0; i < 10; ++i) {
		CHECK(echo.MaySay(BUCKET, t));  // nobody else's rest: always
	}
	echo.Theirs(TANKARD, t);
	for (std::size_t i = 0; i < ObjectRules::ECHO_MAX; ++i) {
		CHECK(echo.MaySay(TANKARD, t + std::chrono::seconds(i + 1)));
	}
	CHECK(!echo.MaySay(TANKARD, t + 10s));
	CHECK(echo.stopped == 1u);
	CHECK(echo.MaySay(TANKARD, t + ObjectRules::ECHO_WINDOW + 1s));  // their rest is old
}

TEST("object waiting: the session's places wait until put; a pickup ends one; at most MAX_WAITING; each put leaves")
{
	ObjectRules::Waiting waiting;
	waiting.Put(RestAt(TANKARD));
	waiting.Put(RestAt(TANKARD, RestKind::kRest, 1200.0f));
	waiting.Put(RestAt(BUCKET));
	CHECK(waiting.Size() == 2u);
	CHECK(waiting.Has({ Protocol::ObjectKind::kRef, TANKARD }));
	waiting.Put(Gone(BUCKET));
	CHECK(waiting.Size() == 1u);
	float      put = 0.0f;
	const auto look = [&](const Rest& a_rest) {
		put = a_rest.x;
		return false;
	};
	const auto take = [](const Rest&) { return true; };
	CHECK(waiting.Try(look) == 0u);
	CHECK(put == 1200.0f);
	CHECK(waiting.Try(take) == 1u);
	CHECK(waiting.Size() == 0u);

	for (std::uint32_t i = 0; i < ObjectRules::Waiting::MAX_WAITING + 3; ++i) {
		waiting.Put(RestAt(0x00800000 + i));
	}
	CHECK(waiting.Size() == ObjectRules::Waiting::MAX_WAITING);
	CHECK(waiting.refused == 3u);
	waiting.Forget({ Protocol::ObjectKind::kRef, 0x00800000 });
	CHECK(waiting.Size() == ObjectRules::Waiting::MAX_WAITING - 1);
	waiting.Clear();
	CHECK(waiting.Size() == 0u);
}
