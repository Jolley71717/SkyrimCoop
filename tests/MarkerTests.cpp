#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"
#include "game/MarkerRules.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

// Friends on the compass and the maps, and the name over their head (docs/DESIGN.md, "Friends on the compass and the
// maps"): each player's status on the wire and through the server, its forms between two load orders, and the rules
// free of the game (src/game/MarkerRules.h): how each friend looks, where on the compass they are, where they are drawn
// on each view, the health beside their name, when a status is sent and the way in kept while inside.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;
	using MarkerRules::View;

	constexpr std::uint32_t TAMRIEL = 0x0000003C;
	constexpr std::uint32_t WHITERUN = 0x0001A26F;  // a walled city's worldspace (its map is Tamriel's)
	constexpr std::uint32_t BREEZEHOME = 0x000165A3;
	constexpr std::uint32_t BANNERED_MARE = 0x000165A7;
	constexpr std::uint32_t WHITERUN_HOLD = 0x00016772;  // a location

	bool Near(float a_a, float a_b, float a_tolerance = 1e-3f)
	{
		return std::fabs(a_a - a_b) <= a_tolerance;
	}

	Protocol::PlayerStatus InsideAt(std::uint32_t a_world, float a_x, float a_y, float a_z)
	{
		Protocol::PlayerStatus status;
		status.flags = Protocol::kStatusInside;
		status.mapWorld = a_world;
		status.mapX = a_x;
		status.mapY = a_y;
		status.mapZ = a_z;
		return status;
	}

	Protocol::PlayerState StateAt(std::uint32_t a_cell, std::uint32_t a_world, float a_x, float a_y, float a_z = 0.0f)
	{
		Protocol::PlayerState state;
		state.cell = a_cell;
		state.worldspace = a_world;
		state.x = a_x;
		state.y = a_y;
		state.z = a_z;
		return state;
	}

	std::vector<std::pair<Protocol::RemoteStatus, bool>> SentStatuses(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<std::pair<Protocol::RemoteStatus, bool>> out;
		for (const auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kRemoteStatus) {
				if (const auto status = Protocol::DecodeRemoteStatus(sent.data)) {
					out.emplace_back(*status, sent.reliable);
				}
			}
		}
		return out;
	}
}

// ---- the wire ----

TEST("status: this protocol is version 25 with statuses as message types 77 and 78, and both carry forms")
{
	static_assert(Protocol::VERSION >= 25);
	static_assert(static_cast<int>(MT::kPlayerStatus) == 77 && static_cast<int>(MT::kRemoteStatus) == 78);
	CHECK(FormTranslate::CarriesForms(MT::kPlayerStatus));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteStatus));
}

TEST("status: a status goes and comes back as it was, alone and with its player")
{
	auto status = InsideAt(TAMRIEL, 1234.5f, -98765.25f, -1500.0f);
	status.health = 37;
	status.flags |= Protocol::kStatusTravelling;
	status.location = WHITERUN_HOLD;
	const auto packet = Protocol::Encode(status);
	CHECK(packet.size() == 1 + Protocol::STATUS_BYTES);
	const auto back = Protocol::DecodePlayerStatus(packet);
	REQUIRE(back);
	CHECK(*back == status);

	const Protocol::RemoteStatus remote{ 7, status };
	const auto remotePacket = Protocol::Encode(remote);
	CHECK(remotePacket.size() == 1 + 4 + Protocol::STATUS_BYTES);
	const auto remoteBack = Protocol::DecodeRemoteStatus(remotePacket);
	REQUIRE(remoteBack);
	CHECK(*remoteBack == remote);

	// Outside: no way in, no spot.
	const Protocol::PlayerStatus outside;
	const auto plain = Protocol::DecodePlayerStatus(Protocol::Encode(outside));
	REQUIRE(plain);
	CHECK(*plain == outside);
}

