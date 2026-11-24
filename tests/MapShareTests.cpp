#include "Test.h"

#include "FormTranslate.h"
#include "MapBook.h"
#include "ServerFixture.h"
#include "game/MapRules.h"

#include <chrono>
#include <optional>
#include <vector>

// Map markers shared (docs/DESIGN.md, "Map markers shared"): the markers a player discovers on the wire and through the
// server's book (common/MapBook.h), their forms between two load orders, and the rules free of the game
// (src/game/MapRules.h): what a marker gained, what one still lacks here, the batches and the notices.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;

	constexpr std::uint32_t WHITERUN_MARKER = 0x000164A9;
	constexpr std::uint32_t RIVERWOOD_MARKER = 0x00013A73;
	constexpr std::uint32_t BLEAK_FALLS_MARKER = 0x000B7E9A;

	constexpr std::uint8_t SEEN = Protocol::kMarkerVisible;
	constexpr std::uint8_t FOUND = Protocol::kMarkerVisible | Protocol::kMarkerTravel;

	std::vector<std::pair<Protocol::MapMarkers, bool>> SentMarkers(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<std::pair<Protocol::MapMarkers, bool>> out;
		for (const auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kMapMarkers) {
				if (auto msg = Protocol::DecodeMapMarkers(sent.data)) {
					out.emplace_back(std::move(*msg), sent.reliable);
				}
			}
		}
		return out;
	}

	Protocol::MapMarkers Found(std::vector<Protocol::MapMarker> a_markers)
	{
		return Protocol::MapMarkers{ 0, std::move(a_markers) };
	}
}

// ---- the wire ----

TEST("map share: this protocol is version 30 with map markers as message type 83, which carries forms")
{
	static_assert(Protocol::VERSION >= 30);
	static_assert(static_cast<int>(MT::kMapMarkers) == 83);
	CHECK(FormTranslate::CarriesForms(MT::kMapMarkers));
}

TEST("map share: markers go and come back as they were, up to the most a message holds; anything else is not read")
{
	const auto msg = Protocol::MapMarkers{ 2, { { WHITERUN_MARKER, FOUND }, { RIVERWOOD_MARKER, SEEN } } };
	const auto out = Protocol::DecodeMapMarkers(Protocol::Encode(msg));
	REQUIRE(out);
	CHECK(*out == msg);

	Protocol::MapMarkers full;
	for (std::uint32_t i = 1; i <= Protocol::MAX_MARKERS; ++i) {
		full.markers.push_back({ i, SEEN });
	}
	const auto packet = Protocol::Encode(full);
	CHECK(packet.size() <= Protocol::MAX_MAP_MARKERS_SIZE);
	REQUIRE(Protocol::DecodeMapMarkers(packet));
	CHECK(Protocol::DecodeMapMarkers(packet)->markers.size() == Protocol::MAX_MARKERS);

	// One more than a message holds is cut when written, and a count past it is not read.
	full.markers.push_back({ 9999, SEEN });
	CHECK(Protocol::DecodeMapMarkers(Protocol::Encode(full))->markers.size() == Protocol::MAX_MARKERS);
	auto tooMany = Protocol::Encode(Found({}));
	tooMany[5] = static_cast<std::uint8_t>((Protocol::MAX_MARKERS + 1) & 0xFF);
	tooMany[6] = static_cast<std::uint8_t>((Protocol::MAX_MARKERS + 1) >> 8);
	CHECK(!Protocol::DecodeMapMarkers(tooMany));

	CHECK(!Protocol::DecodeMapMarkers(Protocol::Encode(Found({ { 0, SEEN } }))));           // no reference
	CHECK(!Protocol::DecodeMapMarkers(Protocol::Encode(Found({ { WHITERUN_MARKER, 0 } }))));  // nothing of it
	CHECK(!Protocol::DecodeMapMarkers(Protocol::Encode(Found({ { WHITERUN_MARKER, 4 } }))));  // a flag no game sends
	auto cut = Protocol::Encode(msg);
	cut.pop_back();
	CHECK(!Protocol::DecodeMapMarkers(cut));
	auto longer = Protocol::Encode(msg);
	longer.push_back(0);
	CHECK(!Protocol::DecodeMapMarkers(longer));
}

