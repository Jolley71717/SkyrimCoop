#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"
#include "game/TravelRules.h"

#include <chrono>
#include <limits>
#include <vector>

// Fast travel together (docs/DESIGN.md, "Fast travel together"): a trip on the wire and through the server, its forms
// between two load orders, and the rule free of the game (src/game/TravelRules.h): who was near, what is a trip, when a
// follow is refused, how long an offer stands and the key's name.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;

	constexpr std::uint32_t TAMRIEL = 0x0000003C;
	constexpr std::uint32_t RIVERWOOD_MARKER = 0x00013A73;
	constexpr std::uint32_t BREEZEHOME = 0x000165A3;

	Protocol::Trip TripFrom(std::uint32_t a_cell, std::uint32_t a_world, float a_x, float a_y, float a_z = 0.0f)
	{
		Protocol::Trip trip;
		trip.destination = RIVERWOOD_MARKER;
		trip.fromCell = a_cell;
		trip.fromWorld = a_world;
		trip.x = a_x;
		trip.y = a_y;
		trip.z = a_z;
		return trip;
	}

	std::vector<std::pair<Protocol::Trip, bool>> SentTrips(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<std::pair<Protocol::Trip, bool>> out;
		for (const auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kTrip) {
				if (const auto trip = Protocol::DecodeTrip(sent.data)) {
					out.emplace_back(*trip, sent.reliable);
				}
			}
		}
		return out;
	}
}

TEST("travel: this protocol is version 31 with trips as message type 84, which carries forms")
{
	static_assert(Protocol::VERSION >= 31);
	static_assert(static_cast<int>(MT::kTrip) == 84);
	CHECK(FormTranslate::CarriesForms(MT::kTrip));
}

TEST("travel: a trip goes and comes back as it was; one to nowhere or from no spot is not read")
{
	auto trip = TripFrom(0x00009732, TAMRIEL, 100.0f, -200.0f, 30.0f);
	trip.traveller = 2;
	const auto packet = Protocol::Encode(trip);
	CHECK(packet.size() == Protocol::TRIP_SIZE);
	const auto back = Protocol::DecodeTrip(packet);
	REQUIRE(back);
	CHECK(*back == trip);

	auto nowhere = trip;
	nowhere.destination = 0;
	CHECK(!Protocol::DecodeTrip(Protocol::Encode(nowhere)));
	auto far = trip;
	far.x = std::numeric_limits<float>::infinity();
	CHECK(!Protocol::DecodeTrip(Protocol::Encode(far)));
	auto cut = packet;
	cut.pop_back();
	CHECK(!Protocol::DecodeTrip(cut));
	auto longer = packet;
	longer.push_back(0);
	CHECK(!Protocol::DecodeTrip(longer));
}

TEST("travel: a trip's marker and place reach the other game as its own IDs; a marker it lacks is not read there")
{
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Mine.esp", "Shared.esp" }, {});
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Shared.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const FormIds::Mapper guestEdge(guest, session);

	auto trip = TripFrom(0x02000900, 0x02000901, 1.0f, 2.0f);
	trip.destination = 0x02000902;
	auto translated = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(trip));
	REQUIRE(translated.kind == FormTranslate::Result::Kind::kChanged);
	auto in = Protocol::DecodeTrip(translated.packet);
	REQUIRE(in);
	CHECK(in->destination == 0x01000902u);
	CHECK(in->fromCell == 0x01000900u);
	CHECK(in->fromWorld == 0x01000901u);

	// Mine.esp's marker: none, which the guest refuses (nowhere to follow to).
	trip.destination = 0x01000902;
	translated = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(trip));
	REQUIRE(translated.kind == FormTranslate::Result::Kind::kChanged);
	CHECK(!Protocol::DecodeTrip(translated.packet));

	// Left from Mine.esp's place: nowhere there, so no one is near it.
	trip = TripFrom(0x01000900, 0x01000901, 1.0f, 2.0f);
	translated = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(trip));
	REQUIRE(translated.kind == FormTranslate::Result::Kind::kChanged);
	in = Protocol::DecodeTrip(translated.packet);
	REQUIRE(in);
	CHECK(in->fromCell == 0u);
	CHECK(in->fromWorld == 0u);
	CHECK(!TravelRules::Near(*in, 0, 0, 1.0f, 2.0f, 0.0f));
}

