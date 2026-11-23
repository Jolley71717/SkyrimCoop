#include "Test.h"

#include "Net.h"
#include "Redact.h"
#include "ServerFixture.h"
#include "SourceTree.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the server writes to its log while real players come and go. Players attach logs to public bug reports, so a
// log never names a player, holds a password, a whole IP address or a whole SteamID64, while still telling a
// maintainer who did what by player number. The players themselves still see each other's names: only the log is
// redacted. And a guard on the source for the plugin's own log, which can't be played in a test.

using namespace std::chrono_literals;

namespace
{
	using MT = Protocol::MessageType;

	constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	// Made-up accounts. A SteamID64 is this base plus the 32-bit account number; in hex and built here, so the
	// sources hold nothing that reads as an account id.
	constexpr std::uint64_t INDIVIDUAL = 0x0110000100000000ull;
	constexpr std::uint64_t STEAM_ID = INDIVIDUAL + 52079950;
	constexpr std::uint64_t OTHER_ID = INDIVIDUAL + 127388593;
	const std::string       ID = std::to_string(STEAM_ID);
	const std::string       OTHER = std::to_string(OTHER_ID);

	// Every PlayerJoined a_peer was sent so far, as id -> name.
	std::map<std::uint32_t, std::string> NamesHeard(Fixture& a_f, PeerId a_peer)
	{
		std::map<std::uint32_t, std::string> names;
		for (const auto& packet : a_f.fake->Take(a_peer)) {
			if (Protocol::PeekType(packet) == MT::kPlayerJoined) {
				const auto joined = Protocol::DecodePlayerJoined(packet);
				REQUIRE(joined);
				names[joined->playerId] = joined->name;
			}
		}
		return names;
	}

	// a_text is in no line of the log.
	bool NowhereInLog(Fixture& a_f, std::string_view a_text)
	{
		return std::ranges::none_of(a_f.LogLines(), [&](const std::string& a_line) { return a_line.find(a_text) != std::string::npos; });
	}

	bool AllOneLine(Fixture& a_f)
	{
		return std::ranges::none_of(a_f.LogLines(), [](const std::string& a_line) { return a_line.find_first_of("\r\n") != std::string::npos; });
	}
}

TEST("log privacy: a whole evening (a wrong password, joins over UDP and Steam, a rejoin, a stray message, leaving) logs players by number only")
{
	Server::Options options;
	options.password = "hunter2";
	Fixture f{ options };

	// Someone guesses the password from the internet.
	f.fake->SetAddress(9, "udp 203.0.113.9:1111");
	auto guess = Fixture::MakeHello("Mallory");
	guess.password = "letmein";
	CHECK(f.Refused(9, guess) == "Wrong password.");

	// Alice joins over UDP from her home address, Bob over Steam (a transport whose description names the account in
	// full: the server's own transports alias it, this one does not).
	f.fake->SetAddress(1, "udp 198.51.100.7:4321");
	f.fake->SetAddress(2, "steam " + ID);
	auto aliceHello = Fixture::MakeHello("Alice", 0xA11CE);
	aliceHello.password = "hunter2";
	auto bobHello = Fixture::MakeHello("Bob", 0xB0B);
	bobHello.password = "hunter2";
	const auto alice = f.Join(1, aliceHello);
	REQUIRE(alice);
	const auto bob = f.Join(2, bobHello);
	REQUIRE(bob);

	// Bob's game crashes and he is back (same identity) before the server noticed.
	f.fake->SetAddress(3, "steam " + ID);
	const auto bobAgain = f.Join(3, bobHello);
	REQUIRE(bobAgain);
	CHECK(bobAgain->playerId == bob->playerId);

	// Alice's game sends a message only the server sends.
	f.fake->Deliver(1, Protocol::Encode(Protocol::Reject{ "Alice says hunter2" }));
	REQUIRE(f.fake->Settle());  // the server has acted on all of it

	// Alice quits.
	f.fake->Take(3);
	f.fake->Drop(1);
	REQUIRE(f.fake->WaitFor(3, Type(MT::kPlayerLeft)));
	REQUIRE(f.fake->Settle());

	const auto bobAlias = Redact::Alias(STEAM_ID);
	CHECK(f.Logged("server: refused udp 203.0.113.x:1111: wrong password"));
	CHECK(f.Logged(std::format("server: player {} joined from udp 198.51.100.x:4321", alice->playerId)));
	CHECK(f.Logged(std::format("server: player {} joined from steam {} (2 in the session)", bob->playerId, bobAlias)));
	CHECK(f.Logged(std::format("server: player {} left (joined again on a new connection)", bob->playerId)));
	CHECK(f.Logged(std::format("server: player {} joined from steam {} (2 in the session)", bobAgain->playerId, bobAlias)));
	CHECK(f.Logged(std::format("server: player {} sent message type {}, which the server does not take", alice->playerId, static_cast<unsigned>(Type(MT::kReject)))));
	CHECK(f.Logged(std::format("server: player {} left (disconnected)", alice->playerId)));

	for (const std::string_view secret : { "Alice", "Bob", "Mallory", "hunter2", "letmein", "198.51.100.7", "203.0.113.9" }) {
		CHECK(NowhereInLog(f, secret));
	}
	CHECK(NowhereInLog(f, ID));
	CHECK(NowhereInLog(f, ID.substr(7)));  // nor the account number's own digits
}

