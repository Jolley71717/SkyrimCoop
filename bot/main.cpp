// Test client that pretends to be a player: connects to a server and walks in a circle.
//
//   SkyrimCoopBot --server 127.0.0.1:7780 --name Bot --cell 0 --worldspace 0000003C
//                 --x 0 --y 0 --z 0 [--radius 300] [--speed 150] [--content-hash HEX] [--password X]
//                 [--identity HEX]  (sent in the hello; the server gives the same identity the same player id again)
//                 [--seconds N]     (leaves after N seconds; without it, runs until killed)
//
// It prints who joins and leaves and, at the end, how many states of other players it heard.
// Exit codes: 0 ok, 1 no network, 2 bad arguments, 3 rejected, 4 disconnected by the server, 5 never welcomed
// before --seconds ran out.

#include "Net.h"
#include "Protocol.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace
{
	template <class T>
	bool ParseNumber(std::string_view a_str, T& a_out, int a_base = 10)
	{
		if constexpr (std::is_floating_point_v<T>) {
			const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), a_out);
			return ec == std::errc{} && ptr == a_str.data() + a_str.size();
		} else {
			if (a_base == 16 && (a_str.starts_with("0x") || a_str.starts_with("0X"))) {
				a_str.remove_prefix(2);
			}
			const auto [ptr, ec] = std::from_chars(a_str.data(), a_str.data() + a_str.size(), a_out, a_base);
			return ec == std::errc{} && ptr == a_str.data() + a_str.size();
		}
	}
}