TEST("server: a trip goes to the others with the traveller named, reliably, not back; a flood of them is cut")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 2u }) {
		(void)f.fake->Take(peer);
	}

	auto trip = TripFrom(0x00009732, TAMRIEL, 1.0f, 2.0f);
	trip.traveller = 7;  // a game's word on who travelled is not taken
	f.fake->Deliver(2, Protocol::Encode(trip));
	REQUIRE(f.fake->Settle());
	const auto toAda = SentTrips(f, 1);
	REQUIRE(toAda.size() == 1);
	CHECK(toAda[0].second);
	CHECK(toAda[0].first.traveller == 2u);
	CHECK(toAda[0].first.destination == RIVERWOOD_MARKER);
	CHECK(SentTrips(f, 2).empty());

	for (int i = 0; i < 20; ++i) {
		f.fake->Deliver(2, Protocol::Encode(trip));
	}
	REQUIRE(f.fake->Settle());
	const auto flood = SentTrips(f, 1);
	CHECK(flood.size() >= 3u);
	CHECK(flood.size() < 20u);
}

TEST("travel: a friend is near a trip's start in the same worldspace or interior, within the range")
{
	const auto outside = TripFrom(0x00009732, TAMRIEL, 0.0f, 0.0f);
	CHECK(TravelRules::Near(outside, 0x00009733, TAMRIEL, 3000.0f, 0.0f, 0.0f));  // another exterior cell is fine
	CHECK(!TravelRules::Near(outside, 0x00009733, TAMRIEL, 4500.0f, 0.0f, 0.0f));
	CHECK(!TravelRules::Near(outside, BREEZEHOME, 0, 10.0f, 0.0f, 0.0f));  // inside while they left from outside

	const auto inside = TripFrom(BREEZEHOME, 0, 0.0f, 0.0f);
	CHECK(TravelRules::Near(inside, BREEZEHOME, 0, 200.0f, 0.0f, 0.0f));
	CHECK(!TravelRules::Near(inside, 0x000165A7, 0, 200.0f, 0.0f, 0.0f));  // another interior
	CHECK(!TravelRules::Near(inside, 0x00009732, TAMRIEL, 0.0f, 0.0f, 0.0f));
}

TEST("travel: a queued move is a trip when it names a marker, or goes far or elsewhere and is no door's")
{
	using TravelRules::Start;
	TravelRules::Queued queued;
	CHECK(TravelRules::TripOf(queued) == Start::kNone);
	queued.valid = true;
	queued.marker = true;
	CHECK(TravelRules::TripOf(queued) == Start::kMarker);  // the map's fast travel
	queued.marker = false;
	queued.distance = 100.0f;
	CHECK(TravelRules::TripOf(queued) == Start::kNone);  // a step
	queued.distance = 50000.0f;
	CHECK(TravelRules::TripOf(queued) == Start::kNearest);  // a carriage moved by script
	queued.distance = 0.0f;
	queued.otherSpace = true;
	CHECK(TravelRules::TripOf(queued) == Start::kNearest);
	queued.door = true;
	CHECK(TravelRules::TripOf(queued) == Start::kNone);  // a load door
}

TEST("travel: a queued move is under way while it goes elsewhere or away from the player's spot, not once it has landed")
{
	TravelRules::Queued queued;
	CHECK(!TravelRules::Active(queued));
	queued.valid = true;
	CHECK(!TravelRules::Active(queued));  // left set on the spot it landed on
	queued.distance = 400.0f;
	CHECK(!TravelRules::Active(queued));
	queued.distance = 600.0f;
	CHECK(TravelRules::Active(queued));
	queued.distance = 0.0f;
	queued.otherSpace = true;
	CHECK(TravelRules::Active(queued));
	queued.valid = false;
	CHECK(!TravelRules::Active(queued));
}

