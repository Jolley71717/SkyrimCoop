#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>

// What becomes of our player's magic on a copy of an NPC another game runs (docs/DESIGN.md, "Magic on NPCs"), and of our
// player's Slow Time. The game's side is src/game/Effects.cpp; no game here: tests/EffectTests.cpp.
//
// A copy's AI is off and its runner's game decides what its NPC does, so an effect that changes what an NPC does
// (staggered, pushed, paralysed, frightened, calmed, enraged, disarmed, slowed, its stamina or magicka drained, a
// script's doing: Unrelenting Force, Dismay, Kyne's Peace, Disarm, Ice Form, Marked for Death, Dragonrend, every spell,
// enchantment and poison of those kinds) does nothing that lasts on the copy. Each one is held back here and put on the
// real NPC in its runner's game, our player's stand-in its caster there, where it does what it would have; the copy
// shows it as it shows the rest of its runner's NPC (its stagger and its flight are its runner's graph events). What it
// takes from health stays here: the copy's health share goes as our player's hit (CopyHealth, kNpcHits), as it always
// did. What makes or moves something in this world (a summon, a raised corpse, a soul trap, a rune's hazard) stays here
// too: it is our game's. And what only shows on the NPC (invisible, ethereal) is put on both.
namespace EffectRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// The game's kinds of magic effect (EffectArchetypes::ArchetypeID; Effects.cpp checks them against the game's).
	enum Archetype : std::int32_t
	{
		kArchNone = -1,
		kValueModifier = 0,
		kScript = 1,
		kDispel = 2,
		kCureDisease = 3,
		kAbsorb = 4,
		kDualValueModifier = 5,
		kCalm = 6,
		kDemoralize = 7,
		kFrenzy = 8,
		kDisarm = 9,
		kCommandSummoned = 10,
		kInvisibility = 11,
		kLight = 12,
		kDarkness = 13,
		kNightEye = 14,
		kLock = 15,
		kOpen = 16,
		kBoundWeapon = 17,
		kSummonCreature = 18,
		kDetectLife = 19,
		kTelekinesis = 20,
		kParalysis = 21,
		kReanimate = 22,
		kSoulTrap = 23,
		kTurnUndead = 24,
		kGuide = 25,
		kWerewolfFeed = 26,
		kCureParalysis = 27,
		kCureAddiction = 28,
		kCurePoison = 29,
		kConcussion = 30,
		kValueAndParts = 31,
		kAccumulateMagnitude = 32,
		kStagger = 33,
		kPeakValueModifier = 34,
		kCloak = 35,
		kWerewolf = 36,
		kSlowTime = 37,
		kRally = 38,
		kEnhanceWeapon = 39,
		kSpawnHazard = 40,
		kEtherealize = 41,
		kBanish = 42,
		kSpawnScriptedRef = 43,
		kDisguise = 44,
		kGrabActor = 45,
		kVampireLord = 46,
	};

	// RE::ActorValue::kHealth (Effects.cpp checks it).
	inline constexpr std::int32_t AV_HEALTH = 24;

	enum class Route
	{
		kHere,    // taken here as before (health, which CopyHealth carries; what makes or moves something here)
		kRunner,  // held back here, put on the real NPC in its runner's game
		kBoth,    // taken here and there (it only shows on the NPC)
	};

	// a_archetype: the effect's kind; a_actorValue: the value it changes (its first; -1 or any for a kind that changes
	// none); a_madeHere: its spell, enchantment or poison was made in this game as it ran (a player's own enchantment,
	// poison or potion): no other game has it to put on, so it is taken here as before.
	[[nodiscard]] constexpr Route OnCopy(std::int32_t a_archetype, std::int32_t a_actorValue, bool a_madeHere = false)
	{
		if (a_madeHere) {
			return Route::kHere;
		}
		switch (a_archetype) {
		// What changes a value: health's is CopyHealth's (the copy's share goes as a hit, and a dual one on health and
		// stamina as well, whole); any other value's (stamina, magicka, speed, armor, a resistance) is its runner's NPC's.
		case kValueModifier:
		case kDualValueModifier:
		case kValueAndParts:
		case kAccumulateMagnitude:
		case kPeakValueModifier:
			return a_actorValue == AV_HEALTH ? Route::kHere : Route::kRunner;
		// An absorb gives to its caster: our player, here (put on in the runner's game it would give to the stand-in).
		case kAbsorb:
		// What makes or moves something in this world, or fills our player's soul gem: ours.
		case kCommandSummoned:
		case kLock:
		case kOpen:
		case kBoundWeapon:
		case kSummonCreature:
		case kTelekinesis:
		case kReanimate:
		case kSoulTrap:
		case kSpawnHazard:
		case kSpawnScriptedRef:
		case kLight:
		case kArchNone:
			return Route::kHere;
		// What only shows on it (and hides it from its runner's NPCs there).
		case kInvisibility:
		case kDarkness:
		case kNightEye:
		case kDetectLife:
		case kEtherealize:
		case kDisguise:
			return Route::kBoth;
		default:
			// Stagger, push, paralysis, fear, calm, frenzy, disarm, turn undead, banish, a grab (Vampiric Grip: the copy is
			// put where its runner's NPC is every frame, so the pull is that NPC's), a script (Dragonrend, Soul Tear, Bend Will),
			// a cure, a dispel, a rally, a werewolf's or a vampire lord's: what its NPC does is its runner's.
			return Route::kRunner;
		}
	}

	[[nodiscard]] constexpr std::string_view RouteName(Route a_route)
	{
		switch (a_route) {
		case Route::kHere:
			return "here";
		case Route::kRunner:
			return "runner";
		case Route::kBoth:
			return "both";
		}
		return "?";
	}

	// ---- Slow Time ----

	// Our player's effect on themselves that every player near shares: Slow Time, the shout's alone (a_fromShout: its spell
	// is a shout's, a voice power), not a perk's, a potion's or a mod's slow-time effect, which stay their player's own
	// (decided: the shout is the group's moment; a slow on every aim or every sneak attack would slow everyone's game at
	// one player's whim). The game slows the world but the
	// player who shouted, in that player's game only; the NPCs another game runs are drawn at their runner's pace, its
	// own world's (unslowed), so a world slowed in one game only leaves half the fight at full speed. Every other game
	// whose player is in the same space and within SLOW_TIME_WITHIN of the shouter's stand-in puts the same effect on its
	// own player: the world slows everywhere and every player near has the edge, as if each had shouted (a co-op
	// choice: the shout is the group's moment).
	[[nodiscard]] constexpr bool SharedWithPlayers(std::int32_t a_archetype, bool a_fromShout)
	{
		return a_archetype == kSlowTime && a_fromShout;
	}

	inline constexpr float SLOW_TIME_WITHIN = 8000.0f;

	[[nodiscard]] inline bool SharesSlowTime(bool a_sameSpace, std::optional<float> a_fromShouter)
	{
		return a_sameSpace && a_fromShouter && *a_fromShouter <= SLOW_TIME_WITHIN;
	}

	// ---- how often ----

	// The same effect of the same item on the same NPC is sent once per this (a concentration spell puts its effects on
	// again several times a second, each a refresh of the one already there; a cone or an explosion puts each on once).
	inline constexpr auto RESEND_AFTER = 250ms;

	template <class Target>
	class Repeats
	{
	public:
		// Whether this one goes (and from now, counts as sent).
		bool Due(const Target& a_target, std::uint32_t a_item, std::uint8_t a_index, Clock::time_point a_now)
		{
			const auto key = std::tuple{ a_target, a_item, a_index };
			const auto it = sent.find(key);
			if (it != sent.end() && a_now - it->second < RESEND_AFTER) {
				return false;
			}
			sent[key] = a_now;
			if (sent.size() > MAX_KEPT) {
				std::erase_if(sent, [&](const auto& a_entry) { return a_now - a_entry.second >= RESEND_AFTER; });
			}
			return true;
		}

		void Clear() { sent.clear(); }
		[[nodiscard]] std::size_t Size() const { return sent.size(); }

	private:
		static constexpr std::size_t                                                    MAX_KEPT = 256;
		std::map<std::tuple<Target, std::uint32_t, std::uint8_t>, Clock::time_point> sent;
	};
}
