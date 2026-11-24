#include "Test.h"

#include "ServerFixture.h"

#include <string>
#include <vector>

// The session server (common/Server.cpp), driven through a FakeTransport: who gets in, what the others hear, and
// what is passed on.

namespace
{
	using MT = Protocol::MessageType;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	Protocol::PlayerState StateAt(std::uint32_t a_sequence, float a_x)
	{
		Protocol::PlayerState state;
		state.sequence = a_sequence;
		state.worldspace = 0x3C;
		state.x = a_x;
		state.y = 20.0f;
		state.z = 30.0f;
		state.heading = 1.5f;
		state.speed = 150.0f;
		state.sentMs = 77;
		return state;
	}

	// The remote states the server has sent a_peer so far, in the order sent.
	std::vector<Protocol::RemoteState> RemoteStates(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<Protocol::RemoteState> out;
		for (const auto& packet : a_fixture.fake->Take(a_peer)) {
			if (Protocol::PeekType(packet) == MT::kRemoteState) {
				if (const auto state = Protocol::DecodeRemoteState(packet)) {
					out.push_back(*state);
				}
			}
		}
		return out;
	}
}

TEST("server: the first player is welcomed as player 1, the second as player 2, and each hears of the other")
{
	Fixture f;
	const auto first = f.Join(1, Fixture::MakeHello("Ada"));
	REQUIRE(first);
	CHECK(first->playerId == 1);
	CHECK(first->hostId == 0);
	CHECK(first->sessionId != 0);

	const auto second = f.Join(2, Fixture::MakeHello("Bo"));
	REQUIRE(second);
	CHECK(second->playerId == 2);
	CHECK(second->sessionId == first->sessionId);

	// The one who joined is told who is there; the one who was there is told who joined.
	const auto toSecond = f.fake->WaitFor(2, Type(MT::kPlayerJoined));
	REQUIRE(toSecond);
	const auto there = Protocol::DecodePlayerJoined(*toSecond);
	REQUIRE(there);
	CHECK(there->playerId == 1);
	CHECK(there->name == "Ada");

	const auto toFirst = f.fake->WaitFor(1, Type(MT::kPlayerJoined));
	REQUIRE(toFirst);
	const auto joined = Protocol::DecodePlayerJoined(*toFirst);
	REQUIRE(joined);
	CHECK(joined->playerId == 2);
	CHECK(joined->name == "Bo");

	CHECK(f.LoggedSoon("server: player 2 joined from fake 2 (2 in the session)"));
	CHECK(f.server.Players() == 2);
}

TEST("server: a player without a name is shown as Player N")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("   ")));
	const auto packet = f.fake->WaitFor(1, Type(MT::kPlayerJoined));
	REQUIRE(packet);
	const auto joined = Protocol::DecodePlayerJoined(*packet);
	REQUIRE(joined);
	CHECK(joined->name == "Player 2");
}

TEST("server: a hello of another protocol version is refused, saying both versions")
{
	Fixture f;
	auto    hello = Fixture::MakeHello("Old");
	hello.version = static_cast<std::uint16_t>(Protocol::VERSION + 1);
	const auto reason = f.Refused(1, hello);
	CHECK(reason == std::format("This game runs another version of the mod (protocol {}, the host has {}). Both players need the same version.",
							Protocol::VERSION + 1, Protocol::VERSION));
	CHECK(f.fake->WaitDisconnected(1));
	CHECK(f.LoggedSoon(std::format("server: refused fake 1: protocol {} (ours is {})", Protocol::VERSION + 1, Protocol::VERSION)));
	CHECK(f.server.Players() == 0);
}

TEST("server: a hello of another program is refused")
{
	Fixture f;
	auto    hello = Fixture::MakeHello("Stranger");
	hello.magic = 0x12345678;
	CHECK(f.Refused(1, hello) == "This is not a SkyrimCoop game.");
	CHECK(f.fake->WaitDisconnected(1));
}

