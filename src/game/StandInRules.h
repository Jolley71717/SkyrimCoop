#pragma once

#include "FormIds.h"
#include "Protocol.h"
#include "TokenBucket.h"
#include "game/NpcRules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a friend's stand-in does, holds and is let do in this game (docs/DESIGN.md, "Players"): which of a player's graph
// events go to the others and how fast, what kind of action each is (for the developer channel and the rig), what the
// stand-in is dressed in and given, how its weapon is drawn and put away (the game's own way), how it aims, and
// which of its spells' effects take hold here. A stand-in shows what its friend does; it never deals anything: its
// friend's own game deals every blow, shot and spell (a hit on a copy of an NPC goes to the NPC's runner, NpcSync), so
// one dealt here too would land twice. The game's side is src/game/RemotePlayers.cpp and src/game/PlayerActions.cpp. No
// game here: tests/StandInTests.cpp.
namespace StandInRules
{
	using Clock = std::chrono::steady_clock;
	using namespace std::chrono_literals;

	// ---- what kind of action an event is ----

	// Every event of a player's graph is played on their stand-in the same way; its kind is only counted, so the developer
	// channel and the rig can say which kinds of action were sent, played and turned away. The kind is read from the
	// event's name as Skyrim's character behavior names them (a mod's own events that follow the same words count the
	// same; any other is kOther, and is played all the same).
	enum class Act : std::uint8_t
	{
		kAttack,    // a swing or a stab, either hand or both (attackStart, attackStartLeftHand, attackStartDualWield)
		kPower,     // a power attack (attackPowerStartForward, ...)
		kBlock,     // raising and lowering a weapon or a shield (blockStart, blockStop)
		kBash,      // a bash, a power bash (bashStart, bashPowerStart)
		kBow,       // a bow or a crossbow drawn, aimed and let go (bowAttackStart, BowZoomStart)
		kCast,      // a spell begun or ended in a hand, both hands, a concentration spell held (BeginCastRight, CastStop)
		kShout,     // a shout or a power (BeginCastVoice, shoutStart, shoutRelease)
		kDraw,      // a weapon or a spell drawn, put away or swapped (WeapEquip, Unequip, MagicForceEquipLeft)
		kKillMove,  // a paired animation's own events
		kOther,
		kCount,
	};

	inline constexpr std::array<std::string_view, static_cast<std::size_t>(Act::kCount)> ACT_NAMES{ "attack", "power", "block", "bash", "bow", "cast", "shout",
		"draw", "killmove", "other" };

	[[nodiscard]] constexpr std::string_view ActName(Act a_act)
	{
		const auto i = static_cast<std::size_t>(a_act);
		return i < ACT_NAMES.size() ? ACT_NAMES[i] : "?";
	}

	[[nodiscard]] inline bool Has(std::string_view a_text, std::string_view a_part)
	{
		if (a_part.size() > a_text.size()) {
			return false;
		}
		for (std::size_t i = 0; i + a_part.size() <= a_text.size(); ++i) {
			if (NpcRules::SameName(a_text.substr(i, a_part.size()), a_part)) {
				return true;
			}
		}
		return false;
	}

	// The most telling word first: a power bash is a bash, a bow's attack a bow's, a shout a shout (its cast is the
	// voice's), a spell put in a hand a draw.
	[[nodiscard]] inline Act ActOf(std::string_view a_event)
	{
		if (Has(a_event, "killmove") || Has(a_event, "paired")) {
			return Act::kKillMove;
		}
		if (Has(a_event, "voice") || Has(a_event, "shout")) {
			return Act::kShout;
		}
		if (Has(a_event, "bash")) {
			return Act::kBash;
		}
		if (Has(a_event, "attackpower")) {
			return Act::kPower;
		}
		if (Has(a_event, "bow")) {
			return Act::kBow;
		}
		if (Has(a_event, "block")) {
			return Act::kBlock;
		}
		if (Has(a_event, "equip")) {
			return Act::kDraw;
		}
		if (Has(a_event, "cast") || Has(a_event, "spell")) {
			return Act::kCast;
		}
		if (Has(a_event, "attack")) {
			return Act::kAttack;
		}
		return Act::kOther;
	}

