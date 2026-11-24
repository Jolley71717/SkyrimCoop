#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Spells in the hands of NPC copies (docs/DESIGN.md, "NPCs": "Spells"): the rules, free of the game (NpcSync.cpp is the
// game's side; tests/SpellTests.cpp).
//
// A copy of an NPC another game runs holds what its runner's NPC holds, spells and the voice (a shout or a power)
// included, so a mage, a Dremora, a dragon priest, a hagraven, a spriggan or an atronach shows its spell in its hand, its
// hand's light and its casting as its runner's graph plays it. A copy holding a spell casts it here as its runner's
// cast events play (the engine fires a held spell at its graph's release), so what that cast does here is settled by
// these rules: its projectile is ended and launched again at its runner's aim (Shots.h); its harm to what this game runs
// stands, as its blows' does; its effects on itself show (a ward, a flesh spell, a cloak, invisibility) and any change
// to its health is set back to its runner's word (CopyHealth.h); and what makes or moves something in this world is
// refused (CopyMagic), since its runner's game makes the real one: a summoned creature or a raised corpse (shared as a
// spawn from the runner's game), a placed reference, an object or actor moved by telekinesis, a lock opened or closed,
// time slowed for everyone. Anything a copy made here all the same (its own AI's cast while its runner's states were
// late, a path the effect hook does not see) is ended (EndsMade).
namespace SpellRules
{
	// The effect archetypes the rules name (RE::EffectArchetypes::ArchetypeID; the same numbers as StandInRules').
	enum Archetype : std::int32_t
	{
		kArchNone = -1,
		kArchCommandSummoned = 10,
		kArchLock = 15,
		kArchOpen = 16,
		kArchBoundWeapon = 17,
		kArchSummonCreature = 18,
		kArchTelekinesis = 20,
		kArchReanimate = 22,
		kArchSlowTime = 37,
		kArchSpawnScriptedRef = 43,
		kArchGrabActor = 45,
	};

	// What a copy's effect is, as the effect hook judges it: let (the engine's, as anyone's), or refused for what it
	// would make or move here.
	enum class CopyMagic : std::uint8_t
	{
		kLet,
		kSummon,  // a summoned creature, a raised corpse, a commanded one: its runner's game makes the real one
		kSpawn,   // a reference a script's effect places
		kMove,    // telekinesis, an actor grabbed: an object or an actor of this world moved
		kLock,    // a lock opened or closed: the doors' sharing settles locks
		kTime,    // time slowed for everyone in this game
		kCount,
	};

	[[nodiscard]] constexpr std::string_view CopyMagicName(CopyMagic a_kind)
	{
		constexpr std::array<std::string_view, static_cast<std::size_t>(CopyMagic::kCount)> names{ "let", "summon", "spawn", "move", "lock", "time" };
		const auto                                                                          i = static_cast<std::size_t>(a_kind);
		return i < names.size() ? names[i] : "?";
	}

	// The verdict on an effect a copy of an NPC another game runs puts on anyone (itself included), by its archetype.
	[[nodiscard]] constexpr CopyMagic JudgeCopyMagic(std::int32_t a_archetype)
	{
		switch (a_archetype) {
		case kArchSummonCreature:
		case kArchReanimate:
		case kArchCommandSummoned:
			return CopyMagic::kSummon;
		case kArchSpawnScriptedRef:
			return CopyMagic::kSpawn;
		case kArchTelekinesis:
		case kArchGrabActor:
			return CopyMagic::kMove;
		case kArchLock:
		case kArchOpen:
			return CopyMagic::kLock;
		case kArchSlowTime:
			return CopyMagic::kTime;
		default:
			return CopyMagic::kLet;
		}
	}

	// Whether an archetype is one the hook needs to ask about at all (it looks the caster up only then).
	[[nodiscard]] constexpr bool Judged(std::int32_t a_archetype)
	{
		return JudgeCopyMagic(a_archetype) != CopyMagic::kLet;
	}

