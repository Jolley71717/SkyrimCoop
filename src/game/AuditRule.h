#pragma once

#include <cstdint>
#include <string_view>

// A row of the quest audit (QuestRules::QUEST_AUDIT, written by tools/questscan.ps1 -Audit into src/game/QuestAudit.h):
// every quest of the base game (Skyrim.esm, Update.esm and the three add-ons, as the game loads them), what its record
// says, and whether a session shares it, with the reason. Kept apart from QuestRules.h so the written rows need nothing
// else. The code reads it (QuestRules::AuditOf): each game checks its own records against it at load, and the dev line
// `quest` says a quest's row; tests/QuestTests.cpp checks every row against QuestRules::Classify and STAGE_RULES, so the
// audit, the rules and the game never say different things, and no quest is left unknown.
namespace QuestRules
{
	enum class AuditVerdict : std::uint8_t
	{
		kShared,     // its stages, objectives and end are everyone's
		kLimited,    // shared, with limits (AuditWhy says which)
		kNotShared,  // each player's own (AuditWhy says why)
	};

	enum class AuditWhy : std::uint8_t
	{
		kWhole,       // kShared: every stage as it is
		kNearStages,  // kLimited: some stages are made only with the friend who did the step near (STAGE_RULES)
		kHostRun,     // kLimited: a radiant job, shared while it runs as the host's run, with the host's targets
		kOwnList,     // kNotShared: a typed quest each player plays for themselves (OWN_QUESTS)
		kMisc,        // kNotShared: the misc tab, in this version
		kUntyped,     // kNotShared: no journal tab: the game's machinery (dialogue, followers, trackers, encounters)
		kNoName,      // kNotShared: no editor ID to name it by when it is made
	};

	[[nodiscard]] constexpr std::string_view AuditVerdictName(AuditVerdict a_verdict)
	{
		switch (a_verdict) {
		case AuditVerdict::kShared:
			return "shared";
		case AuditVerdict::kLimited:
			return "limited";
		case AuditVerdict::kNotShared:
			return "notShared";
		}
		return "?";
	}

	[[nodiscard]] constexpr std::string_view AuditWhyName(AuditWhy a_why)
	{
		switch (a_why) {
		case AuditWhy::kWhole:
			return "whole";
		case AuditWhy::kNearStages:
			return "nearStages";
		case AuditWhy::kHostRun:
			return "hostRun";
		case AuditWhy::kOwnList:
			return "ownList";
		case AuditWhy::kMisc:
			return "misc";
		case AuditWhy::kUntyped:
			return "untyped";
		case AuditWhy::kNoName:
			return "noName";
		}
		return "?";
	}

	struct AuditRow
	{
		std::uint32_t    formId = 0;    // as the game loads it: the master's index (Skyrim.esm 00 ... Dragonborn.esm 04) on top
		std::string_view editorId;
		std::uint8_t     type = 0;      // the journal tab (QuestRules::Facts::type)
		bool             radiant = false;  // started by the Story Manager for an event, and not run once
		std::uint16_t    nearStages = 0;   // its stages in STAGE_RULES
		AuditVerdict     verdict = AuditVerdict::kNotShared;
		AuditWhy         why = AuditWhy::kUntyped;
	};
}
