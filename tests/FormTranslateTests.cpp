#include "Test.h"

#include "FormTranslate.h"
#include "ServerFixture.h"

#include <string>
#include <vector>

// Each game's edge between its form IDs and the session's (common/FormTranslate.h): a form is named on the wire by its
// plugin's file name (its place in the session's plugin table), so two games with their plugins in other orders, or
// with plugins the other lacks, agree on every form they both have and skip the rest.

using FormIds::Mapper;
using FormTranslate::Result;
using FormTranslate::SkipLog;
using FormTranslate::Translate;
using FormTranslate::Way;
using Names = std::vector<std::string>;

namespace
{
	using MT = Protocol::MessageType;

	Names Order(const Names& a_full, const Names& a_light = {})
	{
		return Protocol::ComparedLoadOrder(a_full, a_light);
	}

	// Two games and the session between them: the host's load order, a friend's with the same plugins in another order
	// and one more light plugin, and the session's table the server makes of the two.
	struct TwoGames
	{
		Names  host = Order({ "Skyrim.esm", "Update.esm", "A.esp", "B.esp" }, { "L1.esl" });
		Names  guest = Order({ "Skyrim.esm", "B.esp", "Update.esm", "A.esp" }, { "L2.esl", "L1.esl" });
		Names  session;
		Mapper hostEdge;
		Mapper guestEdge;

		TwoGames()
		{
			FormIds::Extend(session, host);
			FormIds::Extend(session, guest);
			hostEdge = Mapper(host, session);
			guestEdge = Mapper(guest, session);
		}
	};

	// a_packet through a_mapper: the packet the other side reads (the same one when nothing changed).
	std::vector<std::uint8_t> Through(const Mapper& a_mapper, Way a_way, const std::vector<std::uint8_t>& a_packet, Result* a_result = nullptr)
	{
		auto result = Translate(a_mapper, a_way, a_packet);
		auto out = result.kind == Result::Kind::kChanged ? result.packet : a_packet;
		if (a_result) {
			*a_result = std::move(result);
		}
		return out;
	}

	Protocol::PlayerState StateIn(std::uint32_t a_cell, std::uint32_t a_worldspace)
	{
		Protocol::PlayerState state;
		state.sequence = 7;
		state.cell = a_cell;
		state.worldspace = a_worldspace;
		state.x = 100.0f;
		state.y = -200.0f;
		state.z = 30.0f;
		state.heading = 1.0f;
		state.speed = 250.0f;
		state.flags = Protocol::kSprinting;
		state.sentMs = 99;
		state.direction = 64;
		return state;
	}

	// The server passes a player's state and looks on as they came, under the player's number.
	std::vector<std::uint8_t> Relayed(const std::vector<std::uint8_t>& a_sent)
	{
		if (const auto state = Protocol::DecodePlayerState(a_sent)) {
			return Protocol::Encode(Protocol::RemoteState{ 1, *state });
		}
		if (const auto looks = Protocol::DecodePlayerLooks(a_sent)) {
			return Protocol::Encode(Protocol::RemoteLooks{ 1, *looks });
		}
		return {};
	}
}

TEST("form translate: with the same plugins in the same places every packet without a runtime form passes as it is")
{
	const auto   order = Order({ "Skyrim.esm", "A.esp" }, { "L.esl" });
	const Mapper same(order, order);
	REQUIRE(same.Identity());
	const auto state = Protocol::Encode(StateIn(0, 0x0100003C));
	const auto result = Translate(same, Way::kToSession, state);
	CHECK(result.kind == Result::Kind::kSame);
	CHECK(result.packet.empty());
	// Before the session's table has come (a mapper of nothing) too.
	CHECK(Translate(Mapper(), Way::kToLocal, state).kind == Result::Kind::kSame);
}

