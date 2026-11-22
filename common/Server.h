#pragma once

#include "Protocol.h"
#include "Transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The session server: lets players in (same mod version, same load order, the host's password), tells them about
// each other and passes each player's state on to the rest. Runs on its own thread; used by the standalone
// SkyrimCoopServer.exe and, once the game side exists, by the game plugin when a player hosts.
//
// This is the first, small server. The shared world (kills, loot, doors, quests, who runs which NPC) is what
// docs/PLAN.md's later phases add to it.
class Server
{
public:
	struct Options
	{
		std::uint16_t port = Protocol::DEFAULT_PORT;
		std::size_t   maxPlayers = 4;  // at most Protocol::MAX_PLAYERS
		std::string   password;        // empty = no password

		// Accept UDP connections only from this machine (the host's own game), e.g. when friends
		// connect through Steam instead.
		bool udpLoopbackOnly = false;

		// The Hello::identity of the game this server runs in (0: none, a dedicated server). That game,
		// connecting from this machine, is the host: every welcome names it. Any other connection from this
		// machine (a bot, a tunnel to another PC) is an ordinary player.
		std::uint64_t hostIdentity = 0;

		// A connection that has not said hello after this long is dropped. Tests shorten it.
		std::chrono::milliseconds helloTimeout{ 10000 };
	};

	using LogFn = std::function<void(std::string_view)>;

	// a_log is called from the server thread (and from Start). Its lines name no player: "player 2", a masked
	// address or an alias (common/Redact.h), and each has been through Redact::Scrub.
	explicit Server(LogFn a_log);
	~Server();

	Server(const Server&) = delete;
	Server& operator=(const Server&) = delete;

	// Binds the UDP port and starts the server thread. Players can also arrive through any extra
	// transports (e.g. Steam). Returns false if the port can't be bound.
	bool Start(const Options& a_options, std::vector<std::unique_ptr<ServerTransport>> a_extraTransports = {});
	// Drops every connection and stops the server thread.
	void Stop();

	[[nodiscard]] bool Running() const { return running; }
	// Players in the session right now. Any thread.
	[[nodiscard]] std::size_t Players() const { return players; }

private:
	void Run();

	LogFn                                         log;
	Options                                       options;
	std::vector<std::unique_ptr<ServerTransport>> transports;
	std::thread                                   thread;
	std::atomic<bool>                             running{ false };
	std::atomic<bool>                             stopRequested{ false };
	std::atomic<std::size_t>                      players{ 0 };
};