	// How many events of each kind, said as "attack:3,power:1" (the kinds with none left out; "none" when all are).
	struct ActCounts
	{
		std::array<std::uint64_t, static_cast<std::size_t>(Act::kCount)> n{};

		void Add(Act a_act) { ++n[static_cast<std::size_t>(a_act)]; }
		void Add(std::string_view a_event) { Add(ActOf(a_event)); }
		[[nodiscard]] std::uint64_t Of(Act a_act) const { return n[static_cast<std::size_t>(a_act)]; }
		[[nodiscard]] std::uint64_t Total() const
		{
			std::uint64_t total = 0;
			for (const auto v : n) {
				total += v;
			}
			return total;
		}
		[[nodiscard]] std::string Line() const
		{
			std::string out;
			for (std::size_t i = 0; i < n.size(); ++i) {
				if (n[i] != 0) {
					out += (out.empty() ? "" : ",") + std::string{ ACT_NAMES[i] } + ":" + std::to_string(n[i]);
				}
			}
			return out.empty() ? "none" : out;
		}
	};

	// ---- our player's events, on their way out ----

	// The events our player's graph took that the others' stand-in of us plays: each one the NPC copies would play too
	// (NpcRules::Replayed: not the walk's own, which every game works out from the states, and a name a graph could
	// take), in the order heard, each with when it was heard. A budget keeps a burst from flooding the session (a mod
	// that sends its own events every frame): past it, an event is dropped and counted. Taken once a tick, at most
	// Protocol::MAX_PLAYER_ACTIONS at a time (the rest wait for the next tick).
	class Outbox
	{
	public:
		static constexpr std::size_t MAX_WAITING = 256;
		static constexpr double      BURST = 64.0;
		static constexpr double      PER_SECOND = 32.0;

		// False when it is not one to send, or it was dropped.
		bool Heard(std::string_view a_event, std::uint32_t a_sentMs, Clock::time_point a_now)
		{
			if (!NpcRules::Replayed(a_event)) {
				return false;
			}
			++heard;
			byAct.Add(a_event);
			if (queue.size() >= MAX_WAITING || !budget.Take(a_now)) {
				++dropped;
				return false;
			}
			queue.push_back({ a_sentMs, std::string{ a_event } });
			return true;
		}

		[[nodiscard]] std::vector<Protocol::PlayerAction> Take()
		{
			std::vector<Protocol::PlayerAction> out;
			while (!queue.empty() && out.size() < Protocol::MAX_PLAYER_ACTIONS) {
				out.push_back(std::move(queue.front()));
				queue.pop_front();
			}
			sent += out.size();
			return out;
		}

		// Not in a session any more (or a new one): what waits is no one's.
		void Clear() { queue.clear(); }

		[[nodiscard]] std::size_t Waiting() const { return queue.size(); }

		std::uint64_t heard = 0;
		std::uint64_t sent = 0;
		std::uint64_t dropped = 0;
		ActCounts     byAct;

	private:
		std::deque<Protocol::PlayerAction> queue;
		TokenBucket                        budget{ BURST, PER_SECOND };
	};

	// ---- what our player did, as the game shows it ----

	// What our player's actor is doing now, read from the game each tick (PlayerActions::Sample), apart from the graph
	// events the hook hears: the rig's proof that a press did something in the game, so a press the hook heard nothing of
	// is the mod's failure (the events go some way the hook does not see), not the rig's input.
	struct Doing
	{
		std::uint32_t attackState = 0;  // the actor state's melee attack state (the game's ATTACK_STATE_ENUM numbers)
		bool          power = false;    // its attack data is a power attack's
		bool          blocking = false;
		bool          castLeft = false;  // a hand's or the voice's magic caster is under way (any state but none)
		bool          castRight = false;
		bool          castVoice = false;
		float         voiceRecovery = 0.0f;  // seconds left before the voice may be used again (a shout just made)
		bool          drawn = false;
		float         stamina = 0.0f;
		float         magicka = 0.0f;
	};

