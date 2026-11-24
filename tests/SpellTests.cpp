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

TEST("cast events: a hand's, the voice's and a ward's are told from the walk's and the swing's, in any case")
{
	CHECK(SpellRules::CastEvent("BeginCastRight"));
	CHECK(SpellRules::CastEvent("BeginCastLeft"));
	CHECK(SpellRules::CastEvent("BeginCastVoice"));
	CHECK(SpellRules::CastEvent("MLh_SpellFire_Event"));
	CHECK(SpellRules::CastEvent("MRh_SpellAimedStart"));
	CHECK(SpellRules::CastEvent("ShoutStart"));
	CHECK(SpellRules::CastEvent("shoutstop"));
	CHECK(SpellRules::CastEvent("CASTSTOP"));

	CHECK(!SpellRules::CastEvent("moveStart"));
	CHECK(!SpellRules::CastEvent("attackStart"));
	CHECK(!SpellRules::CastEvent("weaponDraw"));
	CHECK(!SpellRules::CastEvent("bowAttackStart"));
	CHECK(!SpellRules::CastEvent(""));
	CHECK(!SpellRules::CastEvent("cas"));
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
