#include "Test.h"

#include "FormTranslate.h"
#include "Protocol.h"
#include "ServerFixture.h"
#include "SitBook.h"
#include "game/SitRules.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <string>
#include <vector>

// Furniture (docs/DESIGN.md, "Furniture"): who is in what on the wire, the server's book of it, what a game says of its
// own, how it finds the furniture here and puts the actor that shows them in it, and gets it up again.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using Sit = Protocol::Sit;
	using Kind = Protocol::SitKind;
	using State = SitRules::SitState;
	using Act = SitRules::Act;
	using Steady = std::chrono::steady_clock;

	constexpr Protocol::NpcKey SOLDIER{ 0x00012345, 0 };
	constexpr Protocol::NpcKey INNKEEPER{ 0x00012346, 0 };
	constexpr Protocol::NpcKey SPAWN_OF_1{ 0xFF000801, 1 };

	constexpr std::uint32_t CHAIR = 0x0001A2B3;
	constexpr std::uint32_t CHAIR_BASE = 0x000BB7C1;
	constexpr std::uint32_t BED = 0x0001A2C0;
	constexpr std::uint32_t BED_BASE = 0x000F5A1B;
	constexpr std::uint32_t FORGE = 0x0001A2D0;
	constexpr std::uint32_t FORGE_BASE = 0x000BAD0C;

	Sit In(std::uint32_t a_furniture, std::uint32_t a_base, Kind a_kind = Kind::kSit, std::uint8_t a_marker = 0)
	{
		Sit sit;
		sit.furniture = a_furniture;
		sit.base = a_base;
		sit.x = 1000.0f;
		sit.y = -2000.0f;
		sit.z = 64.0f;
		sit.marker = a_marker;
		sit.kind = a_kind;
		sit.on = true;
		return sit;
	}
	Sit PlayerIn(std::uint32_t a_player, std::uint32_t a_furniture = CHAIR, std::uint32_t a_base = CHAIR_BASE, Kind a_kind = Kind::kSit)
	{
		auto sit = In(a_furniture, a_base, a_kind);
		sit.player = a_player;
		return sit;
	}
	Sit NpcIn(Protocol::NpcKey a_npc, std::uint32_t a_furniture = FORGE, std::uint32_t a_base = FORGE_BASE, Kind a_kind = Kind::kWork)
	{
		auto sit = In(a_furniture, a_base, a_kind);
		sit.rider = a_npc;
		return sit;
	}
	Sit PlayerUp(std::uint32_t a_player) { return SitBook::Up({ a_player, {} }); }
	Sit NpcUp(Protocol::NpcKey a_npc) { return SitBook::Up({ 0, a_npc }); }

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

	std::vector<Protocol::Sits> SitsTo(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<Protocol::Sits> out;
		for (auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kSits) {
				CHECK(sent.reliable);
				if (auto msg = Protocol::DecodeSits(sent.data)) {
					out.push_back(std::move(*msg));
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

	auto RunsNone() { return [](const Protocol::NpcKey&) { return false; }; }
	auto RunsSoldier() { return [](const Protocol::NpcKey& a_key) { return a_key == SOLDIER; }; }
}

// ---- the message ----

TEST("sit protocol: sits read back as they were written, as message 58 of protocol 14, and carry forms")
{
	auto runtime = PlayerIn(3, 0, BED_BASE, Kind::kSleep);
	runtime.marker = Protocol::NO_MARKER;
	const Protocol::Sits sits{ .sender = 2,
		.full = true,
		.sits = { PlayerIn(2), NpcIn(SOLDIER), NpcIn(SPAWN_OF_1, 0, 0x000D1F00, Kind::kPerch), runtime, NpcUp(INNKEEPER), PlayerUp(4) } };
	CHECK(Protocol::DecodeSits(Protocol::Encode(sits)) == sits);
	CHECK(Protocol::PeekType(Protocol::Encode(sits)) == MT::kSits);
	CHECK(Protocol::DecodeSits(Protocol::Encode(Protocol::Sits{})) == Protocol::Sits{});
	static_assert(static_cast<int>(MT::kSits) == 58);
	static_assert(Protocol::VERSION >= 14);
	CHECK(FormTranslate::CarriesForms(MT::kSits));
	// Every kind reads back.
	for (std::uint8_t kind = 0; kind < Protocol::SIT_KIND_COUNT; ++kind) {
		const Protocol::Sits one{ .sits = { PlayerIn(1, CHAIR, CHAIR_BASE, static_cast<Kind>(kind)) } };
		CHECK(Protocol::DecodeSits(Protocol::Encode(one)) == one);
	}
}

TEST("sit protocol: an end is written naming no furniture; the longest fits; nobody, a player's naming an NPC, a bad kind, on or full, a place off the world, or too many are refused")
{
	// Whatever furniture the caller left in an end is not written.
	auto end = PlayerIn(1);
	end.on = false;
	const auto read = Protocol::DecodeSits(Protocol::Encode(Protocol::Sits{ .sits = { end } }));
	REQUIRE(read);
	REQUIRE(read->sits.size() == 1u);
	CHECK(read->sits[0].furniture == 0u);
	CHECK(read->sits[0].base == 0u);
	CHECK(!read->sits[0].on);

	Protocol::Sits many{ .full = true };
	for (std::uint32_t i = 0; i < Protocol::MAX_SITS; ++i) {
		many.sits.push_back(NpcIn({ 0x00100000 + i, 0 }, 0x00200000 + i, FORGE_BASE));
	}
	const auto bytes = Protocol::Encode(many);
	CHECK(bytes.size() == Protocol::MAX_SITS_SIZE);
	CHECK(Protocol::DecodeSits(bytes) == many);
	many.sits.push_back(NpcIn(SOLDIER));
	CHECK(Protocol::DecodeSits(Protocol::Encode(many))->sits.size() == Protocol::MAX_SITS);  // cut at the limit by the writer

	CHECK(!Protocol::DecodeSits(Protocol::Encode(Protocol::Sits{ .sits = { NpcIn({ 0, 0 }) } })));
	// The writer leaves a player's NPC out; written by hand, a player's sit that names one is refused.
	constexpr std::size_t FIRST = 1 + 4 + 1 + 1;  // type, sender, full, count
	auto withNpc = Protocol::Encode(Protocol::Sits{ .sits = { PlayerIn(1) } });
	withNpc[FIRST + 4] = 0x01;
	CHECK(!Protocol::DecodeSits(withNpc));
	auto badKind = Protocol::Encode(Protocol::Sits{ .sits = { PlayerIn(1) } });
	badKind[badKind.size() - 2] = Protocol::SIT_KIND_COUNT;
	CHECK(!Protocol::DecodeSits(badKind));
	auto badOn = Protocol::Encode(Protocol::Sits{ .sits = { PlayerIn(1) } });
	badOn.back() = 2;
	CHECK(!Protocol::DecodeSits(badOn));
	// An end that names furniture (written by hand) is refused.
	auto endWithChair = Protocol::Encode(Protocol::Sits{ .sits = { PlayerIn(1) } });
	endWithChair.back() = 0;
	CHECK(!Protocol::DecodeSits(endWithChair));
	auto badFull = Protocol::Encode(Protocol::Sits{ .sits = { PlayerIn(1) } });
	badFull[5] = 2;
	CHECK(!Protocol::DecodeSits(badFull));
	auto tooMany = Protocol::Encode(Protocol::Sits{});
	tooMany[6] = static_cast<std::uint8_t>(Protocol::MAX_SITS + 1);
	CHECK(!Protocol::DecodeSits(tooMany));
	auto offWorld = PlayerIn(1);
	offWorld.x = 1.0e30f;
	CHECK(!Protocol::DecodeSits(Protocol::Encode(Protocol::Sits{ .sits = { offWorld } })));
	auto shortOne = Protocol::Encode(Protocol::Sits{ .sits = { PlayerIn(1) } });
	shortOne.pop_back();
	CHECK(!Protocol::DecodeSits(shortOne));
	auto longOne = Protocol::Encode(Protocol::Sits{ .sits = { PlayerIn(1) } });
	longOne.push_back(0);
	CHECK(!Protocol::DecodeSits(longOne));
	CHECK(Protocol::SitHolds(PlayerUp(1)));
	CHECK(!Protocol::SitHolds(Sit{}));
}

TEST("sit form translate: an NPC the reader lacks is left out; furniture or a base it lacks becomes none, the sit kept")
{
	const Edges         e;
	const std::uint32_t ofB = 0x03000ABC;      // B.esp's in the host's numbering
	const std::uint32_t ofExtra = 0x04000ABC;  // Extra.esp's: the guest lacks it
	const std::uint32_t inGuest = 0x01000ABC;
	const std::uint32_t ofSkyrim = 0x00013BBF;

	const auto sits = Protocol::DecodeSits(e.HostToGuest(Protocol::Encode(Protocol::Sits{ .sender = 1,
		.sits = { PlayerIn(1, ofB, ofSkyrim), PlayerIn(2, ofExtra, ofB), NpcIn({ ofExtra, 0 }), NpcIn({ ofSkyrim, 0 }, ofSkyrim, ofExtra), NpcIn(SPAWN_OF_1, 0, ofB),
			NpcUp(SOLDIER) } })));
	REQUIRE(sits);
	CHECK(sits->sender == 1u);
	CHECK(sits->sits == std::vector<Sit>({ PlayerIn(1, inGuest, ofSkyrim), PlayerIn(2, 0, inGuest), NpcIn({ ofSkyrim, 0 }, ofSkyrim, 0), NpcIn(SPAWN_OF_1, 0, inGuest), NpcUp(SOLDIER) }));
}

// ---- the server's book ----

TEST("sit book: a player says only their own sit, an NPC's only its runner; each change once; getting up from nothing is nothing")
{
	SitBook::Book book;
	CHECK(book.Take(1, { PlayerIn(1) }, RunsNone()) == std::vector<Sit>({ PlayerIn(1) }));
	CHECK(book.Take(1, { PlayerIn(1) }, RunsNone()).empty());  // the same again
	CHECK(book.Take(2, { PlayerIn(1, BED, BED_BASE, Kind::kSleep) }, RunsNone()).empty());  // not Bo's to say
	CHECK(book.Take(1, { PlayerIn(1, BED, BED_BASE, Kind::kSleep) }, RunsNone()) == std::vector<Sit>({ PlayerIn(1, BED, BED_BASE, Kind::kSleep) }));
	CHECK(book.Take(1, { NpcIn(SOLDIER) }, RunsNone()).empty());  // Ada does not run the soldier
	CHECK(book.Take(2, { NpcIn(SOLDIER) }, RunsSoldier()) == std::vector<Sit>({ NpcIn(SOLDIER) }));
	CHECK(book.Take(0, { PlayerIn(0) }, RunsNone()).empty());
	CHECK(book.Refused() == 3u);
	CHECK(book.Count() == 2u);
	CHECK(book.Take(2, { PlayerUp(2) }, RunsNone()).empty());  // Bo was in nothing
	CHECK(book.Take(1, { PlayerUp(1) }, RunsNone()) == std::vector<Sit>({ PlayerUp(1) }));
	CHECK(book.All() == std::vector<Sit>({ NpcIn(SOLDIER) }));
}

TEST("sit book: a leaver's sits and their NPCs' end; owed changes go as their newest state; a handed NPC's sit is its new runner's")
{
	SitBook::Book book;
	(void)book.Take(1, { PlayerIn(1), NpcIn(SOLDIER) }, RunsSoldier());
	(void)book.Take(2, { PlayerIn(2, BED, BED_BASE, Kind::kSleep) }, RunsNone());
	const auto ended = book.Leave(1);
	CHECK(ended.size() == 2u);
	CHECK(std::ranges::find(ended, PlayerUp(1)) != ended.end());
	CHECK(std::ranges::find(ended, NpcUp(SOLDIER)) != ended.end());
	CHECK(book.All() == std::vector<Sit>({ PlayerIn(2, BED, BED_BASE, Kind::kSleep) }));

	book.Owe(2, PlayerIn(2, BED, BED_BASE, Kind::kSleep));
	(void)book.Take(2, { PlayerUp(2) }, RunsNone());
	Sit next;
	CHECK(book.NextOwed(2, next));
	CHECK(next == PlayerUp(2));
	CHECK(!book.Owes(2));

	// The soldier goes from Ada's game to Bo's, at the same forge: passed on again with its new sender; Ada's leave keeps it.
	CHECK(book.Take(1, { NpcIn(SOLDIER) }, RunsSoldier()).size() == 1u);
	CHECK(book.Take(2, { NpcIn(SOLDIER) }, RunsSoldier()).size() == 1u);
	CHECK(book.Leave(1).empty());
	CHECK(book.Leave(2) == std::vector<Sit>({ NpcUp(SOLDIER) }));
	book.Owe(3, PlayerIn(3));
	book.Clear();
	CHECK(!book.Owes(3));
	CHECK(book.Count() == 0u);
}

TEST("sit server: a sit goes to the others with its sender named, only from whoever may say it; a joiner hears them all; a leaver gets up")
{
	Fixture f;
	AllIn(f, 2);
	f.fake->Deliver(1, Protocol::Encode(Protocol::NpcClaims{ .claims = { { SOLDIER, Protocol::ClaimWhy::kHave } } }));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	// Bo's word of Ada's chair and of Ada's soldier is nobody's.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Sits{ .sits = { PlayerIn(1), NpcIn(SOLDIER) } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	CHECK(f.fake->Take(2).empty());

	f.fake->Deliver(1, Protocol::Encode(Protocol::Sits{ .sender = 99, .sits = { PlayerIn(1), NpcIn(SOLDIER), PlayerIn(2, BED, BED_BASE, Kind::kSleep) } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	const auto toBo = SitsTo(f, 2);
	REQUIRE(toBo.size() == 1u);
	CHECK(toBo[0].sender == 1u);
	CHECK(!toBo[0].full);
	CHECK(toBo[0].sits == std::vector<Sit>({ PlayerIn(1), NpcIn(SOLDIER) }));

	// The same again is not passed on.
	f.fake->Deliver(1, Protocol::Encode(Protocol::Sits{ .sits = { PlayerIn(1) } }));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(2).empty());

	// Cy joins: every sit, as a full list.
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto toCy = SitsTo(f, 3);
	REQUIRE(toCy.size() == 1u);
	CHECK(toCy[0].full);
	CHECK(toCy[0].sits.size() == 2u);
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	// Ada gets up: Bo and Cy hear it.
	f.fake->Deliver(1, Protocol::Encode(Protocol::Sits{ .sits = { PlayerUp(1) } }));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 2 }, PeerId{ 3 } }) {
		const auto up = SitsTo(f, peer);
		REQUIRE(up.size() == 1u);
		CHECK(up[0].sits == std::vector<Sit>({ PlayerUp(1) }));
	}

	// Ada leaves: Bo and Cy get her soldier up.
	f.fake->Drop(1);
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { PeerId{ 2 }, PeerId{ 3 } }) {
		const auto ends = SitsTo(f, peer);
		REQUIRE(ends.size() == 1u);
		CHECK(ends[0].sits == std::vector<Sit>({ NpcUp(SOLDIER) }));
	}
}