TEST("server: a hello cut short is refused as unreadable")
{
	Fixture f;
	auto    packet = Protocol::Encode(Fixture::MakeHello("Cut"));
	packet.resize(packet.size() - 3);
	f.fake->Connect(1);
	f.fake->Deliver(1, packet);
	const auto answer = f.fake->WaitFor(1, Type(MT::kReject));
	REQUIRE(answer);
	const auto reject = Protocol::DecodeReject(*answer);
	REQUIRE(reject);
	CHECK(reject->reason == "The join request could not be read.");
	CHECK(f.fake->WaitDisconnected(1));
}

TEST("server: the password is asked of everyone when one is set")
{
	Server::Options options;
	options.password = "sweetroll";
	Fixture f{ options };

	auto wrong = Fixture::MakeHello("Guesser");
	wrong.password = "sweetrol";
	CHECK(f.Refused(1, wrong) == "Wrong password.");
	CHECK(f.fake->WaitDisconnected(1));
	CHECK(f.LoggedSoon("server: refused fake 1: wrong password"));

	CHECK(f.Refused(2, Fixture::MakeHello("NoPassword")) == "Wrong password.");

	auto right = Fixture::MakeHello("Friend");
	right.password = "sweetroll";
	const auto welcome = f.Join(3, right);
	REQUIRE(welcome);
	CHECK(welcome->playerId == 1);

	// The log never holds the password itself.
	for (const auto& line : f.LogLines()) {
		CHECK(line.find("sweetrol") == std::string::npos);
	}
}

TEST("server: a full session refuses the next player, and lets one in again when someone leaves")
{
	Server::Options options;
	options.maxPlayers = 2;
	Fixture f{ options };
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));

	CHECK(f.Refused(3, Fixture::MakeHello("Cy")) == "The game is full (2 players).");
	CHECK(f.fake->WaitDisconnected(3));
	CHECK(f.LoggedSoon("server: refused fake 3: the session is full"));

	f.fake->Drop(2);
	CHECK(f.LoggedSoon("server: player 2 left (disconnected)"));
	const auto welcome = f.Join(4, Fixture::MakeHello("Cy"));
	REQUIRE(welcome);
	CHECK(welcome->playerId == 3);
}

TEST("server: more players than the protocol allows are never let in, whatever the option says")
{
	Server::Options options;
	options.maxPlayers = 100;
	Fixture f{ options };
	CHECK(f.LoggedSoon(std::format("(max {} players", Protocol::MAX_PLAYERS)));
	for (PeerId peer = 1; peer <= Protocol::MAX_PLAYERS; ++peer) {
		REQUIRE(f.Join(peer, Fixture::MakeHello(std::format("P{}", peer))));
	}
	CHECK(f.Refused(100, Fixture::MakeHello("Late")) == std::format("The game is full ({} players).", Protocol::MAX_PLAYERS));
}

namespace
{
	// The plugin tables the server sent a_peer so far (everything else it sent is forgotten).
	std::vector<std::vector<std::string>> TablesSent(Fixture& a_f, PeerId a_peer)
	{
		std::vector<std::vector<std::string>> out;
		for (const auto& packet : a_f.fake->Take(a_peer)) {
			if (packet.empty() || packet[0] != Type(MT::kPluginTable)) {
				continue;
			}
			if (const auto table = Protocol::DecodePluginTable(packet)) {
				out.push_back(table->plugins);
			}
		}
		return out;
	}

	Protocol::Hello HelloWith(std::string a_name, const std::vector<std::string>& a_full, const std::vector<std::string>& a_light = {})
	{
		auto hello = Fixture::MakeHello(std::move(a_name));
		hello.plugins = Protocol::ComparedLoadOrder(a_full, a_light);
		hello.contentHash = Protocol::LoadOrderHash(hello.plugins);
		return hello;
	}

	std::vector<std::vector<std::string>> Tables(const std::vector<std::string>& a_table)
	{
		return { a_table };
	}

	bool LogHas(Fixture& a_f, std::string_view a_text)
	{
		return std::ranges::any_of(a_f.LogLines(), [&](const std::string& a_line) { return a_line.find(a_text) != std::string::npos; });
	}
}

