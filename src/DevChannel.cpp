#include "DevChannel.h"

#include "Config.h"
#include "DevCommands.h"
#include "DevLine.h"
#include "MainQueue.h"

#include <array>
#include <future>

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <bcrypt.h>

namespace DevChannel
{
	namespace
	{
		// A loading screen on a slow machine holds the main thread for longer than a few seconds.
		constexpr auto MAIN_THREAD_TIMEOUT = 12s;

		MainQueue   mainQueue;
		std::string token;

		std::optional<std::filesystem::path> TokenPath()
		{
			const auto folder = SKSE::log::log_directory();
			return folder ? std::optional{ *folder / "SkyrimCoop_dev.token" } : std::nullopt;
		}

		bool CreateToken()
		{
			std::array<std::uint8_t, 32> bytes{};
			if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
				return false;
			}
			token.clear();
			for (const auto b : bytes) {
				token += std::format("{:02x}", b);
			}
			const auto path = TokenPath();
			if (!path) {
				return false;
			}
			std::ofstream file{ *path, std::ios::trunc | std::ios::binary };
			file << token;
			return static_cast<bool>(file);
		}

		// Game state may only be touched on the main thread, so every command is handed to it and the listener waits
		// for the answer.
		std::string RunOnMainThread(DevCommands::Handler a_handler, std::string a_args)
		{
			auto promise = std::make_shared<std::promise<std::string>>();
			auto future = promise->get_future();
			// A caller that gave up waiting does not want the command run minutes later (F4Multiplayer's lesson: orders
			// queued behind a long load went off after the test that asked had moved on).
			auto abandoned = std::make_shared<std::atomic<bool>>(false);

			mainQueue.Post([promise, abandoned, a_handler, args = std::move(a_args)]() {
				if (abandoned->load()) {
					promise->set_value({});
					return;
				}
				// An exception must never escape into the game's task loop.
				try {
					promise->set_value(a_handler(args));
				} catch (const std::exception& e) {
					promise->set_value(std::format("error: {}", e.what()));
				} catch (...) {
					promise->set_value("error: command failed");
				}
			});

			if (future.wait_for(MAIN_THREAD_TIMEOUT) != std::future_status::ready) {
				abandoned->store(true);
				return "error: timed out waiting for the game's main thread (is the tick running? `ping` answers without it)";
			}
			return future.get();
		}

		std::string HandleCommand(std::string_view a_line)
		{
			const auto command = DevLine::Split(a_line);
			// Answered here, off the main thread: they work before the tick runs, and say whether it does.
			if (command.name == "ping") {
				return "pong";
			}
			if (command.name == "help") {
				return DevCommands::Help();
			}
			if (const auto handler = DevCommands::Find(command.name)) {
				return RunOnMainThread(handler, std::string{ command.args });
			}
			return std::format("error: unknown command '{}' (try help)", command.name);
		}

		void Send(SOCKET a_client, std::string_view a_reply)
		{
			// One reply is one line: a reply with a line break in it would be read as two.
			std::string line{ a_reply };
			std::ranges::replace(line, '\n', ' ');
			std::ranges::replace(line, '\r', ' ');
			line += '\n';
			send(a_client, line.data(), static_cast<int>(line.size()), 0);
		}

		void ServeClient(SOCKET a_client)
		{
			DevLine::LineBuffer buffer;
			char                chunk[512];
			bool                authed = false;
			// One client is served at a time: a client that connects and goes quiet (a dropped tunnel, a killed script)
			// must not hold the channel for ever.
			const DWORD timeout = 15000;
			setsockopt(a_client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

			for (;;) {
				const int received = recv(a_client, chunk, sizeof(chunk), 0);
				if (received <= 0) {
					break;
				}
				buffer.Append({ chunk, static_cast<std::size_t>(received) });
				if (buffer.Overflowed()) {
					break;
				}
				while (const auto line = buffer.Next()) {
					if (!authed) {
						if (!DevLine::Authorises(*line, token)) {
							logs::warn("Dev channel: refused a connection that did not give the token");
							Send(a_client, "error: not authorized");
							closesocket(a_client);
							return;
						}
						authed = true;
						Send(a_client, "ok");
						continue;
					}
					Send(a_client, HandleCommand(*line));
				}
			}
			closesocket(a_client);
		}

		void Listen(std::uint16_t a_port)
		{
			WSADATA wsa;
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
				logs::error("Dev channel: WSAStartup failed");
				return;
			}
			const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (listener == INVALID_SOCKET) {
				logs::error("Dev channel: socket failed ({})", WSAGetLastError());
				return;
			}
			// Loopback only: this channel must never be reachable from other machines.
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(a_port);
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(listener, 1) == SOCKET_ERROR) {
				logs::error("Dev channel: could not listen on 127.0.0.1:{} ({})", a_port, WSAGetLastError());
				closesocket(listener);
				return;
			}
			logs::info("Dev channel: listening on 127.0.0.1:{}", a_port);
			for (;;) {
				const SOCKET client = accept(listener, nullptr, nullptr);
				if (client == INVALID_SOCKET) {
					std::this_thread::sleep_for(100ms);  // don't spin if accept keeps failing
					continue;
				}
				ServeClient(client);
			}
		}
	}

	void Start()
	{
		const auto& settings = Config::Get();
		if (!settings.devChannel) {
			return;
		}
		if (!CreateToken()) {
			logs::error("Dev channel: could not write its token file; not started");
			return;
		}
		std::thread(Listen, settings.devChannelPort).detach();
	}

	void Frame()
	{
		mainQueue.Drain();
	}
}
