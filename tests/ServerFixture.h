#pragma once

// A running Server the tests talk to through a FakeTransport: they connect players, send packets as them and read
// what the server sent back. The server also binds a UDP port (it always does): a random one on this machine only,
// which ServerEnetTests.cpp connects to with a real ENet client.

#include "FakeTransport.h"
#include "Protocol.h"
#include "Server.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

struct Fixture
{
	using MT = Protocol::MessageType;

	std::mutex               logMutex;
	std::vector<std::string> logLines;
	Server                   server{ [this](std::string_view a_line) {
		std::lock_guard lock{ logMutex };
		logLines.emplace_back(a_line);
	} };
	FakeTransport*           fake = nullptr;
	std::uint16_t            udpPort = 0;

	explicit Fixture(Server::Options a_options = {})
	{
		a_options.udpLoopbackOnly = true;
		// Another program (or another run of the tests) may hold the port picked: try others.
		std::random_device random;
		for (int attempt = 0; attempt < 50 && !fake; ++attempt) {
			a_options.port = static_cast<std::uint16_t>(40000 + random() % 20000);
			auto       transport = std::make_unique<FakeTransport>();
			const auto raw = transport.get();
			std::vector<std::unique_ptr<ServerTransport>> extra;
			extra.push_back(std::move(transport));
			if (server.Start(a_options, std::move(extra))) {
				fake = raw;
				udpPort = a_options.port;
			}
		}
		REQUIRE(fake);
	}

	~Fixture() { server.Stop(); }

	static constexpr std::uint8_t Type(MT a_type) { return static_cast<std::uint8_t>(a_type); }

	static Protocol::Hello MakeHello(std::string a_name, std::uint64_t a_identity = 0)
	{
		Protocol::Hello hello;
		hello.contentHash = 0x1234;
		hello.name = std::move(a_name);
		hello.identity = a_identity;
		return hello;
	}

	// Connects a_peer and says hello as it. The welcome, or nothing when the server did not send one in time
	// (a refusal stays queued for the test to read).
	std::optional<Protocol::Welcome> Join(PeerId a_peer, const Protocol::Hello& a_hello, std::chrono::milliseconds a_timeout = std::chrono::seconds(30))
	{
		fake->Connect(a_peer);
		fake->Deliver(a_peer, Protocol::Encode(a_hello));
		const auto packet = fake->WaitFor(a_peer, Type(MT::kWelcome), a_timeout);
		return packet ? Protocol::DecodeWelcome(*packet) : std::nullopt;
	}

	// Connects a_peer, says hello as it and returns the refusal's reason (empty when none came in time).
	std::string Refused(PeerId a_peer, const Protocol::Hello& a_hello)
	{
		fake->Connect(a_peer);
		fake->Deliver(a_peer, Protocol::Encode(a_hello));
		const auto packet = fake->WaitFor(a_peer, Type(MT::kReject));
		const auto reject = packet ? Protocol::DecodeReject(*packet) : std::nullopt;
		return reject ? reject->reason : std::string{};
	}

	std::vector<std::string> LogLines()
	{
		std::lock_guard lock{ logMutex };
		return logLines;
	}

	bool Logged(std::string_view a_text)
	{
		std::lock_guard lock{ logMutex };
		return std::ranges::any_of(logLines, [&](const std::string& a_line) { return a_line.find(a_text) != std::string::npos; });
	}

	// The server logs from its own thread: waits up to a_timeout for a line that holds a_text.
	bool LoggedSoon(std::string_view a_text, std::chrono::milliseconds a_timeout = std::chrono::seconds(30))
	{
		const auto deadline = std::chrono::steady_clock::now() + a_timeout;
		while (!Logged(a_text)) {
			if (std::chrono::steady_clock::now() >= deadline) {
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return true;
	}
};