TEST("server: the session's plugin table is the first player's load order, sent before the welcome")
{
	Fixture    f;
	const auto host = HelloWith("Host", { "Skyrim.esm", "Update.esm" }, { "Small.esl" });
	f.fake->Connect(1);
	f.fake->Deliver(1, Protocol::Encode(host));
	const auto upToWelcome = f.fake->TakeThrough(1, Type(MT::kWelcome));
	REQUIRE(upToWelcome.size() == 2u);
	const auto table = Protocol::DecodePluginTable(upToWelcome[0]);
	REQUIRE(table);
	CHECK(table->plugins == host.plugins);
	CHECK(f.LoggedSoon("server: the session's plugin table has 3 plugins (3 new)"));
}

TEST("server: another load order is let in, its other plugins added to the table, which everyone in hears first")
{
	Fixture    f;
	const auto host = HelloWith("Host", { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "BigMod.esp" }, { "Small.esl" });
	REQUIRE(f.Join(1, host));
	CHECK(TablesSent(f, 1).size() == 1u);

	// The joiner lacks BigMod.esp and has two plugins the host lacks, in another order.
	const auto joiner = HelloWith("Joiner", { "Skyrim.esm", "Mine.esp", "Update.esm", "Dawnguard.esm" }, { "MyLight.esl", "Small.esl" });
	REQUIRE(f.Join(2, joiner));
	const auto grown = Protocol::ComparedLoadOrder({ "Skyrim.esm", "Update.esm", "Dawnguard.esm", "BigMod.esp", "Mine.esp" }, { "Small.esl", "MyLight.esl" });
	// The joiner hears the grown table before its welcome; the host hears it ahead of the joiner's arrival.
	CHECK(TablesSent(f, 2) == Tables(grown));
	const auto hostHeard = f.fake->TakeThrough(1, Type(MT::kPlayerJoined));
	REQUIRE(hostHeard.size() == 2u);
	const auto hostTable = Protocol::DecodePluginTable(hostHeard[0]);
	REQUIRE(hostTable);
	CHECK(hostTable->plugins == grown);
	CHECK(f.LoggedSoon("server: fake 2 has another load order (2 plugins the session has not, 1 of the session's it lacks); its form IDs are translated by plugin name"));
	CHECK(f.LoggedSoon("server: the session's plugin table has 7 plugins (2 new)"));
	// Never a plugin's name in the server's log: it is each player's own business.
	CHECK(!LogHas(f, "Mine.esp"));
	CHECK(!LogHas(f, "BigMod"));
}

TEST("server: the same plugins in another order are let in, and the table stays as it was")
{
	Fixture    f;
	const auto host = HelloWith("Host", { "Skyrim.esm", "Update.esm", "A.esp", "B.esp" });
	REQUIRE(f.Join(1, host));
	const auto swapped = HelloWith("Swapped", { "Skyrim.esm", "Update.esm", "B.esp", "A.esp" });
	REQUIRE(f.Join(2, swapped));
	CHECK(TablesSent(f, 2) == Tables(host.plugins));
	REQUIRE(f.fake->Settle());
	CHECK(TablesSent(f, 1).size() == 1u);  // the one before its own welcome: nothing grew
	CHECK(f.Logged("server: fake 2 has another load order (the same plugins, in another order)"));

	// Names in another case are the same plugins in the same places: the same load order, let in quietly.
	const auto shouted = HelloWith("Shouted", { "SKYRIM.ESM", "update.esm", "a.ESP", "b.esp" });
	REQUIRE(f.Join(3, shouted));
	REQUIRE(f.fake->Settle());
	CHECK(!f.Logged("server: fake 3 has another load order"));
	CHECK(TablesSent(f, 3) == Tables(host.plugins));
}

TEST("server: another load order without a list to translate by is refused")
{
	Fixture    f;
	const auto host = HelloWith("Host", { "Skyrim.esm", "Update.esm" });
	REQUIRE(f.Join(1, host));

	// A hello that names no plugins (a bot, or a load order too long for a hello) and has another hash.
	auto bare = Fixture::MakeHello("Bare");
	bare.contentHash = 0x9999;
	CHECK(f.Refused(2, bare) == "Your mods differ from the host's.");
	CHECK(f.fake->WaitDisconnected(2));
	CHECK(f.LoggedSoon("server: refused fake 2: another load order, and no list of it to translate by"));
	// With the session's hash it is the session's load order: let in, and handed the table.
	auto same = Fixture::MakeHello("Same");
	same.contentHash = host.contentHash;
	REQUIRE(f.Join(3, same));
	CHECK(TablesSent(f, 3) == Tables(host.plugins));
}