TEST("log privacy: a joiner whose name is a forged log line or a Steam id gets in, and the log holds neither")
{
	Fixture f;
	const auto host = f.Join(1, Fixture::MakeHello("Host"));
	REQUIRE(host);
	f.fake->Take(1);

	// A name with a line break and a fake server line in it, then one that is someone's SteamID64.
	const auto forger = f.Join(2, Fixture::MakeHello("x\nserver: player 1 left (disconnected)"));
	REQUIRE(forger);
	const auto squatter = f.Join(3, Fixture::MakeHello(OTHER));
	REQUIRE(squatter);
	REQUIRE(f.fake->Settle());

	// The host hears them (their names lose the control characters, that is all).
	const auto heard = NamesHeard(f, 1);
	REQUIRE(heard.contains(forger->playerId));
	REQUIRE(heard.contains(squatter->playerId));
	CHECK(heard.at(forger->playerId).find('\n') == std::string::npos);
	CHECK(heard.at(squatter->playerId) == OTHER);

	// The log: one line per event, none of them forged, no id that isn't a player number.
	CHECK(AllOneLine(f));
	CHECK(!f.Logged(std::format("player {} left", host->playerId)));
	CHECK(NowhereInLog(f, OTHER));
	// Not even as an alias: the name is not an account, and a name has no place in the log at all.
	CHECK(NowhereInLog(f, Redact::Alias(OTHER_ID)));
	CHECK(f.Logged(std::format("server: player {} joined from fake 2", forger->playerId)));
	CHECK(f.Logged(std::format("server: player {} joined from fake 3", squatter->playerId)));
}

TEST("log privacy: a second joiner hears the first by name while the log keeps to player numbers")
{
	Fixture f;
	f.fake->SetAddress(1, "udp 192.0.2.44:50000");
	const auto alice = f.Join(1, Fixture::MakeHello("Alice"));
	REQUIRE(alice);
	f.fake->SetAddress(2, "udp 127.0.0.1:50001");
	const auto bob = f.Join(2, Fixture::MakeHello("Bob"));
	REQUIRE(bob);
	REQUIRE(f.fake->Settle());

	const auto bobHeard = NamesHeard(f, 2);
	REQUIRE(bobHeard.contains(alice->playerId));
	CHECK(bobHeard.at(alice->playerId) == "Alice");
	const auto aliceHeard = NamesHeard(f, 1);
	REQUIRE(aliceHeard.contains(bob->playerId));
	CHECK(aliceHeard.at(bob->playerId) == "Bob");

	CHECK(f.Logged(std::format("server: player {} joined from udp 192.0.2.x:50000", alice->playerId)));
	// This machine's own address is no secret and tells a maintainer it was the host's game or a bot.
	CHECK(f.Logged(std::format("server: player {} joined from udp 127.0.0.1:50001", bob->playerId)));
	CHECK(NowhereInLog(f, "Alice"));
	CHECK(NowhereInLog(f, "Bob"));
}