// ---- what a game says of its own ----

TEST("sit states: getting in, in it, getting up; asleep; a number the engine never uses is standing")
{
	CHECK(SitRules::InFurniture(State::kWantToSit));
	CHECK(SitRules::InFurniture(State::kWaitingForSleepAnim));
	CHECK(SitRules::InFurniture(State::kIsSitting));
	CHECK(SitRules::InFurniture(State::kIsSleeping));
	CHECK(!SitRules::InFurniture(State::kWantToStand));  // getting up is out of it already: the others' copy rises as it starts
	CHECK(!SitRules::InFurniture(State::kWantToWake));
	CHECK(!SitRules::InFurniture(State::kNormal));
	CHECK(SitRules::GettingUp(State::kWantToWake));
	CHECK(SitRules::Sleeping(State::kWantToWake));
	CHECK(!SitRules::Sleeping(State::kIsSitting));
	CHECK(SitRules::StateOf(3) == State::kIsSitting);
	CHECK(SitRules::StateOf(8) == State::kWantToWake);
	CHECK(SitRules::StateOf(9) == State::kNormal);
	CHECK(SitRules::StateOf(0xFFFFFFFF) == State::kNormal);
}

TEST("sit kinds: a bed, a perch, a crafting station, a seat, a railing, a work spot; asleep is a bed's whatever the flags")
{
	using namespace SitRules;
	CHECK(KindOf(State::kIsSitting, kCanSit, 0) == Kind::kSit);
	CHECK(KindOf(State::kIsSleeping, kCanSleep, 0) == Kind::kSleep);
	CHECK(KindOf(State::kWantToSleep, kCanSit | kCanSleep, 0) == Kind::kSleep);
	CHECK(KindOf(State::kIsSitting, kCanSleep, 0) == Kind::kSleep);  // a bed sat on
	CHECK(KindOf(State::kIsSitting, kIsPerch, 0) == Kind::kPerch);
	CHECK(KindOf(State::kIsSitting, kCanSit, 3) == Kind::kWork);  // a sitting crafting station (an alchemy lab, a tanning rack)
	CHECK(KindOf(State::kIsSitting, 0, 1) == Kind::kWork);
	CHECK(KindOf(State::kIsSitting, kCanLean, 0) == Kind::kLean);
	CHECK(KindOf(State::kIsSitting, 0, 0) == Kind::kWork);  // a chopping block, an ore vein
	for (std::uint8_t kind = 0; kind < Protocol::SIT_KIND_COUNT; ++kind) {
		CHECK(KindName(static_cast<Kind>(kind)) != "?");
	}
}