TEST("server: a session started without a list sends no table and lets in only its own load order")
{
	Fixture f;
	auto    first = Fixture::MakeHello("First");
	first.contentHash = 0x1111;
	REQUIRE(f.Join(1, first));
	CHECK(TablesSent(f, 1).empty());
	const auto listed = HelloWith("Listed", { "Skyrim.esm" });
	CHECK(f.Refused(2, listed) == "Your mods differ from the host's.");
}

TEST("server: a whole Skyrim load order is let in, and one that differs from it at its end too, with no place left for its plugin")
{
	std::vector<std::string> full;
	for (int i = 0; i < 254; ++i) {
		full.push_back(std::format("Full{:03}.esp", i));
	}
	std::vector<std::string> light;
	for (int i = 0; i < 4096; ++i) {
		light.push_back(std::format("Light{:04}.esl", i));
	}
	Fixture    f;
	const auto host = HelloWith("Host", full, light);
	REQUIRE(f.Join(1, host));
	CHECK(TablesSent(f, 1) == Tables(host.plugins));

	auto same = Fixture::MakeHello("Same");
	same.plugins = host.plugins;
	same.contentHash = host.contentHash;
	CHECK(f.Join(2, same).has_value());

	// Every light place is taken: Other.esl gets none, so its forms are skipped in the games (FormTranslate.h).
	light.back() = "Other.esl";
	const auto other = HelloWith("Other", full, light);
	REQUIRE(f.Join(3, other));
	CHECK(TablesSent(f, 3) == Tables(host.plugins));
	CHECK(f.LoggedSoon("server: fake 3 has another load order (1 plugins the session has not, 1 of the session's it lacks)"));
}

TEST("server: an empty session takes the next player's load order and plugin table")
{
	Fixture    f;
	const auto first = HelloWith("First", { "Skyrim.esm", "First.esp" });
	REQUIRE(f.Join(1, first));
	f.fake->Drop(1);
	CHECK(f.LoggedSoon("server: player 1 left (disconnected)"));

	const auto second = HelloWith("Second", { "Skyrim.esm", "Second.esp" });
	REQUIRE(f.Join(2, second));
	CHECK(TablesSent(f, 2) == Tables(second.plugins));

	// And numbers everyone after by it: the first's plugin comes after the second's.
	REQUIRE(f.Join(3, first));
	CHECK(TablesSent(f, 3) == Tables(Protocol::ComparedLoadOrder({ "Skyrim.esm", "Second.esp", "First.esp" }, {})));

	// A session started without lists holds everyone after to its hash.
	f.fake->Drop(2);
	f.fake->Drop(3);
	CHECK(f.LoggedSoon("server: player 3 left (disconnected)"));
	CHECK(f.LoggedSoon("server: player 2 left (disconnected)"));
	auto bare = Fixture::MakeHello("Bare");
	bare.contentHash = 0x1111;
	REQUIRE(f.Join(4, bare));
	auto other = Fixture::MakeHello("Other");
	other.contentHash = 0x2222;
	CHECK(f.Refused(5, other) == "Your mods differ from the host's.");
}

TEST("server: a player who comes back with the same identity gets the same player id")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada", 0xAAAA)));
	const auto before = f.Join(2, Fixture::MakeHello("Bo", 0xBBBB));
	REQUIRE(before);
	CHECK(before->playerId == 2);

	f.fake->Drop(2);
	const auto leftPacket = f.fake->WaitFor(1, Type(MT::kPlayerLeft));
	REQUIRE(leftPacket);
	const auto left = Protocol::DecodePlayerLeft(*leftPacket);
	REQUIRE(left);
	CHECK(left->playerId == 2);

	REQUIRE(f.Join(3, Fixture::MakeHello("Cy", 0xCCCC)));
	const auto after = f.Join(4, Fixture::MakeHello("Bo", 0xBBBB));
	REQUIRE(after);
	CHECK(after->playerId == 2);

	// A player with no identity is always new.
	f.fake->Drop(3);
	CHECK(f.LoggedSoon("server: player 3 left (disconnected)"));
	const auto anonymous = f.Join(5, Fixture::MakeHello("Dee"));
	REQUIRE(anonymous);
	CHECK(anonymous->playerId == 4);
}