TEST("travel: fast travel is off where an interior's cell or, outside, the worldspace says so")
{
	CHECK(!TravelRules::TravelOff(false, false, false));
	CHECK(TravelRules::TravelOff(false, false, true));   // Sovngarde, the Soul Cairn
	CHECK(!TravelRules::TravelOff(false, true, false));  // an exterior cell's bit is not read
	CHECK(TravelRules::TravelOff(true, true, false));
	CHECK(!TravelRules::TravelOff(true, false, true));   // inside, the cell decides
}

TEST("travel: following is refused with no offer, on the way already, where fast travel is off, busy, in combat or carrying too much, in that order")
{
	using TravelRules::Refusal;
	TravelRules::Now now;
	CHECK(TravelRules::Refuse(now) == Refusal::kNoOffer);
	now.offer = true;
	CHECK(TravelRules::Refuse(now) == Refusal::kNone);
	now.overWeight = true;
	CHECK(TravelRules::Refuse(now) == Refusal::kWeight);
	now.combat = true;
	CHECK(TravelRules::Refuse(now) == Refusal::kCombat);
	now.busy = true;
	CHECK(TravelRules::Refuse(now) == Refusal::kBusy);
	now.placeOff = true;
	CHECK(TravelRules::Refuse(now) == Refusal::kPlace);
	now.travelling = true;
	CHECK(TravelRules::Refuse(now) == Refusal::kTravelling);

	CHECK(TravelRules::RefusalText(Refusal::kCombat) == "travel.refused_combat");
	CHECK(TravelRules::RefusalText(Refusal::kWeight) == "travel.refused_weight");
	CHECK(TravelRules::RefusalText(Refusal::kBusy) == "travel.refused_now");
	CHECK(TravelRules::RefusalText(Refusal::kPlace) == "travel.refused_place");
	CHECK(TravelRules::RefusalName(Refusal::kPlace) == "place");
	CHECK(TravelRules::RefusalText(Refusal::kNoOffer).empty());
	CHECK(TravelRules::RefusalText(Refusal::kTravelling).empty());
}

TEST("travel: an offer stands a minute, a newer one replaces it, and taking it ends it")
{
	TravelRules::Offers offers;
	const auto          t = TravelRules::Clock::time_point{} + 100s;
	CHECK(!offers.Live(t));
	auto first = TripFrom(0x00009732, TAMRIEL, 0.0f, 0.0f);
	first.traveller = 2;
	offers.Take(first, t);
	REQUIRE(offers.Live(t + 59s));
	CHECK(offers.Live(t + 59s)->traveller == 2u);
	CHECK(!offers.Live(t + 60s));

	auto second = first;
	second.traveller = 3;
	offers.Take(first, t);
	offers.Take(second, t + 10s);
	CHECK(offers.Live(t + 65s)->traveller == 3u);
	offers.End();
	CHECK(!offers.Live(t + 11s));
}

TEST("travel: a key's name is the keyboard's: F7 by default, letters and digits, others by number")
{
	CHECK(TravelRules::KeyName(TravelRules::DEFAULT_FOLLOW_KEY) == "F7");
	CHECK(TravelRules::KeyName(0x3B) == "F1");
	CHECK(TravelRules::KeyName(0x44) == "F10");
	CHECK(TravelRules::KeyName(0x57) == "F11");
	CHECK(TravelRules::KeyName(0x58) == "F12");
	CHECK(TravelRules::KeyName(0x22) == "G");
	CHECK(TravelRules::KeyName(0x10) == "Q");
	CHECK(TravelRules::KeyName(0x26) == "L");
	CHECK(TravelRules::KeyName(0x32) == "M");
	CHECK(TravelRules::KeyName(0x02) == "1");
	CHECK(TravelRules::KeyName(0x0B) == "0");
	CHECK(TravelRules::KeyName(0xD2) == "210");
}