	// Who commands an actor this game made as it ran (a summon, a raised corpse, a thrall), as the scan sees it.
	enum class MadeBy : std::uint8_t
	{
		kNobody,
		kOurs,       // our player or an NPC this game runs: shared as our spawn (NpcRules::SharedAsSpawn)
		kStandIn,    // a friend's stand-in
		kOthersCopy, // a copy of an NPC another game runs
		kOther,      // an NPC nobody runs (this game's own AI runs it), or one not shared
	};

	// Whether such an actor is ended here: made by an actor this game shows for another game (a stand-in, a copy of an NPC
	// another game runs), whose game makes the real one, so this one would stand beside the copy of that one. Every kind
	// of actor made so: a summon (an atronach, a familiar, a Dremora: made as the game ran) and a raised or commanded one
	// (a corpse of the load order reanimated, a thrall, a commanded creature: an actor of this world).
	[[nodiscard]] constexpr bool EndsMade(MadeBy a_by)
	{
		return a_by == MadeBy::kStandIn || a_by == MadeBy::kOthersCopy;
	}

	// How such an actor is ended, each time the scan finds it, until it is gone. Never in the scan's own loop over the
	// process lists (NpcSync gathers them and ends them after it), and never disabled and deleted in one frame: the
	// disable's task, run later in the task pool on an actor the delete took out of its cell, crashed both games of the
	// first two-player test (Retire.h).
	//   1. Its making effect is dispelled (a summon's on its caster, a raise's or a command's on itself): the engine's own
	//      end of it, which takes a summon out of the world itself (so one just dispelled is never also deleted here) and
	//      lets a raised corpse fall.
	//   2. A runtime actor (a summon) with no such effect found, or still there MADE_GRACE after its dispel, is handed to
	//      the stand-ins' way out (RemotePlayers::TakeOut: disabled, then deleted only once its 3D is gone; Retire.h).
	//   3. A load-order actor (a raised corpse of the world, a commanded NPC) is never disabled or deleted: it is an NPC of
	//      this world and of its saves. Its dispel is tried again every MADE_GRACE while it stands commanded.
	using Duration = std::chrono::steady_clock::duration;
	inline constexpr auto MADE_GRACE = std::chrono::seconds(5);

	enum class MadeStep : std::uint8_t
	{
		kDispel,   // try its making effect's dispel now
		kWait,     // leave it be this time
		kTakeOut,  // hand it to the way out (a runtime actor only)
	};

	struct MadeFacts
	{
		bool                    loadOrder = false;  // an actor of the load order (never taken out)
		std::optional<Duration> sinceTried;         // since its dispel was last tried (none: never)
		bool                    dispelled = false;  // that try found its effect and dispelled it
		bool                    takenOut = false;   // handed to the way out already
	};

	[[nodiscard]] constexpr MadeStep NextMade(const MadeFacts& a_facts)
	{
		if (a_facts.takenOut) {
			return MadeStep::kWait;
		}
		if (!a_facts.sinceTried) {
			return MadeStep::kDispel;
		}
		if (a_facts.loadOrder) {
			return *a_facts.sinceTried >= MADE_GRACE ? MadeStep::kDispel : MadeStep::kWait;
		}
		if (a_facts.dispelled && *a_facts.sinceTried < MADE_GRACE) {
			return MadeStep::kWait;  // the engine is ending it: never deleted meanwhile
		}
		return MadeStep::kTakeOut;
	}

	// A graph event of a copy's runner's NPC, as a cast: the start of one (a hand's or the voice's wind-up:
	// "BeginCastRight", "BeginCastLeft", "BeginCastVoice", "ShoutStart") or its release (the spell let go: "MRh_SpellFire_Event",
	// "MLh_SpellFire_Event", "Voice_SpellFire_Event", "ShoutRelease..."), or none: a cast broken off or put away
	// ("InterruptCast", "CastStop", "ShoutStop") is no cast, nor is a hand's readying ("MRh_SpellReadyOut",
	// "MLh_Equipped_Event") or any walk's or swing's event. Counted apart on copies, so the rig tells a copy's casts played
	// from its walk's events. Any case.
	enum class Cast : std::uint8_t
	{
		kNone,
		kStart,
		kRelease,
	};