TEST("server: a player who joins again while their old connection stands takes its place")
{
	Server::Options options;
	options.maxPlayers = 2;
	Fixture f{ options };
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada", 0xAAAA)));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo", 0xBBBB)));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	// The session is full, but this is Bo again: let in, on the old id, and the old connection is dropped.
	const auto again = f.Join(3, Fixture::MakeHello("Bo", 0xBBBB));
	REQUIRE(again);
	CHECK(again->playerId == 2);
	CHECK(f.fake->WaitDisconnected(2));
	CHECK(f.LoggedSoon("server: player 2 left (joined again on a new connection)"));

	// The other player hears them leave, then join.
	const auto packets = f.fake->TakeThrough(1, Type(MT::kPlayerJoined));
	REQUIRE(packets.size() == 2);
	CHECK(Protocol::PeekType(packets[0]) == MT::kPlayerLeft);
	CHECK(Protocol::PeekType(packets[1]) == MT::kPlayerJoined);

	REQUIRE(f.fake->Settle());
	CHECK(f.server.Players() == 2);
}

TEST("server: the host is the game on this machine with the host's identity, and every welcome names it")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };

	// The same identity from another machine is an ordinary player.
	const auto remote = f.Join(1, Fixture::MakeHello("Copycat", 0x4057));
	REQUIRE(remote);
	CHECK(remote->hostId == 0);
	f.fake->Drop(1);
	REQUIRE(f.fake->Settle());

	// Another program on this machine (a bot) is an ordinary player too.
	f.fake->SetLocal(2);
	const auto bot = f.Join(2, Fixture::MakeHello("Bot", 0x1111));
	REQUIRE(bot);
	CHECK(bot->hostId == 0);

	f.fake->SetLocal(3);
	const auto host = f.Join(3, Fixture::MakeHello("Host", 0x4057));
	REQUIRE(host);
	CHECK(host->hostId == host->playerId);
	CHECK(f.LoggedSoon(", the host ("));

	const auto friendWelcome = f.Join(4, Fixture::MakeHello("Friend", 0x2222));
	REQUIRE(friendWelcome);
	CHECK(friendWelcome->hostId == host->playerId);

	// Once the host has left, a welcome names no host.
	f.fake->Drop(3);
	REQUIRE(f.fake->Settle());
	const auto late = f.Join(5, Fixture::MakeHello("Late", 0x3333));
	REQUIRE(late);
	CHECK(late->hostId == 0);
}

TEST("server: a player's state is passed on to the others, unreliably, and not back to its sender")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 2u, 3u }) {
		(void)f.fake->Take(peer);
	}

	const auto state = StateAt(1, 10.0f);
	f.fake->Deliver(2, Protocol::Encode(state));
	REQUIRE(f.fake->Settle());

	for (const PeerId peer : { 1u, 3u }) {
		const auto sent = f.fake->TakeSent(peer);
		REQUIRE(sent.size() == 1);
		CHECK(!sent[0].reliable);
		const auto remote = Protocol::DecodeRemoteState(sent[0].data);
		REQUIRE(remote);
		CHECK(remote->playerId == 2);
		CHECK(remote->state == state);
	}
	CHECK(f.fake->Take(2).empty());
}

