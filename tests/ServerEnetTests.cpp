#include "Test.h"

#include "Net.h"
#include "ServerFixture.h"

#include <chrono>
#include <optional>
#include <vector>

// The server over its real UDP transport: an ENet client on this machine joins, as SkyrimCoopBot and the game do.

namespace
{
	using MT = Protocol::MessageType;
	using Clock = std::chrono::steady_clock;

	// A client connection to a server on this machine.
	class Client
	{
	public:
		explicit Client(std::uint16_t a_port)
		{
			REQUIRE(Net::Initialize());
			initialized = true;
			ENetAddress address{};
			REQUIRE(Net::ResolveAddress("127.0.0.1", a_port, address));
			host = enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0);
			REQUIRE(host);
			peer = enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0);
			REQUIRE(peer);
		}

		~Client()
		{
			if (host) {
				if (peer && !gone) {
					enet_peer_disconnect_now(peer, 0);
				}
				enet_host_destroy(host);
			}
			if (initialized) {
				Net::Shutdown();
			}
		}

		Client(const Client&) = delete;
		Client& operator=(const Client&) = delete;

		// Runs the connection until a_done says so or a_timeout passes. Packets received are kept in `received`.
		template <class Done>
		bool Until(Done a_done, std::chrono::milliseconds a_timeout = std::chrono::seconds(30))
		{
			const auto deadline = Clock::now() + a_timeout;
			while (!a_done()) {
				if (Clock::now() >= deadline) {
					return false;
				}
				ENetEvent event;
				while (enet_host_service(host, &event, 5) > 0) {
					switch (event.type) {
					case ENET_EVENT_TYPE_CONNECT:
						connected = true;
						break;
					case ENET_EVENT_TYPE_RECEIVE:
						received.emplace_back(event.packet->data, event.packet->data + event.packet->dataLength);
						enet_packet_destroy(event.packet);
						break;
					case ENET_EVENT_TYPE_DISCONNECT:
						gone = true;
						break;
					default:
						break;
					}
				}
			}
			return true;
		}

		void Send(const std::vector<std::uint8_t>& a_packet, bool a_reliable)
		{
			Net::Send(peer, a_packet, a_reliable);
			enet_host_flush(host);
		}

		// The first packet of a_type received so far (taken out of `received`).
		std::optional<std::vector<std::uint8_t>> Take(MT a_type)
		{
			const auto it = std::ranges::find_if(received, [&](const std::vector<std::uint8_t>& a_packet) { return Protocol::PeekType(a_packet) == a_type; });
			if (it == received.end()) {
				return std::nullopt;
			}
			auto packet = std::move(*it);
			received.erase(it);
			return packet;
		}

		bool Has(MT a_type) const
		{
			return std::ranges::any_of(received, [&](const std::vector<std::uint8_t>& a_packet) { return Protocol::PeekType(a_packet) == a_type; });
		}

		bool                                   connected = false;
		bool                                   gone = false;
		std::vector<std::vector<std::uint8_t>> received;

	private:
		bool      initialized = false;
		ENetHost* host = nullptr;
		ENetPeer* peer = nullptr;
	};
}