TEST("status: a status no game could send is not read")
{
	const auto refused = [](const Protocol::PlayerStatus& a_status) { return !Protocol::DecodePlayerStatus(Protocol::Encode(a_status)).has_value(); };
	const auto good = InsideAt(TAMRIEL, 10.0f, 20.0f, 30.0f);
	CHECK(!refused(good));

	auto health = good;
	health.health = 101;
	CHECK(refused(health));

	auto flags = good;
	flags.flags |= 0x80;
	CHECK(refused(flags));

	for (const float bad : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 1e9f }) {
		auto spot = good;
		spot.mapY = bad;
		CHECK(refused(spot));
	}

	// A spot with no map to put it on.
	auto lost = good;
	lost.mapWorld = 0;
	CHECK(refused(lost));

	const auto packet = Protocol::Encode(good);
	auto       shorter = packet;
	shorter.pop_back();
	CHECK(!Protocol::DecodePlayerStatus(shorter));
	auto longer = packet;
	longer.push_back(0);
	CHECK(!Protocol::DecodePlayerStatus(longer));
	auto remote = Protocol::Encode(Protocol::RemoteStatus{ 2, good });
	remote.pop_back();
	CHECK(!Protocol::DecodeRemoteStatus(remote));
}

// ---- forms ----

TEST("status: a status's place and map reach the other game as its own IDs; a map it lacks takes the spot with it")
{
	// The guest lacks Mine.esp; the session's table has it (the host brought it).
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Mine.esp", "Shared.esp" }, {});
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Shared.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const FormIds::Mapper guestEdge(guest, session);

	// A town of Shared.esp in a worldspace of Shared.esp: 01 in the guest's game.
	auto status = InsideAt(0x02000D00, 100.0f, 200.0f, 300.0f);
	status.location = 0x02000D01;
	status.health = 50;
	auto translated = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(Protocol::RemoteStatus{ 3, status }));
	REQUIRE(translated.kind == FormTranslate::Result::Kind::kChanged);
	auto in = Protocol::DecodeRemoteStatus(translated.packet);
	REQUIRE(in);
	CHECK(in->playerId == 3u);
	CHECK(in->status.mapWorld == 0x01000D00u);
	CHECK(in->status.location == 0x01000D01u);
	CHECK(in->status.mapX == 100.0f);
	CHECK(in->status.health == 50);

	// Mine.esp's town in Mine.esp's worldspace: none, and no spot (the friend is drawn on no map of the guest's).
	status.mapWorld = 0x01000D00;
	status.location = 0x01000D01;
	translated = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(Protocol::RemoteStatus{ 3, status }));
	REQUIRE(translated.kind == FormTranslate::Result::Kind::kChanged);
	in = Protocol::DecodeRemoteStatus(translated.packet);
	REQUIRE(in);
	CHECK(in->status.mapWorld == 0u);
	CHECK(in->status.location == 0u);
	CHECK(in->status.mapX == 0.0f && in->status.mapY == 0.0f && in->status.mapZ == 0.0f);
	CHECK((in->status.flags & Protocol::kStatusInside) != 0);  // still inside, somewhere this game lacks
	CHECK(in->status.health == 50);

	// Skyrim's own forms are the same in both games.
	const auto skyrim = InsideAt(TAMRIEL, 1.0f, 2.0f, 3.0f);
	const auto same = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(Protocol::RemoteStatus{ 3, skyrim }));
	CHECK(same.kind != FormTranslate::Result::Kind::kChanged);
}

// ---- the server ----

TEST("server: a player's status is passed on to the others, reliably, not back to its sender; the same again is not")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 2u, 3u }) {
		(void)f.fake->Take(peer);
	}

	auto status = InsideAt(TAMRIEL, 5.0f, 6.0f, 7.0f);
	f.fake->Deliver(2, Protocol::Encode(status));
	f.fake->Deliver(2, Protocol::Encode(status));
	auto hurt = status;
	hurt.health = 40;
	f.fake->Deliver(2, Protocol::Encode(hurt));
	auto bad = Protocol::Encode(hurt);
	bad.pop_back();
	f.fake->Deliver(2, bad);
	REQUIRE(f.fake->Settle());

	for (const PeerId peer : { 1u, 3u }) {
		const auto sent = SentStatuses(f, peer);
		REQUIRE(sent.size() == 2);
		CHECK(sent[0].second);
		CHECK(sent[0].first.playerId == 2);
		CHECK(sent[0].first.status == status);
		CHECK(sent[1].first.status == hurt);
	}
	CHECK(SentStatuses(f, 2).empty());
}