TEST("server: a state older than the last one passed on is dropped, across a wrap of the sequence too")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(2);

	for (const std::uint32_t sequence : { 5u, 4u, 5u, 6u, 0xFFFFFFFFu, 0xFFFFFFF0u, 2u }) {
		f.fake->Deliver(1, Protocol::Encode(StateAt(sequence, static_cast<float>(sequence % 1000))));
	}
	REQUIRE(f.fake->Settle());

	// 5, 6, then 0xFFFFFFFF reads as older than 6 (a signed difference); 0xFFFFFFF0 too; 2 is older than 6.
	const auto states = RemoteStates(f, 2);
	REQUIRE(states.size() == 2);
	CHECK(states[0].state.sequence == 5);
	CHECK(states[1].state.sequence == 6);

	// Near the top, the sequence wraps to 0 and goes on.
	Fixture g;
	REQUIRE(g.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(g.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(g.fake->Settle());
	(void)g.fake->Take(2);
	for (const std::uint32_t sequence : { 0xFFFFFFFEu, 0xFFFFFFFFu, 0u, 1u, 0xFFFFFFFFu }) {
		g.fake->Deliver(1, Protocol::Encode(StateAt(sequence, 1.0f)));
	}
	REQUIRE(g.fake->Settle());
	const auto wrapped = RemoteStates(g, 2);
	REQUIRE(wrapped.size() == 4);
	CHECK(wrapped[2].state.sequence == 0);
	CHECK(wrapped[3].state.sequence == 1);
}

TEST("server: a state with a position no game could be at is not passed on")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(2);

	auto bad = StateAt(1, std::numeric_limits<float>::quiet_NaN());
	f.fake->Deliver(1, Protocol::Encode(bad));
	bad = StateAt(2, 1.0e30f);
	f.fake->Deliver(1, Protocol::Encode(bad));
	auto cut = Protocol::Encode(StateAt(3, 1.0f));
	cut.pop_back();
	f.fake->Deliver(1, cut);
	f.fake->Deliver(1, Protocol::Encode(StateAt(4, 4.0f)));
	REQUIRE(f.fake->Settle());

	const auto states = RemoteStates(f, 2);
	REQUIRE(states.size() == 1);
	CHECK(states[0].state.sequence == 4);
}

TEST("server: nothing is heard from a connection before its hello")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	f.fake->Connect(2);
	f.fake->Deliver(2, Protocol::Encode(StateAt(1, 1.0f)));
	f.fake->Deliver(2, Protocol::Encode(Protocol::Heartbeat{ 7 }, MT::kHeartbeat));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	CHECK(f.fake->Take(2).empty());
	CHECK(f.server.Players() == 1);
}

TEST("server: a heartbeat is answered with its token, unreliably, and only so many in a burst")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	f.fake->Deliver(1, Protocol::Encode(Protocol::Heartbeat{ 0xBEEF }, MT::kHeartbeat));
	REQUIRE(f.fake->Settle());
	auto sent = f.fake->TakeSent(1);
	REQUIRE(sent.size() == 1);
	CHECK(!sent[0].reliable);
	CHECK(Protocol::PeekType(sent[0].data) == MT::kHeartbeatAck);
	const auto ack = Protocol::DecodeHeartbeat(sent[0].data);
	REQUIRE(ack);
	CHECK(ack->token == 0xBEEF);

	// A flood: the burst is answered, the rest is not (some may be, as the budget refills while they come in).
	for (std::uint32_t i = 0; i < 2000; ++i) {
		f.fake->Deliver(1, Protocol::Encode(Protocol::Heartbeat{ i }, MT::kHeartbeat));
	}
	REQUIRE(f.fake->Settle());
	const auto answers = f.fake->Take(1).size();
	CHECK(answers >= 19);
	CHECK(answers < 1000);
}

TEST("server: a message the server does not take is said once in the log and changes nothing")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	(void)f.fake->Take(2);

	// A welcome is the server's to send; type 200 is no message at all; an empty packet has no type.
	f.fake->Deliver(2, Protocol::Encode(Protocol::Welcome{ 9, 9, 9 }));
	f.fake->Deliver(2, std::vector<std::uint8_t>{ 200, 1, 2, 3 });
	f.fake->Deliver(2, std::vector<std::uint8_t>{});
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	CHECK(f.fake->Take(2).empty());

	std::size_t said = 0;
	for (const auto& line : f.LogLines()) {
		said += line.find("which the server does not take") != std::string::npos;
	}
	CHECK(said == 1);
	CHECK(f.Logged("server: player 2 sent message type 2, which the server does not take"));
	CHECK(f.server.Players() == 2);
}

TEST("server: a second hello from a player already in changes nothing")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);
	f.fake->Deliver(1, Protocol::Encode(Fixture::MakeHello("Ada again")));
	REQUIRE(f.fake->Settle());
	CHECK(f.fake->Take(1).empty());
	CHECK(f.server.Players() == 1);
}

