#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

// A row of the stage rules (QuestRules::STAGE_RULES, written by tools/questscan.ps1 into src/game/StageRows.h): a stage of
// a quest, by the quest's editor ID, that is not made as it is from the session's word. Kept apart from QuestRules.h so
// the written rows need nothing else.
namespace QuestRules
{
	enum class StageVerdict : std::uint8_t
	{
		kMoves,  // its fragment moves the player, or takes their screen or controls (a summons, "come with me", a scene
		         // the player is held in): made only with the friend who did it near
		kOwn,    // each game's own: never made from the session's word
	};

	// What a kMoves row's fragment does to the player, as the scan found it (for the log line of a stage held or given
	// up: a friend frozen or left with a black screen is said as such).
	enum class StageEffect : std::uint8_t
	{
		kMove,      // MoveTo, SetPosition, TranslateTo, a door, fast travel
		kScreen,    // the screen faded or held black (FadeOutGame, an image space applied)
		kControls,  // controls taken (DisablePlayerControls, SetPlayerAIDriven)
		kFrozen,    // the player restrained, made not to move, or their AI off
		kChargen,   // SetInChargen: no saving or waiting
	};

	[[nodiscard]] constexpr std::string_view EffectName(StageEffect a_effect)
	{
		switch (a_effect) {
		case StageEffect::kMove:
			return "moves the player";
		case StageEffect::kScreen:
			return "fades or holds the screen black";
		case StageEffect::kControls:
			return "takes the player's controls";
		case StageEffect::kFrozen:
			return "freezes the player";
		case StageEffect::kChargen:
			return "stops saving and waiting";
		}
		return "?";
	}

	struct StageRule
	{
		std::string_view editorId;
		std::uint16_t    stage = 0;
		StageVerdict     verdict = StageVerdict::kMoves;
		StageEffect      effect = StageEffect::kMove;
		bool             startUp = false;  // the quest's start-up stage: run by any stage made while the quest is not started
	};

	// The rows' order: editor ID in any case (compared upper-cased, as the scan sorts them: C#'s OrdinalIgnoreCase), then
	// stage.
	[[nodiscard]] constexpr char Upper(char a_c) { return a_c >= 'a' && a_c <= 'z' ? static_cast<char>(a_c - 'a' + 'A') : a_c; }

	[[nodiscard]] constexpr int CompareName(std::string_view a_a, std::string_view a_b)
	{
		const auto n = std::min(a_a.size(), a_b.size());
		for (std::size_t i = 0; i < n; ++i) {
			const auto x = static_cast<unsigned char>(Upper(a_a[i]));
			const auto y = static_cast<unsigned char>(Upper(a_b[i]));
			if (x != y) {
				return x < y ? -1 : 1;
			}
		}
		return a_a.size() == a_b.size() ? 0 : (a_a.size() < a_b.size() ? -1 : 1);
	}

	[[nodiscard]] constexpr bool RuleLess(const StageRule& a_a, const StageRule& a_b)
	{
		const auto c = CompareName(a_a.editorId, a_b.editorId);
		return c != 0 ? c < 0 : a_a.stage < a_b.stage;
	}
}