	// The attack states, as the game numbers them (RemotePlayers.cpp checks them against the game's at compile time).
	inline constexpr std::uint32_t kAttackNone = 0;
	inline constexpr std::uint32_t kAttackDraw = 1;
	inline constexpr std::uint32_t kAttackFollowThrough = 5;
	inline constexpr std::uint32_t kAttackBash = 6;
	inline constexpr std::uint32_t kAttackBowDraw = 8;
	inline constexpr std::uint32_t kAttackFired = 17;

	// Each kind of action the actor began (a state it was not in at the last reading), counted by kind like the events;
	// the stamina and magicka it spent (each fall from one reading to the next, added up).
	class Seen
	{
	public:
		void Sample(const Doing& a_now)
		{
			if (last) {
				const auto kindOf = [](const Doing& a_d) -> std::optional<Act> {
					if (a_d.attackState >= kAttackDraw && a_d.attackState <= kAttackFollowThrough) {
						return a_d.power ? Act::kPower : Act::kAttack;
					}
					if (a_d.attackState == kAttackBash) {
						return Act::kBash;
					}
					if (a_d.attackState >= kAttackBowDraw && a_d.attackState <= kAttackFired) {
						return Act::kBow;
					}
					return std::nullopt;
				};
				const auto was = kindOf(*last);
				const auto now = kindOf(a_now);
				if (now && now != was) {
					byAct.Add(*now);
				}
				if (a_now.blocking && !last->blocking) {
					byAct.Add(Act::kBlock);
				}
				if ((a_now.castLeft && !last->castLeft) || (a_now.castRight && !last->castRight)) {
					byAct.Add(Act::kCast);
				}
				const auto shouting = [](const Doing& a_d) { return a_d.castVoice || a_d.voiceRecovery > 0.0f; };
				if (shouting(a_now) && !shouting(*last)) {
					byAct.Add(Act::kShout);
				}
				if (a_now.drawn != last->drawn) {
					byAct.Add(Act::kDraw);
				}
				staminaSpent += (std::max)(0.0f, last->stamina - a_now.stamina);
				magickaSpent += (std::max)(0.0f, last->magicka - a_now.magicka);
			}
			last = a_now;
		}

		// Not read for a while (out of the world, a load): the next reading starts afresh.
		void Forget() { last.reset(); }

		ActCounts byAct;
		float     staminaSpent = 0.0f;
		float     magickaSpent = 0.0f;

	private:
		std::optional<Doing> last;
	};

	// ---- what a stand-in holds ----

	// What a form named for a hand, the quiver or the voice is, as RemotePlayers reads it from this game.
	struct Holdable
	{
		enum class Kind : std::uint8_t
		{
			kNothing,  // no form here by that id
			kWeapon,   // a weapon or a staff
			kSpell,    // a spell cast from a hand (not an ability, a disease, a power)
			kScroll,
			kTorch,    // a light that can be carried
			kShield,   // an armor (a shield, or any piece the game puts in a hand)
			kAmmo,
			kShout,
			kPower,  // a power, a lesser power, a voice power
			kOther,
		};
		Kind kind = Kind::kNothing;
		bool playable = true;  // its non-playable flag is not set (weapons, armor, ammunition)
		bool bound = false;    // a conjured weapon (its bound flag: Bound Sword, Bound Bow, a mod's own)
	};