TEST("sit tell: in furniture or getting in is said with its reference, base, place and marker; a runtime one by base and place; standing, getting up or riding is nothing")
{
	SitRules::Own own{ .state = State::kIsSitting, .found = true, .furniture = true, .ref = CHAIR, .base = CHAIR_BASE, .x = 1000.4f, .y = -2000.6f, .z = 64.0f, .marker = 2, .flags = SitRules::kCanSit };
	const auto    sit = SitRules::Tell(own);
	REQUIRE(sit);
	CHECK(sit->furniture == CHAIR);
	CHECK(sit->base == CHAIR_BASE);
	CHECK(sit->x == 1000.0f);
	CHECK(sit->y == -2001.0f);
	CHECK(sit->marker == 2u);
	CHECK(sit->kind == Kind::kSit);
	CHECK(sit->on);
	CHECK(sit->player == 0u);  // the caller names who

	auto gettingIn = own;
	gettingIn.state = State::kWaitingForSitAnim;
	CHECK(SitRules::Tell(gettingIn));
	auto runtime = own;
	runtime.runtimeRef = true;
	runtime.ref = 0xFF001234;
	CHECK(SitRules::Tell(runtime)->furniture == 0u);
	CHECK(SitRules::Tell(runtime)->base == CHAIR_BASE);
	auto bigMarker = own;
	bigMarker.marker = 300;
	CHECK(SitRules::Tell(bigMarker)->marker == Protocol::NO_MARKER);

	auto standing = own;
	standing.state = State::kNormal;
	CHECK(!SitRules::Tell(standing));
	auto rising = own;
	rising.state = State::kWantToStand;
	CHECK(!SitRules::Tell(rising));
	auto riding = own;
	riding.onMount = true;
	CHECK(!SitRules::Tell(riding));
	auto notFound = own;
	notFound.found = false;
	CHECK(!SitRules::Tell(notFound));
	auto notFurniture = own;
	notFurniture.furniture = false;  // the process names an actor (a mount) or something else
	CHECK(!SitRules::Tell(notFurniture));
	auto offWorld = own;
	offWorld.z = 1.0e30f;
	CHECK(!SitRules::Tell(offWorld));
}

