#include "Test.h"

#include "game/SpellRules.h"

#include <array>
#include <cstdint>

// Spells in the hands of NPC copies (docs/DESIGN.md, "NPCs": "Spells"; src/game/SpellRules.h): what a copy's effects may
// do here, what it made here that is ended, its casts told from its walk, each hand judged, and its magicka kept up.

namespace
{
	using SpellRules::CopyMagic;
	using SpellRules::Hand;
	using SpellRules::MadeBy;

	constexpr std::uint32_t FIREBOLT = 0x0002B96B;
	constexpr std::uint32_t HEALING = 0x00012FCC;
	constexpr std::uint32_t CONJURE_FAMILIAR = 0x000640B6;
	constexpr std::uint32_t IRON_SWORD = 0x00012EB7;
}

TEST("copy magic: what makes or moves something here is refused, by kind; everything else is let")
{
	CHECK(SpellRules::JudgeCopyMagic(SpellRules::kArchSummonCreature) == CopyMagic::kSummon);
	CHECK(SpellRules::JudgeCopyMagic(SpellRules::kArchReanimate) == CopyMagic::kSummon);
	CHECK(SpellRules::JudgeCopyMagic(SpellRules::kArchCommandSummoned) == CopyMagic::kSummon);
	CHECK(SpellRules::JudgeCopyMagic(SpellRules::kArchSpawnScriptedRef) == CopyMagic::kSpawn);
	CHECK(SpellRules::JudgeCopyMagic(SpellRules::kArchTelekinesis) == CopyMagic::kMove);
	CHECK(SpellRules::JudgeCopyMagic(SpellRules::kArchGrabActor) == CopyMagic::kMove);
	CHECK(SpellRules::JudgeCopyMagic(SpellRules::kArchLock) == CopyMagic::kLock);
	CHECK(SpellRules::JudgeCopyMagic(SpellRules::kArchOpen) == CopyMagic::kLock);
	CHECK(SpellRules::JudgeCopyMagic(SpellRules::kArchSlowTime) == CopyMagic::kTime);

	// A ward, a flesh spell, a heal, a cloak, damage, a bound weapon (on the copy itself, gone with it), a light: let.
	constexpr std::array<std::int32_t, 19> lets{ SpellRules::kArchNone, 0, 1, 5, 11, 12, 13, 14, SpellRules::kArchBoundWeapon, 19, 31, 32, 34, 35, 40, 41, 44, 46, 1000 };
	for (const auto let : lets) {
		CHECK(SpellRules::JudgeCopyMagic(let) == CopyMagic::kLet);
		CHECK(!SpellRules::Judged(let));
	}
	CHECK(SpellRules::Judged(SpellRules::kArchSummonCreature));
	CHECK(SpellRules::Judged(SpellRules::kArchSlowTime));
}

TEST("copy magic: each kind has its name for the developer channel")
{
	CHECK(SpellRules::CopyMagicName(CopyMagic::kLet) == "let");
	CHECK(SpellRules::CopyMagicName(CopyMagic::kSummon) == "summon");
	CHECK(SpellRules::CopyMagicName(CopyMagic::kSpawn) == "spawn");
	CHECK(SpellRules::CopyMagicName(CopyMagic::kMove) == "move");
	CHECK(SpellRules::CopyMagicName(CopyMagic::kLock) == "lock");
	CHECK(SpellRules::CopyMagicName(CopyMagic::kTime) == "time");
	CHECK(SpellRules::CopyMagicName(CopyMagic::kCount) == "?");
}

TEST("made here: only what a stand-in or another game's NPC's copy made is ended; ours and this game's own stay")
{
	CHECK(SpellRules::EndsMade(MadeBy::kStandIn));
	CHECK(SpellRules::EndsMade(MadeBy::kOthersCopy));
	CHECK(!SpellRules::EndsMade(MadeBy::kOurs));  // shared as our spawn
	CHECK(!SpellRules::EndsMade(MadeBy::kOther));
	CHECK(!SpellRules::EndsMade(MadeBy::kNobody));
}