TEST("map share: a marker reaches the other game as its own ID; one of a plugin it lacks is left out")
{
	const auto host = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Mine.esp", "Shared.esp" }, {});
	const auto guest = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Shared.esp" }, {});
	std::vector<std::string> session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const FormIds::Mapper guestEdge(guest, session);

	// Session numbering: Mine.esp is 01, Shared.esp 02. In the guest's game Shared.esp is 01.
	const auto msg = Protocol::MapMarkers{ 1, { { WHITERUN_MARKER, FOUND }, { 0x01000800, FOUND }, { 0x02000801, SEEN } } };
	const auto translated = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(msg));
	REQUIRE(translated.kind == FormTranslate::Result::Kind::kChanged);
	const auto in = Protocol::DecodeMapMarkers(translated.packet);
	REQUIRE(in);
	CHECK(in->finder == 1u);
	REQUIRE(in->markers.size() == 2);
	CHECK(in->markers[0] == (Protocol::MapMarker{ WHITERUN_MARKER, FOUND }));
	CHECK(in->markers[1] == (Protocol::MapMarker{ 0x01000801, SEEN }));
	CHECK(translated.entries == 1u);

	const auto same = FormTranslate::Translate(guestEdge, FormTranslate::Way::kToLocal, Protocol::Encode(Found({ { RIVERWOOD_MARKER, FOUND } })));
	CHECK(same.kind != FormTranslate::Result::Kind::kChanged);
}

// ---- the server's book ----

TEST("map share: the book passes on what a marker gains, each with all it has, never the same twice")
{
	MapBook::Book book;
	auto gained = book.Take({ { WHITERUN_MARKER, SEEN }, { RIVERWOOD_MARKER, FOUND } });
	REQUIRE(gained.size() == 2);
	CHECK(gained[0] == (Protocol::MapMarker{ WHITERUN_MARKER, SEEN }));
	CHECK(gained[1] == (Protocol::MapMarker{ RIVERWOOD_MARKER, FOUND }));

	CHECK(book.Take({ { WHITERUN_MARKER, SEEN }, { RIVERWOOD_MARKER, SEEN } }).empty());  // nothing new
	gained = book.Take({ { WHITERUN_MARKER, Protocol::kMarkerTravel } });                    // discovered after seen
	REQUIRE(gained.size() == 1);
	CHECK(gained[0] == (Protocol::MapMarker{ WHITERUN_MARKER, FOUND }));
	CHECK(book.Take({ { WHITERUN_MARKER, FOUND } }).empty());
	CHECK(book.Take({ { 0, FOUND }, { BLEAK_FALLS_MARKER, 0 } }).empty());
	CHECK(book.Size() == 2u);
}

TEST("map share: the book gives a joiner everything in messages' worth and keeps no more than its most")
{
	MapBook::Book                    book;
	std::vector<Protocol::MapMarker> many;
	for (std::uint32_t i = 1; i <= MapBook::MAX_BOOK + 10; ++i) {
		many.push_back({ i, SEEN });
	}
	CHECK(book.Take(many).size() == MapBook::MAX_BOOK);
	CHECK(book.Size() == MapBook::MAX_BOOK);
	CHECK(book.Refused() == 10u);
	// A marker it has still gains.
	CHECK(book.Take({ { 1, FOUND } }).size() == 1u);

	const auto all = book.All();
	REQUIRE(all.size() == (MapBook::MAX_BOOK + Protocol::MAX_MARKERS - 1) / Protocol::MAX_MARKERS);
	std::size_t total = 0;
	for (const auto& part : all) {
		CHECK(part.size() <= Protocol::MAX_MARKERS);
		total += part.size();
	}
	CHECK(total == MapBook::MAX_BOOK);
	CHECK(all.front().front() == (Protocol::MapMarker{ 1, FOUND }));
	CHECK(MapBook::Book{}.All().empty());
}