TEST("server: a player who joins later is told each player's newest status, after that player's join")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	auto status = InsideAt(TAMRIEL, 5.0f, 6.0f, 7.0f);
	f.fake->Deliver(1, Protocol::Encode(status));
	status.health = 20;
	f.fake->Deliver(1, Protocol::Encode(status));
	REQUIRE(f.fake->Settle());

	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto                 sent = f.fake->TakeSent(3);
	std::optional<std::size_t> joinedAt;
	std::optional<std::size_t> statusAt;
	std::size_t                statuses = 0;
	for (std::size_t i = 0; i < sent.size(); ++i) {
		const auto type = Protocol::PeekType(sent[i].data);
		if (const auto joined = type == MT::kPlayerJoined ? Protocol::DecodePlayerJoined(sent[i].data) : std::nullopt; joined && joined->playerId == 1) {
			joinedAt = i;
		}
		if (const auto told = type == MT::kRemoteStatus ? Protocol::DecodeRemoteStatus(sent[i].data) : std::nullopt) {
			++statuses;
			statusAt = i;
			CHECK(told->playerId == 1);
			CHECK(told->status == status);
			CHECK(sent[i].reliable);
		}
	}
	REQUIRE(joinedAt);
	REQUIRE(statusAt);
	CHECK(*statusAt > *joinedAt);
	CHECK(statuses == 1u);  // Bo sent none: nothing is made up for them
}

TEST("server: a burst of statuses is held back past a few, and the newest still reaches everyone")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	Protocol::PlayerStatus last;
	for (int i = 0; i < 30; ++i) {
		last.health = static_cast<std::uint8_t>(99 - i);
		f.fake->Deliver(2, Protocol::Encode(last));
	}
	REQUIRE(f.fake->Settle());
	auto sent = SentStatuses(f, 1);
	CHECK(sent.size() >= 8u);
	CHECK(sent.size() < 30u);
	// The newest is owed and sent within a second.
	const auto deadline = std::chrono::steady_clock::now() + 5s;
	while ((sent.empty() || !(sent.back().first.status == last)) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(20ms);
		for (auto& more : SentStatuses(f, 1)) {
			sent.push_back(std::move(more));
		}
	}
	REQUIRE(!sent.empty());
	CHECK(sent.back().first.status == last);
}

// ---- how each friend looks ----

TEST("markers: the first four players each have their own shape and colour, the fifth the first's again")
{
	for (std::uint32_t a = 1; a <= 4; ++a) {
		for (std::uint32_t b = a + 1; b <= 4; ++b) {
			CHECK(MarkerRules::LookOf(a).shape != MarkerRules::LookOf(b).shape);
			CHECK(MarkerRules::LookOf(a).colour != MarkerRules::LookOf(b).colour);
		}
	}
	CHECK(MarkerRules::LookOf(5).shape == MarkerRules::LookOf(1).shape);
	CHECK(MarkerRules::LookOf(5).colour == MarkerRules::LookOf(1).colour);
	CHECK(MarkerRules::LookOf(0).shape == MarkerRules::LookOf(1).shape);  // no id: the first's
}

TEST("markers: each shape is a closed outline of its own number of corners, about its size across")
{
	using MarkerRules::Shape;
	const std::pair<Shape, std::size_t> corners[] = { { Shape::kCircle, 12 }, { Shape::kSquare, 4 }, { Shape::kDiamond, 4 }, { Shape::kTriangle, 3 } };
	for (const auto& [shape, count] : corners) {
		const auto outline = MarkerRules::Outline(shape, 10.0f);
		CHECK(outline.size() == count);
		for (const auto& point : outline) {
			CHECK(std::fabs(point.x) <= 6.5f);
			CHECK(std::fabs(point.y) <= 6.5f);
		}
	}
}

// ---- the compass ----

TEST("markers: headings are the game's: north 0, east a quarter turn; a friend's bearing is from where the camera looks")
{
	const float quarter = MarkerRules::PI / 2.0f;
	CHECK(Near(MarkerRules::Bearing(0, 1), 0));
	CHECK(Near(MarkerRules::Bearing(1, 0), quarter));
	CHECK(Near(MarkerRules::Bearing(-1, 0), -quarter));
	CHECK(Near(std::fabs(MarkerRules::Bearing(0, -1)), MarkerRules::PI));

	CHECK(Near(std::fabs(MarkerRules::Wrap(3 * MarkerRules::PI)), MarkerRules::PI));
	CHECK(Near(MarkerRules::Wrap(-3 * quarter), quarter));
	CHECK(Near(MarkerRules::Wrap(5 * quarter), quarter));
	CHECK(MarkerRules::Wrap(std::numeric_limits<float>::quiet_NaN()) == 0.0f);

	// Looking east, a friend to the north is a quarter turn to the left; one on our spot is ahead.
	CHECK(Near(MarkerRules::Relative(0, 500, quarter), -quarter));
	CHECK(Near(MarkerRules::Relative(500, 0, quarter), 0));
	CHECK(MarkerRules::Relative(0.2f, 0.1f, quarter) == 0.0f);

	// A camera's forward axis gives its heading; straight down has none.
	CHECK(Near(MarkerRules::Yaw(1, 0), quarter));
	CHECK(MarkerRules::Yaw(0, 0) == 0.0f);
}

