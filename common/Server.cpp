#include "Server.h"

#include "EnetTransport.h"
#include "FormIds.h"
#include "Name.h"
#include "NpcBook.h"
#include "Redact.h"
#include "Text.h"
#include "TokenBucket.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	using Clock = std::chrono::steady_clock;
	using MT = Protocol::MessageType;

	// A connection, known by the transport it came in on and that transport's id for it.
	using PeerKey = std::pair<std::size_t, PeerId>;

	struct Peer
	{
		Clock::time_point connectedAt;
		bool              joined = false;
		std::uint32_t     playerId = 0;
		std::string       name;
		std::uint64_t     identity = 0;
		bool              anyState = false;
		std::uint32_t     lastSequence = 0;
		// The player's newest looks, for whoever joins after (none until the first come). Always the newest: looks are
		// state, and the last of a burst is the one that counts.
		std::optional<Protocol::PlayerLooks> looks;
		// Looks passed on: a burst of ten, then one a second (a game sends them when its player changes clothes). Past
		// it, the newest are kept and owed: passed on from the server's loop once the budget has room.
		TokenBucket looksBudget{ 10.0, 1.0 };
		bool        looksOwed = false;
		// Heartbeats answered: a burst of twenty, then ten a second (the game sends a few a second at most).
		TokenBucket heartbeats{ 20.0, 10.0 };
		bool        toldUnknown = false;
	};

	// How a joiner's load order differs from the session's table, for the log: counts only. A plugin's name is the
	// player's own business (a host can name one after themselves), and the plugins that differ are named in each game's
	// own log (src/net/Session.cpp), where they are that player's. Names are looked up, not searched for: each list may
	// hold thousands (Protocol::MAX_PLUGINS), and any game that connects can send one.
	std::string LoadOrderDifference(const std::vector<std::string>& a_table, const std::vector<std::string>& a_joiner)
	{
		const auto table = FormIds::Table::Of(a_table);
		const auto joiner = FormIds::Table::Of(a_joiner);
		const auto missing = [](const FormIds::Table& a_from, const FormIds::Table& a_in) {
			std::size_t count = 0;
			for (const auto* kind : { &a_from.full, &a_from.light }) {
				count += static_cast<std::size_t>(std::ranges::count_if(*kind, [&](const std::string& a_name) { return !a_in.Find(a_name); }));
			}
			return count;
		};
		const auto theirs = missing(joiner, table);
		const auto lacking = missing(table, joiner);
		if (theirs == 0 && lacking == 0) {
			return "the same plugins, in another order";
		}
		return std::format("{} plugins the session has not, {} of the session's it lacks", theirs, lacking);
	}
}

// Every line goes through Redact::Scrub. The lines name players by number and mask addresses already, but a
// transport's description or a plugin's name (a host can name one after a SteamID64) is not the server's to vouch
// for, and the dedicated server prints its lines as they come.
Server::Server(LogFn a_log) :
	log([a_log = std::move(a_log)](std::string_view a_line) { a_log(Redact::Scrub(a_line)); })
{}

Server::~Server()
{
	Stop();
}

bool Server::Start(const Options& a_options, std::vector<std::unique_ptr<ServerTransport>> a_extraTransports)
{
	if (running) {
		return true;
	}

	options = a_options;
	options.maxPlayers = std::clamp<std::size_t>(options.maxPlayers, 1, Protocol::MAX_PLAYERS);

	// Allow a couple of extra connections so full servers can still send a rejection.
	std::string error;
	auto        enet = EnetServerTransport::Create(options.port, options.maxPlayers + 2, options.udpLoopbackOnly, error);
	if (!enet) {
		log("server: " + error);
		return false;
	}
	transports.clear();
	transports.push_back(std::move(enet));
	for (auto& transport : a_extraTransports) {
		transports.push_back(std::move(transport));
	}

	stopRequested = false;
	players = 0;
	running = true;
	thread = std::thread([this] { Run(); });
	log(std::format("server: listening on UDP port {}{} (max {} players, {} transports, protocol {}{})", options.port,
		options.udpLoopbackOnly ? " (this machine only)" : "", options.maxPlayers, transports.size(), Protocol::VERSION,
		options.password.empty() ? "" : ", password set"));
	return true;
}

