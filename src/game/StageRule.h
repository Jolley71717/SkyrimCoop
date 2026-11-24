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
		kMoves,  // its fragment moves the player (a summons, "come with me"): made only with the friend who did it near
		kOwn,    // each game's own: never made from the session's word
	};

	struct StageRule
	{
		std::string_view editorId;
		std::uint16_t    stage = 0;
		StageVerdict     verdict = StageVerdict::kMoves;
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