TEST("made here: how it is ended, a step each scan: dispelled first, never deleted just after its dispel")
{
	using SpellRules::MadeFacts;
	using SpellRules::MadeStep;
	using namespace std::chrono_literals;
	// Found: its making effect's dispel is tried, a summon's and a raised corpse's alike.
	CHECK(SpellRules::NextMade({}) == MadeStep::kDispel);
	CHECK(SpellRules::NextMade({ .loadOrder = true }) == MadeStep::kDispel);
	// A summon whose effect was dispelled: the engine is taking it out; left be until the grace has run.
	CHECK(SpellRules::NextMade({ .sinceTried = 0s, .dispelled = true }) == MadeStep::kWait);
	CHECK(SpellRules::NextMade({ .sinceTried = SpellRules::MADE_GRACE - 1ms, .dispelled = true }) == MadeStep::kWait);
	// Still there after it: sent out the stand-ins' way (disabled, then deleted: Retire.h).
	CHECK(SpellRules::NextMade({ .sinceTried = SpellRules::MADE_GRACE, .dispelled = true }) == MadeStep::kTakeOut);
	// A summon with no effect of it found: sent out at once.
	CHECK(SpellRules::NextMade({ .sinceTried = 0s, .dispelled = false }) == MadeStep::kTakeOut);
	// Sent out already: nothing more (the way out deletes it).
	CHECK(SpellRules::NextMade({ .sinceTried = 1h, .dispelled = false, .takenOut = true }) == MadeStep::kWait);
	// A load-order NPC (a raised corpse of the world, a thrall) is never taken out: its dispel is tried again after
	// the grace, whether the last try found an effect or not.
	CHECK(SpellRules::NextMade({ .loadOrder = true, .sinceTried = 0s, .dispelled = false }) == MadeStep::kWait);
	CHECK(SpellRules::NextMade({ .loadOrder = true, .sinceTried = 0s, .dispelled = true }) == MadeStep::kWait);
	CHECK(SpellRules::NextMade({ .loadOrder = true, .sinceTried = SpellRules::MADE_GRACE, .dispelled = false }) == MadeStep::kDispel);
	CHECK(SpellRules::NextMade({ .loadOrder = true, .sinceTried = 1h, .dispelled = true }) == MadeStep::kDispel);
	for (const auto since : { 0s, 1s, 10s, 3600s }) {
		for (const bool dispelled : { false, true }) {
			CHECK(SpellRules::NextMade({ .loadOrder = true, .sinceTried = since, .dispelled = dispelled }) != MadeStep::kTakeOut);
		}
	}
}

TEST("cast events: only a cast's start and its release are casts; a cast broken off or put away is none")
{
	using SpellRules::Cast;
	// The wind-up of a hand's or the voice's cast.
	CHECK(SpellRules::CastOf("BeginCastRight") == Cast::kStart);
	CHECK(SpellRules::CastOf("BeginCastLeft") == Cast::kStart);
	CHECK(SpellRules::CastOf("BeginCastVoice") == Cast::kStart);
	CHECK(SpellRules::CastOf("ShoutStart") == Cast::kStart);
	CHECK(SpellRules::CastOf("begincastright") == Cast::kStart);
	// The spell let go.
	CHECK(SpellRules::CastOf("MRh_SpellFire_Event") == Cast::kRelease);
	CHECK(SpellRules::CastOf("MLh_SpellFire_Event") == Cast::kRelease);
	CHECK(SpellRules::CastOf("Voice_SpellFire_Event") == Cast::kRelease);
	CHECK(SpellRules::CastOf("ShoutReleaseSlowTime") == Cast::kRelease);
	CHECK(SpellRules::CastOf("MLH_SPELLFIRE_EVENT") == Cast::kRelease);
	// Broken off, put away or only readied: no cast.
	CHECK(SpellRules::CastOf("InterruptCast") == Cast::kNone);
	CHECK(SpellRules::CastOf("CastStop") == Cast::kNone);
	CHECK(SpellRules::CastOf("CASTSTOP") == Cast::kNone);
	CHECK(SpellRules::CastOf("ShoutStop") == Cast::kNone);
	CHECK(SpellRules::CastOf("MRh_SpellReadyOut") == Cast::kNone);
	CHECK(SpellRules::CastOf("MLh_Equipped_Event") == Cast::kNone);
	CHECK(SpellRules::CastOf("MRh_SpellAimedStart") == Cast::kNone);
	// The walk's and the swing's.
	CHECK(SpellRules::CastOf("moveStart") == Cast::kNone);
	CHECK(SpellRules::CastOf("attackStart") == Cast::kNone);
	CHECK(SpellRules::CastOf("weaponDraw") == Cast::kNone);
	CHECK(SpellRules::CastOf("bowAttackStart") == Cast::kNone);
	CHECK(SpellRules::CastOf("") == Cast::kNone);
	CHECK(SpellRules::CastOf("begin") == Cast::kNone);
	// CastEvent: a start or a release.
	CHECK(SpellRules::CastEvent("BeginCastRight"));
	CHECK(SpellRules::CastEvent("MRh_SpellFire_Event"));
	CHECK(!SpellRules::CastEvent("InterruptCast"));
	CHECK(!SpellRules::CastEvent("CastStop"));
}