TEST("form translate: a state's place, a look's race and worn items reach the other game as its own IDs, both ways")
{
	const TwoGames g;
	REQUIRE(!g.guestEdge.Identity());
	// The host is in A.esp's worldspace (02 in the host's game, 03 in the guest's), wearing an item of Skyrim.esm, one
	// of L1.esl (light 0 in the host's, light 1 in the guest's) and one of B.esp (03 there, 01 here), of a B.esp race.
	const auto       hostState = StateIn(0, 0x0200003C);
	Protocol::PlayerLooks hostLooks{ .race = 0x03000D62, .female = true, .weight = 50.0f, .worn = { 0x00012E46, 0xFE000801, 0x03000900 } };

	const auto stateIn = Protocol::DecodeRemoteState(Through(g.guestEdge, Way::kToLocal, Relayed(Through(g.hostEdge, Way::kToSession, Protocol::Encode(hostState)))));
	REQUIRE(stateIn);
	CHECK(stateIn->playerId == 1u);
	CHECK(stateIn->state.worldspace == 0x0300003Cu);
	CHECK(stateIn->state.cell == 0u);
	auto expected = hostState;
	expected.worldspace = 0x0300003C;
	CHECK(stateIn->state == expected);  // nothing else of the state touched

	const auto looksIn = Protocol::DecodeRemoteLooks(Through(g.guestEdge, Way::kToLocal, Relayed(Through(g.hostEdge, Way::kToSession, Protocol::Encode(hostLooks)))));
	REQUIRE(looksIn);
	CHECK(looksIn->looks.race == 0x01000D62u);
	CHECK(looksIn->looks.worn == std::vector<std::uint32_t>({ 0x00012E46, 0xFE001801, 0x01000900 }));
	CHECK(looksIn->looks.female);
	CHECK(looksIn->looks.weight == 50.0f);

	// And the guest's, the other way: an interior cell of B.esp (01 there, 03 here).
	const auto guestState = StateIn(0x01001234, 0);
	Result     out;
	const auto onWire = Through(g.guestEdge, Way::kToSession, Protocol::Encode(guestState), &out);
	CHECK(out.kind == Result::Kind::kChanged);
	CHECK(!out.Skipped());
	Result     in;
	const auto back = Protocol::DecodeRemoteState(Through(g.hostEdge, Way::kToLocal, Relayed(onWire), &in));
	REQUIRE(back);
	CHECK(back->state.cell == 0x03001234u);
	CHECK(in.kind == Result::Kind::kSame);  // the host's order is the start of the session's: nothing to change
}

TEST("form translate: what names only the game's own base forms is left as it is, though the orders differ")
{
	const TwoGames g;
	const auto     state = Protocol::Encode(StateIn(0, 0x0000003C));  // Tamriel, Skyrim.esm's
	const auto     result = Translate(g.guestEdge, Way::kToSession, state);
	CHECK(result.kind == Result::Kind::kSame);
	CHECK(result.unmapped.empty());
	// None (0) is nobody's and stays none.
	const Protocol::PlayerLooks looks{ .race = 0x00013746, .worn = { 0x00012E46 } };
	CHECK(Translate(g.guestEdge, Way::kToSession, Protocol::Encode(looks)).kind == Result::Kind::kSame);
	CHECK(Translate(g.guestEdge, Way::kToSession, Protocol::Encode(StateIn(0, 0))).kind == Result::Kind::kSame);
}