TEST("log privacy: plugin names carrying a line break or a Steam id never reach the server's log, whatever load orders join")
{
	Fixture f;

	// The host's game (the first to arrive sets the session's load order) has a plugin whose file name holds a line
	// break and a fake log line, and one named after a SteamID64.
	const std::string forged = "Evil.esp\r\nserver: player 1 left (disconnected)";
	auto              hostHello = Fixture::MakeHello("Host");
	hostHello.plugins = Protocol::ComparedLoadOrder({ "Skyrim.esm", forged, ID + ".esp" }, {});
	hostHello.contentHash = Protocol::LoadOrderHash(hostHello.plugins);
	REQUIRE(f.Join(1, hostHello));

	// Joiners with other load orders are let in (their games translate form IDs by plugin name); one brings a plugin
	// named after the account too, which the session's plugin table takes on.
	auto bare = Fixture::MakeHello("Joiner");
	bare.plugins = Protocol::ComparedLoadOrder({ "Skyrim.esm" }, {});
	bare.contentHash = Protocol::LoadOrderHash(bare.plugins);
	REQUIRE(f.Join(2, bare));
	auto closer = Fixture::MakeHello("Closer");
	closer.plugins = Protocol::ComparedLoadOrder({ "Skyrim.esm", forged, "Mine" + ID + ".esp" }, {});
	closer.contentHash = Protocol::LoadOrderHash(closer.plugins);
	REQUIRE(f.Join(3, closer));
	REQUIRE(f.fake->Settle());

	// The host's log: one line for each, counts and masked addresses only: no forged line, no account, no plugin's name,
	// no joiner's name.
	CHECK(AllOneLine(f));
	CHECK(f.Logged("server: fake 2 has another load order (0 plugins the session has not, 2 of the session's it lacks)"));
	CHECK(f.Logged("server: fake 3 has another load order (1 plugins the session has not, 1 of the session's it lacks)"));
	CHECK(std::ranges::none_of(f.LogLines(), [](const std::string& a_line) { return a_line.starts_with("server: player 1 left"); }));
	CHECK(NowhereInLog(f, ID));
	CHECK(NowhereInLog(f, "Evil"));
	CHECK(NowhereInLog(f, "Joiner"));
	CHECK(NowhereInLog(f, "Closer"));
}