	// Another player's game named it: kept only where a player could have it. In a hand: a playable weapon or staff, or a
	// conjured one (Skyrim.esm flags its bound weapons non-playable: a player has one only from its spell, and so does
	// their friend), a spell, a scroll, a torch, a playable shield. The quiver: playable ammunition, or any while a
	// conjured weapon is in a hand (a mod's conjured bow may conjure non-playable arrows of its own). The voice: a shout,
	// or a power.
	[[nodiscard]] constexpr bool KeptInHand(const Holdable& a_held)
	{
		using K = Holdable::Kind;
		switch (a_held.kind) {
		case K::kWeapon:
			return a_held.playable || a_held.bound;
		case K::kSpell:
		case K::kScroll:
		case K::kTorch:
			return true;
		case K::kShield:
			return a_held.playable;
		default:
			return false;
		}
	}

	[[nodiscard]] constexpr bool KeptInQuiver(const Holdable& a_ammo, bool a_boundInHand)
	{
		return a_ammo.kind == Holdable::Kind::kAmmo && (a_ammo.playable || a_boundInHand);
	}

	[[nodiscard]] constexpr bool KeptAsVoice(const Holdable& a_held)
	{
		return a_held.kind == Holdable::Kind::kShout || a_held.kind == Holdable::Kind::kPower;
	}

	// A stand-in's voice is not its friend's: a_wanted, as its friend's looks name it (0: none ready), is not the one it
	// has ready. Not judged when its friend's game could not name it (a_unnamed: the stand-in keeps what it has).
	[[nodiscard]] constexpr bool VoiceDrifted(std::uint32_t a_wanted, bool a_unnamed, std::uint32_t a_shown)
	{
		return !a_unnamed && a_wanted != a_shown;
	}

	// The gear a stand-in is dressed in (NpcRules::PlanGear, as a spawn's copy is: the stand-in is this game's own and
	// never saved, so it is given what it lacks): what its friend wears, holds in each hand and has for ammunition. The
	// worn items' body slots are not needed (a stand-in lacks nothing). What its friend's game could not name to this one
	// (an unnamed hand, the quiver) is kept as the stand-in holds it.
	[[nodiscard]] inline Protocol::NpcGear GearFor(const Protocol::PlayerLooks& a_looks)
	{
		Protocol::NpcGear gear;
		for (const auto id : a_looks.worn) {
			if (std::ranges::none_of(gear.worn, [&](const Protocol::WornItem& a_w) { return a_w.id == id; })) {
				gear.worn.push_back({ id, 0 });
			}
		}
		gear.right = a_looks.right;
		gear.left = a_looks.left;
		gear.ammo = a_looks.ammo;
		gear.unnamed = static_cast<std::uint8_t>(a_looks.unnamed & Protocol::kAllUnnamed);
		return gear;
	}

	// What a change of a friend's looks takes: a new stand-in for another body (race, sex, weight: its base, a preset of
	// that race and sex, and its body are the actor's own), or the same stand-in dressed again for other gear.
	enum class Change
	{
		kSame,
		kRedress,
		kRemake,
	};

	[[nodiscard]] inline Change Compare(const Protocol::PlayerLooks& a_was, const Protocol::PlayerLooks& a_now)
	{
		if (a_was.race != a_now.race || a_was.female != a_now.female || a_was.weight != a_now.weight) {
			return Change::kRemake;
		}
		return a_was == a_now ? Change::kSame : Change::kRedress;
	}

	// A stand-in's ammunition: its friend's game counts their arrows, this one only shows them; a stand-in whose friend
	// shoots all evening is never out. Given AMMO_STOCK whenever it holds fewer than AMMO_LOW.
	inline constexpr std::int32_t AMMO_LOW = 20;
	inline constexpr std::int32_t AMMO_STOCK = 60;

	[[nodiscard]] constexpr std::int32_t AmmoToAdd(std::int32_t a_have)
	{
		return a_have < AMMO_LOW ? AMMO_STOCK - (std::max)(a_have, 0) : 0;
	}

	// ---- what a stand-in's spells do here ----