TEST("sit events: the engine's furniture play is never sent; a rider's (its sit state is the saddle's) is")
{
	CHECK(SitRules::EngineOwnsEvents(State::kWantToSit, false, false));
	CHECK(SitRules::EngineOwnsEvents(State::kIsSleeping, false, false));
	CHECK(SitRules::EngineOwnsEvents(State::kWantToStand, false, false));
	CHECK(!SitRules::EngineOwnsEvents(State::kNormal, false, false));
	CHECK(!SitRules::EngineOwnsEvents(State::kIsSitting, true, false));
	CHECK(!SitRules::EngineOwnsEvents(State::kWantToSit, false, true));
}

TEST("sit said: each sit of ours that begins or changes is said once, and the end of each said before")
{
	using Rider = SitRules::Said::Rider;
	SitRules::Said said;
	const Rider    us{ 1, {} };
	const Rider    soldier{ 0, SOLDIER };
	CHECK(said.Changes({}).empty());
	CHECK(said.Changes({ { us, In(CHAIR, CHAIR_BASE) } }) == std::vector<Sit>({ PlayerIn(1) }));
	CHECK(said.Changes({ { us, In(CHAIR, CHAIR_BASE) } }).empty());
	CHECK(said.Changes({ { us, In(BED, BED_BASE, Kind::kSleep) }, { soldier, In(FORGE, FORGE_BASE, Kind::kWork) } }) ==
		  std::vector<Sit>({ NpcIn(SOLDIER), PlayerIn(1, BED, BED_BASE, Kind::kSleep) }));
	// Our player got up; the soldier is no longer ours (or got up): both end.
	CHECK(said.Changes({}) == std::vector<Sit>({ NpcUp(SOLDIER), PlayerUp(1) }));
	CHECK(said.Count() == 0u);
}