void Server::Stop()
{
	if (!running) {
		return;
	}
	stopRequested = true;
	if (thread.joinable()) {
		thread.join();
	}
	transports.clear();
	players = 0;
	running = false;
	log("server: stopped");
}

void Server::Run()
{
	std::map<PeerKey, Peer> peers;
	// A player who comes back (the same Hello::identity) gets the same player id for this server's run.
	std::unordered_map<std::uint64_t, std::uint32_t> knownIds;
	std::uint32_t                                    nextPlayerId = 1;
	std::uint32_t                                    hostId = 0;
	// The session's load order: the first player's to join (its hash), and the session's plugin table, which numbers
	// every form ID on the wire (Protocol::PluginTable, common/FormIds.h): that load order with every later joiner's
	// other plugins added after it. A joiner with another load order is let in when both lists are known: their game
	// translates its form IDs by plugin name. Without a list (a tool's hello, or one too long to carry) nothing can
	// translate theirs, so they must have the session's load order.
	std::optional<std::uint32_t> sessionHash;
	std::vector<std::string>     sessionTable;
	// Who runs each NPC (common/NpcBook.h), and the last gear each runner reported of its NPCs, for whoever joins after.
	// Only an NPC's runner's states, events and gear are passed on.
	NpcBook::Book                                  npcs;
	std::map<Protocol::NpcKey, Protocol::NpcGear> npcGear;

	std::random_device random;
	const auto         sessionId = (std::uint64_t{ random() } << 32) | random();

	const auto send = [&](const PeerKey& a_to, const std::vector<std::uint8_t>& a_packet, bool a_reliable) {
		transports[a_to.first]->Send(a_to.second, a_packet, a_reliable);
	};
	const auto broadcast = [&](const std::vector<std::uint8_t>& a_packet, bool a_reliable, const PeerKey* a_except) {
		for (const auto& [key, peer] : peers) {
			if (peer.joined && (!a_except || key != *a_except)) {
				send(key, a_packet, a_reliable);
			}
		}
	};
	const auto countPlayers = [&] {
		return static_cast<std::size_t>(std::ranges::count_if(peers, [](const auto& a_entry) { return a_entry.second.joined; }));
	};
	const auto refuse = [&](const PeerKey& a_key, std::string a_reason, std::string_view a_logged) {
		log(std::format("server: refused {}: {}", transports[a_key.first]->Describe(a_key.second), a_logged));
		send(a_key, Protocol::Encode(Protocol::Reject{ std::move(a_reason) }), true);
		transports[a_key.first]->Disconnect(a_key.second);
	};
	// A joined player is gone: the others hear it.
	const auto left = [&](const PeerKey& a_key, const Peer& a_peer, std::string_view a_why) {
		log(std::format("server: player {} left ({})", a_peer.playerId, a_why));
		broadcast(Protocol::Encode(Protocol::PlayerLeft{ a_peer.playerId }), true, &a_key);
		if (a_peer.playerId == hostId) {
			hostId = 0;
		}
		// The NPCs their game ran are nobody's now: the games that have them loaded claim them again. Their spawns go.
		if (const auto released = npcs.Leave(a_peer.playerId); !released.empty()) {
			Protocol::NpcOwners told;
			for (const auto& key : released) {
				told.owners.push_back({ key, 0 });
				npcGear.erase(key);
			}
			broadcast(Protocol::Encode(told), true, &a_key);
			log(std::format("server: the {} NPC(s) player {} ran are nobody's now", released.size(), a_peer.playerId));
		}
	};

	const auto onHello = [&](const PeerKey& a_key, Peer& a_peer, std::span<const std::uint8_t> a_data) {
		if (a_peer.joined) {
			return;
		}
		const auto hello = Protocol::DecodeHello(a_data);
		if (!hello) {
			refuse(a_key, Text::GetEnglish("reject.unreadable"), "a hello that could not be read");
			return;
		}
		if (hello->magic != Protocol::MAGIC) {
			refuse(a_key, Text::GetEnglish("reject.not_this_mod"), "not this mod's hello");
			return;
		}
		if (hello->version != Protocol::VERSION) {
			refuse(a_key, Text::FormatEnglish("reject.version", { { "you", hello->version }, { "host", Protocol::VERSION } }),
				std::format("protocol {} (ours is {})", hello->version, Protocol::VERSION));
			return;
		}
		if (!options.password.empty() && hello->password != options.password) {
			refuse(a_key, Text::GetEnglish("reject.password"), "wrong password");
			return;
		}
		// Someone coming back while their old connection still stands (it has not timed out yet) takes its place.
		std::optional<PeerKey> replaced;
		if (hello->identity != 0) {
			for (const auto& [key, peer] : peers) {
				if (peer.joined && peer.identity == hello->identity) {
					replaced = key;
					break;
				}
			}
		}
		if (!replaced && countPlayers() >= options.maxPlayers) {
			refuse(a_key, Text::FormatEnglish("reject.full", { { "n", options.maxPlayers } }), "the session is full");
			return;
		}
		const bool othersIn = countPlayers() > (replaced ? 1u : 0u);
		const bool otherOrder = othersIn && sessionHash && *sessionHash != hello->contentHash;
		if (otherOrder && (sessionTable.empty() || hello->plugins.empty())) {
			refuse(a_key, Text::GetEnglish("reject.mods"), "another load order, and no list of it to translate by");
			return;
		}

		if (replaced) {
			const auto old = peers.find(*replaced);
			left(*replaced, old->second, "joined again on a new connection");
			transports[replaced->first]->Disconnect(replaced->second);
			peers.erase(old);
		}
		if (!othersIn) {
			sessionHash = hello->contentHash;
			sessionTable.clear();  // the first player's load order numbers the new session
		} else if (otherOrder) {
			// Not their name, and no player number yet: where they come from (masked) says which joiner it was.
			log(std::format("server: {} has another load order ({}); its form IDs are translated by plugin name", transports[a_key.first]->Describe(a_key.second),
				LoadOrderDifference(sessionTable, hello->plugins)));
		}
		// Their plugins the table lacks get places after the rest of their kind (an ID it gave stays what it was). The
		// players already in hear the grown table first, on the reliable channel, ahead of anything numbered by it; the
		// joiner hears it before the welcome. A state on the state channel can still come before it: one that names a
		// place of a plugin new to the table reads as nowhere there, once.
		if (const auto added = FormIds::Extend(sessionTable, hello->plugins); added != 0) {
			log(std::format("server: the session's plugin table has {} plugins ({} new)", FormIds::Table::Of(sessionTable).Size(), added));
			if (othersIn) {
				broadcast(Protocol::Encode(Protocol::PluginTable{ sessionTable }), true, &a_key);
			}
		}
		if (!sessionTable.empty()) {
			send(a_key, Protocol::Encode(Protocol::PluginTable{ sessionTable }), true);
		}

		// a_peer is still valid: std::map keeps its other entries where they are.
		if (const auto known = hello->identity != 0 ? knownIds.find(hello->identity) : knownIds.end(); known != knownIds.end()) {
			a_peer.playerId = known->second;
		} else {
			a_peer.playerId = nextPlayerId++;
			if (hello->identity != 0 && knownIds.size() < 4096) {
				knownIds.emplace(hello->identity, a_peer.playerId);
			}
		}
		a_peer.joined = true;
		a_peer.identity = hello->identity;
		a_peer.name = Name::Shown(hello->name, a_peer.playerId);
		a_peer.anyState = false;
		if (options.hostIdentity != 0 && hello->identity == options.hostIdentity && transports[a_key.first]->IsLocal(a_key.second)) {
			hostId = a_peer.playerId;
		}

		send(a_key, Protocol::Encode(Protocol::Welcome{ a_peer.playerId, sessionId, hostId }), true);
		for (const auto& [key, peer] : peers) {
			if (peer.joined && key != a_key) {
				send(a_key, Protocol::Encode(Protocol::PlayerJoined{ peer.playerId, peer.name }), true);
				if (peer.looks) {
					send(a_key, Protocol::Encode(Protocol::RemoteLooks{ peer.playerId, *peer.looks }), true);
				}
			}
		}
		// Who runs each NPC (a full list: a player back on a new connection forgets what they knew), and their gear.
		send(a_key, Protocol::Encode(Protocol::NpcOwners{ .full = true, .owners = npcs.All() }), true);
		for (const auto& [npc, gear] : npcGear) {
			send(a_key, Protocol::Encode(gear), true);
		}
		broadcast(Protocol::Encode(Protocol::PlayerJoined{ a_peer.playerId, a_peer.name }), true, &a_key);
		players = countPlayers();
		log(std::format("server: player {} joined from {}{} ({} in the session)", a_peer.playerId, transports[a_key.first]->Describe(a_key.second),
			a_peer.playerId == hostId ? ", the host" : "", countPlayers()));
	};

	const auto onPacket = [&](const PeerKey& a_key, Peer& a_peer, std::span<const std::uint8_t> a_data) {
		const auto type = Protocol::PeekType(a_data);
		if (!type) {
			return;
		}
		if (*type == MT::kHello) {
			onHello(a_key, a_peer, a_data);
			return;
		}
		if (!a_peer.joined) {
			return;  // nothing is heard from a connection before its hello
		}
		switch (*type) {
		case MT::kPlayerState:
			if (const auto state = Protocol::DecodePlayerState(a_data)) {
				// Older than the last one passed on (the state channel can reorder): dropped. The difference is
				// taken as signed, so the sequence may wrap.
				if (a_peer.anyState && static_cast<std::int32_t>(state->sequence - a_peer.lastSequence) <= 0) {
					break;
				}
				a_peer.anyState = true;
				a_peer.lastSequence = state->sequence;
				broadcast(Protocol::Encode(Protocol::RemoteState{ a_peer.playerId, *state }), false, &a_key);
			}
			break;
		case MT::kPlayerLooks:
			// Kept for whoever joins later, and passed on to those there now. Reliable, and on the reliable channel, so
			// it comes after the join it belongs to.
			if (auto looks = Protocol::DecodePlayerLooks(a_data)) {
				if (a_peer.looks && *a_peer.looks == *looks) {
					break;  // the same again: nothing for the others, and no budget spent
				}
				a_peer.looks = std::move(*looks);
				if (a_peer.looksBudget.Take(Clock::now())) {
					a_peer.looksOwed = false;
					broadcast(Protocol::Encode(Protocol::RemoteLooks{ a_peer.playerId, *a_peer.looks }), true, &a_key);
				} else {
					a_peer.looksOwed = true;  // the newest goes once the budget allows
				}
			}
			break;
		case MT::kNpcClaims:
			// Each claim as NpcBook rules: everyone hears who runs an NPC that changed hands (the claimant too: its
			// answer), and a refused claimant hears who runs it instead.
			if (const auto msg = Protocol::DecodeNpcClaims(a_data)) {
				const auto          now = Clock::now();
				Protocol::NpcOwners changes;
				Protocol::NpcOwners refused;
				for (const auto& claim : msg->claims) {
					switch (npcs.Claim(a_peer.playerId, claim, now)) {
					case NpcBook::Outcome::kGranted:
						changes.owners.push_back({ claim.key, a_peer.playerId });
						npcGear.erase(claim.key);  // the new runner reports its own
						break;
					case NpcBook::Outcome::kReleased:
						changes.owners.push_back({ claim.key, 0 });
						npcGear.erase(claim.key);
						break;
					case NpcBook::Outcome::kRefused:
						refused.owners.push_back({ claim.key, npcs.Owner(claim.key) });
						break;
					case NpcBook::Outcome::kKept:
					case NpcBook::Outcome::kIgnored:
						break;
					}
				}
				if (!changes.owners.empty()) {
					broadcast(Protocol::Encode(changes), true, nullptr);
				}
				if (!refused.owners.empty()) {
					send(a_key, Protocol::Encode(refused), true);
				}
			}
			break;
		case MT::kNpcStates:
			// Only the states of NPCs the sender runs, the sender named as their runner.
			if (auto msg = Protocol::DecodeNpcStates(a_data)) {
				std::erase_if(msg->npcs, [&](const Protocol::NpcState& a_npc) { return !npcs.Runs(a_peer.playerId, a_npc.key); });
				if (!msg->npcs.empty()) {
					msg->runner = a_peer.playerId;
					broadcast(Protocol::Encode(*msg), false, &a_key);
				}
			}
			break;
		case MT::kNpcActions:
			if (auto msg = Protocol::DecodeNpcActions(a_data)) {
				std::erase_if(msg->actions, [&](const Protocol::NpcAction& a_action) { return !npcs.Runs(a_peer.playerId, a_action.key); });
				if (!msg->actions.empty()) {
					msg->runner = a_peer.playerId;
					broadcast(Protocol::Encode(*msg), true, &a_key);
				}
			}
			break;
		case MT::kNpcGear:
			// Kept for whoever joins later while it has this runner.
			if (auto msg = Protocol::DecodeNpcGear(a_data); msg && npcs.Runs(a_peer.playerId, msg->key)) {
				msg->runner = a_peer.playerId;
				npcGear[msg->key] = *msg;
				broadcast(Protocol::Encode(*msg), true, &a_key);
			}
			break;
		case MT::kHeartbeat:
			if (const auto beat = Protocol::DecodeHeartbeat(a_data); beat && a_peer.heartbeats.Take(Clock::now())) {
				// Unreliable, as the heartbeat itself is sent: a resend would measure the resend, not the connection.
				send(a_key, Protocol::Encode(*beat, MT::kHeartbeatAck), false);
			}
			break;
		default:
			// A message only the server sends, or one this version does not know. Said once for each connection.
			if (!a_peer.toldUnknown) {
				a_peer.toldUnknown = true;
				log(std::format("server: player {} sent message type {}, which the server does not take", a_peer.playerId, static_cast<unsigned>(*type)));
			}
			break;
		}
	};

	std::vector<TransportEvent> events;
	while (!stopRequested) {
		for (std::size_t index = 0; index < transports.size(); ++index) {
			events.clear();
			// Only the first transport waits (a few ms, so the thread does not spin); the others are asked in passing.
			transports[index]->Poll(events, index == 0 ? 5 : 0);
			for (auto& event : events) {
				const PeerKey key{ index, event.peer };
				switch (event.type) {
				case TransportEvent::Type::kConnect:
					peers[key] = Peer{ .connectedAt = Clock::now() };
					break;
				case TransportEvent::Type::kDisconnect:
					if (const auto it = peers.find(key); it != peers.end()) {
						const auto peer = std::move(it->second);
						peers.erase(it);
						if (peer.joined) {
							left(key, peer, "disconnected");
							players = countPlayers();
						}
					}
					break;
				case TransportEvent::Type::kPacket:
					if (const auto it = peers.find(key); it != peers.end()) {
						onPacket(key, it->second, event.data);
					}
					break;
				}
			}
		}

		// A connection that never says hello holds one of the few spare slots: dropped after a while. Its kDisconnect
		// event removes it; until then it is not asked again. And looks held back by the budget go once it has room.
		const auto now = Clock::now();
		for (auto& [key, peer] : peers) {
			if (peer.joined && peer.looksOwed && peer.looks && peer.looksBudget.Take(now)) {
				peer.looksOwed = false;
				broadcast(Protocol::Encode(Protocol::RemoteLooks{ peer.playerId, *peer.looks }), true, &key);
			}
			if (!peer.joined && peer.connectedAt != Clock::time_point::max() && now - peer.connectedAt > options.helloTimeout) {
				log(std::format("server: dropped {}: no hello", transports[key.first]->Describe(key.second)));
				transports[key.first]->Disconnect(key.second);
				peer.connectedAt = Clock::time_point::max();
			}
		}
	}

	for (const auto& transport : transports) {
		transport->Close();
	}
}