	// The kinds of magic effect (the game's EffectArchetypes::ArchetypeID numbers; RemotePlayers.cpp checks them against
	// the game's at compile time).
	enum Archetype : std::int32_t
	{
		kArchNone = -1,
		kValueModifier = 0,
		kScript = 1,
		kDualValueModifier = 5,
		kCommandSummoned = 10,
		kInvisibility = 11,
		kLight = 12,
		kDarkness = 13,
		kNightEye = 14,
		kBoundWeapon = 17,
		kSummonCreature = 18,
		kDetectLife = 19,
		kReanimate = 22,
		kValueAndParts = 31,
		kAccumulateMagnitude = 32,
		kPeakValueModifier = 34,
		kCloak = 35,
		kEtherealize = 41,
		kSpawnHazard = 40,
		kSpawnScriptedRef = 43,
	};

	// Whether an effect of a stand-in's spell (or its weapon's enchantment, or its poison, or its shout) takes hold on an
	// actor here. On anyone else: never. Its friend's game casts the real spell, and its effect on an NPC reaches that
	// NPC's runner as its friend's hit (NpcSync), so one taken here too would hurt, calm, frighten or paralyse it twice;
	// on our player it would be friendly fire nobody aimed in our game. On the stand-in itself: only what shows on it and
	// changes nothing outside it (a ward's or a flesh spell's change to its own values, invisibility, a light, night eye,
	// ethereal form, a cloak's look, which hurts nobody since its hits on others are refused here too). What makes
	// something (a summoned creature, a raised corpse, a bound weapon: its friend's game makes the real one, which is
	// shared as an NPC, or held in a hand and shown by the looks), runs a script, or moves anything is refused.
	[[nodiscard]] constexpr bool MagicKept(bool a_onItself, std::int32_t a_archetype)
	{
		if (!a_onItself) {
			return false;
		}
		switch (a_archetype) {
		case kArchNone:
		case kValueModifier:
		case kDualValueModifier:
		case kInvisibility:
		case kLight:
		case kDarkness:
		case kNightEye:
		case kDetectLife:
		case kValueAndParts:
		case kAccumulateMagnitude:
		case kPeakValueModifier:
		case kCloak:
		case kEtherealize:
			return true;
		default:
			return false;
		}
	}

	// ---- drawn or put away ----

	// An actor's weapon state, as the game keeps it (RE::WEAPON_STATE, static_asserted in RemotePlayers.cpp): what
	// Actor::DrawWeaponMagicHands and the draw and sheathe animation events move it through.
	enum class Weapon : std::uint8_t
	{
		kSheathed = 0,
		kWantToDraw = 1,
		kDrawing = 2,
		kDrawn = 3,
		kWantToSheathe = 4,
		kSheathing = 5,
	};

	// The game draws and puts away an actor's weapon, spells or fists through Actor::DrawWeaponMagicHands (read from
	// Skyrim SE 1.7.104): it asks the actor's graph for the draw or the sheathe through the game's action (ActionDraw,
	// ActionSheathe: the graph event that fits what the actor holds, a sword, a bow, a spell, its fists) and, when the
	// graph turns it away (its animation not done yet, say), leaves the actor wanting to (kWantToDraw, kWantToSheathe).
	// Actor::Update asks again at its end on every update while the actor still wants to; the no-AI update a stand-in
	// gets (UpdateNoAI) never does, so a stand-in asked once at a moment its graph was busy stayed wanting forever and
	// its graph never drew. What the game's own update would ask, if anything: draw (true), sheathe (false), nothing.
	[[nodiscard]] constexpr std::optional<bool> AskAgain(Weapon a_state)
	{
		if (a_state == Weapon::kWantToDraw) {
			return true;
		}
		if (a_state == Weapon::kWantToSheathe) {
			return false;
		}
		return std::nullopt;
	}

