#include "Server.h"

#include "EnetTransport.h"
#include "FormIds.h"
#include "Name.h"
#include "NpcBook.h"
#include "NpcFates.h"
#include "Redact.h"
#include "RideBook.h"
#include "SitBook.h"
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
		// Looks passed on: a burst of ten, then four a second (a game sends them when its player changes clothes, or
		// what a hand holds: a sword for a bow, a spell for a torch). Past it, the newest are kept and owed: passed on from
		// the server's loop once the budget has room.
		TokenBucket looksBudget{ 10.0, 4.0 };
		bool        looksOwed = false;
		// The player's newest face, kept and passed on like the looks: a game sends it once it settles after a change (a
		// barber, a mirror, a race menu), so a burst of four, then one a second, and the newest owed past that.
		std::optional<Protocol::PlayerFace> face;
		TokenBucket                         faceBudget{ 4.0, 1.0 };
		bool                                faceOwed = false;
		// Heartbeats answered: a burst of twenty, then ten a second (the game sends a few a second at most).
		TokenBucket heartbeats{ 20.0, 10.0 };
		// Hits on copies sent on (NpcFates.h): a burst of 64, then 20 a second (a flurry of daggers, a flame spell's
		// ticks on a few copies). Past it, a hit is answered kBusy, never dropped: the hitter gives the drop back.
		TokenBucket hitBudget{ 64.0, 20.0 };
		// A player's graph events passed on, each one: a burst of 96, then 48 a second (a player's fight sends a few a
		// second: a swing is two or three, a cast two). Past it, an event is dropped, never kept: a stand-in shows
		// the next one.
		TokenBucket actionBudget{ 96.0, 48.0 };
		bool        toldActionsDropped = false;
		// Ride changes passed on (common/RideBook.h): a burst of 64, then 16 a second (a player gets on or off a horse now
		// and then, the NPCs a game runs a few at a time). Past it a change is kept and owed, never dropped.
		TokenBucket rideBudget{ 64.0, 16.0 };
		// Furniture changes passed on (common/SitBook.h): a burst of 64, then 16 a second (a player sits or gets up now and
		// then; an inn's NPCs sit and rise a few at a time). Past it a change is kept and owed, never dropped.
		TokenBucket sitBudget{ 64.0, 16.0 };
		// Projectiles passed on (kShots): a burst of 96, then 48 a second (a player's bow or spell sends one a second or
		// two, a concentration spell about ten while held, and the NPCs a game runs a few more in a fight). Past it a shot
		// is dropped, never kept: its flight is over by the time a budget would let it go.
		TokenBucket shotBudget{ 96.0, 48.0 };
		bool        toldShotsDropped = false;
		// Paired animations passed on (kPairs): a burst of 16, then 8 a second (a kill move takes seconds; a fight's NPCs
		// begin a few at a time). Past it one is dropped, never kept: the pair is over by the time a budget would let it go.
		TokenBucket pairBudget{ 16.0, 8.0 };
		bool        toldPairsDropped = false;
		// Its player's down (kDowned): the last said, kept while it waits for help or gets up, for a joiner. Downs said
		// pass a burst of 8, then 2 a second (a change now and then, a wait's seconds every 5 s); helps asked a burst of 4,
		// then 1 a second (one per downed friend every 3 s). Past either, dropped (the next says it again), said once.
		std::optional<Protocol::Downed> downed;
		TokenBucket                     downBudget{ 8.0, 2.0 };
		TokenBucket                     helpBudget{ 4.0, 1.0 };
		bool                            toldDownsDropped = false;
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
	// The health of the NPCs run and those dead (common/NpcFates.h), for whoever joins after, and where each hit on a copy goes.
	NpcFates::Book fates;
	// Who rides what (common/RideBook.h), for whoever joins after.
	RideBook::Book rides;
	// Who is in what furniture (common/SitBook.h), for whoever joins after.
	SitBook::Book sits;

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
	// The connection of a player in the session (none: not in it).
	const auto peerOf = [&](std::uint32_t a_playerId) -> std::optional<PeerKey> {
		for (const auto& [key, peer] : peers) {
			if (peer.joined && peer.playerId == a_playerId) {
				return key;
			}
		}
		return std::nullopt;
	};
	const auto countPlayers = [&] {
		return static_cast<std::size_t>(std::ranges::count_if(peers, [](const auto& a_entry) { return a_entry.second.joined; }));
	};
	const auto refuse = [&](const PeerKey& a_key, std::string a_reason, std::string_view a_logged) {
		log(std::format("server: refused {}: {}", transports[a_key.first]->Describe(a_key.second), a_logged));
		send(a_key, Protocol::Encode(Protocol::Reject{ std::move(a_reason) }), true);
		transports[a_key.first]->Disconnect(a_key.second);
	};
	// Rides passed on in messages of at most MAX_RIDES each: to one player (a_to), or to everyone but a_except. a_full: a
	// joiner's list (its first message says so). a_sender: the game that said them (0: the server's own word).
	const auto sendRides = [&](const PeerKey* a_to, const PeerKey* a_except, std::vector<Protocol::Ride> a_rides, bool a_full, std::uint32_t a_sender = 0) {
		for (std::size_t first = 0; first < a_rides.size(); first += Protocol::MAX_RIDES) {
			Protocol::Rides msg{ .sender = a_sender, .full = a_full && first == 0 };
			const auto      end = (std::min)(a_rides.size(), first + Protocol::MAX_RIDES);
			msg.rides.assign(a_rides.begin() + static_cast<std::ptrdiff_t>(first), a_rides.begin() + static_cast<std::ptrdiff_t>(end));
			if (a_to) {
				send(*a_to, Protocol::Encode(msg), true);
			} else {
				broadcast(Protocol::Encode(msg), true, a_except);
			}
		}
	};
	// The same for who is in what furniture (MAX_SITS each).
	const auto sendSits = [&](const PeerKey* a_to, const PeerKey* a_except, std::vector<Protocol::Sit> a_sits, bool a_full, std::uint32_t a_sender = 0) {
		for (std::size_t first = 0; first < a_sits.size(); first += Protocol::MAX_SITS) {
			Protocol::Sits msg{ .sender = a_sender, .full = a_full && first == 0 };
			const auto     end = (std::min)(a_sits.size(), first + Protocol::MAX_SITS);
			msg.sits.assign(a_sits.begin() + static_cast<std::ptrdiff_t>(first), a_sits.begin() + static_cast<std::ptrdiff_t>(end));
			if (a_to) {
				send(*a_to, Protocol::Encode(msg), true);
			} else {
				broadcast(Protocol::Encode(msg), true, a_except);
			}
		}
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
				fates.Released(key);
			}
			broadcast(Protocol::Encode(told), true, &a_key);
			log(std::format("server: the {} NPC(s) player {} ran are nobody's now", released.size(), a_peer.playerId));
		}
		fates.Left(a_peer.playerId);
		// Their ride, and those of the NPCs their game ran, end: the others get the riders off.
		if (auto ended = rides.Leave(a_peer.playerId); !ended.empty()) {
			sendRides(nullptr, &a_key, std::move(ended), false);
		}
		// And whatever furniture they and their NPCs were in: the others stand them up.
		if (auto ended = sits.Leave(a_peer.playerId); !ended.empty()) {
			sendSits(nullptr, &a_key, std::move(ended), false);
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
			fates.Clear();         // and the deaths and wounds of the last one are not this one's
			rides.Clear();         // nor its rides
			sits.Clear();          // nor who sat where
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
				if (peer.face) {
					send(a_key, Protocol::Encode(Protocol::RemoteFace{ peer.playerId, *peer.face }), true);
				}
			}
		}
		// Who runs each NPC (a full list; a game forgets what it knew at each welcome), and their gear. Nothing when no NPC
		// is run: there is nothing to forget.
		if (auto all = npcs.All(); !all.empty()) {
			send(a_key, Protocol::Encode(Protocol::NpcOwners{ .full = true, .owners = std::move(all) }), true);
		}
		for (const auto& [npc, gear] : npcGear) {
			send(a_key, Protocol::Encode(gear), true);
		}
		// The health of the NPCs run now and those the session has dead (NpcFates.h): a joiner's copies show them so (one
		// its save has dead is raised), and its game kills the dead ones it has alive. Nothing when there are none.
		if (auto said = fates.Healths(); !said.empty()) {
			send(a_key, Protocol::Encode(Protocol::NpcHealths{ .npcs = std::move(said) }), true);
		}
		if (auto dead = fates.Dead(); !dead.empty()) {
			send(a_key, Protocol::Encode(Protocol::NpcDeaths{ .full = true, .deaths = std::move(dead) }), true);
		}
		// Who rides what (after the owners: an NPC rider's ride is read against its runner).
		if (auto all = rides.All(); !all.empty()) {
			sendRides(&a_key, nullptr, std::move(all), true);
		}
		// Who is in what furniture (after the owners, as the rides).
		if (auto all = sits.All(); !all.empty()) {
			sendSits(&a_key, nullptr, std::move(all), true);
		}
		// Who is down now (a joiner's game kneels their stand-ins and may help them up).
		for (const auto& [key, other] : peers) {
			if (other.joined && key != a_key && other.downed) {
				send(a_key, Protocol::Encode(*other.downed), true);
			}
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
		case MT::kPlayerFace:
			// As the looks: kept for whoever joins later, passed on to those there now, the same again ignored.
			if (auto face = Protocol::DecodePlayerFace(a_data)) {
				if (a_peer.face && *a_peer.face == *face) {
					break;
				}
				a_peer.face = std::move(*face);
				if (a_peer.faceBudget.Take(Clock::now())) {
					a_peer.faceOwed = false;
					broadcast(Protocol::Encode(Protocol::RemoteFace{ a_peer.playerId, *a_peer.face }), true, &a_key);
				} else {
					a_peer.faceOwed = true;
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
					// Who ran it: a death in that game a moment after it lost the NPC still counts (NpcFates::DEATH_GRACE).
					const auto was = npcs.Owner(claim.key);
					switch (npcs.Claim(a_peer.playerId, claim, now)) {
					case NpcBook::Outcome::kGranted:
						changes.owners.push_back({ claim.key, a_peer.playerId });
						npcGear.erase(claim.key);  // the new runner reports its own
						if (was != a_peer.playerId) {
							fates.HandedOver(claim.key, was, now);
						}
						break;
					case NpcBook::Outcome::kReleased:
						changes.owners.push_back({ claim.key, 0 });
						npcGear.erase(claim.key);
						fates.Released(claim.key);
						fates.HandedOver(claim.key, was, now);
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
		case MT::kNpcHits:
			// Each hit to its NPC's runner alone, by the server's own book (NpcFates.h), the hitter named; one that can go
			// nowhere (nobody runs it, the hitter does, the session has it dead, past the hitter's budget) is answered here.
			if (const auto msg = Protocol::DecodeNpcHits(a_data)) {
				const auto                                  now = Clock::now();
				Protocol::NpcHitAnswers                     answers;
				std::map<std::uint32_t, Protocol::NpcHits> toRunners;
				for (const auto& hit : msg->hits) {
					auto route = fates.RouteHit(a_peer.playerId, npcs.Owner(hit.key), hit);
					if (!route.refused && !peerOf(route.runner)) {
						route.refused = Protocol::HitVerdict::kNobody;
					}
					if (!route.refused && !a_peer.hitBudget.Take(now)) {
						route.refused = Protocol::HitVerdict::kBusy;
					}
					if (route.refused) {
						answers.answers.push_back({ hit.key, a_peer.playerId, hit.seq, *route.refused, 0.0f });
						continue;
					}
					auto& out = toRunners[route.runner];
					out.hitter = a_peer.playerId;
					out.hits.push_back(hit);
				}
				for (const auto& [runner, hits] : toRunners) {
					if (const auto to = peerOf(runner)) {
						send(*to, Protocol::Encode(hits), true);
					}
				}
				if (!answers.answers.empty()) {
					send(a_key, Protocol::Encode(answers), true);
				}
			}
			break;
		case MT::kNpcHitAnswers:
			// A runner's answers, each to its hitter alone.
			if (const auto msg = Protocol::DecodeNpcHitAnswers(a_data)) {
				std::map<std::uint32_t, Protocol::NpcHitAnswers> toHitters;
				for (const auto& answer : msg->answers) {
					if (answer.hitter != a_peer.playerId) {
						toHitters[answer.hitter].answers.push_back(answer);
					}
				}
				for (const auto& [hitter, answers] : toHitters) {
					if (const auto to = peerOf(hitter)) {
						send(*to, Protocol::Encode(answers), true);
					}
				}
			}
			break;
		case MT::kNpcHealths:
			// Only a runner's word of its own NPCs, named as its; one it says lives that the session had dead is back.
			if (auto msg = Protocol::DecodeNpcHealths(a_data)) {
				// One said alive just after its last runner's death reached us (NpcFates::Health::kStale) is dropped.
				const auto  now = Clock::now();
				std::size_t revived = 0;
				std::erase_if(msg->npcs, [&](const Protocol::NpcHealth& a_npc) {
					const auto taken = fates.Said(a_peer.playerId, npcs.Owner(a_npc.key), a_npc, now);
					revived += taken == NpcFates::Health::kRevived ? 1 : 0;
					return taken == NpcFates::Health::kRefused || taken == NpcFates::Health::kStale;
				});
				if (revived != 0) {
					log(std::format("server: player {} says {} NPC(s) the session had dead live again", a_peer.playerId, revived));
				}
				if (!msg->npcs.empty()) {
					msg->runner = a_peer.playerId;
					broadcast(Protocol::Encode(*msg), true, &a_key);
				}
			}
			break;
		case MT::kNpcDeaths:
			// Only a runner's word, once for each NPC: no game kills one twice. A death from the game that ran it a moment
			// ago (NpcFates::Death::kLate: it died there before that game heard it lost it) goes to every game as the
			// server's word, that game too: its body is a copy now, and the new runner kills the NPC it has alive.
			if (auto msg = Protocol::DecodeNpcDeaths(a_data); msg && !msg->full) {
				const auto          now = Clock::now();
				Protocol::NpcDeaths late;
				std::erase_if(msg->deaths, [&](const Protocol::NpcDeath& a_death) {
					const auto taken = fates.Died(a_peer.playerId, npcs.Owner(a_death.key), a_death, now);
					if (taken == NpcFates::Death::kLate) {
						late.deaths.push_back(a_death);
					}
					return taken != NpcFates::Death::kNew;
				});
				if (!msg->deaths.empty()) {
					msg->runner = a_peer.playerId;
					broadcast(Protocol::Encode(*msg), true, &a_key);
				}
				if (!late.deaths.empty()) {
					log(std::format("server: player {} says {} NPC(s) it ran a moment ago died there: everyone is told", a_peer.playerId, late.deaths.size()));
					broadcast(Protocol::Encode(late), true, nullptr);
				}
			}
			break;
		case MT::kPlayerActions:
			// To the others, the sender named, each event within the sender's budget.
			if (auto msg = Protocol::DecodePlayerActions(a_data)) {
				const auto now = Clock::now();
				const auto before = msg->actions.size();
				std::erase_if(msg->actions, [&](const Protocol::PlayerAction&) { return !a_peer.actionBudget.Take(now); });
				if (msg->actions.size() < before && !a_peer.toldActionsDropped) {
					a_peer.toldActionsDropped = true;
					log(std::format("server: player {} sends graph events faster than they are passed on: the rest are dropped (said once)", a_peer.playerId));
				}
				if (!msg->actions.empty()) {
					broadcast(Protocol::Encode(Protocol::RemoteActions{ a_peer.playerId, std::move(msg->actions) }), true, &a_key);
				}
			}
			break;
		case MT::kRides:
			// A player's own ride and the rides of the NPCs its game runs (RideBook.h), each change once, the sender named;
			// past its budget a change is owed (the loop below passes it on).
			if (const auto msg = Protocol::DecodeRides(a_data)) {
				const auto                  now = Clock::now();
				std::vector<Protocol::Ride> pass;
				for (auto& ride : rides.Take(a_peer.playerId, msg->rides, [&](const Protocol::NpcKey& a_npc) { return npcs.Runs(a_peer.playerId, a_npc); })) {
					if (!rides.Owes(a_peer.playerId) && a_peer.rideBudget.Take(now)) {
						pass.push_back(ride);
					} else {
						rides.Owe(a_peer.playerId, ride);
					}
				}
				if (!pass.empty()) {
					sendRides(nullptr, &a_key, std::move(pass), false, a_peer.playerId);
				}
			}
			break;
		case MT::kSits:
			// Who is in what furniture: a player's own and the NPCs' its game runs (SitBook.h), as the rides above.
			if (const auto msg = Protocol::DecodeSits(a_data)) {
				const auto                 now = Clock::now();
				std::vector<Protocol::Sit> pass;
				for (auto& sit : sits.Take(a_peer.playerId, msg->sits, [&](const Protocol::NpcKey& a_npc) { return npcs.Runs(a_peer.playerId, a_npc); })) {
					if (!sits.Owes(a_peer.playerId) && a_peer.sitBudget.Take(now)) {
						pass.push_back(sit);
					} else {
						sits.Owe(a_peer.playerId, sit);
					}
				}
				if (!pass.empty()) {
					sendSits(nullptr, &a_key, std::move(pass), false, a_peer.playerId);
				}
			}
			break;
		case MT::kShots:
			// To the others, the sender named: a player's own shots and those of the NPCs its game runs, each within the
			// sender's budget (past it dropped, said once).
			if (auto msg = Protocol::DecodeShots(a_data)) {
				const auto now = Clock::now();
				std::erase_if(msg->shots, [&](const Protocol::Shot& a_shot) {
					return a_shot.player != 0 ? a_shot.player != a_peer.playerId : !npcs.Runs(a_peer.playerId, a_shot.npc);
				});
				const auto believed = msg->shots.size();
				std::erase_if(msg->shots, [&](const Protocol::Shot&) { return !a_peer.shotBudget.Take(now); });
				if (msg->shots.size() < believed && !a_peer.toldShotsDropped) {
					a_peer.toldShotsDropped = true;
					log(std::format("server: player {} sends shots faster than they are passed on: the rest are dropped (said once)", a_peer.playerId));
				}
				if (!msg->shots.empty()) {
					msg->sender = a_peer.playerId;
					broadcast(Protocol::Encode(*msg), true, &a_key);
				}
			}
			break;
		case MT::kPairs:
			// To the others, the sender named: a pair its own player began, or an NPC its game runs. A player on the other
			// side is the sender's own (an NPC of its game's on its player): in its game every other player is a stand-in no
			// actor fights. Each within the sender's budget (past it dropped, said once).
			if (auto msg = Protocol::DecodePairs(a_data)) {
				const auto now = Clock::now();
				const auto ours = [&](const Protocol::PairSide& a_side) {
					return a_side.player != 0 ? a_side.player == a_peer.playerId : npcs.Runs(a_peer.playerId, a_side.npc);
				};
				std::erase_if(msg->pairs, [&](const Protocol::Pair& a_pair) {
					return !ours(a_pair.attacker) || (a_pair.victim.player != 0 && a_pair.victim.player != a_peer.playerId);
				});
				const auto believed = msg->pairs.size();
				std::erase_if(msg->pairs, [&](const Protocol::Pair&) { return !a_peer.pairBudget.Take(now); });
				if (msg->pairs.size() < believed && !a_peer.toldPairsDropped) {
					a_peer.toldPairsDropped = true;
					log(std::format("server: player {} sends paired animations faster than they are passed on: the rest are dropped (said once)", a_peer.playerId));
				}
				if (!msg->pairs.empty()) {
					msg->sender = a_peer.playerId;
					broadcast(Protocol::Encode(*msg), true, &a_key);
				}
			}
			break;
		case MT::kDowned:
			// Its own player's down, the sender named, to the others; kept while it is on the ground, for a joiner. An up that
			// ends a kept down always goes (a friend's stand-in must not be left kneeling), and one that ends nothing is
			// dropped (the others have it up), so ups are bounded by downs; a down past the budget is dropped (said again).
			if (auto msg = Protocol::DecodeDowned(a_data)) {
				msg->player = a_peer.playerId;
				const bool up = msg->state == static_cast<std::uint8_t>(0);
				if (up && !a_peer.downed) {
					break;
				}
				if (!up && !a_peer.downBudget.Take(Clock::now())) {
					if (!a_peer.toldDownsDropped) {
						a_peer.toldDownsDropped = true;
						log(std::format("server: player {} says its down faster than it is passed on: the rest are dropped (said once)", a_peer.playerId));
					}
					break;
				}
				const bool was = a_peer.downed.has_value();
				a_peer.downed = up ? std::nullopt : std::optional{ *msg };
				if (was != a_peer.downed.has_value()) {
					log(std::format("server: player {} is {}", a_peer.playerId, up ? "up again" : msg->state == 1 ? "down, waiting for help" : "down, getting up"));
				}
				broadcast(Protocol::Encode(*msg), true, &a_key);
			}
			break;
		case MT::kHelpUp:
			// A help, to the downed player alone, the helper named. Never one's own; never to a player not waiting for help
			// by the server's book (one getting up, up, or gone); within the helper's budget.
			if (const auto msg = Protocol::DecodeHelpUp(a_data)) {
				const auto to = msg->player != a_peer.playerId ? peerOf(msg->player) : std::nullopt;
				const auto target = to ? peers.find(*to) : peers.end();
				const bool waits = target != peers.end() && target->second.downed && target->second.downed->state == 1;
				if (!waits || !a_peer.helpBudget.Take(Clock::now())) {
					log(std::format("server: player {}'s help for player {} dropped ({})", a_peer.playerId, msg->player,
						msg->player == a_peer.playerId ? "itself" : !to ? "no such player" : !waits ? "not waiting for help" : "too many"));
					break;
				}
				log(std::format("server: player {} helps player {} up", a_peer.playerId, msg->player));
				send(*to, Protocol::Encode(Protocol::HelpUp{ a_peer.playerId }), true);
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
		// event removes it; until then it is not asked again. And looks and faces held back by the budget go once it has
		// room.
		const auto now = Clock::now();
		for (auto& [key, peer] : peers) {
			if (peer.joined && peer.looksOwed && peer.looks && peer.looksBudget.Take(now)) {
				peer.looksOwed = false;
				broadcast(Protocol::Encode(Protocol::RemoteLooks{ peer.playerId, *peer.looks }), true, &key);
			}
			if (peer.joined && peer.faceOwed && peer.face && peer.faceBudget.Take(now)) {
				peer.faceOwed = false;
				broadcast(Protocol::Encode(Protocol::RemoteFace{ peer.playerId, *peer.face }), true, &key);
			}
			if (peer.joined && rides.Owes(peer.playerId)) {
				std::vector<Protocol::Ride> pass;
				Protocol::Ride              ride;
				while (pass.size() < Protocol::MAX_RIDES && rides.Owes(peer.playerId) && peer.rideBudget.Take(now) && rides.NextOwed(peer.playerId, ride)) {
					pass.push_back(ride);
				}
				if (!pass.empty()) {
					sendRides(nullptr, &key, std::move(pass), false, peer.playerId);
				}
			}
			if (peer.joined && sits.Owes(peer.playerId)) {
				std::vector<Protocol::Sit> pass;
				Protocol::Sit              sit;
				while (pass.size() < Protocol::MAX_SITS && sits.Owes(peer.playerId) && peer.sitBudget.Take(now) && sits.NextOwed(peer.playerId, sit)) {
					pass.push_back(sit);
				}
				if (!pass.empty()) {
					sendSits(nullptr, &key, std::move(pass), false, peer.playerId);
				}
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