TEST("markers: on the compass a friend ahead is in its middle, one at the span's end at its edge, one behind pinned there")
{
	const float half = MarkerRules::COMPASS_HALF;
	auto        spot = MarkerRules::CompassAt(0, 200);
	CHECK(Near(spot.x, 0) && !spot.pinned);
	spot = MarkerRules::CompassAt(half / 2, 200);
	CHECK(Near(spot.x, 100) && !spot.pinned);
	spot = MarkerRules::CompassAt(-half, 200);
	CHECK(Near(spot.x, -200) && !spot.pinned);
	spot = MarkerRules::CompassAt(half + 0.1f, 200);
	CHECK(Near(spot.x, 200) && spot.pinned);
	spot = MarkerRules::CompassAt(-MarkerRules::PI + 0.01f, 200);
	CHECK(Near(spot.x, -200) && spot.pinned);
	// Another span (the rig's `markers span`).
	spot = MarkerRules::CompassAt(0.5f, 100, 1.0f);
	CHECK(Near(spot.x, 50) && !spot.pinned);

	CHECK(MarkerRules::Alpha(false, false) == 100);
	CHECK(MarkerRules::Alpha(false, true) < 100);
	CHECK(MarkerRules::Alpha(true, false) < MarkerRules::Alpha(false, true));
}

// ---- where each friend is drawn ----

TEST("markers: outside on the same map a friend is where they stand, on every view; travelling, nowhere")
{
	const MarkerRules::Here here{ .cell = 0, .mapWorld = TAMRIEL };
	MarkerRules::Friend     friendOutside{ .state = StateAt(0, WHITERUN, 100, 200, 300), .stateMapWorld = TAMRIEL };
	for (const auto view : { View::kCompass, View::kLocalMap, View::kWorldMap }) {
		const auto spot = MarkerRules::Where(view, here, friendOutside);
		REQUIRE(spot);
		CHECK(*spot == (MarkerRules::Spot{ 100, 200, 300, false }));
	}

	auto travelling = friendOutside;
	travelling.status = Protocol::PlayerStatus{};
	travelling.status->flags = Protocol::kStatusTravelling;
	CHECK(!MarkerRules::Where(View::kCompass, here, travelling));
	CHECK(!MarkerRules::Where(View::kWorldMap, here, travelling));

	// On another map (Solstheim's), nowhere on ours.
	auto elsewhere = friendOutside;
	elsewhere.stateMapWorld = 0x04000800;
	CHECK(!MarkerRules::Where(View::kCompass, here, elsewhere));
	CHECK(!MarkerRules::Where(View::kWorldMap, here, elsewhere));

	// No state yet and no status: nowhere.
	CHECK(!MarkerRules::Where(View::kWorldMap, here, MarkerRules::Friend{}));
}

TEST("markers: a friend inside is drawn at the way in, fainter, while we are outside on that map or look at it")
{
	const MarkerRules::Here outside{ .cell = 0, .mapWorld = TAMRIEL };
	const MarkerRules::Friend inside{ .state = StateAt(BREEZEHOME, 0, 5, 5), .status = InsideAt(WHITERUN, 1000, 2000, 50), .statusMapWorld = TAMRIEL };
	for (const auto view : { View::kCompass, View::kLocalMap, View::kWorldMap }) {
		const auto spot = MarkerRules::Where(view, outside, inside);
		REQUIRE(spot);
		CHECK(*spot == (MarkerRules::Spot{ 1000, 2000, 50, true }));
	}

	// Inside with no way in known (another map, or none came yet): nowhere.
	auto lost = inside;
	lost.statusMapWorld = 0;
	CHECK(!MarkerRules::Where(View::kWorldMap, outside, lost));
	auto none = inside;
	none.status.reset();
	CHECK(!MarkerRules::Where(View::kWorldMap, outside, none));
}

