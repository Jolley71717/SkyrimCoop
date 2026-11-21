// Standalone dedicated server: SkyrimCoopServer.exe [--port N] [--max-players N] [--password X] [--loopback-only]
// --loopback-only: this machine only (tests and soaks): nothing from outside can reach it, and Windows asks nothing.
// Its output names no player: "player 2", a masked address.

#include "Protocol.h"
#include "Server.h"

#ifdef _WIN32
#	include <Windows.h>
#else
#	include <csignal>
#endif

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace
{
	std::atomic<bool> quit{ false };
	std::atomic<bool> stopped{ false };

#ifdef _WIN32
	BOOL WINAPI OnConsoleSignal(DWORD)
	{
		quit = true;
		// Closing the window ends the process as soon as this returns, so give main() time to
		// disconnect everyone cleanly (Windows allows about five seconds).
		for (int i = 0; i < 40 && !stopped; ++i) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return TRUE;
	}
#else
	// Ctrl+C or a kill (SIGINT, SIGTERM): the process goes on until main() returns, so only
	// mark the end here; main() disconnects everyone and returns.
	void OnSignal(int)
	{
		quit = true;
	}
#endif

	template <class T>
	bool ParseNumber(std::string_view a_str, T& a_out)
	{
		const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), a_out);
		return ec == std::errc{} && ptr == a_str.data() + a_str.size();
	}
}

int main(int argc, char* argv[])
{
	Server::Options options;
	options.port = Protocol::DEFAULT_PORT;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		const bool             hasValue = i + 1 < argc;
		if (arg == "--port" && hasValue && ParseNumber(argv[i + 1], options.port)) {
			++i;
		} else if (arg == "--max-players" && hasValue && ParseNumber(argv[i + 1], options.maxPlayers) && options.maxPlayers >= 1 && options.maxPlayers <= Protocol::MAX_PLAYERS) {
			++i;
		} else if (arg == "--password" && hasValue) {
			options.password = argv[++i];
		} else if (arg == "--loopback-only") {
			options.udpLoopbackOnly = true;
		} else {
			std::cerr << "usage: SkyrimCoopServer [--port N] [--max-players 1-" << Protocol::MAX_PLAYERS << "] [--password X] [--loopback-only]\n";
			return 2;
		}
	}

	Server server{ [](std::string_view a_line) {
		std::cout << a_line << std::endl;
	} };

	if (!server.Start(options)) {
		return 1;
	}

#ifdef _WIN32
	SetConsoleCtrlHandler(OnConsoleSignal, TRUE);
#else
	std::signal(SIGINT, OnSignal);
	std::signal(SIGTERM, OnSignal);
#endif
	std::cout << "Press Ctrl+C to stop." << std::endl;

	while (!quit) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}

	server.Stop();
	stopped = true;
	return 0;
}