namespace
{
	// A player's game on this machine, connected to the server's real UDP transport (ENet) the way the bot connects.
	class UdpPlayer
	{
	public:
		explicit UdpPlayer(std::uint16_t a_port)
		{
			initialized = Net::Initialize();
			ENetAddress address{};
			host = initialized && Net::ResolveAddress("127.0.0.1", a_port, address) ? enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0) : nullptr;
			if (host) {
				peer = enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0);
			}
		}

		~UdpPlayer()
		{
			if (peer) {
				enet_peer_reset(peer);
			}
			if (host) {
				enet_host_destroy(host);
			}
			if (initialized) {
				Net::Shutdown();
			}
		}

		UdpPlayer(const UdpPlayer&) = delete;
		UdpPlayer& operator=(const UdpPlayer&) = delete;

		// Services the connection until a packet of a_type arrives (or a_timeout passes).
		std::optional<std::vector<std::uint8_t>> WaitFor(std::uint8_t a_type, std::chrono::milliseconds a_timeout = 30s)
		{
			const auto until = std::chrono::steady_clock::now() + a_timeout;
			while (host && std::chrono::steady_clock::now() < until) {
				ENetEvent event;
				while (enet_host_service(host, &event, 10) > 0) {
					if (event.type == ENET_EVENT_TYPE_CONNECT) {
						connected = true;
					} else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
						connected = false;
						peer = nullptr;
					} else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
						std::vector<std::uint8_t> data{ event.packet->data, event.packet->data + event.packet->dataLength };
						enet_packet_destroy(event.packet);
						if (!data.empty() && data[0] == a_type) {
							return data;
						}
					}
				}
			}
			return std::nullopt;
		}

		bool Connect()
		{
			const auto until = std::chrono::steady_clock::now() + 30s;
			while (peer && !connected && std::chrono::steady_clock::now() < until) {
				WaitFor(0xFF, 20ms);
			}
			return connected;
		}

		void Send(const std::vector<std::uint8_t>& a_packet)
		{
			if (peer) {
				Net::Send(peer, a_packet, true);
				enet_host_flush(host);
			}
		}

		// Quits the way a game does: tells the server, and waits for it to agree.
		void Leave()
		{
			if (!peer) {
				return;
			}
			enet_peer_disconnect(peer, 0);
			const auto until = std::chrono::steady_clock::now() + 30s;
			while (peer && std::chrono::steady_clock::now() < until) {
				WaitFor(0xFF, 20ms);
			}
		}

	private:
		bool      initialized = false;
		ENetHost* host = nullptr;
		ENetPeer* peer = nullptr;
		bool      connected = false;
	};
}

TEST("log privacy: over the real UDP transport, a wrong password, a join and a quit are logged by address and player number, never by name")
{
	Server::Options options;
	options.password = "hunter2";
	Fixture f{ options };
	REQUIRE(f.udpPort != 0);

	// Someone on this machine guesses the password and is turned away.
	{
		UdpPlayer guesser{ f.udpPort };
		REQUIRE(guesser.Connect());
		auto guess = Fixture::MakeHello("Mallory");
		guess.password = "letmein";
		guesser.Send(Protocol::Encode(guess));
		REQUIRE(guesser.WaitFor(Type(MT::kReject)));
	}

	// Alice's game joins and quits.
	UdpPlayer alice{ f.udpPort };
	REQUIRE(alice.Connect());
	auto hello = Fixture::MakeHello("Alice", 0xA11CE);
	hello.password = "hunter2";
	alice.Send(Protocol::Encode(hello));
	const auto packet = alice.WaitFor(Type(MT::kWelcome));
	REQUIRE(packet);
	const auto welcome = Protocol::DecodeWelcome(*packet);
	REQUIRE(welcome);
	alice.Leave();

	// The real transport's own description: "udp", then this machine's address in full (loopback is no secret) and
	// a port.
	CHECK(f.LoggedSoon("server: refused udp 127.0.0.1:"));
	CHECK(std::ranges::any_of(f.LogLines(), [](const std::string& a_line) { return a_line.starts_with("server: refused udp 127.0.0.1:") && a_line.ends_with(": wrong password"); }));
	CHECK(f.LoggedSoon(std::format("server: player {} joined from udp 127.0.0.1:", welcome->playerId)));
	CHECK(f.LoggedSoon(std::format("server: player {} left (disconnected)", welcome->playerId)));
	for (const std::string_view secret : { "Alice", "Mallory", "hunter2", "letmein" }) {
		CHECK(NowhereInLog(f, secret));
	}
}