	[[nodiscard]] inline Cast CastOf(std::string_view a_event)
	{
		const auto lower = [](char a) { return static_cast<char>(std::tolower(static_cast<unsigned char>(a))); };
		const auto same = [&](char a, char b) { return lower(a) == lower(b); };
		const auto startsWith = [&](std::string_view a_part) {
			return a_part.size() <= a_event.size() && std::ranges::equal(a_event.substr(0, a_part.size()), a_part, same);
		};
		const auto holds = [&](std::string_view a_part) {
			for (std::size_t i = 0; i + a_part.size() <= a_event.size(); ++i) {
				if (std::ranges::equal(a_event.substr(i, a_part.size()), a_part, same)) {
					return true;
				}
			}
			return false;
		};
		if (startsWith("begincast") || startsWith("shoutstart")) {
			return Cast::kStart;
		}
		if (holds("spellfire") || startsWith("shoutrelease")) {
			return Cast::kRelease;
		}
		return Cast::kNone;
	}

	[[nodiscard]] inline bool CastEvent(std::string_view a_event)
	{
		return CastOf(a_event) != Cast::kNone;
	}

	// A copy's hand (or its voice), as the developer channel says it: what its runner's NPC holds there against what the
	// copy holds, and whether the copy may hold it (a copy of a load-order NPC is the NPC of this game, which only
	// readies a spell it knows, never one taught for the session, since a spell taught stays in its save; a spawn's copy
	// is this game's own and is taught it).
	enum class Hand : std::uint8_t
	{
		kEmpty,    // nothing wanted there, nothing held
		kShown,    // it holds what its runner's NPC holds
		kLacks,    // a spell or power it does not know, and may not be taught: it keeps its own there
		kUnnamed,  // its runner's game could not name it to this one: it keeps its own there
		kDiffers,  // anything else (not dressed yet, the equip refused): the rig's FAIL when it stays
	};

	[[nodiscard]] constexpr std::string_view HandName(Hand a_hand)
	{
		switch (a_hand) {
		case Hand::kEmpty:
			return "empty";
		case Hand::kShown:
			return "shown";
		case Hand::kLacks:
			return "lacks";
		case Hand::kUnnamed:
			return "unnamed";
		case Hand::kDiffers:
			return "differs";
		}
		return "?";
	}

	// a_wanted: its runner's NPC's (0: none, or unnamed when a_unnamed); a_held: the copy's; a_wantedSpell: the wanted
	// form is a spell or a power; a_known: the copy knows it; a_mayTeach: a spawn's copy (taught what it lacks).
	[[nodiscard]] constexpr Hand JudgeHand(std::uint32_t a_wanted, bool a_unnamed, std::uint32_t a_held, bool a_wantedSpell, bool a_known, bool a_mayTeach)
	{
		if (a_unnamed) {
			return Hand::kUnnamed;
		}
		if (a_wanted == 0) {
			return a_held == 0 ? Hand::kEmpty : Hand::kDiffers;
		}
		if (a_held == a_wanted) {
			return Hand::kShown;
		}
		if (a_wantedSpell && !a_known && !a_mayTeach) {
			return Hand::kLacks;
		}
		return Hand::kDiffers;
	}

	// A copy that holds a spell (or has a power or a shout ready) and is moved by its runner's states has its magicka
	// kept up, so its runner's casts play here whatever it spent: the runner's NPC is the truth of its magicka, and a copy
	// left short would turn its runner's next cast away. Only what it spent here is given back (a damage, never more
	// than its most).
	[[nodiscard]] constexpr float MagickaBack(bool a_mirrored, bool a_holdsMagic, float a_lost)
	{
		return a_mirrored && a_holdsMagic && a_lost > 0.0f ? a_lost : 0.0f;
	}
}