TEST("markers: inside, the compass and the local map show only a friend in the same interior; the world map shows the world")
{
	const MarkerRules::Here   inMare{ .cell = BANNERED_MARE, .mapWorld = 0 };
	const MarkerRules::Friend withUs{ .state = StateAt(BANNERED_MARE, 0, 40, 50, 60), .status = InsideAt(WHITERUN, 1, 2, 3), .statusMapWorld = TAMRIEL };
	for (const auto view : { View::kCompass, View::kLocalMap }) {
		const auto spot = MarkerRules::Where(view, inMare, withUs);
		REQUIRE(spot);
		CHECK(*spot == (MarkerRules::Spot{ 40, 50, 60, false }));
	}
	const MarkerRules::Friend nextDoor{ .state = StateAt(BREEZEHOME, 0, 40, 50), .status = InsideAt(WHITERUN, 1, 2, 3), .statusMapWorld = TAMRIEL };
	CHECK(!MarkerRules::Where(View::kCompass, inMare, nextDoor));
	CHECK(!MarkerRules::Where(View::kLocalMap, inMare, nextDoor));
	const MarkerRules::Friend outside{ .state = StateAt(0, TAMRIEL, 9, 9), .stateMapWorld = TAMRIEL };
	CHECK(!MarkerRules::Where(View::kCompass, inMare, outside));

	// The world map open while we are inside is the map's (FriendMarkers asks with the map's world): both are drawn.
	const MarkerRules::Here map{ .cell = 0, .mapWorld = TAMRIEL };
	CHECK(MarkerRules::Where(View::kWorldMap, map, outside));
	const auto wayIn = MarkerRules::Where(View::kWorldMap, map, nextDoor);
	REQUIRE(wayIn);
	CHECK(wayIn->inside);
}

// ---- the name over their head ----

TEST("markers: a friend's name has their health beside it when hurt: to the nearest ten, 10 to 90, 0 only at 0")
{
	CHECK(!MarkerRules::HealthShown(100));
	CHECK(MarkerRules::HealthShown(99) == 90);
	CHECK(MarkerRules::HealthShown(64) == 60);
	CHECK(MarkerRules::HealthShown(65) == 70);
	CHECK(MarkerRules::HealthShown(4) == 10);
	CHECK(MarkerRules::HealthShown(1) == 10);
	CHECK(MarkerRules::HealthShown(0) == 0);
}

TEST("markers: the name is shown while looked at and for a moment after, within its range")
{
	using MarkerRules::LabelShown;
	CHECK(LabelShown(true, std::nullopt, 100.0f));
	CHECK(LabelShown(true, 0ms, MarkerRules::LABEL_RANGE));
	CHECK(!LabelShown(true, 0ms, MarkerRules::LABEL_RANGE + 1.0f));
	CHECK(!LabelShown(true, 0ms, std::numeric_limits<float>::quiet_NaN()));
	CHECK(LabelShown(false, MarkerRules::LABEL_LINGER - 1ms, 100.0f));
	CHECK(!LabelShown(false, MarkerRules::LABEL_LINGER + 1ms, 100.0f));
	CHECK(!LabelShown(false, std::nullopt, 100.0f));
}

TEST("markers: a camera's view (0 to 1, y up) is placed on a movie's rectangle (y down); out of view is not drawn")
{
	const MarkerRules::Rect stage{ 0, 0, 1280, 720 };
	CHECK(MarkerRules::ToMovie(0.5f, 0.5f, stage) == (MarkerRules::Point{ 640, 360 }));
	CHECK(MarkerRules::ToMovie(0, 1, stage) == (MarkerRules::Point{ 0, 0 }));
	CHECK(MarkerRules::ToMovie(1, 0, stage) == (MarkerRules::Point{ 1280, 720 }));
	const MarkerRules::Rect clip{ 100, 50, 300, 250 };
	CHECK(MarkerRules::ToMovie(0.25f, 0.75f, clip) == (MarkerRules::Point{ 150, 100 }));

	CHECK(MarkerRules::InView(0.5f, 0.5f));
	CHECK(MarkerRules::InView(0, 1));
	CHECK(!MarkerRules::InView(-0.01f, 0.5f));
	CHECK(MarkerRules::InView(-0.01f, 0.5f, 0.05f));
	CHECK(!MarkerRules::InView(std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.0f));
}

// ---- our status, sent ----