TEST("form translate: a runtime form (FF) is skipped both ways in every message, whatever the two load orders")
{
	// A runtime ID is one game's own: in another it names nothing, or that game's own runtime form of the same number
	// (an enchanted helmet for an armor). With the same load orders too, so the identity mapper skips it as well.
	const TwoGames g;
	const auto     order = Order({ "Skyrim.esm", "A.esp" });
	const Mapper   same(order, order);
	REQUIRE(same.Identity());
	for (const Mapper* edge : { &g.guestEdge, &same, &g.hostEdge }) {
		for (const auto way : { Way::kToSession, Way::kToLocal }) {
			// A worn runtime armor is left out of the list; the plugin armors beside it still go, and the race.
			Result     result;
			const auto sent = Protocol::PlayerLooks{ .race = 0x00013746, .worn = { 0x00012E46, 0xFF000D2A, 0xFF000801 } };
			const auto looks = Protocol::DecodePlayerLooks(Through(*edge, way, Protocol::Encode(sent), &result));
			REQUIRE(looks);
			CHECK(looks->worn == std::vector<std::uint32_t>({ 0x00012E46 }));
			CHECK(looks->race == 0x00013746u);
			CHECK(result.kind == Result::Kind::kChanged);
			CHECK(result.entries == 2u);
			CHECK(result.runtime == 2u);
			CHECK(result.Skipped());
			// A runtime race reads as none (the stand-in is a Nord); a relayed looks the same.
			const auto raced = Protocol::DecodeRemoteLooks(Through(*edge, way, Protocol::Encode(Protocol::RemoteLooks{ 3, { .race = 0xFF000900, .worn = {} } }), &result));
			REQUIRE(raced);
			CHECK(raced->looks.race == 0u);
			CHECK(result.fields == 1u);
			// A place with a runtime cell is nowhere this game has.
			const auto state = Protocol::DecodeRemoteState(Through(*edge, way, Protocol::Encode(Protocol::RemoteState{ 3, StateIn(0xFF000A00, 0x0000003C) }), &result));
			REQUIRE(state);
			CHECK(state->state.cell == 0u);
			CHECK(state->state.worldspace == 0u);
			CHECK(result.placeless);
		}
	}
}

TEST("form translate: a form of a plugin the other game lacks is skipped, never guessed, and the message still goes")
{
	// The guest lacks Mine.esp and MyLight.esl; the session's table has them (the host brought them).
	const auto   host = Order({ "Skyrim.esm", "Mine.esp", "Shared.esp" }, { "MyLight.esl" });
	const auto   guest = Order({ "Skyrim.esm", "Shared.esp" });
	Names        session;
	FormIds::Extend(session, host);
	FormIds::Extend(session, guest);
	const Mapper guestEdge(guest, session);

	// The host stands in Mine.esp's worldspace: the guest has no such place. It reads as nowhere it has (both 0), so
	// no stand-in is shown in the wrong world, and the rest of the state still comes.
	Result     result;
	const auto state = Protocol::DecodeRemoteState(Through(guestEdge, Way::kToLocal, Protocol::Encode(Protocol::RemoteState{ 4, StateIn(0, 0x0100003C) }), &result));
	REQUIRE(state);
	CHECK(state->state.cell == 0u);
	CHECK(state->state.worldspace == 0u);
	CHECK(state->state.x == 100.0f);
	CHECK(result.placeless);
	CHECK(result.unmapped == std::vector<std::uint32_t>({ 0x0100003C }));

	// An interior cell of Mine.esp in an exterior's place pair is nowhere too (a cell and a worldspace are one place).
	const auto mixed = Protocol::DecodeRemoteState(Through(guestEdge, Way::kToLocal, Protocol::Encode(Protocol::RemoteState{ 4, StateIn(0x01000805, 0x0200003C) })));
	REQUIRE(mixed);
	CHECK(mixed->state.cell == 0u);
	CHECK(mixed->state.worldspace == 0u);

	// A race of Mine.esp reads as none (the stand-in takes a race every game has); worn items of the plugins the guest
	// lacks are left out, the shared one's mapped.
	const Protocol::PlayerLooks looks{ .race = 0x01000D62, .weight = 20.0f, .worn = { 0x01000900, 0xFE000801, 0x02000901, 0x00012E46 } };
	Result     lookResult;
	const auto lookIn = Protocol::DecodeRemoteLooks(Through(guestEdge, Way::kToLocal, Protocol::Encode(Protocol::RemoteLooks{ 4, looks }), &lookResult));
	REQUIRE(lookIn);
	CHECK(lookIn->looks.race == 0u);
	CHECK(lookIn->looks.worn == std::vector<std::uint32_t>({ 0x01000901, 0x00012E46 }));
	CHECK(lookIn->looks.weight == 20.0f);
	CHECK(lookResult.fields == 1u);
	CHECK(lookResult.entries == 2u);
	CHECK(!lookResult.placeless);
	CHECK(lookResult.unmapped == std::vector<std::uint32_t>({ 0x01000D62, 0x01000900, 0xFE000801 }));

	// Going out, a form of a plugin of ours the session lacks is skipped the same way.
	const Mapper hostEdge(host, Order({ "Skyrim.esm", "Shared.esp" }));
	const auto   out = Protocol::DecodePlayerLooks(Through(hostEdge, Way::kToSession, Protocol::Encode(Protocol::PlayerLooks{ .worn = { 0x01000900, 0x02000901 } })));
	REQUIRE(out);
	CHECK(out->worn == std::vector<std::uint32_t>({ 0x01000901 }));
}

