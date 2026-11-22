#include "net/Session.h"

#include "Config.h"
#include "Protocol.h"
#include "Redact.h"
#include "Server.h"
#include "game/LoadOrder.h"
#include "game/LocalPlayer.h"
#include "game/RemotePlayers.h"
#include "net/Identity.h"
#include "net/LaunchArgs.h"
#include "net/NameChoice.h"
#include "net/NetClient.h"
#include "steam/Steam.h"
#include "steam/SteamTransport.h"

#include <Windows.h>

namespace Session
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr auto STATE_EVERY = 50ms;  // 20 states a second (StateBuffer's delay holds two of them)
		constexpr auto LOOKS_EVERY = 1s;    // looks are read this often and sent when they changed
		constexpr auto BEAT_EVERY = 2s;

		enum class Role
		{
			kNone,
			kHost,
			kFriend,
		};

		Role                              role = Role::kNone;
		std::unique_ptr<Server>           server;
		std::unique_ptr<ClientConnection> client;
		bool                              welcomed = false;
		std::uint32_t                     playerId = 0;
		std::uint32_t                     hostId = 0;
		std::uint32_t                     sequence = 0;
		Clock::time_point                 nextState{};
		Clock::time_point                 nextLooks{};
		Clock::time_point                 nextBeat{};
		Clock::time_point                 joinedAt{};
		// The looks last sent on this connection. The contract with the server: a game sends its looks after every
		// welcome (a new connection starts with none on the server, so Reset and the welcome clear this), and again
		// whenever they change. What comes the other way is another player's game's word: RemotePlayers takes a race
		// only as the race of a load-order preset and a worn id only as a TESObjectARMO.
		std::optional<Protocol::PlayerLooks> sentLooks;
		std::unordered_map<std::uint32_t, Clock::time_point> beats;  // heartbeat token -> when it went
		std::uint32_t                     beatToken = 0;
		float                             rttMs = -1.0f;
		std::uint64_t                     statesSent = 0;
		std::uint64_t                     statesIn = 0;
		std::uint64_t                     looksSent = 0;
		std::string                       lastEnd = "none";
		bool                              launchChecked = false;
		bool                              autoHosted = false;

		void Reset()
		{
			welcomed = false;
			playerId = 0;
			hostId = 0;
			sentLooks.reset();
			beats.clear();
			rttMs = -1.0f;
		}

		void Send(std::vector<std::uint8_t> a_data, bool a_reliable)
		{
			if (client) {
				client->Send(std::move(a_data), a_reliable);
			}
		}

		void SayHello()
		{
			const auto plugins = LoadOrder::Read();
			auto       order = Protocol::ComparedLoadOrder(plugins.full, plugins.light);
			const auto steamName = Config::Get().steam && Steam::Available() ? Steam::PersonaName() : std::string{};
			Protocol::Hello hello{
				.contentHash = Protocol::LoadOrderHash(order),
				.name = NameChoice::Choose(Config::Get().playerName, LocalPlayer::CharacterName(), steamName),
				.password = Config::Get().password,
				.identity = Identity::Get(),
			};
			// The list only says which plugin differs (the host compares the hash); one that would go on the wire cut
			// (too many names, or more than MAX_PLUGIN_LIST_BYTES) is left off whole, so the host never names a
			// difference that is only where the list was cut.
			if (Protocol::PluginsFit(order)) {
				hello.plugins = std::move(order);
			} else {
				logs::warn("Session: the load order ({} names) is too long for a hello's plugin list; only its hash is sent, so a host whose mods differ can't say which",
					order.size());
			}
			Send(Protocol::Encode(hello), true);
			logs::info("Session: said hello ({} plugins, load order {:08X})", plugins.full.size() + plugins.light.size(), hello.contentHash);
		}

		void Join(SteamApi::SteamId a_host)
		{
			Reset();
			role = Role::kFriend;
			client = std::make_unique<SteamClient>();
			client->Connect(SteamTransport::FormatAddress(a_host), Config::Get().port);
			joinedAt = Clock::now();
			logs::info("Session: joining the host {} through Steam", Redact::Alias(a_host));
		}

		void Handle(std::span<const std::uint8_t> a_data)
		{
			const auto type = Protocol::PeekType(a_data);
			if (!type) {
				return;
			}
			switch (*type) {
			case Protocol::MessageType::kWelcome:
				if (const auto msg = Protocol::DecodeWelcome(a_data)) {
					welcomed = true;
					playerId = msg->playerId;
					hostId = msg->hostId;
					sentLooks.reset();
					nextState = nextLooks = nextBeat = Clock::now();
					logs::info("Session: in the session as player {} (the host is player {}){}", playerId, hostId,
						role == Role::kFriend ? std::format(", {} ms after asking", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - joinedAt).count()) : "");
				}
				break;
			case Protocol::MessageType::kReject:
				if (const auto msg = Protocol::DecodeReject(a_data)) {
					logs::warn("Session: the host turned us away: {}", Redact::OneLine(msg->reason));
					lastEnd = "turned away";
				}
				break;
			case Protocol::MessageType::kPlayerJoined:
				if (const auto msg = Protocol::DecodePlayerJoined(a_data); msg && msg->playerId != playerId) {
					RemotePlayers::Joined(msg->playerId, msg->name);
				}
				break;
			case Protocol::MessageType::kPlayerLeft:
				if (const auto msg = Protocol::DecodePlayerLeft(a_data)) {
					RemotePlayers::Left(msg->playerId);
				}
				break;
			case Protocol::MessageType::kRemoteState:
				if (const auto msg = Protocol::DecodeRemoteState(a_data); msg && msg->playerId != playerId && msg->state.IsFinite()) {
					++statesIn;
					RemotePlayers::Moved(msg->playerId, msg->state);
				}
				break;
			case Protocol::MessageType::kRemoteLooks:
				if (const auto msg = Protocol::DecodeRemoteLooks(a_data); msg && msg->playerId != playerId) {
					RemotePlayers::Looked(msg->playerId, msg->looks);
				}
				break;
			case Protocol::MessageType::kHeartbeatAck:
				if (const auto msg = Protocol::DecodeHeartbeat(a_data)) {
					if (const auto it = beats.find(msg->token); it != beats.end()) {
						rttMs = std::chrono::duration<float, std::milli>(Clock::now() - it->second).count();
						beats.erase(it);
					}
				}
				break;
			default:
				break;
			}
		}

		void SendOurs(Clock::time_point a_now)
		{
			if (a_now >= nextState) {
				// From the time the last was due, not when the tick came (about every 16 ms): 20 a second, not 15. One
				// that fell far behind (a stall) starts over from now rather than sending a burst.
				nextState = a_now - nextState < STATE_EVERY ? nextState + STATE_EVERY : a_now + STATE_EVERY;
				if (auto state = LocalPlayer::Read()) {
					state->sequence = ++sequence;
					state->sentMs = Protocol::SentMs(a_now);
					Send(Protocol::Encode(*state), false);
					++statesSent;
				}
			}
			if (a_now >= nextLooks) {
				nextLooks = a_now + LOOKS_EVERY;
				if (const auto looks = LocalPlayer::Looks(); looks && looks != sentLooks) {
					Send(Protocol::Encode(*looks), true);
					sentLooks = looks;
					++looksSent;
					logs::info("Session: sent our looks (race {:08X}, {}, {} worn items)", looks->race, looks->female ? "female" : "male", looks->worn.size());
				}
			}
			if (a_now >= nextBeat) {
				nextBeat = a_now + BEAT_EVERY;
				const auto token = ++beatToken;
				beats[token] = a_now;
				if (beats.size() > 16) {
					beats.erase(beats.begin());
				}
				Send(Protocol::Encode(Protocol::Heartbeat{ token }, Protocol::MessageType::kHeartbeat), false);
			}
		}

		// The join Steam asked for: an invite or Join Game while running, a steam://joinlobby address, or the lobby on the
		// command line this game was started with.
		void LookForJoin()
		{
			if (!Config::Get().steam || !Steam::Available()) {
				return;
			}
			if (!std::exchange(launchChecked, true)) {
				if (const auto lobby = LaunchArgs::Lobby(::GetCommandLineA())) {
					logs::info("Session: the game was started from a friend's invite: joining their lobby");
					Steam::Join(*lobby);
				}
			}
			const auto target = Steam::TakeJoinTarget();
			if (!target) {
				return;
			}
			if (role == Role::kHost) {
				logs::info("Session: a join was asked for while we host; ignored");
				return;
			}
			if (role == Role::kFriend) {
				Leave("joining another host");
			}
			Join(*target);
		}
	}

	bool Host(std::string& a_why)
	{
		if (role != Role::kNone) {
			a_why = "already in a session";
			return false;
		}
		const auto& config = Config::Get();
		const bool  steam = config.steam && config.transport != "enet";
		if (steam && !Steam::Available()) {
			a_why = "Steam is not available: " + Steam::Unavailable();
			return false;
		}
		Server::Options options{
			.port = config.port,
			.maxPlayers = config.maxPlayers,
			.password = config.password,
			.udpLoopbackOnly = steam,  // friends come through Steam; the port takes only this game's own connection
			.hostIdentity = Identity::Get(),
		};
		std::vector<std::unique_ptr<ServerTransport>> extra;
		if (steam) {
			extra.push_back(std::make_unique<SteamServerTransport>(config.maxPlayers));
		}
		server = std::make_unique<Server>([](std::string_view a_line) { logs::info("Server: {}", a_line); });
		if (!server->Start(options, std::move(extra))) {
			server.reset();
			a_why = std::format("the session server could not start (UDP port {} in use?)", config.port);
			return false;
		}
		if (steam) {
			Steam::StartHosting(config.maxPlayers);
		}
		Reset();
		role = Role::kHost;
		client = std::make_unique<NetClient>();
		client->Connect("127.0.0.1", config.port);
		joinedAt = Clock::now();
		logs::info("Session: hosting ({}, up to {} players)", steam ? "friends join through Steam" : "direct UDP", config.maxPlayers);
		return true;
	}

	void Leave(std::string_view a_why)
	{
		if (role == Role::kNone) {
			return;
		}
		const auto was = role;
		client.reset();  // its destructor says goodbye and ends its thread
		if (server) {
			server->Stop();
			server.reset();
		}
		if (Config::Get().steam && Steam::Available()) {
			if (was == Role::kHost) {
				Steam::StopHosting();
			} else {
				Steam::Leave();
			}
		}
		RemotePlayers::Clear();
		Reset();
		role = Role::kNone;
		lastEnd = std::string{ a_why };
		logs::info("Session: left ({})", a_why);
	}

	void Frame()
	{
		LookForJoin();
		if (Config::Get().host && !autoHosted && role == Role::kNone && LocalPlayer::InWorld()) {
			autoHosted = true;
			std::string why;
			if (!Host(why)) {
				logs::warn("Session: bHost is set but hosting failed: {}", why);
			}
		}
		if (!client) {
			return;
		}
		for (auto& event : client->Poll()) {
			switch (event.type) {
			case ClientConnection::Event::Type::kConnected:
				SayHello();
				break;
			case ClientConnection::Event::Type::kPacket:
				Handle(event.data);
				break;
			case ClientConnection::Event::Type::kDisconnected:
				logs::info("Session: the connection to the {} ended", role == Role::kHost ? "session server" : "host");
				Leave(welcomed ? "the connection ended" : "could not get in");
				return;
			}
		}
		if (welcomed) {
			SendOurs(Clock::now());
		}
		RemotePlayers::Frame();
	}

	bool Active()
	{
		return role != Role::kNone;
	}

	std::string Describe()
	{
		const auto status = client ? client->GetStatus() : ClientConnection::Status::kDisconnected;
		return std::format("role={} connection={} welcomed={} player={} host={} players={} sent={} received={} looksSent={} rtt={} serverPlayers={} lastEnd={}",
			role == Role::kHost ? "host" : role == Role::kFriend ? "friend" : "none",
			status == ClientConnection::Status::kConnected ? "up" : status == ClientConnection::Status::kConnecting ? "connecting" : "down",
			welcomed ? 1 : 0, playerId, hostId, RemotePlayers::Count(), statesSent, statesIn, looksSent, rttMs < 0 ? std::string{ "?" } : std::format("{:.0f}", rttMs),
			server ? std::to_string(server->Players()) : std::string{ "-" }, lastEnd);
	}
}