TEST("sit said: an NPC handed to us that another game said sits is said from here, in its furniture or up (its end), even one we said before")
{
	using Rider = SitRules::Said::Rider;
	const Rider soldier{ 0, SOLDIER };
	const Rider keeper{ 0, INNKEEPER };
	SitRules::Said sitting;
	sitting.HandedOver(soldier);
	CHECK(sitting.Changes({ { soldier, In(FORGE, FORGE_BASE, Kind::kWork) } }) == std::vector<Sit>({ NpcIn(SOLDIER) }));
	CHECK(sitting.Changes({ { soldier, In(FORGE, FORGE_BASE, Kind::kWork) } }).empty());
	// Handed away and back, at the same forge all along: said again, so the session hears it from us.
	sitting.HandedOver(soldier);
	CHECK(sitting.Changes({ { soldier, In(FORGE, FORGE_BASE, Kind::kWork) } }) == std::vector<Sit>({ NpcIn(SOLDIER) }));
	SitRules::Said standing;
	standing.HandedOver(soldier);
	standing.HandedOver(keeper);
	CHECK(standing.Changes({}) == std::vector<Sit>({ NpcUp(SOLDIER), NpcUp(INNKEEPER) }));
	CHECK(standing.Changes({}).empty());
	SitRules::Said cleared;
	cleared.HandedOver(soldier);
	cleared.Clear();
	CHECK(cleared.Changes({}).empty());
}