TEST("server: a player's finds go to the others with the finder named, reliably, not back; what the session has is not")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 2u, 3u }) {
		(void)f.fake->Take(peer);
	}

	f.fake->Deliver(2, Protocol::Encode(Found({ { WHITERUN_MARKER, FOUND }, { RIVERWOOD_MARKER, SEEN } })));
	f.fake->Deliver(3, Protocol::Encode(Found({ { WHITERUN_MARKER, FOUND }, { RIVERWOOD_MARKER, FOUND } })));  // Bo found Whiterun first
	f.fake->Deliver(1, Protocol::Encode(Protocol::MapMarkers{ 3, { { BLEAK_FALLS_MARKER, FOUND } } }));        // a finder named is ignored
	auto bad = Protocol::Encode(Found({ { BLEAK_FALLS_MARKER, SEEN } }));
	bad.pop_back();
	f.fake->Deliver(2, bad);
	REQUIRE(f.fake->Settle());

	const auto toAda = SentMarkers(f, 1);
	REQUIRE(toAda.size() == 2);
	CHECK(toAda[0].second);
	CHECK(toAda[0].first == (Protocol::MapMarkers{ 2, { { WHITERUN_MARKER, FOUND }, { RIVERWOOD_MARKER, SEEN } } }));
	CHECK(toAda[1].first == (Protocol::MapMarkers{ 3, { { RIVERWOOD_MARKER, FOUND } } }));

	const auto toBo = SentMarkers(f, 2);
	REQUIRE(toBo.size() == 2);
	CHECK(toBo[0].first == (Protocol::MapMarkers{ 3, { { RIVERWOOD_MARKER, FOUND } } }));
	CHECK(toBo[1].first == (Protocol::MapMarkers{ 1, { { BLEAK_FALLS_MARKER, FOUND } } }));

	const auto toCy = SentMarkers(f, 3);
	REQUIRE(toCy.size() == 2);
	CHECK(toCy[0].first.finder == 2u);
	CHECK(toCy[1].first == (Protocol::MapMarkers{ 1, { { BLEAK_FALLS_MARKER, FOUND } } }));
}

TEST("server: a player who joins later is told the session's markers, the finder none")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.fake->Settle());
	f.fake->Deliver(1, Protocol::Encode(Found({ { WHITERUN_MARKER, SEEN } })));
	f.fake->Deliver(1, Protocol::Encode(Found({ { WHITERUN_MARKER, FOUND }, { RIVERWOOD_MARKER, SEEN } })));
	REQUIRE(f.fake->Settle());

	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	const auto told = SentMarkers(f, 2);
	REQUIRE(told.size() == 1);
	CHECK(told[0].second);
	CHECK(told[0].first.finder == 0u);
	CHECK(told[0].first.markers == (std::vector<Protocol::MapMarker>{ { RIVERWOOD_MARKER, SEEN }, { WHITERUN_MARKER, FOUND } }));

	// None found yet: a joiner is told nothing.
	Fixture empty;
	REQUIRE(empty.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(empty.fake->Settle());
	CHECK(SentMarkers(empty, 1).empty());
}

// ---- the rules ----

TEST("map share: a marker gained only flags it had not; one lost is no news; a heard one is put on only when it adds")
{
	CHECK(MapRules::Gained(0, SEEN) == SEEN);
	CHECK(MapRules::Gained(SEEN, FOUND) == Protocol::kMarkerTravel);
	CHECK(MapRules::Gained(FOUND, FOUND) == 0);
	CHECK(MapRules::Gained(FOUND, SEEN) == 0);  // a quest took it off: not news
	CHECK(MapRules::Gained(0, 0xFF) == FOUND);  // flags no game sends are not counted

	CHECK(MapRules::Lacks(0, SEEN));
	CHECK(MapRules::Lacks(SEEN, FOUND));
	CHECK(!MapRules::Lacks(FOUND, SEEN));
	CHECK(!MapRules::Lacks(FOUND, FOUND));
}

TEST("map share: finds are sent in messages of at most the most each, in order")
{
	std::vector<Protocol::MapMarker> found;
	for (std::uint32_t i = 1; i <= 600; ++i) {
		found.push_back({ i, FOUND });
	}
	const auto batches = MapRules::Batches(found, Protocol::MAX_MARKERS);
	REQUIRE(batches.size() == 3);
	CHECK(batches[0].size() == Protocol::MAX_MARKERS);
	CHECK(batches[2].size() == 600 - 2 * Protocol::MAX_MARKERS);
	CHECK(batches[1].front().ref == Protocol::MAX_MARKERS + 1);
	CHECK(MapRules::Batches({}, Protocol::MAX_MARKERS).empty());
}

TEST("map share: at most three notices in five seconds; more again once the oldest is that old")
{
	MapRules::Feed feed;
	const auto     t = MapRules::Clock::time_point{} + 100s;
	CHECK(feed.Take(t));
	CHECK(feed.Take(t + 1s));
	CHECK(feed.Take(t + 2s));
	CHECK(!feed.Take(t + 3s));
	CHECK(!feed.Take(t + 4900ms));
	CHECK(feed.Take(t + 5s));  // the first is 5 s old
	CHECK(!feed.Take(t + 5s));
	CHECK(feed.Take(t + 20s));
}