// The game's own log (SkyrimCoop.log) can't be played in a test: its lines are written where the game runs. So the
// guard is on the source. A line that names a friend by what they call themselves (their Steam name, by default)
// slipped back into the sister project's log once with a merge; with this, the next one fails the tests instead of
// turning up in a bug report.
namespace
{
	// How the plugin logs (src/pch.h: namespace logs = SKSE::log, which is spdlog's), and how the server does.
	constexpr std::string_view PLUGIN_LOG[] = {
		"logs::trace(",
		"logs::debug(",
		"logs::info(",
		"logs::warn(",
		"logs::error(",
		"logs::critical(",
		"SKSE::log::trace(",
		"SKSE::log::debug(",
		"SKSE::log::info(",
		"SKSE::log::warn(",
		"SKSE::log::error(",
		"SKSE::log::critical(",
		"spdlog::trace(",
		"spdlog::debug(",
		"spdlog::info(",
		"spdlog::warn(",
		"spdlog::error(",
		"spdlog::critical(",
	};
	constexpr std::string_view SERVER_LOG[] = { "log(", "a_log(" };

	struct LogCall
	{
		std::string file;  // relative to the repository, once NamedInLog has read it
		int         line = 0;
		std::string code;  // its arguments with the string and character literals left out
	};

	// Every call of one of a_calls in a_text (a whole name: "log(" is not "dialog(", "logs::info(" not
	// "dialogs::info("), with its arguments as far as the bracket that closes it.
	std::vector<LogCall> LogCalls(std::string_view a_text, std::span<const std::string_view> a_calls)
	{
		std::vector<LogCall> out;
		const auto           ident = [](char a_c) { return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_' || a_c == ':'; };
		for (std::size_t at = 0; at < a_text.size(); ++at) {
			const auto call = std::ranges::find_if(a_calls, [&](std::string_view a_call) { return a_text.substr(at).starts_with(a_call) && (at == 0 || !ident(a_text[at - 1])); });
			if (call == a_calls.end()) {
				continue;
			}
			LogCall found{ .line = static_cast<int>(std::ranges::count(a_text.substr(0, at), '\n')) + 1 };
			int     depth = 1;
			auto    i = at + call->size();
			for (; i < a_text.size() && depth > 0; ++i) {
				const char c = a_text[i];
				if (c == '"' || c == '\'') {
					for (++i; i < a_text.size() && a_text[i] != c; ++i) {
						i += a_text[i] == '\\';  // what a backslash escapes is skipped
					}
					continue;
				}
				depth += c == '(';
				depth -= c == ')';
				found.code += c;
			}
			out.push_back(std::move(found));
			at = i;
		}
		return out;
	}

	std::vector<LogCall> LogCalls(std::string_view a_text, std::initializer_list<std::string_view> a_calls)
	{
		return LogCalls(a_text, std::span{ a_calls.begin(), a_calls.size() });
	}

	// A player's name, as the code holds it: a member called name (a player's, a stand-in's, a hello's), a Steam persona
	// (Steam::PersonaName, Steam::NameOf), the name the player chose (NameChoice::Choose) or the player character's own.
	// Another reference's name (a door's, an NPC's) is no one's secret and stays allowed.
	bool ReadsAName(const LogCall& a_call)
	{
		static const std::regex name{
			R"((\.|->)\s*name\b(?!\s*\()|[Pp]ersona|\bNameOf\s*\(|NameChoice::Choose|(\b(a_)?player|PlayerCharacter::GetSingleton\(\))\s*->\s*Get(DisplayFull)?Name\s*\()"
		};
		return std::regex_search(a_call.code, name);
	}

	// Every call of a_calls in the .cpp and .h files under a_folder (a_root / a_folder).
	std::vector<LogCall> CallsIn(const std::filesystem::path& a_root, std::string_view a_folder, std::span<const std::string_view> a_calls)
	{
		std::vector<LogCall> out;
		for (const auto& entry : std::filesystem::recursive_directory_iterator{ a_root / a_folder }) {
			const auto ext = entry.path().extension();
			if (!entry.is_regular_file() || (ext != ".cpp" && ext != ".h")) {
				continue;
			}
			for (auto& call : LogCalls(SourceTree::Read(entry.path()), a_calls)) {
				call.file = std::filesystem::relative(entry.path(), a_root).generic_string();
				out.push_back(std::move(call));
			}
		}
		return out;
	}
}

TEST("log privacy: the guard on the game's log finds a line that names a friend, and lets one that says player N through")
{
	// A line naming a stand-in, and the one it should be.
	const auto named = LogCalls(R"x(		logs::warn("Players: '{}' joined, but their looks ({:08X}) are {}", remote.name, remote.race,
			remote.race ? "of no race in this game" : "none");)x",
		{ "logs::warn(" });
	REQUIRE(named.size() == 1);
	CHECK(named[0].line == 1);
	CHECK(ReadsAName(named[0]));
	const auto numbered = LogCalls(R"x(logs::info("Players: player {} joined (never their .name or ->name)", remote.id);)x", { "logs::info(" });
	REQUIRE(numbered.size() == 1);
	CHECK(!ReadsAName(numbered[0]));

	// A persona read from Steam, by either function; the name the player chose; the player character's own.
	CHECK(ReadsAName(LogCalls(R"x(logs::info("Steam: {}", Steam::PersonaName());)x", { "logs::info(" }).at(0)));
	CHECK(ReadsAName(LogCalls(R"x(logs::info("Steam: host is {}", Steam::NameOf(owner));)x", { "logs::info(" }).at(0)));
	CHECK(ReadsAName(LogCalls(R"x(logs::info("Name: {}", NameChoice::Choose(ini, character, steam));)x", { "logs::info(" }).at(0)));
	CHECK(ReadsAName(LogCalls(R"x(logs::info("Me: {}", RE::PlayerCharacter::GetSingleton()->GetName());)x", { "logs::info(" }).at(0)));
	CHECK(ReadsAName(LogCalls(R"x(SKSE::log::debug("Me: {}", a_player->GetDisplayFullName());)x", { "SKSE::log::debug(" }).at(0)));
	// A server line.
	CHECK(ReadsAName(LogCalls(R"x(log(std::format("server: '{}' left", a_peer.name));)x", { "log(" }).at(0)));

	// A file's name, a door's and a pad button's are not a player's.
	CHECK(!ReadsAName(LogCalls(R"x(logs::warn("Identity: couldn't write {}", path->filename().string());)x", { "logs::warn(" }).at(0)));
	CHECK(!ReadsAName(LogCalls(R"x(logs::info("Doors: {} opened", door->GetName());)x", { "logs::info(" }).at(0)));
	CHECK(!ReadsAName(LogCalls(R"x(logs::info("Pad: {}", AddressLibraryFile::Name(1, 6));)x", { "logs::info(" }).at(0)));

	// Whole names only.
	CHECK(LogCalls(R"x(dialog(a_player.name);)x", { "log(" }).empty());
	CHECK(LogCalls(R"x(dialogs::info(a_player.name);)x", { "logs::info(" }).empty());
}

TEST("log privacy: no line the game or the server logs names a friend; they are \"player N\" (a bug report's log names nobody)")
{
	const auto root = SourceTree::Root();
	REQUIRE(root);  // run the tests from the repository or its build folder

	const auto plugin = CallsIn(*root, "src", PLUGIN_LOG);
	const auto server = CallsIn(*root, "common", SERVER_LOG);
	// The scan saw the log calls there are (dozens in the plugin, one for each server event), so it can't pass by
	// finding none: a change to how the code logs has to change the lists above.
	CHECK(plugin.size() >= 30);
	CHECK(server.size() >= 5);
	CHECK(std::ranges::any_of(plugin, [](const LogCall& a_call) { return a_call.file == "src/main.cpp"; }));
	CHECK(std::ranges::any_of(server, [](const LogCall& a_call) { return a_call.file == "common/Server.cpp"; }));

	std::vector<LogCall> all = plugin;
	std::ranges::copy(server, std::back_inserter(all));
	for (const auto& call : all) {
		if (ReadsAName(call)) {
			Test::Fail(__FILE__, __LINE__, std::format("a log line reads a player's name at {}:{}", call.file, call.line));
		}
	}
}
