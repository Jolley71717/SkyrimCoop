#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// Wire protocol shared by the game plugin, the server and test tools.
// All values are little-endian. Every packet starts with a one-byte MessageType.
//
// This is the first, small protocol: joining, leaving, where each player is, and a heartbeat. It has the shape of
// F4Multiplayer's (the Reader and Writer, the hello and its load order, the two channels) so what that project learned
// carries over; what the Skyrim side adds comes message by message as docs/PLAN.md's phases land (docs/PORTING.md).
namespace Protocol
{
	inline constexpr std::uint32_t MAGIC = 0x4F434B53;  // "SKCO"
	// Raise it with every change to what goes over the wire or how it is read: two games of different versions refuse
	// each other at the hello, with a reason that says so.
	// 2: a player's looks (PlayerLooks, RemoteLooks), for the stand-in the others show of them.
	// 3: a hello carries a whole Skyrim load order (MAX_PLUGINS names, in at most MAX_PLUGIN_LIST_BYTES), not 512 names.
	// 4: form ids on the wire are the session's (PluginTable, kPluginTable): a form is named by its plugin's file name,
	//    never by a game's load order, so games whose mods are in another order, or differ, play together.
	// 5: doors, locks and mechanisms (RefState, Activation; docs/DESIGN.md "Doors, locks and mechanisms"): a reference's
	//    open state and lock are the session's, kept by the server for whoever joins or loads later, and a mechanism a
	//    player uses (a lever, a pull chain, a pillar) is used in every game by that player's stand-in. Every report is
	//    answered: the session's word, or kRefRefused. (A branch written at the same time takes 5 for NPCs: whichever
	//    is merged second moves to 6; the message numbers do not clash.)
	// 7: quests (QuestBatch, docs/DESIGN.md "Quests"): a shared quest's stages, objectives and end are the session's,
	//    kept by the server for whoever joins or loads later; the host's game says what its world holds at its welcome
	//    and after each load (kQuestReset, then its quests). Written beside 5 (doors) and 6 (NPCs, another branch): the
	//    number is this branch's; whichever of them is merged later renumbers, the message numbers do not clash.
	// 9: the session's clock and weather (ClockState, docs/DESIGN.md "Time and weather"): every game says its time of day,
	//    day count, timescale and outdoor weather; the server passes on the clock owner's alone (the host's game, else
	//    the player there longest) and keeps it for whoever joins. Written on top of 7 (quests) beside 8 (NPC health,
	//    another branch): whichever is merged later renumbers; the message numbers (39, 40) do not clash.
	// 10: containers and loot (LootEntry, docs/DESIGN.md "Containers and loot"): what a player takes from or puts into a
	//    container or a body is numbered by the server, kept for whoever joins or loads later, and made in every game;
	//    a holder a player empties is said empty (its other games' own rolls go too). Written on top of 9 (the clock)
	//    beside 9 (player actions, another branch): whichever is merged later renumbers; the messages (44-48) do not clash.
	// 11: dropped items given (DroppedItem, docs/DESIGN.md "Dropped items"): an item a player drops lies in every game,
	//    each with its own copy, until one player takes it; the server grants each item to the first who asks, and makes
	//    the session agree with what each world holds after its welcome and every load. Written on top of 10 (loot) beside
	//    10 (faces, another branch): whichever is merged later renumbers; the messages (49-54) do not clash.
	// 12: quest runs and their aliases (QuestRun, docs/DESIGN.md "Quests", "Runs and aliases"): the host's game says each
	//    shared quest and radiant job it starts, the references and locations its aliases hold, and a radiant job's stop;
	//    a friend's game starts the quest with those in its aliases (or forces them into a quest running there), so
	//    every game does the same job, and a radiant job begun again is a new run whose old stages are not its own.
	//    Written on top of 11 (drops) beside 11 (swimming, another branch): whichever is merged later renumbers; the
	//    messages (55, 56) do not clash.
	// 15: conversations (Line, Talk; docs/DESIGN.md "Conversations"): the lines said in a player's conversation (what
	//    the NPC says, the topic the player picks) go to the others, who see them in the subtitle area and hear the
	//    recording from the speaker; one player at a time talks with an NPC (the server's talk book), and a friend's own
	//    talk with an NPC someone else talks with is closed. Written on top of 12 (quest runs) beside 12, 13 and 14 (riding,
	//    projectiles and furniture, another branch): the number is above both lines' and is renumbered at merge; its messages
	//    (59-62) clash with neither line's.
	// 18: the host's world said whole (kQuestWorld; docs/DESIGN.md "Quests"): the host's snapshot (at its welcome and
	//    after each load) is its own message, and also says the shared quests its world has ended; the server passes it
	//    on as the session's word (player 0), as to a joiner, so a stage of it that moves a player is the past
	//    everywhere, and a friend that never started a quest the host ended keeps its word aside until the quest starts
	//    there. Written on top of 15 (conversations) beside 16 (mechanism outcomes, types 63-64) and 17 (loose objects,
	//    types 65-67), each on another branch: the number is above all three and is renumbered at merge; its message (68)
	//    clashes with none.
	// 19: the host's reset is a line no quest word crosses (kQuestResetSeen; docs/DESIGN.md "Quests"): the server comes
	//    back to the host with its reset (kQuestReset with its own id), and the host's game makes no other player's word
	//    heard before it (the old world's); each other game says when it heard the reset, and the server drops its quest
	//    reports until then (sent from the old world, in flight). Written on top of 18 (the host's world); renumbered at
	//    merge.
	inline constexpr std::uint16_t VERSION = 19;
	inline constexpr std::uint16_t DEFAULT_PORT = 7780;
	// A name's bytes on the wire: Name::MAX_CHARACTERS characters of UTF-8, up to four bytes each (common/Name.h).
	inline constexpr std::size_t MAX_NAME_LENGTH = 128;
	inline constexpr std::size_t MAX_REASON_LENGTH = 200;
	// A load order on the wire (a hello's): at most MAX_PLUGINS names, the light plugins' mark among them. Skyrim numbers
	// 254 full plugins (form IDs 00 to FD) and 4096 light ones (FE 000 to FFF), the game's own masters and the Creation
	// Club's files among them, so the longest load order a game can have is 254 + the mark + 4096 = 4351 names
	// (FormIds.h checks it); 4352 is that with one to spare. Each name is at most MAX_PLUGIN_NAME_LENGTH bytes (a file
	// name on Windows is at most 260 characters).
	inline constexpr std::size_t MAX_PLUGINS = 4352;
	inline constexpr std::size_t MAX_PLUGIN_NAME_LENGTH = 260;
	// The most bytes one message may be: Steam's limit for one message (k_cbMaxSteamNetworkingSocketsMessageSizeSend),
	// the smaller of the two transports' (the ENet hosts take up to 1 MB). Bigger ones are never sent.
	inline constexpr std::size_t MAX_MESSAGE_SIZE = 512 * 1024;
	// The most bytes a plugin list takes on the wire (its count, and each name's length and bytes), so that a hello fits
	// in one message: 4351 names of 260 bytes would not (about 1.1 MB). 480 KB holds a whole load order of names that
	// average up to 110 bytes; real plugin names are a few tens. A longer list is cut at a whole name (WritePlugins),
	// and a game checks PluginsFit before it joins, to say why rather than send a cut list.
	inline constexpr std::size_t MAX_PLUGIN_LIST_BYTES = 480 * 1024;
	inline constexpr std::size_t MAX_PASSWORD_LENGTH = 64;
	// The most players a server takes, whatever it is asked for.
	inline constexpr std::size_t MAX_PLAYERS = 8;
	// Worn items in a player's looks: armor and clothing, of which Skyrim has 32 body slots (BGSBipedObjectForm), with
	// room to spare.
	inline constexpr std::size_t MAX_WORN = 40;

	// A refusal's reason (Reject) is English built from a reject.* key of src/TextEnglish.h (common/Server.cpp); the
	// joining game shows it in its own language (Text::Reject). The text on the wire is the same as before the keys.

	inline constexpr std::uint8_t CHANNEL_RELIABLE = 0;
	inline constexpr std::uint8_t CHANNEL_STATE = 1;  // unreliable, sequenced
	inline constexpr std::uint8_t CHANNEL_COUNT = 2;

	// A number is never reused for another message: a retired one stays retired.
	enum class MessageType : std::uint8_t
	{
		kHello = 1,         // client -> server, first packet
		kWelcome = 2,       // server -> client: you are in, this is your player id
		kReject = 3,        // server -> client: you are not, and why; the connection closes after it
		kPlayerJoined = 4,  // server -> the others (and each player already there, to the one who joined)
		kPlayerLeft = 5,
		kPlayerState = 6,   // client -> server, on the state channel: where I am
		kRemoteState = 7,   // server -> the others, on the state channel: where that player is
		kHeartbeat = 8,     // answered right away with kHeartbeatAck, to measure the connection
		kHeartbeatAck = 9,
		kPlayerLooks = 10,  // client -> server, reliable: what my character looks like (again whenever it changes)
		kRemoteLooks = 11,  // server -> the others (and each one there, to the one who joined): that player's looks
		kPluginTable = 12,  // server -> client, reliable: the session's plugin table, before the welcome and when it grows
		// 13-23 are left for the NPCs' messages, written at the same time on another branch.
		kRefState = 24,        // client -> server, reliable: a door's (or a chest's) open state or lock changed here
		kRemoteRefState = 25,  // server -> everyone, the reporter too: the session's word on it (and each kept one, to a joiner)
		kActivate = 26,        // client -> server, reliable: our player used a mechanism (a lever, a pull chain, a pillar)
		kRemoteActivate = 27,  // server -> the others: that player used it, so their stand-in uses it here too
		kAskRefStates = 28,    // client -> server, reliable: send me every kept RefState again (after a load)
		kRefRefused = 29,      // server -> the reporter alone, reliable: its kRefState was not taken, and why
		kQuests = 30,          // client -> server, reliable: stages, objectives and ends of shared quests, new here
		kRemoteQuests = 31,    // server -> everyone, the reporter too: what is new to the session (and all of it, to a joiner)
		kQuestAnswer = 32,     // server -> the reporter alone, reliable: its entries that were not new, each with why
		kAskQuests = 33,       // client -> server, reliable: send me every kept quest entry again (after a load)
		kQuestReset = 34,      // host -> server: my world is new (a welcome, a load): its quests follow; server -> the others, and back to the host
		kClock = 39,           // client -> server, reliable: our time of day, day count, timescale and weather (every 5 s, and on a change)
		kRemoteClock = 40,     // server -> the others: the clock owner's (and the kept one, to a joiner)
		kLoot = 44,            // client -> server, reliable: what our player took from or put into containers and bodies
		kRemoteLoot = 45,      // server -> everyone, the reporter too: each move numbered, with who made it (a catch-up's too)
		kAskLoot = 46,         // client -> server, reliable: send me the moves from this number of this log (a welcome, a load)
		kLootCaughtUp = 47,    // server -> the asker alone: the catch-up is done; the log's id and its next number
		kLootAnswer = 48,      // server -> the reporter alone, reliable: its moves that were not taken, each with why
		kReportDrop = 49,      // client -> server, reliable: our player dropped this item for the others to take
		kClaimDrop = 50,       // client -> server, reliable: take a dropped item, give back one we couldn't take, or answer an offer
		kDropRecord = 51,      // client -> server, reliable: what our world holds of the session's drops (a welcome, each load)
		kDropPlaced = 52,      // server -> the dropper alone (an offer, or a refusal: no number), or everyone (it lies there now)
		kDropGone = 53,        // server -> everyone: a dropped item was taken (by takerId: that game takes it) or is back with its owner
		kDropList = 54,        // server -> one game: every item lying in the session now (the answer to its kDropRecord)
		kQuestRun = 55,        // host -> server, reliable: a quest started (a run), its aliases' references read, or a radiant job stopped
		kRemoteQuestRun = 56,  // server -> the others: the host's word (and every run that is on, to a joiner and to one that loaded)
		// 57 and 58 are left for projectiles and furniture, written at the same time on another branch.
		kLine = 59,        // client -> server, reliable: a line said in our player's conversation, or its end
		kRemoteLine = 60,  // server -> the others: that player's line (never kept: a joiner hears only what is said after)
		kTalk = 61,        // client -> server, reliable: our player talks with this NPC (again every few seconds), or no longer
		kTalkAnswer = 62,  // server -> the asker alone: who talks with that NPC (the asker: it is theirs)
		// 63 and 64 are left for mechanism outcomes and 65 to 67 for loose objects, written at the same time on other branches.
		kQuestWorld = 68,  // host -> server, reliable: its world's quests, as kQuests: passed on as the session's word (player 0)
		kQuestResetSeen = 69,  // client -> server, reliable: our game heard the host's reset: its quest reports are of the new world now
	};

