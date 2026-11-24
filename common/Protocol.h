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
	// 5: doors, locks and mechanisms are shared (message types 13-17, in their own change).
	// 6: NPCs are shared: one game runs each (kNpcClaims, kNpcOwners) and the others show a copy of it (kNpcStates,
	//    kNpcActions, kNpcGear: message types 18-22); a state's kFlying. Of the doors' change and this one, whichever
	//    lands second takes the number after the other's.
	// 8: NPC health and deaths agree in every game: a player's hits on a copy go to the NPC's runner as numbered requests
	//    it answers (kNpcHits, kNpcHitAnswers); the runner says each NPC's health (kNpcHealths) and its death
	//    (kNpcDeaths): message types 35-38. (7 is the shared quests' change; whichever lands later renumbers.)
	// 9: a friend's stand-in does what they do: the graph events of each player's actions (kPlayerActions,
	//    kRemoteActions: message types 39-40), and the looks say what each hand, the quiver and the voice hold.
	//    (Another change that lands first with 9 takes it; this one renumbers then.)
	// 10: a friend's stand-in has their face: head parts, hair colour, complexion, skin, height, face sliders and tint
	//    layers (kPlayerFace, kRemoteFace: message types 41-42). (As with 9: whichever change lands later renumbers.)
	// 11: a state says whether the one it is of swims with its head under water (StateFlags::kUnderwater), for the dive
	//    its stand-in or copy shows. (As with 9: whichever change lands later renumbers.)
	// 12: who rides what (kRides, message type 55): a player's horse, and the mounts of the NPCs a game runs, so the others
	//    show the rider in its saddle. (As with 9: whichever change lands later renumbers.)
	// 13: every projectile an actor launches (kShots, message type 57): a player's arrows, bolts, spells and shouts, and
	//    those of the NPCs a game runs, so the others launch the same from the actor that shows the shooter, at the
	//    shooter's origin and aim. (As with 9: whichever change lands later renumbers.)
	// 14: who sits, lies, leans or works in what furniture (kSits, message type 58): a player's chair, bed or crafting
	//    station, and those of the NPCs a game runs, so the others put the actor that shows them in the same one. (As
	//    with 9: whichever change lands later renumbers.)
	// 21: an NPC's gear says its voice too (NpcGear::voice: the shout or power it has ready), so a copy readies what its
	//    runner's NPC has, as it holds the spells in its hands. (The two lines of changes waiting to land each numbered
	//    their own; this one is renumbered when it lands.)
	// 24: paired animations (kPairs, message type 76): a kill move, a sneak kill, a creature's or a dragon's kill move, a
	//    vampire's feeding: the game that runs the actor that begins one says it, and every other game plays the same
	//    pair on the actors that show both there. (As with 21: renumbered when it lands; 22 and 23 are other changes'.)
	// 26: an NPC's state carries its graph's flight values (NpcGraph: TurnDelta, PitchDelta, TargetSpeed), which the
	//    engine feeds a graph from the actor's movement and a copy has none of, so a copied dragon banks, climbs and
	//    dives as its runner's does; and whether it is a body that died flying and still crashes (NpcGraph::crash).
	//    (As with 21: renumbered when it lands; 25 is another change's.)
	inline constexpr std::uint16_t VERSION = 26;
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
		// 13-17: the doors' messages (protocol 5).
		kNpcClaims = 18,    // client -> server, reliable: NPCs this game asks to run, or lets go of
		kNpcOwners = 19,    // server -> client, reliable: who runs each NPC (every one to a joiner, then each change)
		kNpcStates = 20,    // both ways, on the state channel: where the NPCs a game runs are (the server names the runner)
		kNpcActions = 21,   // both ways, reliable: the graph events of the NPCs a game runs, for their copies to play
		kNpcGear = 22,      // both ways, reliable: what an NPC wears and holds, from the game that runs it
		// 24-29: the doors' messages; 30-34: the shared quests' (each in its own change).
		kNpcHits = 35,        // client -> server -> the NPC's runner alone, reliable: our player's hits on copies, numbered
		kNpcHitAnswers = 36,  // runner (or the server) -> server -> the hitter, reliable: each hit applied, or refused and why
		kNpcHealths = 37,     // runner -> server -> everyone, reliable: the health of NPCs it runs (to a joiner: the hurt ones)
		kNpcDeaths = 38,      // runner -> server -> everyone, reliable: NPCs it runs that died (to a joiner: every one dead)
		kPlayerActions = 39,  // client -> server, reliable: our player's graph events (attacks, blocks, casts, shouts...)
		kRemoteActions = 40,  // server -> the others, reliable: that player's, for their stand-in to play
		kPlayerFace = 41,     // client -> server, reliable: our character's face (again whenever it changes and settles)
		kRemoteFace = 42,     // server -> the others (and to a joiner, each one there): that player's face
		// 43: free; 44-48: the shared loot's, 49-54: the dropped items' (each in its own change).
		kRides = 55,  // both ways, reliable: who rides what (a player, the NPCs a game runs); the server names the sender
		// 56: the quest runs' (in its own change).
		kShots = 57,  // both ways, reliable: projectiles our player and the NPCs a game runs launched; the server names the sender
		kSits = 58,   // both ways, reliable: who is in what furniture (a player, the NPCs a game runs); the server names the sender
		// 59-75: the other changes' messages (each in its own change).
		kPairs = 76,  // both ways, reliable: paired animations (kill moves) our player and the NPCs a game runs began
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
		kUnderwater = 1 << 7,  // swimming with its head under water: its pitch is the one it swims at (Gait::DivePitch)
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

	// A hand (or the quiver, or the voice) holds something the sending game could not name to this one (a form made as
	// that game ran, or a mod's this game lacks), which is not the same as holding nothing: the one who shows it keeps
	// its own there (NpcGear::unnamed, PlayerLooks::unnamed).
	enum GearUnnamed : std::uint8_t
	{
		kRightUnnamed = 1 << 0,
		kLeftUnnamed = 1 << 1,
		kAmmoUnnamed = 1 << 2,
		kAllUnnamed = kRightUnnamed | kLeftUnnamed | kAmmoUnnamed,  // the hands and the quiver
		kVoiceUnnamed = 1 << 3,                                     // the voice too: a player's looks, an NPC's gear
		kAllLooksUnnamed = kAllUnnamed | kVoiceUnnamed,
	};

	// What a player's character looks like, as the others' stand-in of them shows it. Form ids in the session's numbering
	// (PluginTable): a race the reading game lacks reads 0, worn items it lacks are left out, a held one it lacks reads 0
	// with its unnamed bit set (FormTranslate.h).
	struct PlayerLooks
	{
		std::uint32_t race = 0;  // TESRace form id; 0: not known
		bool          female = false;
		float         weight = 0.0f;  // the body's weight, 0 to 100
		// The armor (and clothing) worn, as base form ids, at most MAX_WORN.
		std::vector<std::uint32_t> worn;
		// What each hand holds (a weapon, a staff, a spell, a scroll, a torch; a shield is worn armor, and the left hand
		// names it too), the ammunition, and the voice (the shout or power the player has ready); 0: none. A two-handed
		// object (a greatsword, a bow, a ritual spell) is the right hand's alone.
		std::uint32_t right = 0;
		std::uint32_t left = 0;
		std::uint32_t ammo = 0;
		std::uint32_t voice = 0;
		std::uint8_t  unnamed = 0;  // GearUnnamed bits, kAllLooksUnnamed at most

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

	// ---- faces (docs/DESIGN.md, "Players": "Its face") --------------------------------
	// A player's face, as the others' stand-in of them shows it: what the game builds a character's head from. Form ids in
	// the session's numbering: a head part the reading game lacks is left out (partTypes still says the sender had one
	// of its kind, so the stand-in keeps the preset's there rather than show none), a colour form it lacks reads 0 (the
	// colour itself is in hairRgb), a complexion or skin it lacks reads 0 (the preset's then).

	inline constexpr std::size_t MAX_FACE_HEAD_PARTS = 32;  // a character has about seven (TESNPC's numHeadParts is a byte)
	inline constexpr std::size_t MAX_FACE_TINTS = 64;       // a race has about forty tint masks
	inline constexpr std::size_t FACE_MORPHS = 19;          // TESNPC::FaceData::Morphs::kTotal
	inline constexpr std::size_t FACE_PRESETS = 4;          // TESNPC::FaceData::Parts::kTotal (nose, unknown, eyes, mouth)
	inline constexpr std::uint8_t HEAD_PART_TYPES = 7;      // BGSHeadPart::HeadPartType: misc, face, eyes, hair, facial hair, scar, brows
	inline constexpr std::uint8_t TINT_TYPES = 15;          // TintMask::Type: freckles ... dirt
	// FaceData's own "not set" (its kDefault): a slider or a preset part the character never had one for.
	inline constexpr std::uint32_t FACE_UNSET = 0x7F7FFFFF;

	// A colour as four bytes: red, green, blue, alpha (lowest byte first).
	[[nodiscard]] constexpr std::uint32_t PackColor(std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b, std::uint8_t a_a)
	{
		return static_cast<std::uint32_t>(a_r) | (static_cast<std::uint32_t>(a_g) << 8) | (static_cast<std::uint32_t>(a_b) << 16) | (static_cast<std::uint32_t>(a_a) << 24);
	}

	// One tint layer (makeup, war paint, the skin tone, dirt...): which of the race's tint masks (its index, and the hash
	// of its texture's path, so a race a mod renumbered still finds the same mask: FaceRules::TintIndexFor), its colour and
	// how strongly it is laid on.
	struct FaceTint
	{
		std::uint16_t index = 0;  // the race's tint mask index (TINI)
		std::uint32_t path = 0;   // FaceRules::PathHash of the mask's texture
		std::uint32_t color = 0;  // PackColor
		std::uint8_t  value = 0;  // 0 to 100 (TINV)
		std::uint8_t  type = 0;   // TintMask::Type, below TINT_TYPES
		bool operator==(const FaceTint&) const = default;
	};

	struct PlayerFace
	{
		std::uint32_t              race = 0;  // the race the face is for: applied only to a stand-in of that race and sex
		bool                       female = false;
		std::vector<std::uint32_t> headParts;      // BGSHeadPart ids, at most MAX_FACE_HEAD_PARTS
		std::uint8_t               partTypes = 0;  // bit n: the sender's character has a head part of type n
		std::uint32_t              hairColor = 0;  // a BGSColorForm id (0: none, or one the reader lacks)
		std::uint32_t              hairRgb = 0;    // its colour (PackColor): for a colour form the reader lacks, or a runtime one
		std::uint32_t              faceDetails = 0;  // the complexion: a BGSTextureSet id
		std::uint32_t              skin = 0;         // the body's skin: a TESObjectARMO id (0: the race's)
		float                      height = 1.0f;
		std::array<float, FACE_MORPHS>          morphs{};   // FaceData's sliders (any finite value, FACE_UNSET's bits too)
		std::array<std::uint32_t, FACE_PRESETS> presets{};  // FaceData's preset parts (signed; FACE_UNSET: not set)
		std::vector<FaceTint>      tints;         // at most MAX_FACE_TINTS
		std::uint32_t              bodyTint = 0;  // the skin tone (PackColor)

		bool operator==(const PlayerFace&) const = default;
	};

	// Another player's face, as the server passes it on.
	struct RemoteFace
	{
		std::uint32_t playerId = 0;
		PlayerFace    face;
		bool operator==(const RemoteFace&) const = default;
	};

	// A preset part a reader may set: not set, none (-1), or one of the 64 a race can offer (its morph flags are 32 bits;
	// twice that to spare). Any other number is refused: the engine indexes its race's variants by it.
	[[nodiscard]] constexpr bool FacePresetOk(std::uint32_t a_value)
	{
		const auto v = static_cast<std::int32_t>(a_value);
		return a_value == FACE_UNSET || (v >= -1 && v < 64);
	}

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

	// The values an NPC's graph is fed from its movement besides Speed and Direction (the engine's graph data channels:
	// ActorTurnDeltaChannel, ActorPitchDeltaChannel, ActorTargetSpeedChannel), as its runner's graph had them. A copy's AI
	// never runs, so its movement feeds it none of them; a flyer's graph blends its flight by them (banking, climbing,
	// diving, flapping or gliding: docs/DESIGN.md, "Dragons"). TurnDelta and PitchDelta in thousandths (the graph's own
	// units), TargetSpeed in whole units per second; all 0 for an NPC whose race does not fly (its copy's graph keeps the
	// engine's). crash: the NPC died flying and its body still falls or slides (NpcRules::Crash): its runner goes on
	// sending where it is, and every copy follows it there before it dies.
	struct NpcGraph
	{
		std::int16_t turnDelta = 0;
		std::int16_t pitchDelta = 0;
		std::int16_t targetSpeed = 0;
		bool         crash = false;

		bool operator==(const NpcGraph&) const = default;
	};

	inline constexpr float GRAPH_DELTA_SCALE = 1000.0f;
	inline constexpr float GRAPH_SPEED_SCALE = 1.0f;

	[[nodiscard]] inline std::int16_t PackGraphValue(float a_value, float a_scale)
	{
		if (!std::isfinite(a_value)) {
			return 0;
		}
		return static_cast<std::int16_t>(std::clamp(std::lround(a_value * a_scale), -32767L, 32767L));
	}
	[[nodiscard]] inline float UnpackGraphValue(std::int16_t a_value, float a_scale)
	{
		return static_cast<float>(a_value) / a_scale;
	}

	// One NPC's state: base is the NPC its game made it of (its leveled pick's, for one placed from a leveled list; 0
	// when not known), which a spawn's copy is placed from and a copy's look is checked against. state.sequence counts
	// that NPC's states.
	struct NpcState
	{
		NpcKey        key;
		std::uint32_t base = 0;
		PlayerState   state;
		NpcGraph      graph;
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

	// An item an NPC wears: its base form and the body slots it covers (BGSBipedObjectForm's mask), so that a game whose
	// copy lacks the form still knows which of the copy's own items it stands for.
	struct WornItem
	{
		std::uint32_t id = 0;
		std::uint32_t slots = 0;
		bool operator==(const WornItem&) const = default;
		auto operator<=>(const WornItem&) const = default;
	};

	// What an NPC wears (armor, at most MAX_WORN) and holds: each hand's weapon, spell, shield or torch, its ammunition,
	// and its voice (the shout or power it has ready); 0 for none. unnamedSlots: the body slots of worn items left out
	// (none named to this game), so a copy keeps what it wears there.
	struct NpcGear
	{
		std::uint32_t         runner = 0;
		NpcKey                key;
		std::vector<WornItem> worn;
		std::uint32_t         right = 0;
		std::uint32_t         left = 0;
		std::uint32_t         ammo = 0;
		std::uint32_t         voice = 0;
		std::uint8_t          unnamed = 0;  // GearUnnamed bits, kAllLooksUnnamed at most
		std::uint32_t         unnamedSlots = 0;
		bool operator==(const NpcGear&) const = default;
	};

	// ---- NPC health and deaths (docs/DESIGN.md, "NPC health and deaths") ----
	// The game that runs an NPC is the truth of its health. A player's hit on a copy goes to that game as a numbered
	// request, the share of the copy's most health it took (so the hits to kill are as many in every game, however each
	// game rolled the NPC's level), and ends in exactly one answer: applied, or refused and why. The runner says the
	// NPC's health as a share of its most, and its death; only the runner's death counts, once.

	// At most this many hits, answers, healths and deaths in one message (a full list of deaths to a joiner: MAX_NPCS).
	inline constexpr std::size_t MAX_NPC_HITS = 64;
	inline constexpr std::size_t MAX_NPC_HEALTHS = 128;

	// A share of an NPC's most health, as one is sent: a number from 0 to 1.
	[[nodiscard]] inline bool IsShare(float a_value)
	{
		return std::isfinite(a_value) && a_value >= 0.0f && a_value <= 1.0f;
	}

	enum NpcHitFlags : std::uint8_t
	{
		kHitPower = 1 << 0,     // a power attack
		kHitSneak = 1 << 1,     // a sneak attack
		kHitBash = 1 << 2,      // a bash (shield or weapon)
		kHitSpell = 1 << 3,     // a spell, a staff, a scroll or an enchantment's effect
		kHitKilling = 1 << 4,   // it brought the copy to nothing: the NPC dies of it
		kHitKillMove = 1 << 5,  // a kill move (the copy died in our player's paired animation)
		kHitAllFlags = kHitPower | kHitSneak | kHitBash | kHitSpell | kHitKilling | kHitKillMove,
	};

	// seq counts the hitter's hits for the session (from 1), so each answer names the one it answers.
	struct NpcHit
	{
		NpcKey        key;
		std::uint32_t seq = 0;
		float         share = 0;  // of the copy's most health
		std::uint8_t  flags = 0;  // NpcHitFlags
		bool operator==(const NpcHit&) const = default;
	};

	// hitter: 0 from a game; the server names the player who hit.
	struct NpcHits
	{
		std::uint32_t       hitter = 0;
		std::vector<NpcHit> hits;
		bool operator==(const NpcHits&) const = default;
	};

	enum class HitVerdict : std::uint8_t
	{
		kApplied = 0,  // the runner took it off its NPC (share: the NPC's health after)
		kNobody = 1,   // nobody runs it (the server's answer)
		kYours = 2,    // the hitter runs it now: what its copy took stands (the server's answer)
		kDead = 3,     // dead for the session (the server's answer), or dead in the runner's game
		kNotHere = 4,  // the runner has no such NPC loaded, or no longer runs it
		kBusy = 5,     // past the hitter's budget (the server's answer)
	};
	inline constexpr std::uint8_t HIT_VERDICT_COUNT = 6;

	struct NpcHitAnswer
	{
		NpcKey        key;
		std::uint32_t hitter = 0;
		std::uint32_t seq = 0;
		HitVerdict    verdict = HitVerdict::kApplied;
		float         share = 0;  // kApplied: the NPC's health after it, as a share of its most; else 0
		bool operator==(const NpcHitAnswer&) const = default;
	};

	struct NpcHitAnswers
	{
		std::vector<NpcHitAnswer> answers;
		bool operator==(const NpcHitAnswers&) const = default;
	};

	struct NpcHealth
	{
		NpcKey key;
		float  share = 1;  // of its most health, more than 0: a dead NPC's is its death
		bool operator==(const NpcHealth&) const = default;
	};

	// runner: 0 from a game, and on the list the server sends a joiner; the server names the runner.
	struct NpcHealths
	{
		std::uint32_t          runner = 0;
		std::vector<NpcHealth> npcs;
		bool operator==(const NpcHealths&) const = default;
	};

	enum NpcDeathFlags : std::uint8_t
	{
		kDeathKillMove = 1 << 0,  // it died in a kill move
		kDeathAllFlags = kDeathKillMove,
	};

	// killer: the player whose hit killed it (0: no player's, or not known).
	struct NpcDeath
	{
		NpcKey        key;
		std::uint32_t killer = 0;
		std::uint8_t  flags = 0;  // NpcDeathFlags
		bool operator==(const NpcDeath&) const = default;
	};

	// full: the session's every dead NPC (a joiner's first). runner: as NpcHealths'.
	struct NpcDeaths
	{
		std::uint32_t         runner = 0;
		bool                  full = false;
		std::vector<NpcDeath> deaths;
		bool operator==(const NpcDeaths&) const = default;
	};

	// ---- a player's actions (docs/DESIGN.md, "Players") ----
	// Each event a player's animation graph takes (an attack, a power attack, a block, a bash, a bow drawn and let go, a
	// cast from either hand, a shout, a weapon drawn or put away), as its game sent it to the graph, for the others'
	// stand-in of that player to play at the moment it is drawn at (StateBuffer). No form in them: names only. The walk's
	// own events are not sent (each game works those out from the states, Gait.h).

	// At most this many events in one message (a game sends what a tick heard; a burst waits for the next).
	inline constexpr std::size_t MAX_PLAYER_ACTIONS = 32;

	struct PlayerAction
	{
		std::uint32_t sentMs = 0;  // the sender's clock when its graph took it (PlayerState::sentMs's clock; 0: not known)
		std::string   event;
		bool operator==(const PlayerAction&) const = default;
	};

	struct PlayerActions
	{
		std::vector<PlayerAction> actions;
		bool operator==(const PlayerActions&) const = default;
	};

	struct RemoteActions
	{
		std::uint32_t             playerId = 0;
		std::vector<PlayerAction> actions;
		bool operator==(const RemoteActions&) const = default;
	};

	// ---- riding (docs/DESIGN.md, "Riding") ----
	// Who rides what: a player on their horse, an NPC on its mount. A game says its own player's ride and the rides of the
	// NPCs it runs, each time one changes; the server passes on only a player's own and an NPC's runner's
	// (common/RideBook.h) and keeps them for whoever joins after.

	// At most this many rides in one message (a joiner's full list is sent in as many as it takes).
	inline constexpr std::size_t MAX_RIDES = 64;

	// One rider and its mount. player != 0: that player rides (from a game: its own id, which the server checks); else
	// rider is the NPC that rides. mount: what it rides (id 0 on a reader's side: a mount that game lacks, so it shows the
	// rider on foot). on false: it got off.
	struct Ride
	{
		std::uint32_t player = 0;
		NpcKey        rider;
		NpcKey        mount;
		bool          on = false;
		bool operator==(const Ride&) const = default;
	};

	// sender: 0 from a game; the server names the game that sent them (0 on a joiner's list). full: the first of a
	// joiner's list: a game forgets every ride it knew before it.
	struct Rides
	{
		std::uint32_t     sender = 0;
		bool              full = false;
		std::vector<Ride> rides;
		bool operator==(const Rides&) const = default;
	};

	// ---- projectiles (docs/DESIGN.md, "Projectiles") ----
	// Every projectile a player or an NPC launches in its own game (an arrow, a bolt, a spell's of any type, a shout's, a
	// staff's), said as it leaves: what it is, from where, which way and how strong. Every other game launches the same
	// from the actor that shows the shooter there (a friend's stand-in, an NPC's copy), at the moment that actor is drawn
	// at. Only the shooter's game deals what its shot hits: a stand-in's launch deals nothing, as its blows do not, and a
	// copy's deals what its game runs, as its blows do.

	// At most this many shots in one message (a game sends what a tick saw; a burst waits for the next).
	inline constexpr std::size_t MAX_SHOTS = 32;

	// What kind of projectile it is, by its record's type (BGSProjectile's): the developer channel and the rig count them
	// by kind, and a game ends the ones it launched by it (an arrow that stuck, a rune that lies).
	enum class ShotKind : std::uint8_t
	{
		kOther = 0,
		kArrow = 1,    // arrows and crossbow bolts
		kMissile = 2,  // a fire-and-forget spell's bolt or ball
		kLobber = 3,   // lobbed: a rune
		kBeam = 4,     // lightning
		kFlame = 5,    // a flame stream (Flames, a dragon's breath)
		kCone = 6,     // a cone (Unrelenting Force)
		kBarrier = 7,  // a wall
	};
	inline constexpr std::uint8_t SHOT_KIND_COUNT = 8;

	enum ShotFlags : std::uint8_t
	{
		kShotDual = 1 << 0,  // dual cast: its model's scale and its power are the dual cast's
		kShotHeld = 1 << 1,  // a concentration spell's: one of a stream while the cast is held
	};
	inline constexpr std::uint8_t kShotAllFlags = kShotDual | kShotHeld;

	// A magic caster's hand (MagicSystem::CastingSource's numbers): left, right, other (the voice), instant.
	inline constexpr std::uint8_t SHOT_SOURCE_COUNT = 4;

	// One projectile. player != 0: that player's (from a game: its own id, which the server checks); else npc, the NPC that
	// launched it (its runner's word). projectile: its record (a form the reader lacks leaves the shot out). weapon, ammo,
	// spell (a spell, a scroll, a staff's enchantment, a shout's spell), enchant and poison (an arrow's): its sources, 0
	// when none or one the reader lacks. pitch and heading: the angles it left at (radians, the game's angle x and z).
	// power: a bow's draw (1: full) or a spell's power; scale: its model's (a dual cast's is larger).
	struct Shot
	{
		std::uint32_t player = 0;
		NpcKey        npc;
		std::uint32_t sentMs = 0;
		std::uint32_t projectile = 0;
		std::uint32_t weapon = 0;
		std::uint32_t ammo = 0;
		std::uint32_t spell = 0;
		std::uint32_t enchant = 0;
		std::uint32_t poison = 0;
		ShotKind      kind = ShotKind::kOther;
		std::uint8_t  source = 0;
		std::uint8_t  flags = 0;
		float         x = 0, y = 0, z = 0;
		float         pitch = 0, heading = 0;
		float         power = 1.0f;
		float         scale = 1.0f;
		bool operator==(const Shot&) const = default;
	};

	// sender: 0 from a game; the server names the game that sent them.
	struct Shots
	{
		std::uint32_t     sender = 0;
		std::vector<Shot> shots;
		bool operator==(const Shots&) const = default;
	};

	// ---- furniture (docs/DESIGN.md, "Furniture") ----
	// Who sits, lies, leans or works in what: a player in a chair, a bed or at a crafting station, an NPC at its work spot
	// or a dragon on its perch. A game says its own player's and those of the NPCs it runs, each time one changes; the
	// server passes on only a player's own and an NPC's runner's (common/SitBook.h) and keeps them for whoever joins after.

	// At most this many in one message (a joiner's full list is sent in as many as it takes).
	inline constexpr std::size_t MAX_SITS = 48;

	// What the furniture is used for, as the sender's game has it (its sit or sleep state, the furniture's flags and its
	// bench type): every kind is shown the same way (the engine's own way in); the kind is for the log, the developer
	// channel and the rig.
	enum class SitKind : std::uint8_t
	{
		kSit = 0,    // a chair, a bench, a stool, a throne
		kSleep = 1,  // a bed, a bedroll, a coffin
		kLean = 2,   // a railing, a wall, a counter
		kWork = 3,   // a crafting station (forge, anvil, grindstone, workbench, tanning rack, smelter, alchemy, enchanting,
		             // cooking pot) or a work spot (a chopping block, an ore vein, a mill)
		kPerch = 4,  // a dragon's perch
	};
	inline constexpr std::uint8_t SIT_KIND_COUNT = 5;

	// A furniture marker that is not known (the sender's game named none).
	inline constexpr std::uint8_t NO_MARKER = 0xFF;

	// One actor in one piece of furniture. player != 0: that player (from a game: its own id, which the server checks);
	// else rider is the NPC. furniture: the reference (0: one that is not of the load order, a runtime one, or one the
	// reader lacks), base: its record (0: one the reader lacks), x y z: where it stands in the sender's world, to find a
	// runtime one, or one the reader lacks, by its base and place. marker: which of its markers (NO_MARKER: unknown).
	// on false: it got up (it names no furniture then).
	struct Sit
	{
		std::uint32_t player = 0;
		NpcKey        rider;
		std::uint32_t furniture = 0;
		std::uint32_t base = 0;
		float         x = 0, y = 0, z = 0;
		std::uint8_t  marker = NO_MARKER;
		SitKind       kind = SitKind::kSit;
		bool          on = false;
		bool operator==(const Sit&) const = default;
	};

	// sender: 0 from a game; the server names the game that sent them (0 on a joiner's list). full: the first of a
	// joiner's list: a game forgets every sit it knew before it.
	struct Sits
	{
		std::uint32_t    sender = 0;
		bool             full = false;
		std::vector<Sit> sits;
		bool operator==(const Sits&) const = default;
	};

	// ---- paired animations (docs/DESIGN.md, "Kill moves and paired animations") ----
	// A paired animation is one the engine plays on two actors at once, the second placed against the first: every kill
	// move (a weapon's, a sneak kill, an unarmed one, a creature's, a dragon's, a werewolf's or a vampire lord's) and a
	// vampire's feeding. The game that runs the actor that begins one (its player, or an NPC it runs) says it once; every
	// other game plays the same pair on the actors that show both there. The server passes on only a player's own and an
	// NPC's runner's.

	// At most this many in one message (a game sends what a tick saw).
	inline constexpr std::size_t MAX_PAIRS = 16;

	enum PairFlags : std::uint8_t
	{
		kPairKill = 1 << 0,   // a kill move: the second actor dies of it (in the game that runs it)
		kPairSneak = 1 << 1,  // begun sneaking (a sneak kill)
	};
	inline constexpr std::uint8_t kPairAllFlags = kPairKill | kPairSneak;

	// One side of a pair: a player (player != 0; the attacker, from a game, is its own player, which the server checks),
	// or an NPC (npc.id != 0) as the session names it.
	struct PairSide
	{
		std::uint32_t player = 0;
		NpcKey        npc;
		bool operator==(const PairSide&) const = default;
	};

	// One paired animation. attacker: the actor that began it (the idle is played on it); victim: the one placed against
	// it. idle: its idle record (0: one the reader lacks, found by its event then); event: the idle's animation event (how
	// a game finds the idle when the record is none, and what the log and the rig say). x y z heading: where the attacker
	// began it, in the sender's world.
	struct Pair
	{
		PairSide      attacker;
		PairSide      victim;
		std::uint32_t sentMs = 0;
		std::uint32_t idle = 0;
		std::string   event;
		std::uint8_t  flags = 0;
		float         x = 0, y = 0, z = 0;
		float         heading = 0;
		bool operator==(const Pair&) const = default;
	};

	// sender: 0 from a game; the server names the game that sent them.
	struct Pairs
	{
		std::uint32_t     sender = 0;
		std::vector<Pair> pairs;
		bool operator==(const Pairs&) const = default;
	};

	[[nodiscard]] inline bool PairSideHolds(const PairSide& a_side)
	{
		return a_side.player != 0 ? a_side.npc == NpcKey{} : a_side.npc.id != 0;
	}

	// Whether a pair can be: each side one actor, not the same one twice, an event of printable text, known flags, a place
	// a game can hold and a heading within a turn either way.
	[[nodiscard]] inline bool PairHolds(const Pair& a_pair)
	{
		const bool sides = PairSideHolds(a_pair.attacker) && PairSideHolds(a_pair.victim) && !(a_pair.attacker == a_pair.victim);
		const bool event = !a_pair.event.empty() && a_pair.event.size() <= MAX_EVENT_NAME &&
		                   std::ranges::none_of(a_pair.event, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
		return sides && event && (a_pair.flags & ~kPairAllFlags) == 0 && InWorld(a_pair.x) && InWorld(a_pair.y) && InWorld(a_pair.z) &&
		       std::isfinite(a_pair.heading) && std::fabs(a_pair.heading) <= 7.0f;
	}

	// Whether a sit can be: one actor (a player and no NPC, or an NPC of id != 0), a known kind, a place a game can hold,
	// and for an end no furniture.
	[[nodiscard]] inline bool SitHolds(const Sit& a_sit)
	{
		const bool who = a_sit.player != 0 ? a_sit.rider == NpcKey{} : a_sit.rider.id != 0;
		const bool place = InWorld(a_sit.x) && InWorld(a_sit.y) && InWorld(a_sit.z);
		const bool end = a_sit.on || (a_sit.furniture == 0 && a_sit.base == 0);
		return who && place && end && static_cast<std::uint8_t>(a_sit.kind) < SIT_KIND_COUNT;
	}

	// Whether a shot can be: one shooter (a player and no NPC, or an NPC of id != 0), a record, a known kind, hand and
	// flags, an origin a game can hold, angles within a turn either way, and a power and scale from 0 to 10.
	[[nodiscard]] inline bool ShotHolds(const Shot& a_shot)
	{
		const bool shooter = a_shot.player != 0 ? a_shot.npc == NpcKey{} : a_shot.npc.id != 0;
		const auto angle = [](float a_v) { return std::isfinite(a_v) && std::fabs(a_v) <= 7.0f; };
		const auto amount = [](float a_v) { return std::isfinite(a_v) && a_v >= 0.0f && a_v <= 10.0f; };
		return shooter && a_shot.projectile != 0 && static_cast<std::uint8_t>(a_shot.kind) < SHOT_KIND_COUNT && a_shot.source < SHOT_SOURCE_COUNT &&
		       (a_shot.flags & ~kShotAllFlags) == 0 && InWorld(a_shot.x) && InWorld(a_shot.y) && InWorld(a_shot.z) && angle(a_shot.pitch) &&
		       angle(a_shot.heading) && amount(a_shot.power) && amount(a_shot.scale);
	}

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
		a_writer.U32(a_looks.right);
		a_writer.U32(a_looks.left);
		a_writer.U32(a_looks.ammo);
		a_writer.U32(a_looks.voice);
		a_writer.U8(a_looks.unnamed);
	}

	// None for more than MAX_WORN items, a sex byte other than 0 or 1, a weight that is not 0 to 100 (another player's
	// game sent it: it would go onto a stand-in's body as it is), or an unnamed bit this version does not know.
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
		looks.right = a_reader.U32();
		looks.left = a_reader.U32();
		looks.ammo = a_reader.U32();
		looks.voice = a_reader.U32();
		looks.unnamed = a_reader.U8();
		if (!a_reader.Ok() || (looks.unnamed & ~kAllLooksUnnamed) != 0) {
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

	inline void WriteFace(Writer& a_writer, const PlayerFace& a_face)
	{
		a_writer.U32(a_face.race);
		a_writer.U8(a_face.female ? 1 : 0);
		const auto parts = (std::min)(a_face.headParts.size(), MAX_FACE_HEAD_PARTS);
		a_writer.U8(static_cast<std::uint8_t>(parts));
		for (std::size_t i = 0; i < parts; ++i) {
			a_writer.U32(a_face.headParts[i]);
		}
		a_writer.U8(a_face.partTypes);
		a_writer.U32(a_face.hairColor);
		a_writer.U32(a_face.hairRgb);
		a_writer.U32(a_face.faceDetails);
		a_writer.U32(a_face.skin);
		a_writer.F32(a_face.height);
		for (const float v : a_face.morphs) {
			a_writer.F32(v);
		}
		for (const auto v : a_face.presets) {
			a_writer.U32(v);
		}
		const auto tints = (std::min)(a_face.tints.size(), MAX_FACE_TINTS);
		a_writer.U8(static_cast<std::uint8_t>(tints));
		for (std::size_t i = 0; i < tints; ++i) {
			const auto& t = a_face.tints[i];
			a_writer.U16(t.index);
			a_writer.U32(t.path);
			a_writer.U32(t.color);
			a_writer.U8(t.value);
			a_writer.U8(t.type);
		}
		a_writer.U32(a_face.bodyTint);
	}

	// A face no game could have is not read: a part count or a tint count past the limit, a kind of head part or tint no
	// game knows, a slider or the height not a finite number (or a height no race has), a preset part the engine could
	// not index, a tint laid on over 100.
	inline std::optional<PlayerFace> ReadFace(Reader& a_reader)
	{
		PlayerFace face;
		face.race = a_reader.U32();
		const auto female = a_reader.U8();
		const auto parts = a_reader.U8();
		if (!a_reader.Ok() || female > 1 || parts > MAX_FACE_HEAD_PARTS) {
			return std::nullopt;
		}
		face.female = female == 1;
		face.headParts.reserve(parts);
		for (std::size_t i = 0; i < parts; ++i) {
			face.headParts.push_back(a_reader.U32());
		}
		face.partTypes = a_reader.U8();
		face.hairColor = a_reader.U32();
		face.hairRgb = a_reader.U32();
		face.faceDetails = a_reader.U32();
		face.skin = a_reader.U32();
		face.height = a_reader.F32();
		for (auto& v : face.morphs) {
			v = a_reader.F32();
		}
		for (auto& v : face.presets) {
			v = a_reader.U32();
		}
		const auto tints = a_reader.U8();
		if (!a_reader.Ok() || tints > MAX_FACE_TINTS || (face.partTypes >> HEAD_PART_TYPES) != 0 || !std::isfinite(face.height) || face.height < 0.25f ||
			face.height > 4.0f || !std::ranges::all_of(face.morphs, [](float a_v) { return std::isfinite(a_v); }) ||
			!std::ranges::all_of(face.presets, [](std::uint32_t a_v) { return FacePresetOk(a_v); })) {
			return std::nullopt;
		}
		face.tints.reserve(tints);
		for (std::size_t i = 0; i < tints; ++i) {
			FaceTint t;
			t.index = a_reader.U16();
			t.path = a_reader.U32();
			t.color = a_reader.U32();
			t.value = a_reader.U8();
			t.type = a_reader.U8();
			if (t.value > 100 || t.type >= TINT_TYPES) {
				return std::nullopt;
			}
			face.tints.push_back(t);
		}
		face.bodyTint = a_reader.U32();
		if (!a_reader.Ok()) {
			return std::nullopt;
		}
		return face;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerFace& a_msg)
	{
		Writer w{ MessageType::kPlayerFace };
		WriteFace(w, a_msg);
		return w.Data();
	}

	inline std::optional<PlayerFace> DecodePlayerFace(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		auto face = ReadFace(r);
		if (!face || !r.AtEnd()) {
			return std::nullopt;
		}
		return face;
	}

	inline std::vector<std::uint8_t> Encode(const RemoteFace& a_msg)
	{
		Writer w{ MessageType::kRemoteFace };
		w.U32(a_msg.playerId);
		WriteFace(w, a_msg.face);
		return w.Data();
	}

	inline std::optional<RemoteFace> DecodeRemoteFace(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		RemoteFace msg;
		msg.playerId = r.U32();
		auto face = ReadFace(r);
		if (!face || !r.AtEnd()) {
			return std::nullopt;
		}
		msg.face = std::move(*face);
		return msg;
	}

	// The most bytes a face can be, every list at its longest (the tests encode one); a kRemoteFace is 4 more.
	inline constexpr std::size_t MAX_FACE_SIZE = 1 + 4 + 1 + 1 + MAX_FACE_HEAD_PARTS * 4 + 1 + 4 * 4 + 4 + FACE_MORPHS * 4 + FACE_PRESETS * 4 + 1 +
	                                             MAX_FACE_TINTS * (2 + 4 + 4 + 1 + 1) + 4;
	static_assert(MAX_FACE_SIZE + 4 <= 2048, "a face stays small");
	static_assert(MAX_FACE_HEAD_PARTS <= 0xFF && MAX_FACE_TINTS <= 0xFF && HEAD_PART_TYPES <= 8);

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
			const auto& graph = a_msg.npcs[i].graph;
			w.U16(static_cast<std::uint16_t>(graph.turnDelta));
			w.U16(static_cast<std::uint16_t>(graph.pitchDelta));
			w.U16(static_cast<std::uint16_t>(graph.targetSpeed));
			w.U8(graph.crash ? 1 : 0);
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
			npc.graph.turnDelta = static_cast<std::int16_t>(r.U16());
			npc.graph.pitchDelta = static_cast<std::int16_t>(r.U16());
			npc.graph.targetSpeed = static_cast<std::int16_t>(r.U16());
			npc.graph.crash = r.U8() != 0;
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
			w.U32(a_msg.worn[i].id);
			w.U32(a_msg.worn[i].slots);
		}
		w.U32(a_msg.right);
		w.U32(a_msg.left);
		w.U32(a_msg.ammo);
		w.U32(a_msg.voice);
		w.U8(a_msg.unnamed);
		w.U32(a_msg.unnamedSlots);
		return w.Data();
	}

	// None for more than MAX_WORN worn items, or an unknown unnamed bit. An NPC of id 0 reads: the translation left it
	// out (FormTranslate.h), and the reader skips it.
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
			const auto id = r.U32();
			msg.worn.push_back({ id, r.U32() });
		}
		msg.right = r.U32();
		msg.left = r.U32();
		msg.ammo = r.U32();
		msg.voice = r.U32();
		msg.unnamed = r.U8();
		msg.unnamedSlots = r.U32();
		if (!r.AtEnd() || (msg.unnamed & ~kAllLooksUnnamed) != 0) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const NpcHits& a_msg)
	{
		Writer     w{ MessageType::kNpcHits };
		const auto count = (std::min)(a_msg.hits.size(), MAX_NPC_HITS);
		w.U32(a_msg.hitter);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			WriteKey(w, a_msg.hits[i].key);
			w.U32(a_msg.hits[i].seq);
			w.F32(a_msg.hits[i].share);
			w.U8(a_msg.hits[i].flags);
		}
		return w.Data();
	}

	// None for more than MAX_NPC_HITS, a hit of no NPC or numbered 0, a share that is no share, or an unknown flag. An
	// empty list reads (the translation may leave every hit out).
	inline std::optional<NpcHits> DecodeNpcHits(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		NpcHits msg;
		msg.hitter = r.U32();
		const auto count = r.U8();
		if (!r.Ok() || count > MAX_NPC_HITS) {
			return std::nullopt;
		}
		msg.hits.reserve(count);
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			NpcHit hit;
			hit.key = ReadKey(r);
			hit.seq = r.U32();
			hit.share = r.F32();
			hit.flags = r.U8();
			if (r.Ok() && (hit.key.id == 0 || hit.seq == 0 || !IsShare(hit.share) || (hit.flags & ~kHitAllFlags) != 0)) {
				return std::nullopt;
			}
			msg.hits.push_back(hit);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const NpcHitAnswers& a_msg)
	{
		Writer     w{ MessageType::kNpcHitAnswers };
		const auto count = (std::min)(a_msg.answers.size(), MAX_NPC_HITS);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			WriteKey(w, a_msg.answers[i].key);
			w.U32(a_msg.answers[i].hitter);
			w.U32(a_msg.answers[i].seq);
			w.U8(static_cast<std::uint8_t>(a_msg.answers[i].verdict));
			w.F32(a_msg.answers[i].share);
		}
		return w.Data();
	}

	// None for more than MAX_NPC_HITS, an answer of no NPC, no hitter or no hit, an unknown verdict, or a share that is
	// no share.
	inline std::optional<NpcHitAnswers> DecodeNpcHitAnswers(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		const auto count = r.U8();
		if (!r.Ok() || count > MAX_NPC_HITS) {
			return std::nullopt;
		}
		NpcHitAnswers msg;
		msg.answers.reserve(count);
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			NpcHitAnswer answer;
			answer.key = ReadKey(r);
			answer.hitter = r.U32();
			answer.seq = r.U32();
			const auto verdict = r.U8();
			answer.share = r.F32();
			if (r.Ok() && (answer.key.id == 0 || answer.hitter == 0 || answer.seq == 0 || verdict >= HIT_VERDICT_COUNT || !IsShare(answer.share))) {
				return std::nullopt;
			}
			answer.verdict = static_cast<HitVerdict>(verdict);
			msg.answers.push_back(answer);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const NpcHealths& a_msg)
	{
		Writer     w{ MessageType::kNpcHealths };
		const auto count = (std::min)(a_msg.npcs.size(), MAX_NPCS);
		w.U32(a_msg.runner);
		w.U16(static_cast<std::uint16_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			WriteKey(w, a_msg.npcs[i].key);
			w.F32(a_msg.npcs[i].share);
		}
		return w.Data();
	}

	// None for more than MAX_NPCS, an NPC of id 0, or a share that is no share (or 0: a death is said as one).
	inline std::optional<NpcHealths> DecodeNpcHealths(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		NpcHealths msg;
		msg.runner = r.U32();
		const auto count = r.U16();
		if (!r.Ok() || count > MAX_NPCS) {
			return std::nullopt;
		}
		msg.npcs.reserve(count);
		for (std::uint16_t i = 0; i < count && r.Ok(); ++i) {
			NpcHealth npc;
			npc.key = ReadKey(r);
			npc.share = r.F32();
			if (r.Ok() && (npc.key.id == 0 || !IsShare(npc.share) || npc.share <= 0.0f)) {
				return std::nullopt;
			}
			msg.npcs.push_back(npc);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const NpcDeaths& a_msg)
	{
		Writer     w{ MessageType::kNpcDeaths };
		const auto count = (std::min)(a_msg.deaths.size(), MAX_NPCS);
		w.U32(a_msg.runner);
		w.U8(a_msg.full ? 1 : 0);
		w.U16(static_cast<std::uint16_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			WriteKey(w, a_msg.deaths[i].key);
			w.U32(a_msg.deaths[i].killer);
			w.U8(a_msg.deaths[i].flags);
		}
		return w.Data();
	}

	// None for more than MAX_NPCS, an NPC of id 0, or an unknown flag.
	inline std::optional<NpcDeaths> DecodeNpcDeaths(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		NpcDeaths msg;
		msg.runner = r.U32();
		const auto full = r.U8();
		const auto count = r.U16();
		if (!r.Ok() || full > 1 || count > MAX_NPCS) {
			return std::nullopt;
		}
		msg.full = full == 1;
		msg.deaths.reserve(count);
		for (std::uint16_t i = 0; i < count && r.Ok(); ++i) {
			NpcDeath death;
			death.key = ReadKey(r);
			death.killer = r.U32();
			death.flags = r.U8();
			if (r.Ok() && (death.key.id == 0 || (death.flags & ~kDeathAllFlags) != 0)) {
				return std::nullopt;
			}
			msg.deaths.push_back(death);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline std::vector<std::uint8_t> Encode(const Rides& a_msg)
	{
		Writer     w{ MessageType::kRides };
		const auto count = (std::min)(a_msg.rides.size(), MAX_RIDES);
		w.U32(a_msg.sender);
		w.U8(a_msg.full ? 1 : 0);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			const auto& ride = a_msg.rides[i];
			w.U32(ride.player);
			WriteKey(w, ride.player != 0 ? NpcKey{} : ride.rider);
			WriteKey(w, ride.mount);
			w.U8(ride.on ? 1 : 0);
		}
		return w.Data();
	}

	// Whether a ride names one rider and a mount that can be: a player (and no NPC) or an NPC of id != 0, and a mount of id
	// 0 only with no spawner.
	[[nodiscard]] inline bool RideHolds(const Ride& a_ride)
	{
		const bool rider = a_ride.player != 0 ? a_ride.rider == NpcKey{} : a_ride.rider.id != 0;
		return rider && (a_ride.mount.id != 0 || a_ride.mount.spawner == 0);
	}

	// None for more than MAX_RIDES, a ride that does not hold (RideHolds), or an on that is not 0 or 1. A mount of id 0
	// reads: none, or one the reader lacks.
	inline std::optional<Rides> DecodeRides(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Rides msg;
		msg.sender = r.U32();
		const auto full = r.U8();
		const auto count = r.U8();
		if (!r.Ok() || full > 1 || count > MAX_RIDES) {
			return std::nullopt;
		}
		msg.full = full == 1;
		msg.rides.reserve(count);
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			Ride ride;
			ride.player = r.U32();
			ride.rider = ReadKey(r);
			ride.mount = ReadKey(r);
			const auto on = r.U8();
			ride.on = on == 1;
			if (r.Ok() && (on > 1 || !RideHolds(ride))) {
				return std::nullopt;
			}
			msg.rides.push_back(ride);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline constexpr std::size_t MAX_RIDES_SIZE = 1 + 4 + 1 + 1 + MAX_RIDES * (4 + 8 + 8 + 1);
	static_assert(MAX_RIDES <= 0xFF && MAX_RIDES_SIZE <= 2048, "a message of rides stays small");

	inline std::vector<std::uint8_t> Encode(const Shots& a_msg)
	{
		Writer     w{ MessageType::kShots };
		const auto count = (std::min)(a_msg.shots.size(), MAX_SHOTS);
		w.U32(a_msg.sender);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			const auto& shot = a_msg.shots[i];
			w.U32(shot.player);
			WriteKey(w, shot.player != 0 ? NpcKey{} : shot.npc);
			w.U32(shot.sentMs);
			for (const auto id : { shot.projectile, shot.weapon, shot.ammo, shot.spell, shot.enchant, shot.poison }) {
				w.U32(id);
			}
			w.U8(static_cast<std::uint8_t>(shot.kind));
			w.U8(shot.source);
			w.U8(shot.flags);
			for (const auto v : { shot.x, shot.y, shot.z, shot.pitch, shot.heading, shot.power, shot.scale }) {
				w.F32(v);
			}
		}
		return w.Data();
	}

	// None for more than MAX_SHOTS, or a shot that does not hold (ShotHolds).
	inline std::optional<Shots> DecodeShots(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Shots msg;
		msg.sender = r.U32();
		const auto count = r.U8();
		if (!r.Ok() || count > MAX_SHOTS) {
			return std::nullopt;
		}
		msg.shots.reserve(count);
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			Shot shot;
			shot.player = r.U32();
			shot.npc = ReadKey(r);
			shot.sentMs = r.U32();
			for (auto* id : { &shot.projectile, &shot.weapon, &shot.ammo, &shot.spell, &shot.enchant, &shot.poison }) {
				*id = r.U32();
			}
			shot.kind = static_cast<ShotKind>(r.U8());
			shot.source = r.U8();
			shot.flags = r.U8();
			for (auto* v : { &shot.x, &shot.y, &shot.z, &shot.pitch, &shot.heading, &shot.power, &shot.scale }) {
				*v = r.F32();
			}
			if (r.Ok() && !ShotHolds(shot)) {
				return std::nullopt;
			}
			msg.shots.push_back(shot);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline constexpr std::size_t SHOT_BYTES = 4 + 8 + 4 + 6 * 4 + 3 + 7 * 4;
	inline constexpr std::size_t MAX_SHOTS_SIZE = 1 + 4 + 1 + MAX_SHOTS * SHOT_BYTES;
	static_assert(MAX_SHOTS <= 0xFF && MAX_SHOTS_SIZE <= 4096, "a message of shots stays small");

	inline std::vector<std::uint8_t> Encode(const Sits& a_msg)
	{
		Writer     w{ MessageType::kSits };
		const auto count = (std::min)(a_msg.sits.size(), MAX_SITS);
		w.U32(a_msg.sender);
		w.U8(a_msg.full ? 1 : 0);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			const auto& sit = a_msg.sits[i];
			w.U32(sit.player);
			WriteKey(w, sit.player != 0 ? NpcKey{} : sit.rider);
			// An end names no furniture: whatever the caller left in it is not written.
			w.U32(sit.on ? sit.furniture : 0);
			w.U32(sit.on ? sit.base : 0);
			for (const auto v : { sit.x, sit.y, sit.z }) {
				w.F32(v);
			}
			w.U8(sit.marker);
			w.U8(static_cast<std::uint8_t>(sit.kind));
			w.U8(sit.on ? 1 : 0);
		}
		return w.Data();
	}

	// None for more than MAX_SITS, a sit that does not hold (SitHolds), or an on or a full that is not 0 or 1.
	inline std::optional<Sits> DecodeSits(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Sits msg;
		msg.sender = r.U32();
		const auto full = r.U8();
		const auto count = r.U8();
		if (!r.Ok() || full > 1 || count > MAX_SITS) {
			return std::nullopt;
		}
		msg.full = full == 1;
		msg.sits.reserve(count);
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			Sit sit;
			sit.player = r.U32();
			sit.rider = ReadKey(r);
			sit.furniture = r.U32();
			sit.base = r.U32();
			for (auto* v : { &sit.x, &sit.y, &sit.z }) {
				*v = r.F32();
			}
			sit.marker = r.U8();
			sit.kind = static_cast<SitKind>(r.U8());
			const auto on = r.U8();
			sit.on = on == 1;
			if (r.Ok() && (on > 1 || !SitHolds(sit))) {
				return std::nullopt;
			}
			msg.sits.push_back(sit);
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline constexpr std::size_t SIT_BYTES = 4 + 8 + 4 + 4 + 3 * 4 + 3;
	inline constexpr std::size_t MAX_SITS_SIZE = 1 + 4 + 1 + 1 + MAX_SITS * SIT_BYTES;
	static_assert(MAX_SITS <= 0xFF && MAX_SITS_SIZE <= 2048, "a message of sits stays small");

	inline void WritePairSide(Writer& a_writer, const PairSide& a_side)
	{
		a_writer.U32(a_side.player);
		WriteKey(a_writer, a_side.player != 0 ? NpcKey{} : a_side.npc);  // a player's side names no NPC
	}

	inline PairSide ReadPairSide(Reader& a_reader)
	{
		PairSide side;
		side.player = a_reader.U32();
		side.npc = ReadKey(a_reader);
		return side;
	}

	inline std::vector<std::uint8_t> Encode(const Pairs& a_msg)
	{
		Writer     w{ MessageType::kPairs };
		const auto count = (std::min)(a_msg.pairs.size(), MAX_PAIRS);
		w.U32(a_msg.sender);
		w.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			const auto& pair = a_msg.pairs[i];
			WritePairSide(w, pair.attacker);
			WritePairSide(w, pair.victim);
			w.U32(pair.sentMs);
			w.U32(pair.idle);
			w.Str(pair.event, MAX_EVENT_NAME);
			w.U8(pair.flags);
			for (const auto v : { pair.x, pair.y, pair.z, pair.heading }) {
				w.F32(v);
			}
		}
		return w.Data();
	}

	// None for more than MAX_PAIRS, or a pair that does not hold (PairHolds).
	inline std::optional<Pairs> DecodePairs(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		Pairs msg;
		msg.sender = r.U32();
		const auto count = r.U8();
		if (!r.Ok() || count > MAX_PAIRS) {
			return std::nullopt;
		}
		msg.pairs.reserve(count);
		for (std::uint8_t i = 0; i < count && r.Ok(); ++i) {
			Pair pair;
			pair.attacker = ReadPairSide(r);
			pair.victim = ReadPairSide(r);
			pair.sentMs = r.U32();
			pair.idle = r.U32();
			pair.event = r.Str(MAX_EVENT_NAME);
			pair.flags = r.U8();
			for (auto* v : { &pair.x, &pair.y, &pair.z, &pair.heading }) {
				*v = r.F32();
			}
			if (r.Ok() && !PairHolds(pair)) {
				return std::nullopt;
			}
			msg.pairs.push_back(std::move(pair));
		}
		if (!r.AtEnd()) {
			return std::nullopt;
		}
		return msg;
	}

	inline constexpr std::size_t PAIR_BYTES = 2 * (4 + 8) + 4 + 4 + 1 + MAX_EVENT_NAME + 1 + 4 * 4;
	inline constexpr std::size_t MAX_PAIRS_SIZE = 1 + 4 + 1 + MAX_PAIRS * PAIR_BYTES;
	static_assert(MAX_PAIRS <= 0xFF && MAX_PAIRS_SIZE <= 2048, "a message of pairs stays small");

	inline void WritePlayerActions(Writer& a_writer, const std::vector<PlayerAction>& a_actions)
	{
		const auto count = (std::min)(a_actions.size(), MAX_PLAYER_ACTIONS);
		a_writer.U8(static_cast<std::uint8_t>(count));
		for (std::size_t i = 0; i < count; ++i) {
			a_writer.U32(a_actions[i].sentMs);
			a_writer.Str(a_actions[i].event, MAX_EVENT_NAME);
		}
	}

	// None for more than MAX_PLAYER_ACTIONS, or an empty or too long event name.
	inline std::optional<std::vector<PlayerAction>> ReadPlayerActions(Reader& a_reader)
	{
		const auto count = a_reader.U8();
		if (!a_reader.Ok() || count > MAX_PLAYER_ACTIONS) {
			return std::nullopt;
		}
		std::vector<PlayerAction> out;
		out.reserve(count);
		for (std::uint8_t i = 0; i < count && a_reader.Ok(); ++i) {
			PlayerAction action;
			action.sentMs = a_reader.U32();
			action.event = a_reader.Str(MAX_EVENT_NAME);
			if (a_reader.Ok() && action.event.empty()) {
				return std::nullopt;
			}
			out.push_back(std::move(action));
		}
		if (!a_reader.Ok()) {
			return std::nullopt;
		}
		return out;
	}

	inline std::vector<std::uint8_t> Encode(const PlayerActions& a_msg)
	{
		Writer w{ MessageType::kPlayerActions };
		WritePlayerActions(w, a_msg.actions);
		return w.Data();
	}

	inline std::optional<PlayerActions> DecodePlayerActions(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		auto actions = ReadPlayerActions(r);
		if (!actions || !r.AtEnd()) {
			return std::nullopt;
		}
		return PlayerActions{ std::move(*actions) };
	}

	inline std::vector<std::uint8_t> Encode(const RemoteActions& a_msg)
	{
		Writer w{ MessageType::kRemoteActions };
		w.U32(a_msg.playerId);
		WritePlayerActions(w, a_msg.actions);
		return w.Data();
	}

	inline std::optional<RemoteActions> DecodeRemoteActions(std::span<const std::uint8_t> a_data)
	{
		Reader r{ a_data };
		r.U8();
		RemoteActions msg;
		msg.playerId = r.U32();
		auto actions = ReadPlayerActions(r);
		if (!actions || !r.AtEnd()) {
			return std::nullopt;
		}
		msg.actions = std::move(*actions);
		return msg;
	}

	inline constexpr std::size_t MAX_PLAYER_ACTIONS_SIZE = 1 + 4 + 1 + MAX_PLAYER_ACTIONS * (4 + 1 + MAX_EVENT_NAME);
	static_assert(MAX_PLAYER_ACTIONS_SIZE <= 2048, "a message of a player's actions stays small");

	// The most bytes each NPC message can be, every list at its longest (the tests encode those): each fits one message.
	inline constexpr std::size_t KEY_BYTES = 8;
	inline constexpr std::size_t STATE_BYTES = 4 + 4 + 4 + 4 * 5 + 1 + 4 + 1 + 1;
	inline constexpr std::size_t MAX_NPC_OWNERS_SIZE = 1 + 1 + 2 + MAX_NPCS * (KEY_BYTES + 4);
	inline constexpr std::size_t GRAPH_BYTES = 2 * 3 + 1;
	inline constexpr std::size_t MAX_NPC_STATES_SIZE = 1 + 4 + 1 + MAX_NPC_STATES * (KEY_BYTES + 4 + STATE_BYTES + GRAPH_BYTES);
	inline constexpr std::size_t MAX_NPC_ACTIONS_SIZE = 1 + 4 + 1 + MAX_NPC_ACTIONS * (KEY_BYTES + 4 + 1 + MAX_EVENT_NAME);
	static_assert(MAX_NPC_OWNERS_SIZE <= MAX_MESSAGE_SIZE && MAX_NPC_ACTIONS_SIZE <= MAX_MESSAGE_SIZE);
	static_assert(MAX_NPC_STATES_SIZE <= 1200, "a batch of NPC states stays about one datagram");
	static_assert(MAX_NPCS <= 0xFFFF && MAX_NPC_CLAIMS <= 0xFFFF && MAX_NPC_STATES <= 0xFF && MAX_NPC_ACTIONS <= 0xFF);
	inline constexpr std::size_t MAX_NPC_HITS_SIZE = 1 + 4 + 1 + MAX_NPC_HITS * (KEY_BYTES + 4 + 4 + 1);
	inline constexpr std::size_t MAX_NPC_HIT_ANSWERS_SIZE = 1 + 1 + MAX_NPC_HITS * (KEY_BYTES + 4 + 4 + 1 + 4);
	inline constexpr std::size_t MAX_NPC_HEALTHS_SIZE = 1 + 4 + 2 + MAX_NPCS * (KEY_BYTES + 4);
	inline constexpr std::size_t MAX_NPC_DEATHS_SIZE = 1 + 4 + 1 + 2 + MAX_NPCS * (KEY_BYTES + 4 + 1);
	static_assert(MAX_NPC_HEALTHS_SIZE <= MAX_MESSAGE_SIZE && MAX_NPC_DEATHS_SIZE <= MAX_MESSAGE_SIZE);
	static_assert(MAX_NPC_HITS <= 0xFF && MAX_NPC_HEALTHS <= MAX_NPCS);

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