	// Drawn or put away, done or under way: where the state stands or is heading (kDrawing, kDrawn and kWantToDraw
	// toward drawn).
	[[nodiscard]] constexpr bool TowardDrawn(Weapon a_state)
	{
		return a_state == Weapon::kWantToDraw || a_state == Weapon::kDrawing || a_state == Weapon::kDrawn;
	}

	[[nodiscard]] constexpr bool Settled(Weapon a_state)
	{
		return a_state == Weapon::kDrawn || a_state == Weapon::kSheathed;
	}

	// A stand-in draws and puts away as its friend does, the game's own way: told to (DrawWeaponMagicHands) the moment
	// its friend's drawn flag and its own weapon state part (kFirst), once it holds what its friend holds (a_ready),
	// so it draws the friend's sword and not its fists; the game then finishes it (AskAgain). Its friend's own graph
	// events (WeapEquip, Unequip, a spell's) are played too, as they come: whichever its graph takes first, the
	// animation's weaponDraw or weaponSheathe event sets the state. If after that ask the state still is the other
	// one, settled, for GRACE (something put it back: a draw turned away and given up), it is told again (kAgain), no
	// sooner than COOL after the last time; those are counted (fixes: the rig's drawFixes=).
	class DrawFix
	{
	public:
		static constexpr auto GRACE = 1500ms;
		static constexpr auto COOL = 3s;

		enum class Ask : std::uint8_t
		{
			kNone,
			kFirst,  // its friend's drawn flag changed (or a new stand-in): told now
			kAgain,  // told before, and its state went back: told again
		};

		// a_wanted: its friend has it drawn; a_state: the stand-in's weapon state; a_ready: it holds what its friend does.
		[[nodiscard]] Ask Due(bool a_wanted, Weapon a_state, bool a_ready, Clock::time_point a_now)
		{
			if (!a_ready) {
				since.reset();
				return Ask::kNone;
			}
			if (TowardDrawn(a_state) == a_wanted) {
				// There, or on its way there (the game finishes a want: AskAgain), told or by its friend's own events:
				// remembered as told, so its friend's next change is told at once and not left to the grace.
				asked = a_wanted;
				since.reset();
				return Ask::kNone;
			}
			if (asked != a_wanted) {
				asked = a_wanted;
				since.reset();
				last = a_now;
				++asks;
				return Ask::kFirst;
			}
			if (!Settled(a_state)) {
				since.reset();  // sheathing while its friend draws, or the other way: told already, under way
				return Ask::kNone;
			}
			if (!since) {
				since = a_now;
			}
			if (a_now - *since < GRACE || (last && a_now - *last < COOL)) {
				return Ask::kNone;
			}
			last = a_now;
			since.reset();
			++fixes;
			return Ask::kAgain;
		}

		// A new stand-in: nothing it was told before counts (it is told its friend's state again when it is ready).
		void Reset()
		{
			asked.reset();
			since.reset();
			last.reset();
		}

		std::uint64_t asks = 0;   // told as its friend's drawn flag changed
		std::uint64_t fixes = 0;  // told again: the first time did not hold

	private:
		std::optional<bool>              asked;
		std::optional<Clock::time_point> since;
		std::optional<Clock::time_point> last;
	};

	// ---- aim ----

	// The pitch a stand-in aims at, in the game's radians (down positive, as an actor's angle x holds it): its friend's
	// while a weapon or a spell is out (a bow aimed up at a dragon, a spell down at a skeever), level otherwise (a pitch
	// on an actor that is only walking would tilt its head for nothing). Held within MAX_AIM either way.
	inline constexpr float MAX_AIM_DEGREES = 60.0f;

	[[nodiscard]] inline float AimPitch(const Protocol::PlayerState& a_state)
	{
		if ((a_state.flags & Protocol::kWeaponDrawn) == 0) {
			return 0.0f;
		}
		const float degrees = std::clamp(static_cast<float>(a_state.pitch), -MAX_AIM_DEGREES, MAX_AIM_DEGREES);
		return -degrees * std::numbers::pi_v<float> / 180.0f;
	}
}
