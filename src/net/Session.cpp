#include "net/Session.h"

#include "Config.h"
#include "FormIds.h"
#include "FormTranslate.h"
#include "Protocol.h"
#include "Redact.h"
#include "Server.h"
#include "game/Face.h"
#include "game/FaceRules.h"
#include "game/LoadOrder.h"
#include "game/LocalPlayer.h"
#include "game/NpcSync.h"
#include "game/PlayerActions.h"
#include "game/RemotePlayers.h"
#include "game/Riding.h"
#include "game/Shots.h"
#include "game/Water.h"
#include "net/FormsCommand.h"
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
		// What the hands hold is read this often alone, and the looks go at once when it changed: a sword taken out, a
		// spell readied or a bow put away is on the others' stand-in before the swing, cast or shot that follows it.
		constexpr auto HANDS_EVERY = 100ms;
		constexpr auto FACE_EVERY = 1s;  // our face is read this often, on its own timer (FaceRules::Settle counts on it)
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
		Clock::time_point                 nextHands{};
		Clock::time_point                 nextFace{};
		Clock::time_point                 nextBeat{};
		Clock::time_point                 joinedAt{};
		// The looks last sent on this connection. The contract with the server: a game sends its looks after every
		// welcome (a new connection starts with none on the server, so Reset and the welcome clear this), and again
		// whenever they change. What comes the other way is another player's game's word: RemotePlayers takes a race
		// only as the race of a load-order preset and a worn id only as a TESObjectARMO.
		std::optional<Protocol::PlayerLooks> sentLooks;
		// Our face, the same way: sent after every welcome and when it changed (FaceRules::Settle: once it holds still).
		std::optional<Protocol::PlayerFace> sentFace;
		FaceRules::Settle                   faceSettle;
		std::uint64_t                       facesSent = 0;
		std::unordered_map<std::uint32_t, Clock::time_point> beats;  // heartbeat token -> when it went
		std::uint32_t                     beatToken = 0;
		float                             rttMs = -1.0f;
		std::uint64_t                     statesSent = 0;
		std::uint64_t                     statesIn = 0;
		std::uint64_t                     looksSent = 0;
		std::uint64_t                     actionsSent = 0;  // our player's graph events sent (PlayerActions)
		std::string                       lastEnd = "none";
		// This game's edge to the session's form IDs (common/FormTranslate.h): our load order as the hello sent it, and
		// the session's plugin table once the server sends it (before the welcome, and again whenever it grows). Until
		// then the mapper is the identity, and nothing that carries a form is sent or read before the welcome.
		std::vector<std::string>          ourOrder;
		FormIds::Mapper                   forms;
		std::size_t                       tableSize = 0;
		FormTranslate::SkipLog            skippedPlugins;
		std::uint64_t                     formsTranslated = 0;  // packets that went or came with other IDs
		std::uint64_t                     formsSkipped = 0;     // packets with a form left out, set to none or nowhere
		std::uint64_t                     formsPlaceless = 0;   // states of a friend somewhere this game lacks
		bool                              launchChecked = false;
		bool                              autoHosted = false;

		void Reset()
		{
			welcomed = false;
			playerId = 0;
			hostId = 0;
			sentLooks.reset();
			sentFace.reset();
			faceSettle.Reset();
			PlayerActions::Recording(false);
			beats.clear();
			rttMs = -1.0f;
			forms = {};
			tableSize = 0;
			skippedPlugins.Clear();
			formsTranslated = formsSkipped = formsPlaceless = 0;
		}

		// What a translation did, counted; each plugin whose forms it skipped is named in the log once a session.
		void Note(const FormTranslate::Result& a_result, FormTranslate::Way a_way)
		{
			if (a_result.kind == FormTranslate::Result::Kind::kChanged) {
				++formsTranslated;
			}
			if (!a_result.Skipped()) {
				return;
			}
			++formsSkipped;
			formsPlaceless += a_result.placeless ? 1 : 0;
			const bool out = a_way == FormTranslate::Way::kToSession;
			if (skippedPlugins.FreshRuntime(a_result)) {
				logs::info("Session: forms made as a game ran (FF) are skipped {}: no other game has them (said once)", out ? "on the way out" : "on the way in");
			}
			for (const auto& plugin : skippedPlugins.Fresh(forms, a_way, a_result)) {
				logs::info("Session: forms of {} are skipped: {} (said once)", plugin.empty() ? "a plugin past the session's table" : FormsCommand::Shown(plugin),
					out ? "the others' games don't all have it" : "this game doesn't have it");
			}
		}

		// Every packet goes out in the session's numbering of forms.
		void Send(std::vector<std::uint8_t> a_data, bool a_reliable)
		{
			if (!client) {
				return;
			}
			auto result = FormTranslate::Translate(forms, FormTranslate::Way::kToSession, a_data);
			Note(result, FormTranslate::Way::kToSession);
			client->Send(result.kind == FormTranslate::Result::Kind::kChanged ? std::move(result.packet) : std::move(a_data), a_reliable);
		}

		// The plugins that differ from the session's, named in this game's log (a few of each; the counts say the rest).
		void SayDifferences()
		{
			constexpr std::size_t SHOWN = 8;
			const auto            list = [&](const FormIds::Table& a_from, const FormIds::Table& a_in) {
				std::string out;
				std::size_t count = 0;
				for (const auto* kind : { &a_from.full, &a_from.light }) {
					for (const auto& name : *kind) {
						if (!a_in.Find(name) && count++ < SHOWN) {
							out += (out.empty() ? "" : ", ") + FormsCommand::Shown(name);
						}
					}
				}
				return count > SHOWN ? std::format("{} and {} more", out, count - SHOWN) : out;
			};
			if (forms.OursOnly() != 0) {
				logs::info("Session: plugins of ours the session's table lacks (their forms are not shared): {}", list(forms.Local(), forms.Session()));
			}
			if (forms.SessionsOnly() != 0) {
				logs::info("Session: plugins of the session's this game lacks (their forms are skipped here): {}", list(forms.Session(), forms.Local()));
			}
		}

		void OnPluginTable(const Protocol::PluginTable& a_table)
		{
			forms = FormIds::Mapper(ourOrder, a_table.plugins);
			tableSize = forms.Session().Size();
			logs::info("Session: the session's plugin table has {} plugins: {} ({} of ours it lacks, {} of its this game lacks)", tableSize,
				forms.Identity() ? "the same as ours, nothing to translate" : "form ids are translated by plugin name", forms.OursOnly(), forms.SessionsOnly());
			SayDifferences();
		}

		void SayHello()
		{
			const auto plugins = LoadOrder::Read();
			auto       order = Protocol::ComparedLoadOrder(plugins.full, plugins.light);
			ourOrder = order;  // what the session's plugin table is matched against, whether or not the list fits a hello
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
				logs::warn("Session: the load order ({} names) is too long for a hello's plugin list; only its hash is sent, so this game can join only a session with exactly the same load order, and its forms are not translated",
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
			// Every packet is read in this game's numbering of forms.
			auto translated = FormTranslate::Translate(forms, FormTranslate::Way::kToLocal, a_data);
			Note(translated, FormTranslate::Way::kToLocal);
			if (translated.kind == FormTranslate::Result::Kind::kChanged) {
				a_data = translated.packet;
			}
			const auto type = Protocol::PeekType(a_data);
			if (!type) {
				return;
			}
			switch (*type) {
			case Protocol::MessageType::kPluginTable:
				if (const auto msg = Protocol::DecodePluginTable(a_data)) {
					OnPluginTable(*msg);
				}
				break;
			case Protocol::MessageType::kWelcome:
				if (const auto msg = Protocol::DecodeWelcome(a_data)) {
					welcomed = true;
					playerId = msg->playerId;
					hostId = msg->hostId;
					sentLooks.reset();
					sentFace.reset();
					faceSettle.Reset();
					nextState = nextLooks = nextHands = nextFace = nextBeat = Clock::now();
					PlayerActions::Recording(true);
					NpcSync::Started(playerId, hostId);
					Riding::Started(playerId);
					Shots::Started(playerId);
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
					Riding::Left(msg->playerId);
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
			case Protocol::MessageType::kRemoteFace:
				if (const auto msg = Protocol::DecodeRemoteFace(a_data); msg && msg->playerId != playerId) {
					RemotePlayers::Faced(msg->playerId, msg->face);
				}
				break;
			case Protocol::MessageType::kRemoteActions:
				if (const auto msg = Protocol::DecodeRemoteActions(a_data); msg && msg->playerId != playerId) {
					RemotePlayers::Acted(msg->playerId, msg->actions);
				}
				break;
			case Protocol::MessageType::kNpcOwners:
				if (const auto msg = Protocol::DecodeNpcOwners(a_data)) {
					NpcSync::Owners(*msg);
				}
				break;
			case Protocol::MessageType::kNpcStates:
				if (const auto msg = Protocol::DecodeNpcStates(a_data); msg && msg->runner != playerId) {
					NpcSync::States(*msg);
				}
				break;
			case Protocol::MessageType::kNpcActions:
				if (const auto msg = Protocol::DecodeNpcActions(a_data); msg && msg->runner != playerId) {
					NpcSync::Actions(*msg);
				}
				break;
			case Protocol::MessageType::kNpcGear:
				if (const auto msg = Protocol::DecodeNpcGear(a_data); msg && msg->runner != playerId) {
					NpcSync::Gear(*msg);
				}
				break;
			case Protocol::MessageType::kNpcHits:
				if (const auto msg = Protocol::DecodeNpcHits(a_data); msg && msg->hitter != playerId) {
					NpcSync::Hits(*msg);
				}
				break;
			case Protocol::MessageType::kNpcHitAnswers:
				if (const auto msg = Protocol::DecodeNpcHitAnswers(a_data)) {
					NpcSync::HitAnswers(*msg);
				}
				break;
			case Protocol::MessageType::kNpcHealths:
				if (const auto msg = Protocol::DecodeNpcHealths(a_data); msg && msg->runner != playerId) {
					NpcSync::Healths(*msg);
				}
				break;
			case Protocol::MessageType::kNpcDeaths:
				if (const auto msg = Protocol::DecodeNpcDeaths(a_data); msg && msg->runner != playerId) {
					NpcSync::Deaths(*msg);
				}
				break;
			case Protocol::MessageType::kRides:
				if (const auto msg = Protocol::DecodeRides(a_data); msg && msg->sender != playerId) {
					Riding::Rides(*msg);
				}
				break;
			case Protocol::MessageType::kShots:
				if (const auto msg = Protocol::DecodeShots(a_data); msg && msg->sender != playerId) {
					Shots::Heard(*msg);
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
			if (a_now >= nextHands) {
				nextHands = a_now + HANDS_EVERY;
				if (const auto hands = LocalPlayer::Hands(); hands && sentLooks &&
					*hands != Gear::Hands{ sentLooks->right, sentLooks->left, sentLooks->ammo, sentLooks->voice, sentLooks->unnamed }) {
					nextLooks = a_now;
				}
			}
			if (a_now >= nextLooks) {
				nextLooks = a_now + LOOKS_EVERY;
				if (const auto looks = LocalPlayer::Looks(); looks && looks != sentLooks) {
					Send(Protocol::Encode(*looks), true);
					// Said when the body or the armor changed; a hand's change (many a fight) is counted only.
					const bool body = !sentLooks || sentLooks->race != looks->race || sentLooks->female != looks->female || sentLooks->worn != looks->worn;
					sentLooks = looks;
					++looksSent;
					if (body) {
						logs::info("Session: sent our looks (race {:08X}, {}, {} worn items)", looks->race, looks->female ? "female" : "male", looks->worn.size());
					}
				}
			}
			// Our face, on its own timer (a change of hands starts the looks early, never this): the first of the session at
			// once, a change once two reads a second apart agree, and nothing while a face editor is open, then the face as
			// it was left once it closes (FaceRules::Settle).
			if (a_now >= nextFace) {
				nextFace = a_now + FACE_EVERY;
				if (const auto read = Face::Read()) {
					const bool editing = Face::Editing();
					if (editing && !faceSettle.Editing()) {
						logs::info("Session: a face editor is open: our face is sent when it closes");
					}
					if (const auto face = faceSettle.Next(*read, sentFace, editing)) {
						Send(Protocol::Encode(*face), true);
						sentFace = face;
						++facesSent;
						Face::Sent(*face);  // which says it in the log
					}
				}
			}
			// What our player did since the last tick: reliable, in order, so a swing is never played without its end.
			for (auto actions = PlayerActions::Take(); !actions.empty(); actions = PlayerActions::Take()) {
				actionsSent += actions.size();
				Send(Protocol::Encode(Protocol::PlayerActions{ std::move(actions) }), true);
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
		Shots::Clear();   // our launches ended while the stand-ins and copies that shot them are still there
		Riding::Clear();  // riders off their mounts before the stand-ins and copies go
		NpcSync::Clear();
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
			PlayerActions::Sample();  // what the game shows our player doing, for the rig (dev `actions` seen=)
			Water::Sample();          // our player in and out of the water, for the rig (dev `swim`)
			SendOurs(Clock::now());
			NpcSync::Frame([](std::vector<std::uint8_t> a_data, bool a_reliable) { Send(std::move(a_data), a_reliable); });
			Riding::Frame([](std::vector<std::uint8_t> a_data, bool a_reliable) { Send(std::move(a_data), a_reliable); });
			Shots::Frame([](std::vector<std::uint8_t> a_data, bool a_reliable) { Send(std::move(a_data), a_reliable); });
		}
		RemotePlayers::Frame();
	}

	bool Active()
	{
		return role != Role::kNone;
	}

	std::string Forms(std::string_view a_args)
	{
		const auto query = FormsCommand::Parse(a_args);
		const auto ours = [] {
			const auto plugins = LoadOrder::Read();
			return FormIds::Table::Of(Protocol::ComparedLoadOrder(plugins.full, plugins.light));
		};
		switch (query.kind) {
		case FormsCommand::Kind::kStatus:
			return std::format("identity={} table={} ours={} oursOnly={} theirsOnly={} translated={} skipped={} placeless={} plugins={}", forms.Identity() ? 1 : 0,
				tableSize, ourOrder.empty() ? 0 : FormIds::Table::Of(ourOrder).Size(), forms.OursOnly(), forms.SessionsOnly(), formsTranslated, formsSkipped,
				formsPlaceless, skippedPlugins.Count());
		case FormsCommand::Kind::kId:
			return FormsCommand::DescribeId(forms, ours(), query.number);
		case FormsCommand::Kind::kFind:
			return FormsCommand::DescribeFind(ours(), query.plugin, query.number);
		case FormsCommand::Kind::kPlace:
			return FormsCommand::DescribePlace(ours(), query.plugin);
		case FormsCommand::Kind::kArmor:
			if (const auto id = LoadOrder::FirstPlayableArmor(query.plugin)) {
				return std::format("{} armor=1", FormsCommand::DescribeId(forms, ours(), *id));
			}
			return std::format("armor=0 plugin={}", FormsCommand::Shown(query.plugin));
		case FormsCommand::Kind::kBad:
			break;
		}
		return std::string{ FormsCommand::USAGE };
	}

	std::uint32_t SessionFaceHash(const Protocol::PlayerFace& a_face)
	{
		const auto bytes = Protocol::Encode(a_face);
		const auto result = FormTranslate::Translate(forms, FormTranslate::Way::kToSession, bytes);
		const auto face = Protocol::DecodePlayerFace(result.kind == FormTranslate::Result::Kind::kChanged ? std::span<const std::uint8_t>{ result.packet } : std::span<const std::uint8_t>{ bytes });
		return face ? FaceRules::Hash(*face) : 0;
	}

	std::string Describe()
	{
		const auto status = client ? client->GetStatus() : ClientConnection::Status::kDisconnected;
		return std::format("role={} connection={} welcomed={} player={} host={} players={} sent={} received={} looksSent={} facesSent={} actionsSent={} rtt={} serverPlayers={} lastEnd={}",
			role == Role::kHost ? "host" : role == Role::kFriend ? "friend" : "none",
			status == ClientConnection::Status::kConnected ? "up" : status == ClientConnection::Status::kConnecting ? "connecting" : "down",
			welcomed ? 1 : 0, playerId, hostId, RemotePlayers::Count(), statesSent, statesIn, looksSent, facesSent, actionsSent, rttMs < 0 ? std::string{ "?" } : std::format("{:.0f}", rttMs),
			server ? std::to_string(server->Players()) : std::string{ "-" }, lastEnd);
	}
}