TEST("server: a connection that never says hello is dropped")
{
	Server::Options options;
	options.helloTimeout = std::chrono::milliseconds(250);
	Fixture f{ options };
	f.fake->Connect(1);
	CHECK(f.fake->WaitDisconnected(1));
	CHECK(f.LoggedSoon("server: dropped fake 1: no hello"));

	// One that did say hello stays.
	REQUIRE(f.Join(2, Fixture::MakeHello("Ada")));
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	REQUIRE(f.fake->Settle());
	CHECK(!f.fake->Disconnected(2));
	CHECK(f.server.Players() == 1);
}

TEST("server: the log names no player and holds no whole address")
{
	Fixture f;
	f.fake->SetAddress(1, "udp 203.0.113.x:5000");
	REQUIRE(f.Join(1, Fixture::MakeHello("A Very Private Name")));
	f.fake->Drop(1);
	CHECK(f.LoggedSoon("server: player 1 left (disconnected)"));
	f.server.Stop();
	CHECK(f.Logged("server: stopped"));
	CHECK(f.Logged("server: player 1 joined from udp 203.0.113.x:5000"));
	for (const auto& line : f.LogLines()) {
		CHECK(line.find("Private") == std::string::npos);
	}
}

TEST("server: a second server on the same port does not start, and says why")
{
	Fixture                  f;
	std::vector<std::string> lines;
	Server                   second{ [&](std::string_view a_line) { lines.emplace_back(a_line); } };
	Server::Options          options;
	options.port = f.udpPort;
	options.udpLoopbackOnly = true;
	CHECK(!second.Start(options));
	CHECK(!second.Running());
	REQUIRE(lines.size() == 1);
	CHECK(lines[0] == std::format("server: could not bind UDP port {} (already in use?)", f.udpPort));
}

namespace
{
	Protocol::PlayerLooks LooksOf(std::uint32_t a_race, std::uint32_t a_worn)
	{
		Protocol::PlayerLooks looks;
		looks.race = a_race;
		looks.weight = 40.0f;
		looks.worn = { a_worn, a_worn + 1 };
		return looks;
	}

	// The looks the server has sent a_peer so far, in the order sent, each with whether it was sent reliably.
	std::vector<std::pair<Protocol::RemoteLooks, bool>> SentLooks(Fixture& a_fixture, PeerId a_peer)
	{
		std::vector<std::pair<Protocol::RemoteLooks, bool>> out;
		for (const auto& sent : a_fixture.fake->TakeSent(a_peer)) {
			if (Protocol::PeekType(sent.data) == MT::kRemoteLooks) {
				if (const auto looks = Protocol::DecodeRemoteLooks(sent.data)) {
					out.emplace_back(*looks, sent.reliable);
				}
			}
		}
		return out;
	}
}

TEST("server: a player's looks are passed on to the others, reliably, and not back to its sender")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	for (const PeerId peer : { 1u, 2u, 3u }) {
		(void)f.fake->Take(peer);
	}

	const auto looks = LooksOf(0x00013746, 0x00012E49);
	f.fake->Deliver(2, Protocol::Encode(looks));
	REQUIRE(f.fake->Settle());

	for (const PeerId peer : { 1u, 3u }) {
		const auto sent = SentLooks(f, peer);
		REQUIRE(sent.size() == 1);
		CHECK(sent[0].second);
		CHECK(sent[0].first.playerId == 2);
		CHECK(sent[0].first.looks == looks);
	}
	CHECK(f.fake->Take(2).empty());
}