// ---- finding the furniture here ----

TEST("sit pick: the reference named when it is here and of that base; else the nearest of that base where they said; none")
{
	const std::array<SitRules::Candidate, 4> here{ {
		{ .id = CHAIR, .base = CHAIR_BASE, .x = 1000, .y = -2000, .z = 64, .usable = true },
		{ .id = 0xFF000010, .base = BED_BASE, .x = 1030, .y = -2000, .z = 64, .usable = true },
		{ .id = 0xFF000011, .base = BED_BASE, .x = 1010, .y = -2000, .z = 64, .usable = true },
		{ .id = 0xFF000012, .base = BED_BASE, .x = 1001, .y = -2000, .z = 64, .usable = false },
	} };
	CHECK(SitRules::Pick(PlayerIn(1), here) == CHAIR);
	// A runtime bedroll: by base, the nearest usable within the radius.
	CHECK(SitRules::Pick(PlayerIn(1, 0, BED_BASE, Kind::kSleep), here) == 0xFF000011u);
	// Named, but of another base here (another plugin numbered there): by base and place.
	CHECK(SitRules::Pick(PlayerIn(1, CHAIR, BED_BASE, Kind::kSleep), here) == 0xFF000011u);
	// Named, base unknown here: the reference.
	CHECK(SitRules::Pick(PlayerIn(1, CHAIR, 0), here) == CHAIR);
	// Too far, or nothing known: none (shown standing).
	auto distant = PlayerIn(1, 0, BED_BASE, Kind::kSleep);
	distant.x += SitRules::MATCH_RADIUS + 50.0f;  // 68 from the nearest bedroll
	CHECK(!SitRules::Pick(distant, here));
	CHECK(!SitRules::Pick(PlayerIn(1, 0, 0), here));
	CHECK(!SitRules::Pick(PlayerIn(1, 0x00ABCDEF, 0), here));
}