TEST("form translate: a plugin full in one game and light in the other, and a form no light plugin can number")
{
	const auto   ours = Order({ "Skyrim.esm" }, { "Both.esp" });
	const auto   session = Order({ "Skyrim.esm", "Both.esp" });
	const Mapper m(ours, session);
	const Protocol::PlayerLooks looks{ .worn = { 0xFE000801 } };
	const auto   out = Protocol::DecodePlayerLooks(Through(m, Way::kToSession, Protocol::Encode(looks)));
	REQUIRE(out);
	CHECK(out->worn == std::vector<std::uint32_t>({ 0x01000801 }));
	// Coming in: Both.esp's form 1000 (full there) has no ID in a game that loads it light.
	Result     result;
	const auto in = Protocol::DecodePlayerLooks(Through(m, Way::kToLocal, Protocol::Encode(Protocol::PlayerLooks{ .worn = { 0x01001000, 0x01000801 } }), &result));
	REQUIRE(in);
	CHECK(in->worn == std::vector<std::uint32_t>({ 0xFE000801 }));
	CHECK(result.entries == 1u);
}

TEST("form translate: a packet that can't be read, and every message that names no form, are left as they are")
{
	const TwoGames g;
	auto           cut = Protocol::Encode(StateIn(0, 0x0200003C));
	cut.pop_back();
	CHECK(Translate(g.guestEdge, Way::kToSession, cut).kind == Result::Kind::kSame);
	CHECK(Translate(g.guestEdge, Way::kToSession, std::vector<std::uint8_t>{}).kind == Result::Kind::kSame);
	const auto hello = Protocol::Encode(Protocol::Hello{ .name = "Lydia", .plugins = g.guest });
	CHECK(Translate(g.guestEdge, Way::kToSession, hello).kind == Result::Kind::kSame);
	CHECK(Translate(g.guestEdge, Way::kToLocal, Protocol::Encode(Protocol::PluginTable{ g.session })).kind == Result::Kind::kSame);
	CHECK(Translate(g.guestEdge, Way::kToLocal, std::vector<std::uint8_t>{ 200, 1, 2, 3 }).kind == Result::Kind::kSame);  // a type this version doesn't know

	CHECK(FormTranslate::CarriesForms(MT::kPlayerState));
	CHECK(FormTranslate::CarriesForms(MT::kRemoteLooks));
	CHECK(!FormTranslate::CarriesForms(MT::kHello));
	CHECK(!FormTranslate::CarriesForms(MT::kPluginTable));
	CHECK(!FormTranslate::CarriesForms(static_cast<MT>(200)));
}

