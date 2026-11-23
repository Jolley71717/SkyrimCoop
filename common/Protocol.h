#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <compare>
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
	// 5: NPCs are shared: one game runs each (kNpcClaims, kNpcOwners) and the others show a copy of it (kNpcStates,
	//    kNpcActions, kNpcGear); a state's kFlying.
	inline constexpr std::uint16_t VERSION = 5;
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
		kNpcClaims = 13,    // client -> server, reliable: NPCs this game asks to run, or lets go of
		kNpcOwners = 14,    // server -> client, reliable: who runs each NPC (every one to a joiner, then each change)
		kNpcStates = 15,    // both ways, on the state channel: where the NPCs a game runs are (the server names the runner)
		kNpcActions = 16,   // both ways, reliable: the graph events of the NPCs a game runs, for their copies to play
		kNpcGear = 17,      // both ways, reliable: what an NPC wears and holds, from the game that runs it
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
		kFlying = 1 << 6,  // an NPC in flight (a dragon): its height and pitch are its own, not the ground's
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

	// ---- NPCs (docs/DESIGN.md, "NPCs") ----------------------------------------------
	// Each NPC near a player is run by one game, its AI included; the others show a copy of it, moved by the runner's
	// states, its graph's events and its gear. The server keeps who runs each (common/NpcBook.h).

	// At most this many claims in one kNpcClaims, states in one kNpcStates (a batch of about 1 KB, so one lost packet
	// loses little), events in one kNpcActions, and NPCs run in a session at once (the server refuses claims past it).
	inline constexpr std::size_t MAX_NPC_CLAIMS = 128;
	inline constexpr std::size_t MAX_NPC_STATES = 20;
	inline constexpr std::size_t MAX_NPC_ACTIONS = 64;
	inline constexpr std::size_t MAX_NPCS = 4096;
	// A graph event's name: Skyrim's are short ("attackStart", "bowAttackStart", "FlyStartCruise").
	inline constexpr std::size_t MAX_EVENT_NAME = 48;

	// An NPC as every game names it. spawner 0: a reference of the load order (id in the session's numbering: two games
	// that both have it agree on it). Else id is a reference that player's game made as it ran (FF: a summoned atronach,
	// a conjured familiar), which no other game has: the others place a copy of it from its base, and only that player's
	// game ever runs it. Never 0.
	struct NpcKey
	{
		std::uint32_t id = 0;
		std::uint32_t spawner = 0;

		[[nodiscard]] bool IsSpawn() const { return spawner != 0; }
		auto operator<=>(const NpcKey&) const = default;
	};

	enum class ClaimWhy : std::uint8_t
	{
		kHave = 0,      // loaded here and nobody runs it: ours if nobody else has asked first
		kFight = 1,     // our player fights or talks to it: ours unless it changed hands or its runner fought it lately
		kRelease = 2,   // we let go of it (unloaded here, or our player went far): someone else's to claim
		kFollower = 3,  // it follows our player: ours, unless it also follows its runner's player
	};
	inline constexpr std::uint8_t CLAIM_WHY_COUNT = 4;

	struct NpcClaim
	{
		NpcKey   key;
		ClaimWhy why = ClaimWhy::kHave;
		bool operator==(const NpcClaim&) const = default;
	};

	struct NpcClaims
	{
		std::vector<NpcClaim> claims;
		bool operator==(const NpcClaims&) const = default;
	};

	// owner 0: nobody runs it now.
	struct NpcOwner
	{
		NpcKey        key;
		std::uint32_t owner = 0;
		bool operator==(const NpcOwner&) const = default;
	};

	// full: every NPC someone runs (a joiner's first): a game forgets owners the list doesn't name. Else changes only.
	struct NpcOwners
	{
		bool                  full = false;
		std::vector<NpcOwner> owners;
		bool operator==(const NpcOwners&) const = default;
	};

	// One NPC's state: base is the NPC its game made it of (its leveled pick's, for one placed from a leveled list; 0
	// when not known), which a spawn's copy is placed from and a copy's look is checked against. state.sequence counts
	// that NPC's states.
	struct NpcState
	{
		NpcKey        key;
		std::uint32_t base = 0;
		PlayerState   state;
		bool operator==(const NpcState&) const = default;
	};

	// runner: 0 from a game; the server names the game that sent them.
	struct NpcStates
	{
		std::uint32_t         runner = 0;
		std::vector<NpcState> npcs;
		bool operator==(const NpcStates&) const = default;
	};

	// An event its runner's graph took, when (the runner's clock, as PlayerState::sentMs), for a copy to play at the
	// same point of its states.
	struct NpcAction
	{
		NpcKey        key;
		std::uint32_t sentMs = 0;
		std::string   event;
		bool operator==(const NpcAction&) const = default;
	};

	struct NpcActions
	{
		std::uint32_t          runner = 0;
		std::vector<NpcAction> actions;
		bool operator==(const NpcActions&) const = default;
	};

	// What an NPC wears (armor, as base form ids, at most MAX_WORN) and holds: each hand's weapon, spell, shield or
	// torch, and its ammunition; 0 for none.
	struct NpcGear
	{
		std::uint32_t              runner = 0;
		NpcKey                     key;
		std::vector<std::uint32_t> worn;
		std::uint32_t              right = 0;
		std::uint32_t              left = 0;
		std::uint32_t              ammo = 0;
		bool operator==(const NpcGear&) const = default;
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

	inline void WriteKey(Writer& a_writer, const NpcKey& a_key)
	{
		a_writer.U32(a_key.id);
		a_writer.U32(a_key.spawner);
	}

	inline NpcKey ReadKey(Reader& a_reader)
	{
		NpcKey key;
		key.id = a_reader.U32();
		key.spawner = a_reader.U32();
		return key;
	}

	inline std::vector<std::uint8_t> Encode(const NpcClaims& a_msg)
	{
		Writer     w{ MessageType::kNpcClaims };
		const auto count = (std::min)(a_msg.claims.size(), MAX_NPC_CLAIMS);
		w.U16(static_cast<std::uint16_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			WriteKey(w, a_msg.claims[i].key);
			w.U8(static_cast<std::uint8_t>(a_msg.claims[i].why));
		}
		return w.Data();
	}

	// None for more than MAX_NPC_CLAIMS, a claim of no NPC (id 0) or a reason this version does not know.
	inline std::optional<NpcClaims> DecodeNpcClaims(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		const auto count = r.U16();
		if (!r.Ok() || count > MAX_NPC_CLAIMS) {
			return std::nullopt;
		}
		NpcClaims msg;
		msg.claims.reserve(count);
		for (std::uint16_t i = 0; i < count && r.Ok(); ++i) {
			NpcClaim claim;
			claim.key = ReadKey(r);
			const auto why = r.U8();
			if (claim.key.id == 0 || why >= CLAIM_WHY_COUNT) {
				return std::nullopt;
			}
			claim.why = static_cast<ClaimWhy>(why);
			msg.claims.push_back(claim);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const NpcOwners& a_msg)
	{
		Writer     w{ MessageType::kNpcOwners };
		const auto count = (std::min)(a_msg.owners.size(), MAX_NPCS);
		w.U8(a_msg.full ? 1 : 0);
		w.U16(static_cast<std::uint16_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			WriteKey(w, a_msg.owners[i].key);
			w.U32(a_msg.owners[i].owner);
		}
		return w.Data();
	}

	inline std::optional<NpcOwners> DecodeNpcOwners(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		const auto full = r.U8();
		const auto count = r.U16();
		if (!r.Ok() || full > 1 || count > MAX_NPCS) {
			return std::nullopt;
		}
		NpcOwners msg;
		msg.full = full == 1;
		msg.owners.reserve(count);
		for (std::uint16_t i = 0; i < count && r.Ok(); ++i) {
			NpcOwner owner;
			owner.key = ReadKey(r);
			owner.owner = r.U32();
			if (owner.key.id == 0) {
				return std::nullopt;
			}
			msg.owners.push_back(owner);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const NpcStates& a_msg)
	{
		Writer     w{ MessageType::kNpcStates };
		const auto count = (std::min)(a_msg.npcs.size(), MAX_NPC_STATES);
		w.U32(a_msg.runner);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			WriteKey(w, a_msg.npcs[i].key);
			w.U32(a_msg.npcs[i].base);
			WriteState(w, a_msg.npcs[i].state);
		}
		return w.Data();
	}

	// None for more than MAX_NPC_STATES, an NPC of id 0 or a place no game could be at.
	inline std::optional<NpcStates> DecodeNpcStates(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		NpcStates msg;
		msg.runner = r.U32();
		const auto count = r.U8();
		if (!r.Ok() || count > MAX_NPC_STATES) {
			return std::nullopt;
		}
		msg.npcs.reserve(count);
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			NpcState npc;
			npc.key = ReadKey(r);
			npc.base = r.U32();
			npc.state = ReadState(r);
			if (r.Ok() && (npc.key.id == 0 || !npc.state.IsFinite())) {
				return std::nullopt;
			}
			msg.npcs.push_back(npc);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const NpcActions& a_msg)
	{
		Writer     w{ MessageType::kNpcActions };
		const auto count = (std::min)(a_msg.actions.size(), MAX_NPC_ACTIONS);
		w.U32(a_msg.runner);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			WriteKey(w, a_msg.actions[i].key);
			w.U32(a_msg.actions[i].sentMs);
			w.Str(a_msg.actions[i].event, MAX_EVENT_NAME);
		}
		return w.Data();
	}

	// None for more than MAX_NPC_ACTIONS, an NPC of id 0, or an empty or too long event name.
	inline std::optional<NpcActions> DecodeNpcActions(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		NpcActions msg;
		msg.runner = r.U32();
		const auto count = r.U8();
		if (!r.Ok() || count > MAX_NPC_ACTIONS) {
			return std::nullopt;
		}
		msg.actions.reserve(count);
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			NpcAction action;
			action.key = ReadKey(r);
			action.sentMs = r.U32();
			action.event = r.Str(MAX_EVENT_NAME);
			if (r.Ok() && (action.key.id == 0 || action.event.empty())) {
				return std::nullopt;
			}
			msg.actions.push_back(std::move(action));
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const NpcGear& a_msg)
	{
		Writer w{ MessageType::kNpcGear };
		w.U32(a_msg.runner);
		WriteKey(w, a_msg.key);
		const auto count = (std::min)(a_msg.worn.size(), MAX_WORN);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			w.U32(a_msg.worn[i]);
		}
		w.U32(a_msg.right);
		w.U32(a_msg.left);
		w.U32(a_msg.ammo);
		return w.Data();
	}

	// None for more than MAX_WORN worn items. An NPC of id 0 reads: the translation left it out (FormTranslate.h), and
	// the reader skips it.
	inline std::optional<NpcGear> DecodeNpcGear(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		NpcGear msg;
		msg.runner = r.U32();
		msg.key = ReadKey(r);
		const auto count = r.U8();
		if (!r.Ok() || count > MAX_WORN) {
			return std::nullopt;
		}
		msg.worn.reserve(count);
		for (std::uint8_t i = 0; i < count; ++i) {
			msg.worn.push_back(r.U32());
		}
		msg.right = r.U32();
		msg.left = r.U32();
		msg.ammo = r.U32();
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	// The most bytes each NPC message can be, every list at its longest (the tests encode those): each fits one message.
	inline constexpr std::size_t KEY_BYTES = 8;
	inline constexpr std::size_t STATE_BYTES = 4 + 4 + 4 + 4 * 5 + 1 + 4 + 1 + 1;
	inline constexpr std::size_t MAX_NPC_OWNERS_SIZE = 1 + 1 + 2 + MAX_NPCS * (KEY_BYTES + 4);
	inline constexpr std::size_t MAX_NPC_STATES_SIZE = 1 + 4 + 1 + MAX_NPC_STATES * (KEY_BYTES + 4 + STATE_BYTES);
	inline constexpr std::size_t MAX_NPC_ACTIONS_SIZE = 1 + 4 + 1 + MAX_NPC_ACTIONS * (KEY_BYTES + 4 + 1 + MAX_EVENT_NAME);
	static_assert(MAX_NPC_OWNERS_SIZE <= MAX_MESSAGE_SIZE && MAX_NPC_ACTIONS_SIZE <= MAX_MESSAGE_SIZE);
	static_assert(MAX_NPC_STATES_SIZE <= 1200, "a batch of NPC states stays about one datagram");
	static_assert(MAX_NPCS <= 0xFFFF && MAX_NPC_CLAIMS <= 0xFFFF && MAX_NPC_STATES <= 0xFF && MAX_NPC_ACTIONS <= 0xFF);

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