TEST("sit taken: furniture our player is in is not given to theirs at the same marker, an unknown one, or one with a single marker")
{
	CHECK(!SitRules::TakenByUs(false, 0, 0, 1));
	CHECK(SitRules::TakenByUs(true, 1, 1, 2));
	CHECK(!SitRules::TakenByUs(true, 1, 0, 2));  // the other end of the bench
	CHECK(SitRules::TakenByUs(true, Protocol::NO_MARKER, 0, 2));
	CHECK(SitRules::TakenByUs(true, 1, 0, 1));
}

TEST("sit held: in furniture, getting in or up, occupying it, or near the furniture its friend is in: not placed")
{
	CHECK(SitRules::Held(false, false, State::kIsSitting, false));
	CHECK(SitRules::Held(false, false, State::kWantToStand, false));
	CHECK(SitRules::Held(false, false, State::kNormal, true));
	CHECK(SitRules::Held(true, true, State::kNormal, false));
	CHECK(!SitRules::Held(true, false, State::kNormal, false));  // far off: it walks to where its friend is
	CHECK(!SitRules::Held(false, true, State::kNormal, false));  // its friend stood up: placed again
}

// ---- putting it in, getting it up ----

TEST("sit seat: the furniture activated, then the console, in rounds 5 s apart; given up after 3; a new sit tries afresh")
{
	SitRules::Seat      seat;
	auto                t = Steady::now();
	SitRules::SeatFacts facts{ .wanted = true, .ready = false };
	CHECK(seat.Step(facts, t) == Act::kNone);  // not ready: the actor or the furniture still loading
	facts.ready = true;
	for (int round = 0; round < SitRules::MAX_ROUNDS; ++round) {
		CHECK(seat.Step(facts, t) == Act::kActivate);
		CHECK(seat.Step(facts, t + 1s) == Act::kNone);
		CHECK(seat.Step(facts, t + SitRules::CONSOLE_AFTER) == Act::kConsole);
		CHECK(seat.Step(facts, t + 3s) == Act::kNone);
		CHECK(seat.Step(facts, t + SitRules::ROUND_LENGTH) == Act::kNone);
		CHECK(seat.Rounds() == round + 1);
		CHECK(seat.Step(facts, t + SitRules::ROUND_LENGTH + 1s) == Act::kNone);  // the gap between rounds
		t += SitRules::ROUND_LENGTH + SitRules::ROUND_GAP;
	}
	CHECK(seat.GivenUp());
	CHECK(seat.Step(facts, t + 1h) == Act::kNone);
	CHECK(!seat.Seated());
	CHECK(!seat.Assist());
	CHECK(seat.Step(SitRules::SeatFacts{}, t) == Act::kNone);  // its friend got up
	CHECK(!seat.GivenUp());
	CHECK(seat.Step(facts, t) == Act::kActivate);
}