TEST("server: a player who joins later is told the looks of each player there, after that player's join")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	const auto first = LooksOf(0x00013746, 0x00012E49);
	f.fake->Deliver(1, Protocol::Encode(first));
	REQUIRE(f.fake->Settle());
	// Bo sends none: nothing is made up for them.

	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto sent = f.fake->TakeSent(3);
	std::optional<std::size_t> joinedAt;
	std::optional<std::size_t> looksAt;
	std::size_t                looksCount = 0;
	for (std::size_t i = 0; i < sent.size(); ++i) {
		if (const auto joined = Protocol::PeekType(sent[i].data) == MT::kPlayerJoined ? Protocol::DecodePlayerJoined(sent[i].data) : std::nullopt; joined && joined->playerId == 1) {
			joinedAt = i;
		}
		if (const auto looks = Protocol::PeekType(sent[i].data) == MT::kRemoteLooks ? Protocol::DecodeRemoteLooks(sent[i].data) : std::nullopt) {
			++looksCount;
			looksAt = i;
			CHECK(looks->playerId == 1);
			CHECK(looks->looks == first);
			CHECK(sent[i].reliable);
		}
	}
	REQUIRE(joinedAt);
	REQUIRE(looksAt);
	CHECK(*looksAt > *joinedAt);
	CHECK(looksCount == 1);
}

TEST("server: the same looks sent again are not passed on again; changed ones are")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	const auto looks = LooksOf(0x00013746, 0x00012E49);
	f.fake->Deliver(2, Protocol::Encode(looks));
	f.fake->Deliver(2, Protocol::Encode(looks));
	auto changed = looks;
	changed.worn.push_back(0x00012E4D);
	f.fake->Deliver(2, Protocol::Encode(changed));
	REQUIRE(f.fake->Settle());

	const auto sent = SentLooks(f, 1);
	REQUIRE(sent.size() == 2);
	CHECK(sent[0].first.looks == looks);
	CHECK(sent[1].first.looks == changed);
}

TEST("server: looks that cannot be read are not passed on")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	auto bad = Protocol::Encode(LooksOf(0x00013746, 0x00012E49));
	bad.pop_back();
	f.fake->Deliver(2, bad);
	auto heavy = LooksOf(0x00013746, 0x00012E49);
	heavy.weight = 1e30f;
	f.fake->Deliver(2, Protocol::Encode(heavy));
	REQUIRE(f.fake->Settle());
	CHECK(SentLooks(f, 1).empty());
}

TEST("server: a burst of looks faster than a game changes clothes is held back, and its last still reaches everyone")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	// Thirty different looks at once (a load that re-equips everything): the burst passes ten on at once, and the
	// newest follows once the budget has room, a moment later; never the ones between.
	constexpr std::uint32_t BURST = 30;
	const auto              last = LooksOf(0x00013746, 0x00020000 + (BURST - 1) * 2);
	for (std::uint32_t i = 0; i < BURST; ++i) {
		f.fake->Deliver(2, Protocol::Encode(LooksOf(0x00013746, 0x00020000 + i * 2)));
	}
	REQUIRE(f.fake->Settle());
	auto sent = SentLooks(f, 1);
	CHECK(sent.size() >= 10);
	CHECK(sent.size() < BURST);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while ((sent.empty() || sent.back().first.looks != last) && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		for (auto& more : SentLooks(f, 1)) {
			sent.push_back(std::move(more));
		}
	}
	REQUIRE(!sent.empty());
	CHECK(sent.back().first.looks == last);
	CHECK(sent.back().second);
	CHECK(sent.size() <= 12);

	// And one who joins now is told the newest.
	REQUIRE(f.Join(3, Fixture::MakeHello("Cy")));
	REQUIRE(f.fake->Settle());
	const auto told = SentLooks(f, 3);
	REQUIRE(told.size() == 1);
	CHECK(told[0].first.looks == last);
}

TEST("server: the same looks sent again and again spend none of the budget")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Ada")));
	REQUIRE(f.Join(2, Fixture::MakeHello("Bo")));
	REQUIRE(f.fake->Settle());
	(void)f.fake->Take(1);

	const auto same = LooksOf(0x00013746, 0x00012E49);
	for (int i = 0; i < 40; ++i) {
		f.fake->Deliver(2, Protocol::Encode(same));
	}
	auto changed = same;
	changed.worn.push_back(0x00012E4D);
	f.fake->Deliver(2, Protocol::Encode(changed));
	REQUIRE(f.fake->Settle());
	const auto sent = SentLooks(f, 1);
	REQUIRE(sent.size() == 2);
	CHECK(sent[1].first.looks == changed);  // at once: the repeats used up nothing
}