TEST("hands: shown when it holds its runner's, empty when neither holds anything")
{
	CHECK(SpellRules::JudgeHand(FIREBOLT, false, FIREBOLT, true, true, false) == Hand::kShown);
	CHECK(SpellRules::JudgeHand(IRON_SWORD, false, IRON_SWORD, false, false, false) == Hand::kShown);
	CHECK(SpellRules::JudgeHand(0, false, 0, false, false, false) == Hand::kEmpty);
	// Its runner's NPC holds nothing there, the copy something: differs (to be taken off).
	CHECK(SpellRules::JudgeHand(0, false, HEALING, false, true, false) == Hand::kDiffers);
}

TEST("hands: a load-order NPC's copy is never taught a spell: one it does not know is lacked, not a failure")
{
	// The copy holds its own spell (or nothing) because it does not know its runner's: lacks.
	CHECK(SpellRules::JudgeHand(CONJURE_FAMILIAR, false, HEALING, true, false, false) == Hand::kLacks);
	CHECK(SpellRules::JudgeHand(CONJURE_FAMILIAR, false, 0, true, false, false) == Hand::kLacks);
	// It knows it but does not hold it: not dressed yet, or the equip refused.
	CHECK(SpellRules::JudgeHand(CONJURE_FAMILIAR, false, HEALING, true, true, false) == Hand::kDiffers);
	// A spawn's copy is taught it: not holding it is a failure, never "lacks".
	CHECK(SpellRules::JudgeHand(CONJURE_FAMILIAR, false, 0, true, false, true) == Hand::kDiffers);
	// A weapon it lacks is not a spell: differs (the gear's own missing count says why).
	CHECK(SpellRules::JudgeHand(IRON_SWORD, false, 0, false, false, false) == Hand::kDiffers);
}

TEST("hands: one its runner's game could not name leaves the copy's own, whatever it holds")
{
	CHECK(SpellRules::JudgeHand(0, true, HEALING, false, false, false) == Hand::kUnnamed);
	CHECK(SpellRules::JudgeHand(0, true, 0, false, false, true) == Hand::kUnnamed);
}

TEST("hands: each state has its name for the developer channel")
{
	CHECK(SpellRules::HandName(Hand::kEmpty) == "empty");
	CHECK(SpellRules::HandName(Hand::kShown) == "shown");
	CHECK(SpellRules::HandName(Hand::kLacks) == "lacks");
	CHECK(SpellRules::HandName(Hand::kUnnamed) == "unnamed");
	CHECK(SpellRules::HandName(Hand::kDiffers) == "differs");
}

TEST("magicka: a mirrored copy holding magic is given back what it spent; never more, never otherwise")
{
	CHECK(SpellRules::MagickaBack(true, true, 35.0f) == 35.0f);
	CHECK(SpellRules::MagickaBack(true, true, 0.0f) == 0.0f);
	CHECK(SpellRules::MagickaBack(true, true, -10.0f) == 0.0f);  // fortified past its most: nothing taken
	CHECK(SpellRules::MagickaBack(false, true, 35.0f) == 0.0f);  // its own AI runs it: its magicka is its own
	CHECK(SpellRules::MagickaBack(true, false, 35.0f) == 0.0f);  // holds no spell: a drain stays
}