TEST("sit seat: the engine's update assists it in; seated by whichever try took; up by the engine's own getting up, then at once")
{
	SitRules::Seat      seat;
	const auto          t = Steady::now();
	SitRules::SeatFacts facts{ .wanted = true, .ready = true };
	CHECK(seat.Step(facts, t) == Act::kActivate);
	CHECK(seat.Step(facts, t + SitRules::CONSOLE_AFTER) == Act::kConsole);
	facts.inWanted = true;
	facts.state = State::kWantToSit;
	CHECK(seat.Step(facts, t + 3s) == Act::kNone);
	CHECK(seat.Assist());  // its sit procedure is the AI's: the engine's own update runs for it
	CHECK(!seat.Seated());
	facts.state = State::kIsSitting;
	CHECK(seat.Step(facts, t + 4s) == Act::kNone);
	CHECK(seat.Seated());
	CHECK(!seat.Assist());
	CHECK(seat.SeatedBy() == Act::kConsole);
	CHECK(seat.Seats() == 1u);
	CHECK(seat.Rounds() == 0);

	// Its friend got up: the engine's own getting up, assisted, then out at once if it is not up in time.
	facts.wanted = false;
	CHECK(seat.Step(facts, t + 5s) == Act::kStand);
	CHECK(!seat.Seated());
	facts.state = State::kWantToStand;
	CHECK(seat.Step(facts, t + 5s + 500ms) == Act::kNone);
	CHECK(seat.Assist());
	CHECK(seat.Step(facts, t + 5s + SitRules::STAND_WAIT) == Act::kQuick);
	CHECK(seat.Step(facts, t + 5s + SitRules::STAND_WAIT + 500ms) == Act::kNone);  // at most once a second
	CHECK(seat.Step(facts, t + 5s + SitRules::STAND_WAIT + SitRules::QUICK_EVERY) == Act::kQuick);
	facts.state = State::kNormal;
	facts.inWanted = false;
	CHECK(seat.Step(facts, t + 10s) == Act::kNone);
	CHECK(seat.Stands() == 1u);
	CHECK(seat.Quicks() == 2u);

	// Already in when first looked at (a joiner's list, a world loaded): seated, nothing tried.
	SitRules::Seat already;
	CHECK(already.Step({ .wanted = true, .ready = true, .state = State::kIsSitting, .inWanted = true }, t) == Act::kNone);
	CHECK(already.Seated());
	CHECK(already.SeatedBy() == Act::kNone);
	// Asleep, its friend woke: the same getting up (want-to-wake is the caller's).
	CHECK(already.Step({ .wanted = false, .ready = true, .state = State::kIsSleeping, .inWanted = true }, t + 1s) == Act::kStand);
}

TEST("sit seat: one stalled getting in is taken out and the rounds go on; one in other furniture gets up first; one getting up already is assisted")
{
	const auto t = Steady::now();
	{
		SitRules::Seat      seat;
		SitRules::SeatFacts facts{ .wanted = true, .ready = true };
		CHECK(seat.Step(facts, t) == Act::kActivate);
		facts.inWanted = true;
		facts.state = State::kWaitingForSitAnim;
		CHECK(seat.Step(facts, t + 1s) == Act::kNone);
		CHECK(seat.Assist());
		CHECK(seat.Step(facts, t + 1s + SitRules::ENTRY_MAX - 1ms) == Act::kNone);
		CHECK(seat.Step(facts, t + 1s + SitRules::ENTRY_MAX) == Act::kQuick);
		CHECK(seat.Stalls() == 1u);
		CHECK(!seat.Assist());
		facts.inWanted = false;
		facts.state = State::kNormal;
		CHECK(seat.Step(facts, t + 7500ms) == Act::kConsole);  // the round goes on
	}
	{
		SitRules::Seat seat;
		CHECK(seat.Step({ .wanted = true, .ready = true, .state = State::kIsSitting, .inOther = true }, t) == Act::kStand);
		CHECK(seat.Step({ .wanted = true, .ready = true, .state = State::kNormal }, t + 1s) == Act::kActivate);
	}
	{
		SitRules::Seat seat;
		CHECK(seat.Step({ .wanted = false, .state = State::kWantToStand, .inOther = true }, t) == Act::kNone);
		CHECK(seat.Assist());
		CHECK(seat.Stands() == 0u);
		CHECK(seat.Step({ .wanted = false, .state = State::kWantToStand, .inOther = true }, t + SitRules::STAND_WAIT) == Act::kQuick);
	}
	{
		// Getting in when it should not: out at once (no getting up from half-way in).
		SitRules::Seat seat;
		CHECK(seat.Step({ .wanted = false, .state = State::kWantToSit }, t) == Act::kQuick);
	}
	{
		// Getting up by itself while its friend is still in: it waits to be up, then the rounds put it back.
		SitRules::Seat seat;
		CHECK(seat.Step({ .wanted = true, .ready = true, .state = State::kWantToStand, .inWanted = true }, t) == Act::kNone);
		CHECK(seat.Step({ .wanted = true, .ready = true }, t + 1s) == Act::kActivate);
	}
}

TEST("sit acts: each try has a name for the developer channel")
{
	for (const auto act : { Act::kNone, Act::kActivate, Act::kConsole, Act::kStand, Act::kQuick }) {
		CHECK(SitRules::ActName(act) != "?");
	}
}