	// A coordinate a game can hold: finite and within the range a float keeps whole units in.
	[[nodiscard]] inline bool InWorld(float a_v)
	{
		return std::isfinite(a_v) && std::fabs(a_v) < 16777216.0f;
	}

	enum StateFlags : std::uint8_t
	{
		kNone = 0,
		kSneaking = 1 << 0,
		kSprinting = 1 << 1,
		kWeaponDrawn = 1 << 2,
		kJumping = 1 << 3,  // in the air: jumped, or falling
		kSwimming = 1 << 4,
		kMounted = 1 << 5,
	};

	// A player's transform and movement, sampled on the owning client.
	struct PlayerState
	{
		std::uint32_t sequence = 0;  // increases per sample; receivers drop older ones
		// Interior cell form ID, or 0 when outside; worldspace form ID, or 0 when inside. Both 0: somewhere the reading
		// game lacks (a cell or worldspace of a plugin it doesn't have, FormTranslate.h): nowhere it can show them.
		std::uint32_t cell = 0;
		std::uint32_t worldspace = 0;
		float         x = 0, y = 0, z = 0;
		float         heading = 0;  // radians
		float         speed = 0;    // units/second, for picking walk/run animations
		std::uint8_t  flags = kNone;
		// The sender's clock when it was sampled, in ms (any start; it wraps after 49 days), so the receiver can
		// place it in time however late it came; 0 when not known.
		std::uint32_t sentMs = 0;
		// Travel relative to facing in 256ths of a turn (0 forward, 64 right, 128 back, 192 left), and the pitch the
		// player aims at, up (+) or down (-) in degrees.
		std::uint8_t direction = 0;
		std::int8_t  pitch = 0;

		[[nodiscard]] bool IsFinite() const
		{
			return InWorld(x) && InWorld(y) && InWorld(z) && std::isfinite(heading) && std::isfinite(speed);
		}

		bool operator==(const PlayerState&) const = default;
	};

	// PlayerState::direction from a fraction of a turn, and back.
	inline std::uint8_t PackDirection(float a_turns)
	{
		if (!std::isfinite(a_turns)) {
			return 0;
		}
		return static_cast<std::uint8_t>(std::lround((a_turns - std::floor(a_turns)) * 256.0f) & 0xFF);
	}
	inline float UnpackDirection(std::uint8_t a_direction)
	{
		return static_cast<float>(a_direction) / 256.0f;
	}

	// PlayerState::pitch from a pitch in degrees, held to a quarter turn either way.
	inline std::int8_t PackPitch(float a_degrees)
	{
		return std::isfinite(a_degrees) ? static_cast<std::int8_t>(std::clamp(std::lround(a_degrees), -90L, 90L)) : std::int8_t{ 0 };
	}