int main(int argc, char* argv[])
{
	std::string   server = "127.0.0.1";
	std::string   name = "Bot";
	std::string   password;
	std::uint32_t cell = 0;
	std::uint32_t worldspace = 0x3C;  // Tamriel
	std::uint32_t contentHash = 0;
	std::uint64_t identity = 0;
	float         cx = 0, cy = 0, cz = 0, radius = 300, walkSpeed = 150;
	int           seconds = 0;  // 0 = run until killed

	if (argc % 2 == 0) {
		std::cerr << "missing value for " << argv[argc - 1] << '\n';
		return 2;
	}
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string_view key = argv[i];
		const std::string_view value = argv[i + 1];
		bool                   ok = true;
		if (key == "--server") {
			server = value;
		} else if (key == "--name") {
			name = value;
		} else if (key == "--password") {
			password = value;
		} else if (key == "--cell") {
			ok = ParseNumber(value, cell, 16);
		} else if (key == "--worldspace") {
			ok = ParseNumber(value, worldspace, 16);
		} else if (key == "--content-hash") {
			ok = ParseNumber(value, contentHash, 16);
		} else if (key == "--identity") {
			ok = ParseNumber(value, identity, 16);
		} else if (key == "--x") {
			ok = ParseNumber(value, cx);
		} else if (key == "--y") {
			ok = ParseNumber(value, cy);
		} else if (key == "--z") {
			ok = ParseNumber(value, cz);
		} else if (key == "--radius") {
			ok = ParseNumber(value, radius);
		} else if (key == "--speed") {
			ok = ParseNumber(value, walkSpeed);
		} else if (key == "--seconds") {
			ok = ParseNumber(value, seconds) && seconds >= 0;
		} else {
			ok = false;
		}
		if (!ok) {
			std::cerr << "bad argument: " << key << ' ' << value << '\n';
			return 2;
		}
	}

	if (!Net::Initialize()) {
		std::cerr << "ENet initialization failed\n";
		return 1;
	}
	ENetAddress address{};
	if (!Net::ResolveAddress(server, Protocol::DEFAULT_PORT, address)) {
		std::cerr << "cannot resolve the server's address\n";
		Net::Shutdown();
		return 1;
	}
	const auto host = enet_host_create(nullptr, 1, Protocol::CHANNEL_COUNT, 0, 0);
	const auto peer = host ? enet_host_connect(host, &address, Protocol::CHANNEL_COUNT, 0) : nullptr;
	if (!peer) {
		std::cerr << "could not create the connection\n";
		if (host) {
			enet_host_destroy(host);
		}
		Net::Shutdown();
		return 1;
	}

	using Clock = std::chrono::steady_clock;
	const auto    started = Clock::now();
	auto          nextState = started;
	auto          nextBeat = started;
	bool          welcomed = false;
	int           result = 0;
	bool          done = false;
	std::uint32_t sequence = 0;
	std::uint32_t beats = 0;
	std::uint32_t acks = 0;
	std::uint32_t statesHeard = 0;

	while (!done) {
		const auto now = Clock::now();
		if (seconds > 0 && now - started >= std::chrono::seconds(seconds)) {
			break;
		}

		ENetEvent event;
		while (!done && enet_host_service(host, &event, 5) > 0) {
			switch (event.type) {
			case ENET_EVENT_TYPE_CONNECT:
				Net::Send(peer, Protocol::Encode(Protocol::Hello{ .contentHash = contentHash, .name = name, .password = password, .identity = identity }), true);
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				{
					const std::span<const std::uint8_t> data{ event.packet->data, event.packet->dataLength };
					switch (Protocol::PeekType(data).value_or(Protocol::MessageType{})) {
					case Protocol::MessageType::kWelcome:
						if (const auto msg = Protocol::DecodeWelcome(data)) {
							welcomed = true;
							std::cout << "welcomed as player " << msg->playerId << (msg->hostId ? " (a host is in the session)" : "") << std::endl;
						}
						break;
					case Protocol::MessageType::kReject:
						if (const auto msg = Protocol::DecodeReject(data)) {
							std::cout << "rejected: " << msg->reason << std::endl;
						}
						result = 3;
						done = true;
						break;
					case Protocol::MessageType::kPlayerJoined:
						if (const auto msg = Protocol::DecodePlayerJoined(data)) {
							std::cout << "player " << msg->playerId << " is here" << std::endl;
						}
						break;
					case Protocol::MessageType::kPlayerLeft:
						if (const auto msg = Protocol::DecodePlayerLeft(data)) {
							std::cout << "player " << msg->playerId << " left" << std::endl;
						}
						break;
					case Protocol::MessageType::kRemoteState:
						statesHeard += Protocol::DecodeRemoteState(data).has_value();
						break;
					case Protocol::MessageType::kHeartbeatAck:
						acks += Protocol::DecodeHeartbeat(data).has_value();
						break;
					default:
						break;
					}
					enet_packet_destroy(event.packet);
				}
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
				if (result == 0) {
					std::cout << "disconnected by the server" << std::endl;
					result = 4;
				}
				done = true;
				break;
			default:
				break;
			}
		}
		if (done || !welcomed) {
			continue;
		}

		// Twenty states a second, walking a circle around the given point and facing along it.
		if (now >= nextState) {
			nextState = now + std::chrono::milliseconds(50);
			const float elapsed = std::chrono::duration<float>(now - started).count();
			const float angle = radius > 0.0f ? elapsed * walkSpeed / radius : 0.0f;
			Protocol::PlayerState state;
			state.sequence = ++sequence;
			state.cell = cell;
			state.worldspace = cell ? 0 : worldspace;
			state.x = cx + radius * std::cos(angle);
			state.y = cy + radius * std::sin(angle);
			state.z = cz;
			state.heading = std::fmod(std::numbers::pi_v<float> * 2.0f - angle, std::numbers::pi_v<float> * 2.0f);
			state.speed = walkSpeed;
			state.sentMs = Protocol::SentMs(now);
			Net::Send(peer, Protocol::Encode(state), false);
		}
		if (now >= nextBeat) {
			nextBeat = now + std::chrono::seconds(1);
			Net::Send(peer, Protocol::Encode(Protocol::Heartbeat{ ++beats }, Protocol::MessageType::kHeartbeat), false);
		}
	}

	if (result == 0) {
		enet_peer_disconnect(peer, 0);
		enet_host_flush(host);
	}
	enet_host_destroy(host);
	Net::Shutdown();

	std::cout << "heard " << statesHeard << " states of other players; " << acks << " of " << beats << " heartbeats answered" << std::endl;
	if (result == 0 && !welcomed) {
		std::cout << "never welcomed" << std::endl;
		return 5;
	}
	return result;
}