TEST("server over UDP: a client on this machine joins, is the host, and trades states with another player")
{
	Server::Options options;
	options.hostIdentity = 0x4057;
	Fixture f{ options };

	Client client{ f.udpPort };
	REQUIRE(client.Until([&] { return client.connected; }));
	client.Send(Protocol::Encode(Fixture::MakeHello("Host", 0x4057)), true);
	REQUIRE(client.Until([&] { return client.Has(MT::kWelcome); }));
	const auto welcome = Protocol::DecodeWelcome(*client.Take(MT::kWelcome));
	REQUIRE(welcome);
	CHECK(welcome->playerId == 1);
	// It came from this machine with the host's identity.
	CHECK(welcome->hostId == 1);
	CHECK(f.LoggedSoon("server: player 1 joined from udp 127.0.0.1:"));
	CHECK(f.Logged(", the host (1 in the session)"));

	// A second player, through the other transport.
	const auto other = f.Join(1, Fixture::MakeHello("Friend"));
	REQUIRE(other);
	CHECK(other->playerId == 2);
	CHECK(other->hostId == 1);
	REQUIRE(client.Until([&] { return client.Has(MT::kPlayerJoined); }));
	const auto joined = Protocol::DecodePlayerJoined(*client.Take(MT::kPlayerJoined));
	REQUIRE(joined);
	CHECK(joined->playerId == 2);
	CHECK(joined->name == "Friend");

	// The client's state reaches the friend (sent a few times: the state channel may lose one)...
	Protocol::PlayerState state;
	state.worldspace = 0x3C;
	state.x = 100.0f;
	std::optional<std::vector<std::uint8_t>> heard;
	for (std::uint32_t sequence = 1; sequence <= 50 && !heard; ++sequence) {
		state.sequence = sequence;
		client.Send(Protocol::Encode(state), false);
		(void)client.Until([] { return false; }, std::chrono::milliseconds(20));
		heard = f.fake->WaitFor(1, Fixture::Type(MT::kRemoteState), std::chrono::milliseconds(100));
	}
	REQUIRE(heard);
	const auto remote = Protocol::DecodeRemoteState(*heard);
	REQUIRE(remote);
	CHECK(remote->playerId == 1);
	CHECK(remote->state.x == 100.0f);

	// ...and the friend's reaches the client.
	for (std::uint32_t sequence = 1; sequence <= 50 && !client.Has(MT::kRemoteState); ++sequence) {
		state.sequence = sequence;
		state.x = 200.0f;
		f.fake->Deliver(1, Protocol::Encode(state));
		(void)client.Until([&] { return client.Has(MT::kRemoteState); }, std::chrono::milliseconds(100));
	}
	REQUIRE(client.Has(MT::kRemoteState));
	const auto back = Protocol::DecodeRemoteState(*client.Take(MT::kRemoteState));
	REQUIRE(back);
	CHECK(back->playerId == 2);
	CHECK(back->state.x == 200.0f);

	// A heartbeat comes back with its token.
	for (std::uint32_t attempt = 0; attempt < 50 && !client.Has(MT::kHeartbeatAck); ++attempt) {
		client.Send(Protocol::Encode(Protocol::Heartbeat{ 0x5EED }, MT::kHeartbeat), false);
		(void)client.Until([&] { return client.Has(MT::kHeartbeatAck); }, std::chrono::milliseconds(100));
	}
	REQUIRE(client.Has(MT::kHeartbeatAck));
	const auto ack = Protocol::DecodeHeartbeat(*client.Take(MT::kHeartbeatAck));
	REQUIRE(ack);
	CHECK(ack->token == 0x5EED);
}

TEST("server over UDP: a refused client is told why, then disconnected")
{
	Server::Options options;
	options.password = "sweetroll";
	Fixture f{ options };

	Client client{ f.udpPort };
	REQUIRE(client.Until([&] { return client.connected; }));
	client.Send(Protocol::Encode(Fixture::MakeHello("Guesser")), true);
	REQUIRE(client.Until([&] { return client.gone; }));
	const auto packet = client.Take(MT::kReject);
	REQUIRE(packet);
	const auto reject = Protocol::DecodeReject(*packet);
	REQUIRE(reject);
	CHECK(reject->reason == "Wrong password.");
	CHECK(f.LoggedSoon("server: refused udp 127.0.0.1:"));
	CHECK(f.server.Players() == 0);
}

TEST("server over UDP: a client that leaves is told to the others")
{
	Fixture f;
	REQUIRE(f.Join(1, Fixture::MakeHello("Stays")));
	{
		Client client{ f.udpPort };
		REQUIRE(client.Until([&] { return client.connected; }));
		client.Send(Protocol::Encode(Fixture::MakeHello("Leaves")), true);
		REQUIRE(client.Until([&] { return client.Has(MT::kWelcome); }));
	}
	const auto packet = f.fake->WaitFor(1, Fixture::Type(MT::kPlayerLeft));
	REQUIRE(packet);
	const auto left = Protocol::DecodePlayerLeft(*packet);
	REQUIRE(left);
	CHECK(left->playerId == 2);
	CHECK(f.LoggedSoon("server: player 2 left (disconnected)"));
}