TEST("form translate: each plugin whose forms were skipped is named once, by file name, from the right table")
{
	const auto   host = Order({ "Skyrim.esm", "Mine.esp" }, { "MyLight.esl" });
	const auto   guest = Order({ "Skyrim.esm" });
	Names        session;
	FormIds::Extend(session, host);
	const Mapper guestEdge(guest, session);
	SkipLog      log;

	const auto twenty = Protocol::Encode(Protocol::RemoteState{ 2, StateIn(0, 0x0100003C) });
	auto       result = Translate(guestEdge, Way::kToLocal, twenty);
	CHECK(log.Fresh(guestEdge, Way::kToLocal, result) == Names({ "Mine.esp" }));
	// A friend in a mod's world sends 20 states a second: the plugin is not named again.
	for (int i = 0; i < 20; ++i) {
		result = Translate(guestEdge, Way::kToLocal, twenty);
		CHECK(log.Fresh(guestEdge, Way::kToLocal, result).empty());
	}
	// Another form of it (any case of its name) is not either; another plugin is.
	result = Translate(guestEdge, Way::kToLocal, Protocol::Encode(Protocol::RemoteLooks{ 2, { .worn = { 0x01000900, 0xFE000801 } } }));
	CHECK(log.Fresh(guestEdge, Way::kToLocal, result) == Names({ "MyLight.esl" }));
	CHECK(log.Count() == 2u);

	// A place past the session's table (a table that grew after the packet went, or a peer's mistake) has no name: it is
	// said once as that.
	result = Translate(guestEdge, Way::kToLocal, Protocol::Encode(Protocol::RemoteState{ 2, StateIn(0, 0x0700003C) }));
	CHECK(log.Fresh(guestEdge, Way::kToLocal, result) == Names({ "" }));
	CHECK(log.Fresh(guestEdge, Way::kToLocal, result).empty());

	// Going out, our own plugins are named from our own table.
	const Mapper hostEdge(host, guest);
	SkipLog      ours;
	result = Translate(hostEdge, Way::kToSession, Protocol::Encode(StateIn(0x01000805, 0)));
	CHECK(ours.Fresh(hostEdge, Way::kToSession, result) == Names({ "Mine.esp" }));
	log.Clear();
	CHECK(log.Count() == 0u);

	// Runtime forms are no plugin's: never named as one, said once a session on their own.
	result = Translate(guestEdge, Way::kToLocal, Protocol::Encode(Protocol::RemoteLooks{ 2, { .worn = { 0xFF000D2A, 0x01000900 } } }));
	CHECK(log.Fresh(guestEdge, Way::kToLocal, result) == Names({ "Mine.esp" }));
	CHECK(!log.FreshRuntime(Translate(guestEdge, Way::kToLocal, twenty)));  // a plugin's form skipped, no runtime one
	CHECK(log.FreshRuntime(result));
	CHECK(!log.FreshRuntime(result));
	log.Clear();
	CHECK(log.FreshRuntime(result));
}

TEST("form translate: a plugin's form is named by file name and local id, the same form in every load order that has it")
{
	const TwoGames g;
	const auto     host = FormIds::Table::Of(g.host);
	const auto     guest = FormIds::Table::Of(g.guest);
	CHECK(host.FormOf("A.esp", 0x000900) == 0x02000900u);
	CHECK(guest.FormOf("a.ESP", 0x000900) == 0x03000900u);
	CHECK(host.FormOf("L1.esl", 0x801) == 0xFE000801u);
	CHECK(guest.FormOf("L1.esl", 0x801) == 0xFE001801u);
	CHECK(!host.FormOf("L2.esl", 0x801));      // not in the host's game
	CHECK(!guest.FormOf("L1.esl", 0x1000));    // past what a light plugin numbers
	// Both say one session ID for it.
	CHECK(g.hostEdge.ToSession(*host.FormOf("A.esp", 0x900)) == g.guestEdge.ToSession(*guest.FormOf("A.esp", 0x900)));
}

