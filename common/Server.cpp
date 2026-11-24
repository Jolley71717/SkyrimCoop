#include "Server.h"

#include "EnetTransport.h"
#include "FormIds.h"
#include "Name.h"
#include "QuestBook.h"
#include "Redact.h"
#include "RefBook.h"
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
		// Doors and mechanisms (docs/DESIGN.md, "Doors, locks and mechanisms"): reports of a reference's state, a burst of
		// 32 then 8 a second (a game paces each reference itself: src/game/RefRules.h); uses of a mechanism, 8 then 4 a
		// second (a player pulls a lever by hand); asks for every kept state again, 2 then one in 5 s (one per load).
		// A report past its budget is answered "busy" (kRefRefused), and the game reports it again later; an ask past
		// its budget is owed, and answered from the server's loop once the budget has room.
		TokenBucket refBudget{ 32.0, 8.0 };
		TokenBucket actBudget{ 8.0, 4.0 };
		TokenBucket askBudget{ 2.0, 0.2 };
		bool        asksOwed = false;
		std::uint64_t refsDropped = 0;  // past the budget: said once in the log, counted after
		std::uint64_t actsDropped = 0;
		// Quests (docs/DESIGN.md, "Quests"): batches of entries, a burst of 64 then 8 a second (the host's game says its
		// whole world at its welcome and after a load, a few dozen batches; play makes a few a minute). A batch past its
		// budget is answered busy, entry by entry, and the game sends it again later. Asks for every kept entry again, as
		// for doors.
		TokenBucket questBudget{ 64.0, 8.0 };
		TokenBucket questAskBudget{ 2.0, 0.2 };
		bool        questAsksOwed = false;
		std::uint64_t questsDropped = 0;
		// The clock (docs/DESIGN.md, "Time and weather"): every game says its own every 5 s and on a change; only the
		// clock owner's is passed on, a burst of 4 then one a second. Past the budget the newest is kept and owed, as looks.
		Clock::time_point joinedAt{};
		TokenBucket       clockBudget{ 4.0, 1.0 };
		bool              clockOwed = false;
		bool              toldNotClockOwner = false;
		// A friend's weather magic asked of the clock owner (kWeatherAsk): a burst of 4, then one every 2 s. A release is
		// never dropped (one follows each held ask at most), so the owner is never left holding a friend's weather.
		TokenBucket       weatherAskBudget{ 4.0, 0.5 };
		bool              toldOwnAsk = false;
		bool        toldNotHost = false;
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
	// The session's word on each door's open state and lock (common/RefBook.h), in the table's numbering: a new session
	// (its first player in) starts it again with the table.
	RefBook refs;
	bool    refsFullSaid = false;
	// The session's word on each shared quest (common/QuestBook.h), in the table's numbering: started again with a new
	// session, and cleared when the host's game says its world is new (kQuestReset).
	QuestBook quests;
	bool      questsFullSaid = false;
	// The session's clock: the clock owner's last report, kept for whoever joins (a new session starts with none).
	std::optional<Protocol::ClockState> sessionClock;

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
	};

	// Whose clock is the session's: the host's game while it is in, else the player in the session longest (a dedicated
	// server's session, or the host gone), the lower player id on a tie; 0 with nobody in. As the sister project's.
	const auto clockOwner = [&]() -> std::uint32_t {
		const Peer* longest = nullptr;
		for (const auto& [key, peer] : peers) {
			if (!peer.joined) {
				continue;
			}
			if (hostId != 0 && peer.playerId == hostId) {
				return hostId;
			}
			if (!longest || peer.joinedAt < longest->joinedAt || (peer.joinedAt == longest->joinedAt && peer.playerId < longest->playerId)) {
				longest = &peer;
			}
		}
		return longest ? longest->playerId : 0;
	};

	// Every kept door state to one game, the session's word (player id 0), in the order first reported.
	const auto sendRefs = [&](const PeerKey& a_to) {
		for (const auto& state : refs.All()) {
			send(a_to, Protocol::Encode(state, MT::kRemoteRefState), true);
		}
	};

	// Every kept quest entry to one game, the session's word (player id 0), in the book's order.
	const auto sendQuests = [&](const PeerKey& a_to) {
		for (const auto& packet : Protocol::Batches(quests.All(), MT::kRemoteQuests, 0)) {
			send(a_to, packet, true);
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
			refs.Clear();          // ... and the doors were numbered by the old one
			refsFullSaid = false;
			quests.Clear();        // ... and the quests
			questsFullSaid = false;
			sessionClock.reset();  // ... and its clock was another world's
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
		a_peer.joinedAt = Clock::now();
		a_peer.clockOwed = false;
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
		// The session's doors, after the players: what was opened, closed, locked and picked before they came.
		sendRefs(a_key);
		sendQuests(a_key);  // ... and its quests: the stages done, the objectives, the ends
		// ... and its clock, while the game that said it still owns it (a report is at most a few seconds old)
		if (sessionClock && sessionClock->playerId != a_peer.playerId && sessionClock->playerId == clockOwner()) {
			send(a_key, Protocol::Encode(*sessionClock, MT::kRemoteClock), true);
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
		case MT::kRefState:
			// Passed on to every game, the reporter's too, in the order heard: each applies the last word, so two players
			// at one door at once end on the same state. One that changes nothing goes back to the reporter alone (its
			// game counts its reports answered, src/game/RefRules.h). Every report is answered: one not taken goes back
			// to its reporter alone as kRefRefused, with why and the session's word, so its game neither waits for an echo
			// that is not coming nor takes its own report for the session's word.
			if (const auto report = Protocol::DecodeRefState(a_data)) {
				using Refusal = Protocol::RefRefusal;
				const auto refuse = [&](Refusal::Reason a_reason) { send(a_key, Protocol::Encode(Refusal::Of(*report, a_reason, refs.Find(report->refId))), true); };
				if (!a_peer.refBudget.Take(Clock::now())) {
					if (a_peer.refsDropped++ == 0) {
						log(std::format("server: player {} reports doors faster than the server passes them on; the rest are answered busy", a_peer.playerId));
					}
					refuse(Refusal::kBusy);
					break;
				}
				auto       said = *report;
				said.playerId = a_peer.playerId;
				Protocol::RefState out;
				switch (refs.Report(said, out)) {
				case RefBook::Take::kNews:
					broadcast(Protocol::Encode(out, MT::kRemoteRefState), true, nullptr);
					break;
				case RefBook::Take::kSame:
					send(a_key, Protocol::Encode(out, MT::kRemoteRefState), true);
					break;
				case RefBook::Take::kFull:
					if (!std::exchange(refsFullSaid, true)) {
						log(std::format("server: the session keeps {} doors' states, its most; a new one is not kept or passed on", refs.Size()));
					}
					refuse(Refusal::kFull);
					break;
				case RefBook::Take::kRefused:
					refuse(Refusal::kRefused);
					break;
				}
			}
			break;
		case MT::kActivate:
			// Not kept: a use is an event. What it changes in the host's world is the host's save's, and a door it
			// opens is a door's state as any other.
			if (const auto use = Protocol::DecodeActivation(a_data); use && use->refId != 0 && !FormIds::IsRuntime(use->refId)) {
				if (!a_peer.actBudget.Take(Clock::now())) {
					if (a_peer.actsDropped++ == 0) {
						log(std::format("server: player {} uses mechanisms faster than the server passes them on; the rest are dropped", a_peer.playerId));
					}
					break;
				}
				broadcast(Protocol::Encode(Protocol::Activation{ use->refId, a_peer.playerId }, MT::kRemoteActivate), true, &a_key);
			}
			break;
		case MT::kAskRefStates:
			if (a_data.size() == 1) {
				if (a_peer.askBudget.Take(Clock::now())) {
					a_peer.asksOwed = false;
					sendRefs(a_key);
				} else {
					a_peer.asksOwed = true;  // answered from the loop once the budget has room: never dropped
				}
			}
			break;
		case MT::kQuests:
			// Each entry new to the session goes to every game, the reporter's too, with who said it; the rest go back to
			// the reporter alone (kQuestAnswer), each with why: every entry is answered, so a game never waits for an
			// echo that is not coming. A batch past the budget is answered busy, all of it.
			if (const auto batch = Protocol::DecodeQuestBatch(a_data)) {
				using E = Protocol::QuestEntry;
				std::vector<E> news;
				std::vector<E> answers;
				if (!a_peer.questBudget.Take(Clock::now())) {
					if (a_peer.questsDropped++ == 0) {
						log(std::format("server: player {} reports quests faster than the server passes them on; the rest are answered busy", a_peer.playerId));
					}
					for (auto e : batch->entries) {
						e.value = E::kBusy;
						answers.push_back(e);
					}
				} else {
					for (const auto& e : batch->entries) {
						E out;
						auto answer = e;
						switch (quests.Report(e, out)) {
						case QuestBook::Take::kNews:
							news.push_back(out);
							continue;
						case QuestBook::Take::kSame:
							answer.value = E::kSame;
							break;
						case QuestBook::Take::kFull:
							if (!std::exchange(questsFullSaid, true)) {
								log(std::format("server: the session keeps {} quests ({} entries), its most; a new one is not kept or passed on", quests.Quests(), quests.Entries()));
							}
							answer.value = E::kFull;
							break;
						case QuestBook::Take::kRefused:
							answer.value = E::kRefused;
							break;
						}
						answers.push_back(answer);
					}
				}
				for (const auto& packet : Protocol::Batches(news, MT::kRemoteQuests, a_peer.playerId)) {
					broadcast(packet, true, nullptr);
				}
				for (const auto& packet : Protocol::Batches(answers, MT::kQuestAnswer)) {
					send(a_key, packet, true);
				}
			}
			break;
		case MT::kAskQuests:
			if (a_data.size() == 1) {
				if (a_peer.questAskBudget.Take(Clock::now())) {
					a_peer.questAsksOwed = false;
					sendQuests(a_key);
				} else {
					a_peer.questAsksOwed = true;  // answered from the loop once the budget has room: never dropped
				}
			}
			break;
		case MT::kQuestReset:
			// The host's world is new (its welcome, a load): the session's quests are what its game says next. Only the
			// host's game may say so; a friend's world is never the session's.
			if (Protocol::DecodeQuestReset(a_data, false)) {
				if (a_peer.playerId != hostId || hostId == 0) {
					if (!std::exchange(a_peer.toldNotHost, true)) {
						log(std::format("server: player {} is not the host: its quest reset is ignored", a_peer.playerId));
					}
					break;
				}
				quests.Clear();
				questsFullSaid = false;
				broadcast(Protocol::Encode(Protocol::QuestReset{ a_peer.playerId }, true), true, &a_key);
			}
			break;
		case MT::kClock:
			// Every game says its clock; only the owner's is the session's (kept, and passed on to the others). The
			// others' are read and dropped, said once for each connection: a friend's wait, sleep or console never moves
			// anyone else's time, and their game is brought back to the owner's.
			if (auto said = Protocol::DecodeClockState(a_data)) {
				if (const auto owner = clockOwner(); a_peer.playerId != owner) {
					if (!std::exchange(a_peer.toldNotClockOwner, true)) {
						log(std::format("server: player {} does not keep the session's clock (player {} does): its clock is not passed on", a_peer.playerId, owner));
					}
					break;
				}
				said->playerId = a_peer.playerId;
				sessionClock = *said;
				if (a_peer.clockBudget.Take(Clock::now())) {
					a_peer.clockOwed = false;
					broadcast(Protocol::Encode(*said, MT::kRemoteClock), true, &a_key);
				} else {
					a_peer.clockOwed = true;  // the newest goes once the budget allows
				}
			}
			break;
		case MT::kWeatherAsk:
			// A friend's weather magic goes to the clock owner alone, whose game sets it (its sky is the session's). The
			// owner's own magic is its own weather, said by its clock: an ask from it is dropped (logged once).
			if (auto ask = Protocol::DecodeWeatherAsk(a_data)) {
				const auto owner = clockOwner();
				if (owner == 0 || a_peer.playerId == owner) {
					if (!std::exchange(a_peer.toldOwnAsk, true)) {
						log(owner == 0 ? std::format("server: player {} asked for a weather with no clock owner in the session: dropped", a_peer.playerId)
						                : std::format("server: player {} asked for a weather while keeping the session's clock itself: dropped", a_peer.playerId));
					}
					break;
				}
				if (!ask->release && !a_peer.weatherAskBudget.Take(Clock::now())) {
					break;  // past the budget: a later ask (or the release) says the rest
				}
				ask->playerId = a_peer.playerId;
				for (const auto& [key, peer] : peers) {
					if (peer.joined && peer.playerId == owner) {
						send(key, Protocol::Encode(*ask, MT::kRemoteWeatherAsk), true);
						break;
					}
				}
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
			if (peer.joined && peer.asksOwed && peer.askBudget.Take(now)) {
				peer.asksOwed = false;
				sendRefs(key);
			}
			if (peer.joined && peer.clockOwed && sessionClock && sessionClock->playerId == peer.playerId && peer.clockBudget.Take(now)) {
				peer.clockOwed = false;
				broadcast(Protocol::Encode(*sessionClock, MT::kRemoteClock), true, &key);
			}
			if (peer.joined && peer.questAsksOwed && peer.questAskBudget.Take(now)) {
				peer.questAsksOwed = false;
				sendQuests(key);
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