	// PlayerState::sentMs for a moment on the sender's steady clock (never 0, which means not known).
	inline std::uint32_t SentMs(std::chrono::steady_clock::time_point a_at)
	{
		const auto ms = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(a_at.time_since_epoch()).count());
		return ms ? ms : 1;
	}

	struct Hello
	{
		std::uint32_t magic = MAGIC;
		std::uint16_t version = VERSION;
		std::uint32_t contentHash = 0;  // LoadOrderHash of the client's load order; a hello without plugins must match the session's
		std::string   name;
		std::string   password;
		// Random, kept on the player's PC: the server gives a player who comes back the same player
		// ID, so their own changes to the world are still known as theirs. 0 = none.
		std::uint64_t identity = 0;
		// The load order (plugin file names, in order: ComparedLoadOrder), which the session's plugin table is made of.
		// At most MAX_PLUGINS names in MAX_PLUGIN_LIST_BYTES (WritePlugins leaves the rest off); contentHash is over the
		// whole list all the same.
		std::vector<std::string> plugins;

		bool operator==(const Hello&) const = default;
	};

	// The session's plugin table: the plugins that number every form ID on the wire, as Hello::plugins lays a load order
	// out (the full ones, LIGHT_PLUGINS, the light ones). The first player's load order, with the plugins each later
	// joiner brings added after the rest of their kind (FormIds::Extend), so an ID it gave never changes. Every game
	// translates its own form IDs to it and back by plugin name (common/FormIds.h, FormTranslate.h). The server sends it
	// to a joiner before the welcome, and to everyone in when it grows, on the reliable channel.
	struct PluginTable
	{
		std::vector<std::string> plugins;

		bool operator==(const PluginTable&) const = default;
	};

	struct Welcome
	{
		std::uint32_t playerId = 0;
		std::uint64_t sessionId = 0;  // random per server run
		// The host's own game's player id; 0: none in the session (a dedicated server).
		std::uint32_t hostId = 0;

		bool operator==(const Welcome&) const = default;
	};

	struct Reject
	{
		std::string reason;
	};

	struct PlayerJoined
	{
		std::uint32_t playerId = 0;
		std::string   name;

		bool operator==(const PlayerJoined&) const = default;
	};

	struct PlayerLeft
	{
		std::uint32_t playerId = 0;
	};

	// Another player's state, as the server passes it on.
	struct RemoteState
	{
		std::uint32_t playerId = 0;
		PlayerState   state;

		bool operator==(const RemoteState&) const = default;
	};

	struct Heartbeat
	{
		std::uint32_t token = 0;
	};

	// What a player's character looks like, as the others' stand-in of them shows it. Form ids in the session's numbering
	// (PluginTable): a race the reading game lacks reads 0, worn items it lacks are left out (FormTranslate.h).
	struct PlayerLooks
	{
		std::uint32_t race = 0;  // TESRace form id; 0: not known
		bool          female = false;
		float         weight = 0.0f;  // the body's weight, 0 to 100
		// The armor (and clothing) worn, as base form ids, at most MAX_WORN.
		std::vector<std::uint32_t> worn;

		[[nodiscard]] bool IsFinite() const { return std::isfinite(weight); }
		bool operator==(const PlayerLooks&) const = default;
	};

	// Another player's looks, as the server passes them on.
	struct RemoteLooks
	{
		std::uint32_t playerId = 0;
		PlayerLooks   looks;
		bool operator==(const RemoteLooks&) const = default;
	};

	// A reference's open state and lock, as the session knows them (docs/DESIGN.md, "Doors, locks and mechanisms"): a
	// door, a gate that opens like one, a chest's lock. refId is in the session's numbering (PluginTable); 0 is no
	// reference (one the reading game lacks reads 0, FormTranslate.h) and is never kept nor applied. A part a report
	// leaves kUnknown is not said: the receiver leaves that part as it is.
	struct RefState
	{
		enum Value : std::uint8_t
		{
			kNo = 0,
			kYes = 1,
			kUnknown = 2,  // not said: leave it as it is
		};
		// The lock's level, as the game numbers it (RE::LOCK_LEVEL): 0 very easy to 4 very hard, 5 needs its key.
		static constexpr std::uint8_t LEVEL_MAX = 5;
		static constexpr std::uint8_t LEVEL_UNKNOWN = 0xFF;

		std::uint32_t refId = 0;
		std::uint8_t  open = kUnknown;
		std::uint8_t  locked = kUnknown;
		std::uint8_t  lockLevel = LEVEL_UNKNOWN;
		// Who said it (set by the server; what a client sends carries none). 0: the session's word as kept, sent again
		// (to a joiner, or to a game that loaded a save).
		std::uint32_t playerId = 0;

		// The same state, whoever said it.
		[[nodiscard]] bool SameState(const RefState& a_other) const
		{
			return refId == a_other.refId && open == a_other.open && locked == a_other.locked && lockLevel == a_other.lockLevel;
		}
		[[nodiscard]] bool Says() const { return open != kUnknown || locked != kUnknown || lockLevel != LEVEL_UNKNOWN; }
		bool operator==(const RefState&) const = default;
	};

	// a_said laid over a_known: each part a_said says replaces a_known's, the rest stays (the reference is a_said's).
	[[nodiscard]] inline RefState MergeRefState(RefState a_known, const RefState& a_said)
	{
		a_known.refId = a_said.refId;
		a_known.playerId = a_said.playerId;
		if (a_said.open != RefState::kUnknown) {
			a_known.open = a_said.open;
		}
		if (a_said.locked != RefState::kUnknown) {
			a_known.locked = a_said.locked;
		}
		if (a_said.lockLevel != RefState::LEVEL_UNKNOWN) {
			a_known.lockLevel = a_said.lockLevel;
		}
		return a_known;
	}

	// A player used a mechanism: an activator such as a lever, a pull chain, a button, a rotating pillar or a claw door's
	// keyhole. refId in the session's numbering; playerId set by the server (kRemoteActivate), none in kActivate.
	struct Activation
	{
		std::uint32_t refId = 0;
		std::uint32_t playerId = 0;
		bool operator==(const Activation&) const = default;
	};

	// The server's answer to a report it did not take (kRefRefused), to the reporter alone: every report is answered, so
	// a game never waits for an echo that is not coming. refId and the parts reported (said*), why, and the session's
	// word on that reference now (session*: all kUnknown when it has none). refId in the session's numbering.
	struct RefRefusal
	{
		enum Reason : std::uint8_t
		{
			kBusy = 0,     // past that player's budget: report it again a little later
			kFull = 1,     // a new reference, and the session keeps MAX_REF_STATES already: it stays each game's own
			kRefused = 2,  // no reference, a runtime one, or a report that says nothing: never taken
		};
		static constexpr std::uint8_t REASON_MAX = kRefused;

		std::uint32_t refId = 0;
		std::uint8_t  reason = kBusy;
		std::uint8_t  saidOpen = RefState::kUnknown;
		std::uint8_t  saidLocked = RefState::kUnknown;
		std::uint8_t  saidLevel = RefState::LEVEL_UNKNOWN;
		std::uint8_t  sessionOpen = RefState::kUnknown;
		std::uint8_t  sessionLocked = RefState::kUnknown;
		std::uint8_t  sessionLevel = RefState::LEVEL_UNKNOWN;

		static RefRefusal Of(const RefState& a_said, Reason a_reason, const RefState* a_session)
		{
			RefRefusal out{ .refId = a_said.refId, .reason = a_reason, .saidOpen = a_said.open, .saidLocked = a_said.locked, .saidLevel = a_said.lockLevel };
			if (a_session) {
				out.sessionOpen = a_session->open;
				out.sessionLocked = a_session->locked;
				out.sessionLevel = a_session->lockLevel;
			}
			return out;
		}
		[[nodiscard]] RefState Said() const { return RefState{ .refId = refId, .open = saidOpen, .locked = saidLocked, .lockLevel = saidLevel }; }
		[[nodiscard]] RefState Session() const { return RefState{ .refId = refId, .open = sessionOpen, .locked = sessionLocked, .lockLevel = sessionLevel }; }
		bool operator==(const RefRefusal&) const = default;
	};

	// The most reference states the server keeps for a session: a long evening opens a few hundred doors; past this the
	// oldest word is not dropped, a new reference is refused (common/RefBook.h).
	inline constexpr std::size_t MAX_REF_STATES = 16384;

	// One thing about a shared quest (docs/DESIGN.md, "Quests"): a stage done, an objective's state, or the quest's end.
	// questId is in the session's numbering (PluginTable); an entry whose quest the reading game lacks is left out of
	// its batch (FormTranslate.h). What a stage or an objective is, is the quest's own numbering: the same in every game
	// that has the quest.
	struct QuestEntry
	{
		enum Kind : std::uint8_t
		{
			kStage = 0,      // stage `index` is done
			kObjective = 1,  // objective `index` is in state `value` (RE::QUEST_OBJECTIVE_STATE: 0 dormant to 5)
			kCompleted = 2,  // the quest is completed (index and value 0)
			kFailed = 3,     // the quest failed (index and value 0)
		};
		static constexpr std::uint8_t KIND_MAX = kFailed;
		static constexpr std::uint8_t OBJECTIVE_STATE_MAX = 5;

		// In a kQuestAnswer, why the session did not take it as new (value holds it).
		enum Answer : std::uint8_t
		{
			kSame = 0,     // the session had it already (nothing to pass on)
			kBusy = 1,     // past that player's budget: report it again a little later
			kFull = 2,     // the session keeps MAX_QUESTS quests (or MAX_QUEST_ENTRIES of this one) already
			kRefused = 3,  // no quest (0), a runtime one, a state out of range, or not the host's to say
		};
		static constexpr std::uint8_t ANSWER_MAX = kRefused;

		std::uint32_t questId = 0;
		std::uint8_t  kind = kStage;
		std::uint16_t index = 0;
		std::uint8_t  value = 0;
		// An objective: the quest's current stage in the game that read it, the stage its shown part holds at (a later
		// stage's own script may withdraw it, so a game at another stage does not show it from this word). 0 otherwise.
		std::uint16_t stage = 0;

		[[nodiscard]] bool SameThing(const QuestEntry& a_other) const { return questId == a_other.questId && kind == a_other.kind && index == a_other.index; }
		bool operator==(const QuestEntry&) const = default;
	};

	// A batch of quest entries: kQuests (a game's news, playerId 0), kQuestWorld (the host's world, its snapshot: playerId
	// 0), kRemoteQuests (the server's word, with the player
	// who said it; 0 for the session's word sent again: to a joiner, or after a load), kQuestAnswer (to the reporter
	// alone; each entry's value is its QuestEntry::Answer, so an objective's state is not said).
	struct QuestBatch
	{
		std::uint32_t           playerId = 0;
		std::vector<QuestEntry> entries;
		bool operator==(const QuestBatch&) const = default;
	};

	// The host's world is new (kQuestReset): from the host, empty; from the server, to the others and back to the host, the host's player id.
	struct QuestReset
	{
		std::uint32_t playerId = 0;
		bool operator==(const QuestReset&) const = default;
	};

	// One alias of a quest, a reference alias or a location alias, as the host's game holds it filled (docs/DESIGN.md,
	// "Quests", "Runs and aliases"). aliasId is the quest's own numbering (the same in every game that has the quest);
	// refId is a form in the session's numbering, and only with kRef (a reference) or kLoc (a location).
	struct AliasFill
	{
		enum Kind : std::uint8_t
		{
			kEmpty = 0,    // the alias holds nothing (an optional one the quest did not fill, or one a script cleared)
			kRef = 1,      // a reference alias that holds refId, a reference of the load order
			kRuntime = 2,  // it holds a form the host's game made as it ran (a created alias's actor): no other game has it
			kUnknown = 3,  // it holds a form of a plugin the reading game lacks (FormTranslate.h), or one the host's game could
			               // not read (a location while the engine's layout is not confirmed there): that game's own pick stays
			kLoc = 4,      // a location alias that holds refId, a location of the load order
		};
		static constexpr std::uint8_t KIND_MAX = kLoc;

		std::uint32_t aliasId = 0;
		std::uint32_t refId = 0;
		std::uint8_t  kind = kEmpty;
		bool operator==(const AliasFill&) const = default;
	};

	// A run of a quest: the host's game started a shared quest or a radiant job (a run, numbered by the host's game, never
	// twice in its session), read the references its aliases hold (kFilled, with them; again whenever they change), or
	// stopped a radiant job (kStopped). kQuestRun from the host (playerId 0), kRemoteQuestRun from the server (the host's
	// player id; 0 for the session's word sent again: to a joiner, or after a load).
	struct QuestRun
	{
		enum Flags : std::uint8_t
		{
			kFresh = 1 << 0,    // a radiant job begun (again): the quest's stages of an earlier run are not this run's
			kFilled = 1 << 1,   // fills holds the references the quest's reference aliases hold now
			kStopped = 1 << 2,  // the radiant job stopped: the run is over (its stages go with it)
		};
		static constexpr std::uint8_t FLAGS_ALL = kFresh | kFilled | kStopped;

		std::uint32_t          playerId = 0;
		std::uint32_t          questId = 0;
		std::uint32_t          run = 0;
		std::uint8_t           flags = 0;
		std::vector<AliasFill> fills;  // with kFilled only, by alias id, at most MAX_ALIAS_FILLS
		bool operator==(const QuestRun&) const = default;
	};
	// The most aliases a run names (a quest has a few dozen at most; the radiant trackers a few hundred, and those are
	// untyped, never shared): past it the rest are each game's own picks.
	inline constexpr std::size_t MAX_ALIAS_FILLS = 256;

	// A line said in a player's conversation (docs/DESIGN.md, "Conversations"): one response of what the NPC they talk
	// with says (its subtitle as the talker's game shows it, and the recording it plays), or the topic the player picked
	// (their own line: Skyrim's player is not voiced). kLine from the talker (playerId 0), kRemoteLine from the server
	// (the talker's player id). kEnd says the talker's line of that number ended there (the talker skipped it, or the
	// conversation was left): a reader still playing it ends it too.
	struct Line
	{
		enum Kind : std::uint8_t
		{
			kSay = 0,
			kEnd = 1,
		};
		enum Speaker : std::uint8_t
		{
			kTalker = 0,  // the talking player themselves (the topic they picked): their stand-in says it
			kRef = 1,     // a reference (the NPC): `speaker`, or none the reader has (0), when `name` shows who it was
		};
		std::uint32_t playerId = 0;
		std::uint8_t  kind = kSay;
		std::uint8_t  speakerKind = kRef;
		std::uint32_t speaker = 0;   // with kRef: the reference saying it; 0 for one the reader's game lacks
		std::uint32_t info = 0;      // the dialogue response's info (TESTopicInfo); 0: not known, or the reader lacks it
		std::uint8_t  response = 0;  // its response number, from 1; 0: not known
		std::uint32_t seq = 0;       // the talker's number for the line, from 1, rising; kEnd names the line it ends
		std::string   name;          // with kRef: the speaker's name as the talker's game shows it, for a reader without it
		std::string   text;          // with kSay: what is said, never blank
		std::string   voice;         // with kSay: the recording's path under Data (Sound\Voice\...); empty: none
		bool operator==(const Line&) const = default;
	};
	// A line's text, a speaker's name and a recording's path at most (bytes of UTF-8: a response is a sentence or three,
	// some languages take three bytes a character; a path is at most Windows' 260 characters).
	inline constexpr std::size_t MAX_LINE_TEXT = 1024;
	inline constexpr std::size_t MAX_SPEAKER_NAME = 128;
	inline constexpr std::size_t MAX_VOICE_PATH = 260;

	// Our player talks with an NPC (on), said when the conversation starts and again every few seconds while it goes on,
	// or no longer (off). The server keeps one talker for each NPC and answers each "on" with who that is (TalkAnswer).
	struct Talk
	{
		std::uint32_t ref = 0;  // the NPC
		bool          on = true;
		bool operator==(const Talk&) const = default;
	};

	// Who talks with an NPC, to the player who asked: themselves (it is theirs), or another player (busy: their game
	// ends its own talk with it).
	struct TalkAnswer
	{
		std::uint32_t ref = 0;
		std::uint32_t holderId = 0;
		bool operator==(const TalkAnswer&) const = default;
	};

	// A game's clock and weather (kClock, playerId 0; kRemoteClock, the clock owner's player id). Its forms are the
	// worldspace the sender stands in outside (0: inside, or a place the reader's game lacks) and the weather there (0:
	// none said, or one the reader's game lacks); a reader forces no weather from a report without both.
	struct ClockState
	{
		std::uint32_t playerId = 0;
		float         gameHour = 0.0f;    // GameHour, 0 to under 24
		float         daysPassed = 0.0f;  // GameDaysPassed: tells a long sleep from a short one, and a load from both
		float         timeScale = 0.0f;   // TimeScale: game seconds per real second (0 stands time still)
		std::uint32_t worldspace = 0;
		std::uint32_t weather = 0;
		bool operator==(const ClockState&) const = default;
	};
	// The most days a clock may say (a save of years of play is a few thousand) and the largest timescale (mods offer a
	// few hundred at most): a number past either is no game's.
	inline constexpr float MAX_DAYS_PASSED = 1.0e6f;
	inline constexpr float MAX_TIME_SCALE = 100000.0f;

	// The most entries in one batch (a few hundred bytes: one reliable packet).
	inline constexpr std::size_t MAX_QUEST_BATCH = 64;
	// The most quests the server keeps for a session (Skyrim and its DLC have about 1,400 quests; mods add more), and
	// the most entries (stages, objectives, its end) it keeps of one quest (a quest has a few dozen stages at most).
	// Past either, a new one is refused (kFull), never the oldest dropped.
	inline constexpr std::size_t MAX_QUESTS = 4096;
	inline constexpr std::size_t MAX_QUEST_ENTRIES = 512;

	// One move of loot (docs/DESIGN.md, "Containers and loot"): a player took `count` of the item `itemId` from the
	// container or body `holderId`, or put them into it; or the holder was emptied. Forms in the session's numbering; an
	// entry whose holder or item the reading game lacks is left out of its batch (FormTranslate.h).
	struct LootEntry
	{
		enum Kind : std::uint8_t
		{
			kTake = 0,     // `count` of the item went from the holder to the player who made the move
			kPut = 1,      // `count` of the item went from that player into the holder
			kEmptied = 2,  // the holder holds nothing the session shares in the sender's game (itemId 0); `count` is the
			               // sender's ledger as of then (the first number it had not made), so a later put stays
		};
		static constexpr std::uint8_t KIND_MAX = kEmptied;

		// In a kLootAnswer, why the session did not take it.
		enum Answer : std::uint8_t
		{
			kBusy = 0,     // past that player's budget: report it again a little later
			kFull = 1,     // the session's log holds MAX_LOOT_LOG moves already: it stays the reporter's game's own
			kRefused = 2,  // no holder, no item, a runtime form, a count out of range: never taken
		};
		static constexpr std::uint8_t ANSWER_MAX = kRefused;

		std::uint32_t number = 0;    // kRemoteLoot: the session's number for it (1 up); 0 otherwise
		std::uint32_t playerId = 0;  // kRemoteLoot: who made it; 0 otherwise
		std::uint32_t tag = 0;       // the reporting game's own name for the move, carried back as it was in its echo and
		                             // its answer: the reporter knows its moves by it (a form may come back as 0)
		std::uint32_t holderId = 0;
		std::uint32_t itemId = 0;
		std::uint32_t count = 0;
		std::uint8_t  kind = kTake;
		std::uint8_t  answer = kBusy;  // kLootAnswer only

		bool operator==(const LootEntry&) const = default;
	};

	// A batch of loot moves: kLoot (a game's own, in the order made), kRemoteLoot (the session's, numbered; `ask` is the
	// kAskLoot it answers as a catch-up, 0 for a move passed on as it came), kLootAnswer (to the reporter alone, each with
	// its answer).
	struct LootBatch
	{
		std::uint32_t          ask = 0;
		std::vector<LootEntry> entries;
		bool operator==(const LootBatch&) const = default;
	};

	// kAskLoot: the moves of the log `logId` from `from` on (a game's ledger: what its world holds already). A log id that
	// is not the server's (0: none; another session's) is answered from the first move. `ask` names it (never 0): its
	// catch-up and its end carry it, so a game tells the answer to its newest ask from the answers to the asks it made
	// before (the server owes a game one ask at most, the newest: an older one it replaced is never answered).
	struct LootAsk
	{
		std::uint32_t ask = 1;
		std::uint64_t logId = 0;
		std::uint32_t from = 1;
		bool operator==(const LootAsk&) const = default;
	};

	// kLootCaughtUp: every move from `from` on has been sent for the ask `ask`; the log is `logId`, its next number `next`.
	struct LootCaughtUp
	{
		std::uint32_t ask = 1;
		std::uint64_t logId = 0;
		std::uint32_t from = 1;
		std::uint32_t next = 1;
		bool operator==(const LootCaughtUp&) const = default;
	};

	// The most moves in one batch (a container's Take All is a few dozen: one or two batches).
	inline constexpr std::size_t MAX_LOOT_BATCH = 64;
	// The most of one item one move carries (a heap of gold is a few thousand; mods sell in the hundreds of thousands).
	inline constexpr std::uint32_t MAX_LOOT_COUNT = 1000000;
	// The most moves the server keeps for a session (an evening loots a few thousand); past it a new move is refused
	// (kFull) and stays its game's own, never the oldest dropped: a game that loads asks for them by number.
	inline constexpr std::size_t MAX_LOOT_LOG = 30000;

	// ---- dropped items (protocol 11, docs/DESIGN.md "Dropped items") ------------------------------------------------

	// A player gives an item by dropping it: it then lies in every game at the same spot, as each game's own copy, until
	// one player takes it, and the server grants each item to one taker (common/DropStore.h): it moves once, never into two
	// inventories and never into none. A dropped stack is one reference, its count the engine's 16-bit one.
	inline constexpr std::uint32_t MAX_DROP_COUNT = 32767;
	// A name the player gave the item (a renamed sword), in bytes of UTF-8.
	inline constexpr std::size_t MAX_ITEM_NAME_LENGTH = 128;
	// What a kDropRecord lists at most of each kind (the oldest forgotten past it).
	inline constexpr std::size_t MAX_DROP_RECORD = 4096;
	// Drops in one kDropList packet.
	inline constexpr std::size_t MAX_DROPS_PER_PACKET = 64;
	// The highest soul a soul gem holds (Skyrim's SOUL_LEVEL: none, petty, lesser, common, greater, grand).
	inline constexpr std::uint8_t MAX_SOUL = 5;

	// One dropped item, as its dropper's game had it: its base form and count, and what makes it that item and not a
	// fresh one of its base (tempering, an enchantment and its charge, a poison, a soul, a name the player gave it), and
	// where it came to rest. In a report (kReportDrop) dropId and ownerId are 0; the server sets them. A kDropPlaced with
	// dropId 0 is a refusal, to the dropper alone: the item stays theirs, an ordinary one in their world.
	struct DroppedItem
	{
		std::uint32_t dropId = 0;
		std::uint32_t ownerId = 0;
		std::uint64_t nonce = 0;            // the dropper's game's own number for it (DropRecord::made); never 0
		std::uint32_t base = 0;             // a base form: weapon, armor, ammo, potion, ingredient, misc, soul gem, scroll, torch
		std::uint32_t count = 1;            // 1 .. MAX_DROP_COUNT
		float         health = -1.0f;       // a tempered weapon's or armor's (above 1); -1: none
		std::uint32_t enchantment = 0;      // an enchantment of a plugin's on the item itself (not its base's); 0: none
		std::uint16_t enchantmentMax = 0;   // ... and the charge it holds at most
		float         charge = -1.0f;       // a weapon's charge left; -1: none (full, or no enchantment)
		std::uint32_t poison = 0;           // a poison on a weapon; 0: none
		std::uint32_t poisonCount = 0;      // ... and the hits it has left (at most 65535)
		std::uint8_t  soul = 0;             // a soul gem's soul, 0 (none) .. MAX_SOUL
		std::string   name;                 // a name the player gave it; empty: the game's own
		std::uint32_t cell = 0;             // interior cell form ID, or 0 when outside
		std::uint32_t worldspace = 0;       // worldspace form ID, or 0 when inside
		float         x = 0, y = 0, z = 0;  // where it came to rest
		float         angle = 0;            // its heading there (radians)
		std::uint32_t takerId = 0;          // in the server's book only (never sent): who took it

		bool operator==(const DroppedItem&) const = default;
	};

	// kClaimDrop. kTake: the sender's player pressed Take on its copy; the server grants it to the first who asks (kDropGone
	// naming them), and answers a later one with who has it. kGiveBack: a grant the sender's game could not finish (its
	// copy gone, a load first): it lies there again for anyone.
	//
	// A new drop is offered to its dropper's game alone (kDropPlaced), and lies there for the others only once that game
	// has taken the item it dropped out of its world: kPlaced says it has. kKept: its player picked the item up again
	// before the offer came, so it never lay anywhere. An offer that is neither goes when its dropper leaves. So nobody
	// can take an item that is still in its dropper's world.
	enum class DropClaim : std::uint8_t
	{
		kTake = 0,
		kGiveBack = 1,
		kPlaced = 2,
		kKept = 3,
	};

	struct ClaimDrop
	{
		std::uint32_t dropId = 0;
		DropClaim     op = DropClaim::kTake;

		bool operator==(const ClaimDrop&) const = default;
	};

	// kDropGone: the item no longer lies anywhere. takerId: the player it was granted to (their game takes it now; every
	// other game removes its copy), or 0: it is back with its owner (they left, or their world holds it again).
	struct DropGone
	{
		std::uint32_t dropId = 0;
		std::uint32_t takerId = 0;

		bool operator==(const DropGone&) const = default;
	};

	// kDropRecord: what the sender's world holds of the session's drops, as its save had it (the co-save) and its play
	// since: the drops it made (by nonce) and the ones it took (by drop ID), in sessionId (the welcome's; 0 or another:
	// its save has no record of this session, so is from before any of it). The server makes the session agree
	// (DropStore::Reconcile): a drop of theirs not listed is back in their world (a save from before they dropped it), so
	// it is withdrawn; one they took and don't list isn't in their world (a save from before they took it), so it lies
	// there again. Answered with kDropList.
	struct DropRecord
	{
		std::uint64_t              sessionId = 0;
		std::vector<std::uint64_t> made;
		std::vector<std::uint32_t> taken;

		bool operator==(const DropRecord&) const = default;
	};

	// kDropList: the items lying in the session, in packets; the last has last set. A game removes its copies of any not
	// listed by then.
	struct DropList
	{
		bool                     last = true;
		std::vector<DroppedItem> drops;

		bool operator==(const DropList&) const = default;
	};

	// ---- serialization -------------------------------------------------------------

	// The longest start of a_text that is at most a_maxBytes long and doesn't end inside a UTF-8 character: a cut name
	// loses its last character whole, never leaving half of one (which the game would show as junk).
	[[nodiscard]] inline std::string_view Utf8Prefix(std::string_view a_text, std::size_t a_maxBytes)
	{
		if (a_text.size() <= a_maxBytes) {
			return a_text;
		}
		std::size_t length = a_maxBytes;
		// Back off continuation bytes (10xxxxxx) to the start of the character cut through, at most three.
		for (int i = 0; i < 3 && length > 0 && (static_cast<unsigned char>(a_text[length]) & 0xC0) == 0x80; ++i) {
			--length;
		}
		return a_text.substr(0, length);
	}

	class Writer
	{
	public:
		explicit Writer(MessageType a_type) { U8(static_cast<std::uint8_t>(a_type)); }

		void U8(std::uint8_t a_value) { buffer.push_back(a_value); }
		void U16(std::uint16_t a_value) { Raw(&a_value, sizeof(a_value)); }
		void U32(std::uint32_t a_value) { Raw(&a_value, sizeof(a_value)); }
		void U64(std::uint64_t a_value) { Raw(&a_value, sizeof(a_value)); }
		void F32(float a_value) { Raw(&a_value, sizeof(a_value)); }

		// Cut short at a whole character (Utf8Prefix).
		void Str(std::string_view a_value, std::size_t a_maxLength)
		{
			const auto length = Utf8Prefix(a_value, (std::min<std::size_t>)(a_maxLength, 255)).size();
			U8(static_cast<std::uint8_t>(length));
			Raw(a_value.data(), length);
		}

		// A string longer than Str's 255 bytes may be (a plugin's file name): a U16 length, then the bytes.
		void LongStr(std::string_view a_value, std::size_t a_maxLength)
		{
			const auto length = Utf8Prefix(a_value, (std::min<std::size_t>)(a_maxLength, 0xFFFF)).size();
			U16(static_cast<std::uint16_t>(length));
			Raw(a_value.data(), length);
		}

		[[nodiscard]] const std::vector<std::uint8_t>& Data() const { return buffer; }

	private:
		void Raw(const void* a_data, std::size_t a_size)
		{
			const auto bytes = static_cast<const std::uint8_t*>(a_data);
			buffer.insert(buffer.end(), bytes, bytes + a_size);
		}

		std::vector<std::uint8_t> buffer;
	};

	// Reads values from a packet. Any read past the end sets the failed flag and
	// returns zero, so decoders can read everything and check Ok() once at the end.
	class Reader
	{
	public:
		explicit Reader(std::span<const std::uint8_t> a_data) :
			data(a_data)
		{}

		std::uint8_t  U8() { return Read<std::uint8_t>(); }
		std::uint16_t U16() { return Read<std::uint16_t>(); }
		std::uint32_t U32() { return Read<std::uint32_t>(); }
		std::uint64_t U64() { return Read<std::uint64_t>(); }
		float         F32() { return Read<float>(); }

		std::string Str(std::size_t a_maxLength)
		{
			const auto length = U8();
			if (failed || length > a_maxLength || offset + length > data.size()) {
				failed = true;
				return {};
			}
			std::string out(reinterpret_cast<const char*>(data.data() + offset), length);
			offset += length;
			// Every string on the wire is text someone reads (a name, a reason, a line): control
			// characters are dropped here, before any of it reaches a HUD, a log or a status line.
			std::erase_if(out, [](const unsigned char c) { return c < 0x20 || c == 0x7F; });
			return out;
		}

		// Writer::LongStr's string, its control characters dropped as Str's are.
		std::string LongStr(std::size_t a_maxLength)
		{
			const auto length = U16();
			if (failed || length > a_maxLength || offset + length > data.size()) {
				failed = true;
				return {};
			}
			std::string out(reinterpret_cast<const char*>(data.data() + offset), length);
			offset += length;
			std::erase_if(out, [](const unsigned char c) { return c < 0x20 || c == 0x7F; });
			return out;
		}

		[[nodiscard]] bool Ok() const { return !failed; }
		// Everything read, and all of it there: a read that ran past the end leaves the offset short of it, except one
		// that began exactly at the end, which read nothing and must not pass as the end reached.
		[[nodiscard]] bool AtEnd() const { return !failed && offset == data.size(); }

	private:
		template <class T>
		T Read()
		{
			if (failed || offset + sizeof(T) > data.size()) {
				failed = true;
				return T{};
			}
			T value;
			std::memcpy(&value, data.data() + offset, sizeof(T));
			offset += sizeof(T);
			return value;
		}

		std::span<const std::uint8_t> data;
		std::size_t                   offset = 0;
		bool                          failed = false;
	};

	inline std::optional<MessageType> PeekType(std::span<const std::uint8_t> a_data)
	{
		if (a_data.empty()) {
			return std::nullopt;
		}
		return static_cast<MessageType>(a_data[0]);
	}

	// Each Encode returns a complete packet; each Decode expects one (including the type byte)
	// and returns nullopt if it is malformed.

	inline void WriteState(Writer& a_writer, const PlayerState& a_state)
	{
		a_writer.U32(a_state.sequence);
		a_writer.U32(a_state.cell);
		a_writer.U32(a_state.worldspace);
		a_writer.F32(a_state.x);
		a_writer.F32(a_state.y);
		a_writer.F32(a_state.z);
		a_writer.F32(a_state.heading);
		a_writer.F32(a_state.speed);
		a_writer.U8(a_state.flags);
		a_writer.U32(a_state.sentMs);
		a_writer.U8(a_state.direction);
		a_writer.U8(static_cast<std::uint8_t>(a_state.pitch));
	}

	inline PlayerState ReadState(Reader& a_reader)
	{
		PlayerState state;
		state.sequence = a_reader.U32();
		state.cell = a_reader.U32();
		state.worldspace = a_reader.U32();
		state.x = a_reader.F32();
		state.y = a_reader.F32();
		state.z = a_reader.F32();
		state.heading = a_reader.F32();
		state.speed = a_reader.F32();
		state.flags = a_reader.U8();
		state.sentMs = a_reader.U32();
		state.direction = a_reader.U8();
		state.pitch = static_cast<std::int8_t>(a_reader.U8());
		return state;
	}

	// The bytes one name of a plugin list takes on the wire: its U16 length and its bytes, cut at MAX_PLUGIN_NAME_LENGTH.
	[[nodiscard]] inline std::size_t PluginNameBytes(std::string_view a_name)
	{
		return sizeof(std::uint16_t) + Utf8Prefix(a_name, MAX_PLUGIN_NAME_LENGTH).size();
	}

	// How many of a_plugins, from the first, a plugin list carries: at most a_max names in at most MAX_PLUGIN_LIST_BYTES.
	[[nodiscard]] inline std::size_t PluginsCarried(const std::vector<std::string>& a_plugins, std::size_t a_max = MAX_PLUGINS)
	{
		std::size_t bytes = sizeof(std::uint16_t);
		std::size_t count = 0;
		for (; count < a_plugins.size() && count < a_max; ++count) {
			bytes += PluginNameBytes(a_plugins[count]);
			if (bytes > MAX_PLUGIN_LIST_BYTES) {
				break;
			}
		}
		return count;
	}

	// The list goes on the wire whole and as it is: no name left off, none cut short. A game checks it before it joins
	// and says why it can't, rather than send a list the host would compare cut.
	[[nodiscard]] inline bool PluginsFit(const std::vector<std::string>& a_plugins, std::size_t a_max = MAX_PLUGINS)
	{
		return PluginsCarried(a_plugins, a_max) == a_plugins.size() &&
		       std::ranges::all_of(a_plugins, [](const std::string& a_name) { return a_name.size() <= MAX_PLUGIN_NAME_LENGTH; });
	}

	// A list of plugins: a U16 count, then each name (LongStr, at most MAX_PLUGIN_NAME_LENGTH bytes). At most a_max names
	// and MAX_PLUGIN_LIST_BYTES in all (PluginsCarried); past either the rest are left off, whole names only.
	inline void WritePlugins(Writer& a_writer, const std::vector<std::string>& a_plugins, std::size_t a_max = MAX_PLUGINS)
	{
		const auto count = PluginsCarried(a_plugins, a_max);
		a_writer.U16(static_cast<std::uint16_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			a_writer.LongStr(a_plugins[i], MAX_PLUGIN_NAME_LENGTH);
		}
	}

	// None for more than a_max names, a name too long or a list past MAX_PLUGIN_LIST_BYTES (the reader has failed then,
	// or it is no list WritePlugins writes).
	inline std::optional<std::vector<std::string>> ReadPlugins(Reader& a_reader, std::size_t a_max = MAX_PLUGINS)
	{
		const auto count = a_reader.U16();
		if (!a_reader.Ok() || count > a_max) {
			return std::nullopt;
		}
		std::vector<std::string> plugins;
		plugins.reserve(count);
		std::size_t bytes = sizeof(std::uint16_t);
		for (std::uint16_t i = 0; i < count && a_reader.Ok(); ++i) {
			plugins.push_back(a_reader.LongStr(MAX_PLUGIN_NAME_LENGTH));
			bytes += PluginNameBytes(plugins.back());
		}
		if (!a_reader.Ok() || bytes > MAX_PLUGIN_LIST_BYTES) {
			return std::nullopt;
		}
		return plugins;
	}

	inline std::vector<std::uint8_t> Encode(const Hello& a_msg)
	{
		Writer w{ MessageType::kHello };
		w.U32(a_msg.magic);
		w.U16(a_msg.version);
		w.U32(a_msg.contentHash);
		w.Str(a_msg.name, MAX_NAME_LENGTH);
		w.Str(a_msg.password, MAX_PASSWORD_LENGTH);
		w.U64(a_msg.identity);
		WritePlugins(w, a_msg.plugins);
		return w.Data();
	}

	// The most bytes Encode(Hello) writes: the fields before the list at their longest, then the longest list. The tests
	// encode that hello too, so a field added above without a change here is caught.
	inline constexpr std::size_t MAX_HELLO_SIZE = sizeof(std::uint8_t) + sizeof(std::uint32_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t) +
	                                              (1 + MAX_NAME_LENGTH) + (1 + MAX_PASSWORD_LENGTH) + sizeof(std::uint64_t) + MAX_PLUGIN_LIST_BYTES;
	static_assert(MAX_HELLO_SIZE <= MAX_MESSAGE_SIZE, "a hello goes in one message on every transport");
	static_assert(MAX_PLUGINS <= 0xFFFF, "a plugin list's count is a U16");

	// A hello of another version (or with another magic) is read up to its version only: the rest may be laid out
	// otherwise, and the server needs no more to say the versions differ.
	inline std::optional<Hello> DecodeHello(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Hello msg;
		msg.magic = r.U32();
		msg.version = r.U16();
		if (!r.Ok()) {
			return std::nullopt;
		}
		if (msg.magic != MAGIC || msg.version != VERSION) {
			return msg;
		}
		msg.contentHash = r.U32();
		msg.name = r.Str(MAX_NAME_LENGTH);
		msg.password = r.Str(MAX_PASSWORD_LENGTH);
		msg.identity = r.U64();
		auto plugins = ReadPlugins(r);
		if (!plugins || !r.AtEnd()) {
			return std::nullopt;
		}
		msg.plugins = std::move(*plugins);
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Welcome& a_msg)
	{
		Writer w{ MessageType::kWelcome };
		w.U32(a_msg.playerId);
		w.U64(a_msg.sessionId);
		w.U32(a_msg.hostId);
		return w.Data();
	}

	inline std::optional<Welcome> DecodeWelcome(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Welcome msg;
		msg.playerId = r.U32();
		msg.sessionId = r.U64();
		msg.hostId = r.U32();
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Reject& a_msg)
	{
		Writer w{ MessageType::kReject };
		w.Str(a_msg.reason, MAX_REASON_LENGTH);
		return w.Data();
	}

	inline std::optional<Reject> DecodeReject(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Reject msg;
		msg.reason = r.Str(MAX_REASON_LENGTH);
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerJoined& a_msg)
	{
		Writer w{ MessageType::kPlayerJoined };
		w.U32(a_msg.playerId);
		w.Str(a_msg.name, MAX_NAME_LENGTH);
		return w.Data();
	}

	inline std::optional<PlayerJoined> DecodePlayerJoined(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		PlayerJoined msg;
		msg.playerId = r.U32();
		msg.name = r.Str(MAX_NAME_LENGTH);
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerLeft& a_msg)
	{
		Writer w{ MessageType::kPlayerLeft };
		w.U32(a_msg.playerId);
		return w.Data();
	}

	inline std::optional<PlayerLeft> DecodePlayerLeft(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		PlayerLeft msg;
		msg.playerId = r.U32();
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerState& a_msg)
	{
		Writer w{ MessageType::kPlayerState };
		WriteState(w, a_msg);
		return w.Data();
	}

	// None for a state that is cut short, has bytes left over, or holds a position no game could be at.
	inline std::optional<PlayerState> DecodePlayerState(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		const auto state = ReadState(r);
		if (!r.AtEnd() || !state.IsFinite()) {
			return std::nullopt;
		}
		return state;
	}

	inline std::vector<std::uint8_t> Encode(const RemoteState& a_msg)
	{
		Writer w{ MessageType::kRemoteState };
		w.U32(a_msg.playerId);
		WriteState(w, a_msg.state);
		return w.Data();
	}

	inline std::optional<RemoteState> DecodeRemoteState(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		RemoteState msg;
		msg.playerId = r.U32();
		msg.state = ReadState(r);
		if (!r.AtEnd() || !msg.state.IsFinite()) {
			return std::nullopt;
		}
		return msg;
	}

	// a_type: kHeartbeat or kHeartbeatAck (the answer carries the token back).
	inline std::vector<std::uint8_t> Encode(const Heartbeat& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.token);
		return w.Data();
	}

	inline std::optional<Heartbeat> DecodeHeartbeat(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Heartbeat msg;
		msg.token = r.U32();
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline void WriteLooks(Writer& a_writer, const PlayerLooks& a_looks)
	{
		a_writer.U32(a_looks.race);
		a_writer.U8(a_looks.female ? 1 : 0);
		a_writer.F32(a_looks.weight);
		const auto count = (std::min)(a_looks.worn.size(), MAX_WORN);
		a_writer.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			a_writer.U32(a_looks.worn[i]);
		}
	}

	// None for more than MAX_WORN items, a sex byte other than 0 or 1, or a weight that is not 0 to 100 (another
	// player's game sent it: it would go onto a stand-in's body as it is).
	inline std::optional<PlayerLooks> ReadLooks(Reader& a_reader)
	{
		PlayerLooks looks;
		looks.race = a_reader.U32();
		const auto female = a_reader.U8();
		looks.weight = a_reader.F32();
		const auto count = a_reader.U8();
		if (!a_reader.Ok() || female > 1 || count > MAX_WORN || !looks.IsFinite() || looks.weight < 0.0f || looks.weight > 100.0f) {
			return std::nullopt;
		}
		looks.female = female == 1;
		looks.worn.reserve(count);
		for (std::uint8_t i = 0; i < count; ++i) {
			looks.worn.push_back(a_reader.U32());
		}
		if (!a_reader.Ok()) {
			return std::nullopt;
		}
		return looks;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerLooks& a_msg)
	{
		Writer w{ MessageType::kPlayerLooks };
		WriteLooks(w, a_msg);
		return w.Data();
	}

	inline std::optional<PlayerLooks> DecodePlayerLooks(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		auto looks = ReadLooks(r);
		if (!looks || !r.AtEnd()) {
			return std::nullopt;
		}
		return looks;
	}

	inline std::vector<std::uint8_t> Encode(const RemoteLooks& a_msg)
	{
		Writer w{ MessageType::kRemoteLooks };
		w.U32(a_msg.playerId);
		WriteLooks(w, a_msg.looks);
		return w.Data();
	}

	inline std::optional<RemoteLooks> DecodeRemoteLooks(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		RemoteLooks msg;
		msg.playerId = r.U32();
		auto looks = ReadLooks(r);
		if (!looks || !r.AtEnd()) {
			return std::nullopt;
		}
		msg.looks = std::move(*looks);
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const PluginTable& a_msg)
	{
		Writer w{ MessageType::kPluginTable };
		WritePlugins(w, a_msg.plugins);
		return w.Data();
	}

	inline std::optional<PluginTable> DecodePluginTable(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		auto plugins = ReadPlugins(r);
		if (!plugins || !r.AtEnd()) {
			return std::nullopt;
		}
		return PluginTable{ std::move(*plugins) };
	}

	// kRefState: refId, open, locked, lockLevel. kRemoteRefState: the same, then playerId. None for a part out of its
	// range (open or locked past kUnknown, a level past LEVEL_MAX that is not LEVEL_UNKNOWN).
	inline std::vector<std::uint8_t> Encode(const RefState& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.refId);
		w.U8(a_msg.open);
		w.U8(a_msg.locked);
		w.U8(a_msg.lockLevel);
		if (a_type == MessageType::kRemoteRefState) {
			w.U32(a_msg.playerId);
		}
		return w.Data();
	}
	inline std::vector<std::uint8_t> Encode(const RefState& a_msg) { return Encode(a_msg, MessageType::kRefState); }

	inline std::optional<RefState> DecodeRefState(std::span<const std::uint8_t> a_data)
	{
		const auto type = PeekType(a_data);
		if (type != MessageType::kRefState && type != MessageType::kRemoteRefState) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		RefState msg;
		msg.refId = r.U32();
		msg.open = r.U8();
		msg.locked = r.U8();
		msg.lockLevel = r.U8();
		if (type == MessageType::kRemoteRefState) {
			msg.playerId = r.U32();
		}
		if (!r.AtEnd() || msg.open > RefState::kUnknown || msg.locked > RefState::kUnknown ||
			(msg.lockLevel > RefState::LEVEL_MAX && msg.lockLevel != RefState::LEVEL_UNKNOWN)) {
			return std::nullopt;
		}
		return msg;
	}

	// kRefRefused: refId, reason, the three parts said, the three parts of the session's word.
	inline std::vector<std::uint8_t> Encode(const RefRefusal& a_msg, MessageType a_type = MessageType::kRefRefused)
	{
		Writer w{ a_type };
		w.U32(a_msg.refId);
		w.U8(a_msg.reason);
		w.U8(a_msg.saidOpen);
		w.U8(a_msg.saidLocked);
		w.U8(a_msg.saidLevel);
		w.U8(a_msg.sessionOpen);
		w.U8(a_msg.sessionLocked);
		w.U8(a_msg.sessionLevel);
		return w.Data();
	}

	inline std::optional<RefRefusal> DecodeRefRefusal(std::span<const std::uint8_t> a_data)
	{
		if (PeekType(a_data) != MessageType::kRefRefused) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		RefRefusal msg;
		msg.refId = r.U32();
		msg.reason = r.U8();
		msg.saidOpen = r.U8();
		msg.saidLocked = r.U8();
		msg.saidLevel = r.U8();
		msg.sessionOpen = r.U8();
		msg.sessionLocked = r.U8();
		msg.sessionLevel = r.U8();
		const auto part = [](std::uint8_t a_v) { return a_v <= RefState::kUnknown; };
		const auto level = [](std::uint8_t a_v) { return a_v <= RefState::LEVEL_MAX || a_v == RefState::LEVEL_UNKNOWN; };
		if (!r.AtEnd() || msg.reason > RefRefusal::REASON_MAX || !part(msg.saidOpen) || !part(msg.saidLocked) || !level(msg.saidLevel) ||
			!part(msg.sessionOpen) || !part(msg.sessionLocked) || !level(msg.sessionLevel)) {
			return std::nullopt;
		}
		return msg;
	}

	// kActivate: refId. kRemoteActivate: refId, then playerId.
	inline std::vector<std::uint8_t> Encode(const Activation& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		w.U32(a_msg.refId);
		if (a_type == MessageType::kRemoteActivate) {
			w.U32(a_msg.playerId);
		}
		return w.Data();
	}
	inline std::vector<std::uint8_t> Encode(const Activation& a_msg) { return Encode(a_msg, MessageType::kActivate); }

	inline std::optional<Activation> DecodeActivation(std::span<const std::uint8_t> a_data)
	{
		const auto type = PeekType(a_data);
		if (type != MessageType::kActivate && type != MessageType::kRemoteActivate) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		Activation msg;
		msg.refId = r.U32();
		if (type == MessageType::kRemoteActivate) {
			msg.playerId = r.U32();
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	// kQuests, kQuestWorld, kRemoteQuests, kQuestAnswer: playerId (kRemoteQuests only), a count (1 to MAX_QUEST_BATCH), then each
	// entry: questId, kind, index, value, stage. None for an empty batch, a kind not known, an objective state past
	// OBJECTIVE_STATE_MAX, or an answer past ANSWER_MAX. Encode writes at most MAX_QUEST_BATCH entries (Batches splits).
	inline std::vector<std::uint8_t> Encode(const QuestBatch& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		if (a_type == MessageType::kRemoteQuests) {
			w.U32(a_msg.playerId);
		}
		const auto count = (std::min)(a_msg.entries.size(), MAX_QUEST_BATCH);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			const auto& e = a_msg.entries[i];
			w.U32(e.questId);
			w.U8(e.kind);
			w.U16(e.index);
			w.U8(e.value);
			w.U16(e.stage);
		}
		return w.Data();
	}
	inline std::vector<std::uint8_t> Encode(const QuestBatch& a_msg) { return Encode(a_msg, MessageType::kQuests); }

	// a_entries in batches of MAX_QUEST_BATCH, in order, each encoded as a_type with a_playerId (none for no entries).
	inline std::vector<std::vector<std::uint8_t>> Batches(const std::vector<QuestEntry>& a_entries, MessageType a_type, std::uint32_t a_playerId = 0)
	{
		std::vector<std::vector<std::uint8_t>> out;
		for (std::size_t at = 0; at < a_entries.size(); at += MAX_QUEST_BATCH) {
			QuestBatch batch{ a_playerId, {} };
			const auto end = (std::min)(a_entries.size(), at + MAX_QUEST_BATCH);
			batch.entries.assign(a_entries.begin() + static_cast<std::ptrdiff_t>(at), a_entries.begin() + static_cast<std::ptrdiff_t>(end));
			out.push_back(Encode(batch, a_type));
		}
		return out;
	}

	inline std::optional<QuestBatch> DecodeQuestBatch(std::span<const std::uint8_t> a_data)
	{
		const auto type = PeekType(a_data);
		if (type != MessageType::kQuests && type != MessageType::kQuestWorld && type != MessageType::kRemoteQuests && type != MessageType::kQuestAnswer) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		QuestBatch msg;
		if (type == MessageType::kRemoteQuests) {
			msg.playerId = r.U32();
		}
		const auto count = r.U8();
		if (count == 0 || count > MAX_QUEST_BATCH) {
			return std::nullopt;
		}
		msg.entries.reserve(count);
		for (std::uint8_t i = 0; i < count; ++i) {
			QuestEntry e;
			e.questId = r.U32();
			e.kind = r.U8();
			e.index = r.U16();
			e.value = r.U8();
			e.stage = r.U16();
			if (e.kind > QuestEntry::KIND_MAX) {
				return std::nullopt;
			}
			const bool badValue = type == MessageType::kQuestAnswer ? e.value > QuestEntry::ANSWER_MAX
			                                                         : (e.kind == QuestEntry::kObjective && e.value > QuestEntry::OBJECTIVE_STATE_MAX);
			if (badValue) {
				return std::nullopt;
			}
			msg.entries.push_back(e);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	// kQuestReset: from the host, nothing more; from the server, the host's player id.
	inline std::vector<std::uint8_t> Encode(const QuestReset& a_msg, bool a_fromServer)
	{
		Writer w{ MessageType::kQuestReset };
		if (a_fromServer) {
			w.U32(a_msg.playerId);
		}
		return w.Data();
	}

	inline std::optional<QuestReset> DecodeQuestReset(std::span<const std::uint8_t> a_data, bool a_fromServer)
	{
		if (PeekType(a_data) != MessageType::kQuestReset) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		QuestReset msg;
		if (a_fromServer) {
			msg.playerId = r.U32();
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	// kQuestRun (no player id: the server knows whose it is) or kRemoteQuestRun (the host's player id first): questId,
	// run, flags, then with kFilled a count (0 to MAX_ALIAS_FILLS) and each fill: aliasId, refId, kind. Without kFilled no
	// fills are written. None for no quest, run 0, a flag not known, kFilled with kStopped, a kind not known, a form with
	// a kind that names none (or none with kRef or kLoc), or a count past MAX_ALIAS_FILLS. Encode writes at most
	// MAX_ALIAS_FILLS fills.
	inline std::vector<std::uint8_t> Encode(const QuestRun& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		if (a_type == MessageType::kRemoteQuestRun) {
			w.U32(a_msg.playerId);
		}
		w.U32(a_msg.questId);
		w.U32(a_msg.run);
		w.U8(a_msg.flags);
		if ((a_msg.flags & QuestRun::kFilled) != 0) {
			const auto count = (std::min)(a_msg.fills.size(), MAX_ALIAS_FILLS);
			w.U16(static_cast<std::uint16_t>(count));
			for (std::size_t i = 0; i < count; ++i) {
				const auto& f = a_msg.fills[i];
				w.U32(f.aliasId);
				w.U32(f.refId);
				w.U8(f.kind);
			}
		}
		return w.Data();
	}
	inline std::vector<std::uint8_t> Encode(const QuestRun& a_msg) { return Encode(a_msg, MessageType::kQuestRun); }

	inline std::optional<QuestRun> DecodeQuestRun(std::span<const std::uint8_t> a_data)
	{
		const auto type = PeekType(a_data);
		if (type != MessageType::kQuestRun && type != MessageType::kRemoteQuestRun) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		QuestRun msg;
		if (type == MessageType::kRemoteQuestRun) {
			msg.playerId = r.U32();
		}
		msg.questId = r.U32();
		msg.run = r.U32();
		msg.flags = r.U8();
		if (!r.Ok() || msg.questId == 0 || msg.run == 0 || (msg.flags & ~QuestRun::FLAGS_ALL) != 0 ||
			((msg.flags & QuestRun::kFilled) != 0 && (msg.flags & QuestRun::kStopped) != 0)) {
			return std::nullopt;
		}
		if ((msg.flags & QuestRun::kFilled) != 0) {
			const auto count = r.U16();
			if (!r.Ok() || count > MAX_ALIAS_FILLS) {
				return std::nullopt;
			}
			msg.fills.reserve(count);
			for (std::uint16_t i = 0; i < count; ++i) {
				AliasFill f;
				f.aliasId = r.U32();
				f.refId = r.U32();
				f.kind = r.U8();
				if (!r.Ok() || f.kind > AliasFill::KIND_MAX || (f.kind == AliasFill::kRef || f.kind == AliasFill::kLoc) != (f.refId != 0)) {
					return std::nullopt;
				}
				msg.fills.push_back(f);
			}
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	// A text with nothing to read in it (empty, or only spaces and the control characters the Reader drops).
	[[nodiscard]] inline bool BlankText(std::string_view a_text)
	{
		return std::ranges::all_of(a_text, [](char a_c) { return a_c == ' ' || static_cast<unsigned char>(a_c) < 0x20; });
	}

	// kLine (no player id: the server knows whose it is) or kRemoteLine (the talker's player id first): kind, speakerKind,
	// speaker, info, response, seq, then the name (Str), the text and the voice (LongStr). None for a kind or speaker kind
	// not known; a kSay with a blank text; a kEnd with a text, a recording or no line number; a line of the talker
	// themselves naming a reference, an info, a response, a name or a recording (their own line is the topic's text alone);
	// a response with no info; any text past its MAX_*.
	inline std::vector<std::uint8_t> Encode(const Line& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		if (a_type == MessageType::kRemoteLine) {
			w.U32(a_msg.playerId);
		}
		w.U8(a_msg.kind);
		w.U8(a_msg.speakerKind);
		w.U32(a_msg.speaker);
		w.U32(a_msg.info);
		w.U8(a_msg.response);
		w.U32(a_msg.seq);
		w.Str(a_msg.name, MAX_SPEAKER_NAME);
		w.LongStr(a_msg.text, MAX_LINE_TEXT);
		w.LongStr(a_msg.voice, MAX_VOICE_PATH);
		return w.Data();
	}
	inline std::vector<std::uint8_t> Encode(const Line& a_msg) { return Encode(a_msg, MessageType::kLine); }

	inline std::optional<Line> DecodeLine(std::span<const std::uint8_t> a_data)
	{
		const auto type = PeekType(a_data);
		if (type != MessageType::kLine && type != MessageType::kRemoteLine) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		Line msg;
		if (type == MessageType::kRemoteLine) {
			msg.playerId = r.U32();
		}
		msg.kind = r.U8();
		msg.speakerKind = r.U8();
		msg.speaker = r.U32();
		msg.info = r.U32();
		msg.response = r.U8();
		msg.seq = r.U32();
		msg.name = r.Str(MAX_SPEAKER_NAME);
		msg.text = r.LongStr(MAX_LINE_TEXT);
		msg.voice = r.LongStr(MAX_VOICE_PATH);
		if (!r.AtEnd() || msg.seq == 0 || (msg.kind != Line::kSay && msg.kind != Line::kEnd) ||
			(msg.speakerKind != Line::kTalker && msg.speakerKind != Line::kRef)) {
			return std::nullopt;
		}
		if (msg.kind == Line::kSay ? BlankText(msg.text) : (!msg.text.empty() || !msg.voice.empty())) {
			return std::nullopt;
		}
		if (msg.speakerKind == Line::kTalker && (msg.speaker != 0 || msg.info != 0 || msg.response != 0 || !msg.name.empty() || !msg.voice.empty())) {
			return std::nullopt;
		}
		if (msg.response != 0 && msg.info == 0) {
			return std::nullopt;
		}
		return msg;
	}

	// kTalk: ref, on (0 or 1). None for no NPC or an "on" that is neither.
	inline std::vector<std::uint8_t> Encode(const Talk& a_msg)
	{
		Writer w{ MessageType::kTalk };
		w.U32(a_msg.ref);
		w.U8(a_msg.on ? 1 : 0);
		return w.Data();
	}

	inline std::optional<Talk> DecodeTalk(std::span<const std::uint8_t> a_data)
	{
		if (PeekType(a_data) != MessageType::kTalk) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		Talk msg;
		msg.ref = r.U32();
		const auto on = r.U8();
		if (!r.AtEnd() || msg.ref == 0 || on > 1) {
			return std::nullopt;
		}
		msg.on = on == 1;
		return msg;
	}

	// kTalkAnswer: ref, holderId. None for no NPC or no holder (an answer always names who has it).
	inline std::vector<std::uint8_t> Encode(const TalkAnswer& a_msg)
	{
		Writer w{ MessageType::kTalkAnswer };
		w.U32(a_msg.ref);
		w.U32(a_msg.holderId);
		return w.Data();
	}

	inline std::optional<TalkAnswer> DecodeTalkAnswer(std::span<const std::uint8_t> a_data)
	{
		if (PeekType(a_data) != MessageType::kTalkAnswer) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		TalkAnswer msg;
		msg.ref = r.U32();
		msg.holderId = r.U32();
		if (!r.AtEnd() || msg.ref == 0 || msg.holderId == 0) {
			return std::nullopt;
		}
		return msg;
	}

	// kClock (no player id: the server knows whose it is) or kRemoteClock (the owner's player id first).
	inline std::vector<std::uint8_t> Encode(const ClockState& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		if (a_type == MessageType::kRemoteClock) {
			w.U32(a_msg.playerId);
		}
		w.F32(a_msg.gameHour);
		w.F32(a_msg.daysPassed);
		w.F32(a_msg.timeScale);
		w.U32(a_msg.worldspace);
		w.U32(a_msg.weather);
		return w.Data();
	}
	inline std::vector<std::uint8_t> Encode(const ClockState& a_msg) { return Encode(a_msg, MessageType::kClock); }

	// A clock no game could have (an hour out of the day, a day count or timescale out of range, not a number) is no
	// message: a game never moves its clock by it.
	inline std::optional<ClockState> DecodeClockState(std::span<const std::uint8_t> a_data)
	{
		const auto type = PeekType(a_data);
		if (type != MessageType::kClock && type != MessageType::kRemoteClock) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		ClockState msg;
		if (type == MessageType::kRemoteClock) {
			msg.playerId = r.U32();
		}
		msg.gameHour = r.F32();
		msg.daysPassed = r.F32();
		msg.timeScale = r.F32();
		msg.worldspace = r.U32();
		msg.weather = r.U32();
		if (!r.AtEnd() || !std::isfinite(msg.gameHour) || msg.gameHour < 0.0f || msg.gameHour >= 24.0f || !std::isfinite(msg.daysPassed) ||
			msg.daysPassed < 0.0f || msg.daysPassed > MAX_DAYS_PASSED || !std::isfinite(msg.timeScale) || msg.timeScale < 0.0f ||
			msg.timeScale > MAX_TIME_SCALE) {
			return std::nullopt;
		}
		return msg;
	}

	// kLoot: a count (1 to MAX_LOOT_BATCH), then each move: tag, holderId, itemId, count, kind. kRemoteLoot: the ask it
	// answers (0: none, moves passed on as they came), the count, then each: number, playerId, tag, holderId, itemId,
	// count, kind. kLootAnswer: the count, then each move as kLoot has it and its answer. None for an empty batch, a kind not known, an answer past ANSWER_MAX, a take or
	// put of a count out of 1 to MAX_LOOT_COUNT (or of no item, in kRemoteLoot), an emptied marker that names an item,
	// or a kRemoteLoot move with no number. Encode writes at most MAX_LOOT_BATCH moves (LootBatches splits).
	inline std::vector<std::uint8_t> Encode(const LootBatch& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		if (a_type == MessageType::kRemoteLoot) {
			w.U32(a_msg.ask);
		}
		const auto count = (std::min)(a_msg.entries.size(), MAX_LOOT_BATCH);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			const auto& e = a_msg.entries[i];
			if (a_type == MessageType::kRemoteLoot) {
				w.U32(e.number);
				w.U32(e.playerId);
			}
			w.U32(e.tag);
			w.U32(e.holderId);
			w.U32(e.itemId);
			w.U32(e.count);
			w.U8(e.kind);
			if (a_type == MessageType::kLootAnswer) {
				w.U8(e.answer);
			}
		}
		return w.Data();
	}
	inline std::vector<std::uint8_t> Encode(const LootBatch& a_msg) { return Encode(a_msg, MessageType::kLoot); }

	// a_entries in batches of MAX_LOOT_BATCH, in order, each encoded as a_type (none for no entries); a_ask: the ask a
	// kRemoteLoot catch-up answers (0: moves passed on as they came).
	inline std::vector<std::vector<std::uint8_t>> LootBatches(const std::vector<LootEntry>& a_entries, MessageType a_type, std::uint32_t a_ask = 0)
	{
		std::vector<std::vector<std::uint8_t>> out;
		for (std::size_t at = 0; at < a_entries.size(); at += MAX_LOOT_BATCH) {
			LootBatch  batch{ a_ask, {} };
			const auto end = (std::min)(a_entries.size(), at + MAX_LOOT_BATCH);
			batch.entries.assign(a_entries.begin() + static_cast<std::ptrdiff_t>(at), a_entries.begin() + static_cast<std::ptrdiff_t>(end));
			out.push_back(Encode(batch, a_type));
		}
		return out;
	}

	// Whether a move says something a game could make: a take or a put of an item, 1 to MAX_LOOT_COUNT of it; an emptied
	// marker of no item. The holder is not judged here (FormTranslate may have made it 0: the server refuses that).
	[[nodiscard]] inline bool LootMoveReadable(const LootEntry& a_e)
	{
		if (a_e.kind > LootEntry::KIND_MAX) {
			return false;
		}
		if (a_e.kind == LootEntry::kEmptied) {
			return a_e.itemId == 0;
		}
		return a_e.itemId != 0 && a_e.count != 0 && a_e.count <= MAX_LOOT_COUNT;
	}

	inline std::optional<LootBatch> DecodeLootBatch(std::span<const std::uint8_t> a_data)
	{
		const auto type = PeekType(a_data);
		if (type != MessageType::kLoot && type != MessageType::kRemoteLoot && type != MessageType::kLootAnswer) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		LootBatch msg;
		if (type == MessageType::kRemoteLoot) {
			msg.ask = r.U32();
		}
		const auto count = r.U8();
		if (count == 0 || count > MAX_LOOT_BATCH) {
			return std::nullopt;
		}
		msg.entries.reserve(count);
		for (std::uint8_t i = 0; i < count; ++i) {
			LootEntry e;
			if (type == MessageType::kRemoteLoot) {
				e.number = r.U32();
				e.playerId = r.U32();
			}
			e.tag = r.U32();
			e.holderId = r.U32();
			e.itemId = r.U32();
			e.count = r.U32();
			e.kind = r.U8();
			if (type == MessageType::kLootAnswer) {
				e.answer = r.U8();
				if (e.answer > LootEntry::ANSWER_MAX) {
					return std::nullopt;
				}
			}
			// A game's move and its answer are read even for an item no form names (0: FormTranslate found none on the
			// other side): the server refuses it, and every move a game sends is answered once.
			auto judged = e;
			if (type != MessageType::kRemoteLoot && judged.kind != LootEntry::kEmptied && judged.itemId == 0) {
				judged.itemId = 1;
			}
			if (!LootMoveReadable(judged) || (type == MessageType::kRemoteLoot && e.number == 0)) {
				return std::nullopt;
			}
			msg.entries.push_back(e);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const LootAsk& a_msg)
	{
		Writer w{ MessageType::kAskLoot };
		w.U32(a_msg.ask);
		w.U64(a_msg.logId);
		w.U32(a_msg.from);
		return w.Data();
	}

	// None for an ask of 0 or a from of 0 (the log's numbers start at 1).
	inline std::optional<LootAsk> DecodeLootAsk(std::span<const std::uint8_t> a_data)
	{
		if (PeekType(a_data) != MessageType::kAskLoot) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		LootAsk msg;
		msg.ask = r.U32();
		msg.logId = r.U64();
		msg.from = r.U32();
		if (!r.AtEnd() || msg.ask == 0 || msg.from == 0) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const LootCaughtUp& a_msg)
	{
		Writer w{ MessageType::kLootCaughtUp };
		w.U32(a_msg.ask);
		w.U64(a_msg.logId);
		w.U32(a_msg.from);
		w.U32(a_msg.next);
		return w.Data();
	}

	// None for an ask of 0, a number of 0 (the log's numbers start at 1) or a from past next.
	inline std::optional<LootCaughtUp> DecodeLootCaughtUp(std::span<const std::uint8_t> a_data)
	{
		if (PeekType(a_data) != MessageType::kLootCaughtUp) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		LootCaughtUp msg;
		msg.ask = r.U32();
		msg.logId = r.U64();
		msg.from = r.U32();
		msg.next = r.U32();
		if (!r.AtEnd() || msg.ask == 0 || msg.next == 0 || msg.from == 0 || msg.from > msg.next) {

			return std::nullopt;
		}
		return msg;
	}

	// ---- dropped items ----------------------------------------------------------------------------------------------

	namespace detail
	{
		inline void WriteDrop(Writer& a_w, const DroppedItem& a_d)
		{
			a_w.U32(a_d.dropId);
			a_w.U32(a_d.ownerId);
			a_w.U64(a_d.nonce);
			a_w.U32(a_d.base);
			a_w.U16(static_cast<std::uint16_t>((std::min)(a_d.count, MAX_DROP_COUNT)));
			a_w.F32(a_d.health);
			a_w.U32(a_d.enchantment);
			a_w.U16(a_d.enchantmentMax);
			a_w.F32(a_d.charge);
			a_w.U32(a_d.poison);
			a_w.U16(static_cast<std::uint16_t>((std::min<std::uint32_t>)(a_d.poisonCount, 0xFFFF)));
			a_w.U8(a_d.soul);
			a_w.Str(a_d.name, MAX_ITEM_NAME_LENGTH);
			a_w.U32(a_d.cell);
			a_w.U32(a_d.worldspace);
			a_w.F32(a_d.x);
			a_w.F32(a_d.y);
			a_w.F32(a_d.z);
			a_w.F32(a_d.angle);
		}

		// Whether a_v is none (-1) or an amount a game holds (a tempered item's health, a weapon's charge).
		[[nodiscard]] inline bool DropAmount(float a_v)
		{
			return a_v == -1.0f || (std::isfinite(a_v) && a_v >= 0.0f && a_v < 1.0e6f);
		}

		// None for a drop no game could place: no nonce, a count out of 1 to MAX_DROP_COUNT, a soul past MAX_SOUL, a
		// health or charge that is neither none (-1) nor an amount, a place out of the world, or a heading that is no
		// number. A base of 0 is read (FormTranslate's "this game lacks it"): the server refuses such a report, and a game
		// places no copy of it.
		inline std::optional<DroppedItem> ReadDrop(Reader& a_r)
		{
			DroppedItem d;
			d.dropId = a_r.U32();
			d.ownerId = a_r.U32();
			d.nonce = a_r.U64();
			d.base = a_r.U32();
			d.count = a_r.U16();
			d.health = a_r.F32();
			d.enchantment = a_r.U32();
			d.enchantmentMax = a_r.U16();
			d.charge = a_r.F32();
			d.poison = a_r.U32();
			d.poisonCount = a_r.U16();
			d.soul = a_r.U8();
			d.name = a_r.Str(MAX_ITEM_NAME_LENGTH);
			d.cell = a_r.U32();
			d.worldspace = a_r.U32();
			d.x = a_r.F32();
			d.y = a_r.F32();
			d.z = a_r.F32();
			d.angle = a_r.F32();
			if (!a_r.Ok() || d.nonce == 0 || d.count == 0 || d.count > MAX_DROP_COUNT || d.soul > MAX_SOUL || !DropAmount(d.health) ||
				!DropAmount(d.charge) || !InWorld(d.x) || !InWorld(d.y) || !InWorld(d.z) || !std::isfinite(d.angle) || std::fabs(d.angle) > 100.0f) {
				return std::nullopt;
			}
			return d;
		}
	}

	// kReportDrop, kDropPlaced: one drop.
	inline std::vector<std::uint8_t> Encode(const DroppedItem& a_msg, MessageType a_type)
	{
		Writer w{ a_type };
		detail::WriteDrop(w, a_msg);
		return w.Data();
	}

	inline std::optional<DroppedItem> DecodeDroppedItem(std::span<const std::uint8_t> a_data)
	{
		const auto type = PeekType(a_data);
		if (type != MessageType::kReportDrop && type != MessageType::kDropPlaced) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		auto drop = detail::ReadDrop(r);
		if (!drop || !r.AtEnd()) {
			return std::nullopt;
		}
		return drop;
	}

	inline std::vector<std::uint8_t> Encode(const ClaimDrop& a_msg)
	{
		Writer w{ MessageType::kClaimDrop };
		w.U32(a_msg.dropId);
		w.U8(static_cast<std::uint8_t>(a_msg.op));
		return w.Data();
	}

	// None for a drop ID of 0 or a claim not known.
	inline std::optional<ClaimDrop> DecodeClaimDrop(std::span<const std::uint8_t> a_data)
	{
		if (PeekType(a_data) != MessageType::kClaimDrop) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		ClaimDrop  msg;
		msg.dropId = r.U32();
		const auto op = r.U8();
		if (!r.AtEnd() || msg.dropId == 0 || op > static_cast<std::uint8_t>(DropClaim::kKept)) {
			return std::nullopt;
		}
		msg.op = static_cast<DropClaim>(op);
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const DropGone& a_msg)
	{
		Writer w{ MessageType::kDropGone };
		w.U32(a_msg.dropId);
		w.U32(a_msg.takerId);
		return w.Data();
	}

	inline std::optional<DropGone> DecodeDropGone(std::span<const std::uint8_t> a_data)
	{
		if (PeekType(a_data) != MessageType::kDropGone) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		DropGone msg;
		msg.dropId = r.U32();
		msg.takerId = r.U32();
		if (!r.AtEnd() || msg.dropId == 0) {
			return std::nullopt;
		}
		return msg;
	}

	// kDropRecord: the session, then the made (at most MAX_DROP_RECORD, the newest kept), then the taken (likewise).
	inline std::vector<std::uint8_t> Encode(const DropRecord& a_msg)
	{
		Writer     w{ MessageType::kDropRecord };
		const auto newest = [](std::size_t a_size) { return a_size > MAX_DROP_RECORD ? a_size - MAX_DROP_RECORD : 0; };
		w.U64(a_msg.sessionId);
		const auto madeFrom = newest(a_msg.made.size());
		w.U16(static_cast<std::uint16_t>(a_msg.made.size() - madeFrom));
		for (std::size_t i = madeFrom; i < a_msg.made.size(); ++i) {
			w.U64(a_msg.made[i]);
		}
		const auto takenFrom = newest(a_msg.taken.size());
		w.U16(static_cast<std::uint16_t>(a_msg.taken.size() - takenFrom));
		for (std::size_t i = takenFrom; i < a_msg.taken.size(); ++i) {
			w.U32(a_msg.taken[i]);
		}
		return w.Data();
	}

	inline std::optional<DropRecord> DecodeDropRecord(std::span<const std::uint8_t> a_data)
	{
		if (PeekType(a_data) != MessageType::kDropRecord) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		DropRecord msg;
		msg.sessionId = r.U64();
		const auto made = r.U16();
		if (!r.Ok() || made > MAX_DROP_RECORD) {
			return std::nullopt;
		}
		msg.made.reserve(made);
		for (std::uint16_t i = 0; i < made; ++i) {
			msg.made.push_back(r.U64());
		}
		const auto taken = r.U16();
		if (!r.Ok() || taken > MAX_DROP_RECORD) {
			return std::nullopt;
		}
		msg.taken.reserve(taken);
		for (std::uint16_t i = 0; i < taken; ++i) {
			msg.taken.push_back(r.U32());
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	// kDropList: last (0 or 1), a count (0 to MAX_DROPS_PER_PACKET), each drop. Encode writes at most
	// MAX_DROPS_PER_PACKET drops (DropLists splits).
	inline std::vector<std::uint8_t> Encode(const DropList& a_msg)
	{
		Writer     w{ MessageType::kDropList };
		const auto count = (std::min)(a_msg.drops.size(), MAX_DROPS_PER_PACKET);
		w.U8(a_msg.last ? 1 : 0);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			detail::WriteDrop(w, a_msg.drops[i]);
		}
		return w.Data();
	}

	// None for a drop in it that has no number.
	inline std::optional<DropList> DecodeDropList(std::span<const std::uint8_t> a_data)
	{
		if (PeekType(a_data) != MessageType::kDropList) {
			return std::nullopt;
		}
		Reader r{ a_data };
		r.U8();
		DropList   msg;
		const auto last = r.U8();
		const auto count = r.U8();
		if (!r.Ok() || last > 1 || count > MAX_DROPS_PER_PACKET) {
			return std::nullopt;
		}
		msg.last = last == 1;
		msg.drops.reserve(count);
		for (std::uint8_t i = 0; i < count; ++i) {
			auto drop = detail::ReadDrop(r);
			if (!drop || drop->dropId == 0) {
				return std::nullopt;
			}
			msg.drops.push_back(std::move(*drop));
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	// a_drops in packets of MAX_DROPS_PER_PACKET, the last marked last; one empty packet for none (the list's end still
	// says nothing lies there).
	inline std::vector<std::vector<std::uint8_t>> DropLists(const std::vector<DroppedItem>& a_drops)
	{
		std::vector<std::vector<std::uint8_t>> out;
		std::size_t                            at = 0;
		do {
			DropList   list;
			const auto end = (std::min)(a_drops.size(), at + MAX_DROPS_PER_PACKET);
			list.drops.assign(a_drops.begin() + static_cast<std::ptrdiff_t>(at), a_drops.begin() + static_cast<std::ptrdiff_t>(end));
			list.last = end == a_drops.size();
			out.push_back(Encode(list));
			at = end;
		} while (at < a_drops.size());
		return out;
	}

	// ---- load orders ---------------------------------------------------------------

	// In a hello's load order, between the full plugins and the light ones (.esl files, and plugins flagged light).
	// The two kinds are numbered apart: a full plugin's form IDs start with its place among the full ones (09000004),
	// a light one's with FE and its place among the light ones.
	inline constexpr std::string_view LIGHT_PLUGINS = "--light--";

	[[nodiscard]] inline bool SamePluginName(std::string_view a_a, std::string_view a_b)
	{
		return std::ranges::equal(a_a, a_b, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
	}

	// A plugin's name as SamePluginName compares it, for looking names up rather than searching a list for them.
	[[nodiscard]] inline std::string LowerPluginName(std::string_view a_name)
	{
		std::string out(a_name);
		for (auto& c : out) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return out;
	}

	// A game's load order as its hello carries it: the full plugins, LIGHT_PLUGINS, the light ones.
	[[nodiscard]] inline std::vector<std::string> ComparedLoadOrder(std::vector<std::string> a_full, const std::vector<std::string>& a_light)
	{
		auto out = std::move(a_full);
		out.emplace_back(LIGHT_PLUGINS);
		out.insert(out.end(), a_light.begin(), a_light.end());
		return out;
	}

	// FNV-1a over a load order (ComparedLoadOrder), names in any case: the hello's content hash. Players whose hashes
	// match play together without the lists being compared.
	[[nodiscard]] inline std::uint32_t LoadOrderHash(const std::vector<std::string>& a_plugins)
	{
		std::uint32_t hash = 2166136261u;
		for (const auto& name : a_plugins) {
			for (const char c : name) {
				hash ^= static_cast<std::uint8_t>(std::tolower(static_cast<unsigned char>(c)));
				hash *= 16777619u;
			}
			hash ^= '|';
			hash *= 16777619u;
		}
		return hash;
	}
}