TEST("form translate: two games with their plugins in other orders, through the real server, each read the other's forms as their own")
{
	Fixture    f;
	const auto hostOrder = Order({ "Skyrim.esm", "Update.esm", "A.esp", "B.esp" }, { "L1.esl" });
	const auto guestOrder = Order({ "Skyrim.esm", "B.esp", "Update.esm", "A.esp", "GuestOnly.esp" }, { "L1.esl" });
	auto       hostHello = Fixture::MakeHello("Host");
	hostHello.plugins = hostOrder;
	hostHello.contentHash = Protocol::LoadOrderHash(hostOrder);
	auto guestHello = Fixture::MakeHello("Guest");
	guestHello.plugins = guestOrder;
	guestHello.contentHash = Protocol::LoadOrderHash(guestOrder);

	// Each game makes its edge from the newest table it heard (the guest's grew the host's by GuestOnly.esp).
	const auto newestTable = [&](PeerId a_peer) {
		std::optional<Names> newest;
		for (const auto& packet : f.fake->Take(a_peer)) {
			if (!packet.empty() && packet[0] == static_cast<std::uint8_t>(MT::kPluginTable)) {
				newest = Protocol::DecodePluginTable(packet)->plugins;
			}
		}
		return newest;
	};
	const auto hostWelcome = f.Join(1, hostHello);
	REQUIRE(hostWelcome);
	const auto guestWelcome = f.Join(2, guestHello);
	REQUIRE(guestWelcome);
	REQUIRE(f.fake->WaitFor(1, static_cast<std::uint8_t>(MT::kPlayerJoined)));
	const auto guestTable = newestTable(2);
	REQUIRE(guestTable);
	// The host heard the same grown table before the guest's arrival (the server's tests check that); both use it.
	const Mapper hostEdge(hostOrder, *guestTable);
	const Mapper guestEdge(guestOrder, *guestTable);
	CHECK(hostEdge.SessionsOnly() == 1u);  // GuestOnly.esp
	CHECK(guestEdge.OursOnly() == 0u);

	// The host wears an item of B.esp (03 in its game, 01 in the guest's).
	const auto hostSends = Through(hostEdge, Way::kToSession, Protocol::Encode(Protocol::PlayerLooks{ .race = 0x00013746, .worn = { 0x03000900 } }));
	f.fake->Deliver(1, hostSends);
	const auto toGuest = f.fake->WaitFor(2, static_cast<std::uint8_t>(MT::kRemoteLooks));
	REQUIRE(toGuest);
	const auto guestReads = Protocol::DecodeRemoteLooks(Through(guestEdge, Way::kToLocal, *toGuest));
	REQUIRE(guestReads);
	CHECK(guestReads->playerId == hostWelcome->playerId);
	CHECK(guestReads->looks.worn == std::vector<std::uint32_t>({ 0x01000900 }));

	// The guest walks into GuestOnly.esp's worldspace (04 there; the host has none), then into A.esp's (03 there, 02 here).
	f.fake->Deliver(2, Through(guestEdge, Way::kToSession, Protocol::Encode(StateIn(0, 0x0400003C))));
	const auto first = f.fake->WaitFor(1, static_cast<std::uint8_t>(MT::kRemoteState));
	REQUIRE(first);
	Result     nowhere;
	const auto hostReadsFirst = Protocol::DecodeRemoteState(Through(hostEdge, Way::kToLocal, *first, &nowhere));
	REQUIRE(hostReadsFirst);
	CHECK(hostReadsFirst->state.worldspace == 0u);
	CHECK(nowhere.placeless);
	auto later = StateIn(0, 0x0300003C);
	later.sequence = 8;
	f.fake->Deliver(2, Through(guestEdge, Way::kToSession, Protocol::Encode(later)));
	const auto second = f.fake->WaitFor(1, static_cast<std::uint8_t>(MT::kRemoteState));
	REQUIRE(second);
	const auto hostReads = Protocol::DecodeRemoteState(Through(hostEdge, Way::kToLocal, *second));
	REQUIRE(hostReads);
	CHECK(hostReads->state.worldspace == 0x0200003Cu);
}