TEST("markers: a status is sent first, travel at once, other changes no sooner than the gap and only when they count")
{
	using MarkerRules::Worth;
	Protocol::PlayerStatus sent;
	sent.health = 80;
	CHECK(Worth(std::nullopt, sent, 0ms));
	CHECK(!Worth(sent, sent, 10s));

	auto travel = sent;
	travel.flags = Protocol::kStatusTravelling;
	CHECK(Worth(sent, travel, 0ms));
	CHECK(Worth(travel, sent, 0ms));  // arrived

	auto hurt = sent;
	hurt.health = 70;
	CHECK(!Worth(sent, hurt, MarkerRules::STATUS_GAP - 1ms));
	CHECK(Worth(sent, hurt, MarkerRules::STATUS_GAP));
	auto scratch = sent;
	scratch.health = 78;
	CHECK(!Worth(sent, scratch, 10s));
	auto healed = scratch;
	healed.health = 100;
	auto almost = sent;
	almost.health = 98;
	CHECK(Worth(almost, healed, 10s));  // onto full: the name loses its health
	auto dead = sent;
	dead.health = 2;
	auto zero = dead;
	zero.health = 0;
	CHECK(Worth(dead, zero, 10s));

	auto in = InsideAt(TAMRIEL, 0, 0, 0);
	CHECK(Worth(sent, in, 1s));
	auto moved = in;
	moved.mapX = MarkerRules::SPOT_MOVE + 1.0f;
	CHECK(Worth(in, moved, 1s));
	auto nudged = in;
	nudged.mapX = 100.0f;
	CHECK(!Worth(in, nudged, 1s));
	auto place = sent;
	place.location = WHITERUN_HOLD;
	CHECK(Worth(sent, place, 1s));
}

TEST("markers: health in percent is 0 only when dead, 1 for a sliver, 100 when not known")
{
	CHECK(MarkerRules::HealthPercent(50, 100) == 50);
	CHECK(MarkerRules::HealthPercent(100, 100) == 100);
	CHECK(MarkerRules::HealthPercent(150, 100) == 100);
	CHECK(MarkerRules::HealthPercent(0.1f, 500) == 1);
	CHECK(MarkerRules::HealthPercent(0, 100) == 0);
	CHECK(MarkerRules::HealthPercent(-5, 100) == 0);
	CHECK(MarkerRules::HealthPercent(50, 0) == 100);
	CHECK(MarkerRules::HealthPercent(std::numeric_limits<float>::quiet_NaN(), 100) == 100);
}

TEST("markers: inside, the way in is where we were last outside; after a load, the door out; nothing known, none")
{
	MarkerRules::WayIn     wayIn;
	Protocol::PlayerStatus status;
	wayIn.Fill(status);
	CHECK((status.flags & Protocol::kStatusInside) != 0);
	CHECK(status.mapWorld == 0u);
	CHECK(Protocol::DecodePlayerStatus(Protocol::Encode(status)));  // inside, nowhere on a map, still sent

	Protocol::PlayerStatus fromDoor;
	wayIn.Fill(fromDoor, std::pair{ TAMRIEL, MarkerRules::Point{ 10, 20 } }, 30);
	CHECK(fromDoor.mapWorld == TAMRIEL);
	CHECK(fromDoor.mapX == 10.0f && fromDoor.mapY == 20.0f && fromDoor.mapZ == 30.0f);

	wayIn.Outside(WHITERUN, 1, 2, 3);
	CHECK(wayIn.Known());
	Protocol::PlayerStatus kept;
	wayIn.Fill(kept, std::pair{ TAMRIEL, MarkerRules::Point{ 10, 20 } }, 30);  // the walk in, not the door
	CHECK(kept.mapWorld == WHITERUN);
	CHECK(kept.mapX == 1.0f && kept.mapY == 2.0f && kept.mapZ == 3.0f);

	wayIn.Outside(0, 5, 5, 5);  // no world: not kept
	wayIn.Outside(TAMRIEL, std::numeric_limits<float>::quiet_NaN(), 5, 5);
	Protocol::PlayerStatus still;
	wayIn.Fill(still);
	CHECK(still.mapWorld == WHITERUN);

	wayIn.Forget();
	CHECK(!wayIn.Known());
	Protocol::PlayerStatus forgot;
	wayIn.Fill(forgot);
	CHECK(forgot.mapWorld == 0u);
}
