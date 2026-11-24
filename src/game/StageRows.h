#pragma once

#include "StageRule.h"

#include <array>

// Written by tools/questscan.ps1 -Write from the game's own files (Skyrim.esm, Update.esm, Dawnguard.esm,
// HearthFires.esm, Dragonborn.esm and the scripts in Skyrim - Misc.bsa); do not edit by hand, run it again. Each row is a
// stage of a quest a session may share whose fragment moves the player, or takes their screen or controls away, itself or
// through what it calls, sets or starts (tools/questscan.cs, MoveScan): made only with the friend who did it near.
// The comment after each row is the chain the scan followed to it. Sorted by editor ID (any case), then stage.
namespace QuestRules
{
	inline constexpr std::array<StageRule, 122> STAGE_RULES{ {
		{ "CWFunctions", 0, StageVerdict::kMoves },  // QF_CWFunctions_0002BFAE.Fragment_10: MoveTo temp26
		{ "CWFunctions", 100, StageVerdict::kMoves },  // QF_CWFunctions_0002BFAE.Fragment_15: MoveTo temp39
		{ "CWFunctions", 101, StageVerdict::kMoves },  // QF_CWFunctions_0002BFAE.Fragment_23: MoveTo temp96
		{ "CWFunctions", 102, StageVerdict::kMoves },  // QF_CWFunctions_0002BFAE.Fragment_27: MoveTo temp84
		{ "CWFunctions", 105, StageVerdict::kMoves },  // QF_CWFunctions_0002BFAE.Fragment_19: MoveTo temp62
		{ "CWFunctions", 106, StageVerdict::kMoves },  // QF_CWFunctions_0002BFAE.Fragment_25: MoveTo temp151
		{ "CWFunctions", 110, StageVerdict::kMoves },  // 101 -> QF_CWFunctions_0002BFAE.Fragment_23: MoveTo temp96
		{ "CWFunctions", 300, StageVerdict::kMoves },  // QF_CWFunctions_0002BFAE.Fragment_21: MoveTo temp17
		{ "CWSiegeQuickStart", 1110, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 1120, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 1210, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 1220, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 1225, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2110, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2120, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2210, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2220, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2225, StageVerdict::kMoves },  // QF_CWSiegeQuickStart_0009726A.Fragment_33: MoveTo temp79
		{ "CWSiegeQuickStart", 3220, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4110, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4120, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4210, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4215, StageVerdict::kMoves },  // QF_CWSiegeQuickStart_0009726A.Fragment_35: MoveTo temp8
		{ "CWSiegeQuickStart", 4220, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4221, StageVerdict::kMoves },  // 4220 -> cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4225, StageVerdict::kMoves },  // QF_CWSiegeQuickStart_0009726A.Fragment_32: MoveTo temp58
		{ "CWSiegeQuickStart", 5220, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 6110, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 7110, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8110, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8115, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8120, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8210, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8220, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 9110, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 9115, StageVerdict::kMoves },  // QF_CWSiegeQuickStart_0009726A.Fragment_34: MoveTo temp28
		{ "CWSiegeQuickStart", 9120, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 9210, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 9220, StageVerdict::kMoves },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "DA01", 70, StageVerdict::kMoves },  // QF_DA01_00028AD6.Fragment_13: MoveTo StarPlayerMarker
		{ "DA01", 75, StageVerdict::kMoves },  // QF_DA01_00028AD6.Fragment_28: Game.DisablePlayerControls
		{ "DA01", 85, StageVerdict::kMoves },  // QF_DA01_00028AD6.Fragment_46: MoveTo StarPlayerMarker
		{ "DA01", 90, StageVerdict::kMoves },  // QF_DA01_00028AD6.Fragment_48: MoveTo pShrineReturnMarker
		{ "DA01", 95, StageVerdict::kMoves },  // QF_DA01_00028AD6.Fragment_49: MoveTo pNelacarReturnMarker
		{ "DA03", 0, StageVerdict::kMoves },  // QF_DA03_0001BFC4.Fragment_0: MoveTo DA03TEMPMoveMarker
		{ "DA08", 1, StageVerdict::kMoves },  // QF_DA08_0004A37B.Fragment_32: MoveTo StartMarker
		{ "DA09", 1, StageVerdict::kMoves },  // QF_DA09_0004E4E1.Fragment_13: MoveTo temp3
		{ "DA09", 200, StageVerdict::kMoves },  // da09script.movePlayerToSky -> DA09Script.movePlayerToSky: MoveTo DA09SkyMarker
		{ "DA09", 300, StageVerdict::kMoves },  // da09script.movePlayerToEarth -> DA09Script.movePlayerToEarth: MoveTo DA09StatueMarker
		{ "DA09", 450, StageVerdict::kMoves },  // da09script.TeleportPlayerToSky -> DA09Script.TeleportPlayerToSky: MoveTo DA09SkyMarker
		{ "DA09", 500, StageVerdict::kMoves },  // da09script.movePlayerToEarth -> DA09Script.movePlayerToEarth: MoveTo DA09StatueMarker
		{ "DA11", 20, StageVerdict::kMoves },  // QF_DA11_0002C358.Fragment_39: the player through dunReachcliffSecExitDoor
		{ "DA14", 0, StageVerdict::kMoves },  // 5 -> QF_DA14_0001BB9B.Fragment_3: MoveTo temp8
		{ "DA14", 5, StageVerdict::kMoves },  // QF_DA14_0001BB9B.Fragment_3: MoveTo temp8
		{ "DA14", 200, StageVerdict::kMoves },  // QF_DA14_0001BB9B.Fragment_37: MoveTo temp23
		{ "DA15", 0, StageVerdict::kMoves },  // QF_DA15_0002AC68.Fragment_0: MoveTo temp13
		{ "DA15", 55, StageVerdict::kMoves },  // QF_DA15_0002AC68.Fragment_31: MoveTo temp10
		{ "DA15", 190, StageVerdict::kMoves },  // 200 -> DA15Return 0 -> 10 -> QF_DA15Return_0010FF8F.Fragment_5: MoveTo temp2
		{ "DA15", 200, StageVerdict::kMoves },  // DA15Return 0 -> 10 -> QF_DA15Return_0010FF8F.Fragment_5: MoveTo temp2
		{ "DA16", 145, StageVerdict::kMoves },  // QF_DA16_000242AF.Fragment_118: Game.DisablePlayerControls
		{ "DA16", 155, StageVerdict::kMoves },  // QF_DA16_000242AF.Fragment_120: MoveTo temp66
		{ "DA16", 190, StageVerdict::kMoves },  // QF_DA16_000242AF.Fragment_113: Game.DisablePlayerControls
		{ "DLC1LD", 1, StageVerdict::kMoves },  // DLC1_QF_DLC1LD_01005817.Fragment_77: MoveTo DLC1LD1
		{ "DLC1LD", 2, StageVerdict::kMoves },  // DLC1_QF_DLC1LD_01005817.Fragment_79: MoveTo DLC1LD2
		{ "DLC1LD", 3, StageVerdict::kMoves },  // DLC1_QF_DLC1LD_01005817.Fragment_80: MoveTo DLC1LD3
		{ "DLC1LD", 4, StageVerdict::kMoves },  // DLC1_QF_DLC1LD_01005817.Fragment_81: MoveTo DLC1LD4
		{ "DLC1LD", 5, StageVerdict::kMoves },  // DLC1_QF_DLC1LD_01005817.Fragment_60: MoveTo DLC1LD5
		{ "DLC1LD", 6, StageVerdict::kMoves },  // DLC1_QF_DLC1LD_01005817.Fragment_83: MoveTo DLC1LD6
		{ "DLC1LD", 7, StageVerdict::kMoves },  // 6 -> DLC1_QF_DLC1LD_01005817.Fragment_83: MoveTo DLC1LD6
		{ "DLC1VQ02", 0, StageVerdict::kMoves },  // DLC1_QF_DLC1VQ01COPY0000_01002F65.Fragment_7: MoveTo DLC1VQ02PlayerStart
		{ "DLC1VQ03Vampire", 0, StageVerdict::kMoves },  // DLC1Radiant 10 -> dlc1vampireturnscript.ReceiveHarkonsGift -> playervampirequestscript.VampireChange -> PlayerVampireQuestScript.VampireChange: Game.DisablePlayerControls
		{ "DLC1VQ04", 0, StageVerdict::kMoves },  // DLC1_QF_VQ04Hunter_0100284F.Fragment_0: MoveTo pDLC1PlayerStartXMarker
		{ "DLC1VQ04", 1, StageVerdict::kMoves },  // DLC1_QF_VQ04Hunter_0100284F.Fragment_31: MoveTo pDLC1VQ04VampirePlayerStart
		{ "DLC1VQ04", 110, StageVerdict::kMoves },  // dlc1vq04questscript.SeranaBite -> dlc1vampireturnscript.ReceiveSeranasGift -> DLC1VampireTurnScript.ReceiveHarkonsGift -> playervampirequestscript.VampireChange -> PlayerVampireQuestScript.VampireChange: Game.DisablePlayerControls
		{ "DLC1VQ05", 0, StageVerdict::kMoves },  // DLC1_QF_DLC1VQ05_01002850.Fragment_0: MoveTo pDLC1VQ05PlayerTempStartMarker
		{ "DLC1VQ06", 0, StageVerdict::kMoves },  // DLC1_QF_DLC1VQ06_01002852.Fragment_0: MoveTo pDLC1VQ06PlayerStartMarker
		{ "DLC1VQ06", 61, StageVerdict::kMoves },  // dlc1vq06readingtriggerscript.PlayerReadsScroll -> DLC1VQ06ReadingTriggerScript.PrepForReading -> DLC1VQ06ReadingTriggerScript.PrepForReading: Game.DisablePlayerControls
		{ "DLC1VQ07", 0, StageVerdict::kMoves },  // DLC1_QF_DLC1VQ07_01002853.Fragment_0: MoveTo pDLC1VQ07PlayerStart
		{ "DLC1VQ08", 0, StageVerdict::kMoves },  // DLC1_QF_DLC1VQ08_01007C25.Fragment_4: MoveTo PlayerStart
		{ "DLC2dunKolbjornQST", 0, StageVerdict::kMoves },  // DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_12: MoveTo DLC2dunKolbjornQSTStage0Marker
		{ "DLC2dunKolbjornQST", 1, StageVerdict::kMoves },  // DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2dunKolbjornQST", 2, StageVerdict::kMoves },  // 1 -> DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2dunKolbjornQST", 3, StageVerdict::kMoves },  // 2 -> 1 -> DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2dunKolbjornQST", 4, StageVerdict::kMoves },  // 3 -> 2 -> 1 -> DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2dunKolbjornQST", 5, StageVerdict::kMoves },  // 4 -> 3 -> 2 -> 1 -> DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2MQ05", 320, StageVerdict::kMoves },  // DLC2_QF_DLC2MQ04_020179DE.Fragment_42: Game.DisablePlayerControls
		{ "DLC2MQ06", 330, StageVerdict::kMoves },  // DLC2_QF_DLC2MQ07_020179D7.Fragment_6: Game.DisablePlayerControls
		{ "DLC2RR01", 0, StageVerdict::kMoves },  // DLC2_QF_DLC2RR01_02018B13.Fragment_1: MoveTo pDLC2RR01StartMarker
		{ "mg06", 99, StageVerdict::kMoves },  // QF_MG06_0001F256.Fragment_18: MoveTo MG05PlayerStartMarker
		{ "MG07", 200, StageVerdict::kMoves },  // MG08 0 -> QF_MG08_0001F258.Fragment_0: MoveTo MG07TolfdirBridgeMarker
		{ "MG08", 0, StageVerdict::kMoves },  // QF_MG08_0001F258.Fragment_0: MoveTo MG07TolfdirBridgeMarker
		{ "MG08", 1, StageVerdict::kMoves },  // QF_MG08_0001F258.Fragment_15: MoveTo temp1
		{ "MQ101", 0, StageVerdict::kMoves },  // 10 -> QF_MQ101_0003372B.Fragment_4: Game.DisablePlayerControls
		{ "MQ101", 5, StageVerdict::kMoves },  // QF_MQ101_0003372B.Fragment_56: MoveTo PlayerMarker4
		{ "MQ101", 6, StageVerdict::kMoves },  // QF_MQ101_0003372B.Fragment_57: MoveTo PlayerImperialKeepMarker1
		{ "MQ101", 7, StageVerdict::kMoves },  // QF_MQ101_0003372B.Fragment_59: MoveTo PlayerSonKeepMarker1
		{ "MQ101", 10, StageVerdict::kMoves },  // QF_MQ101_0003372B.Fragment_4: Game.DisablePlayerControls
		{ "MQ101", 30, StageVerdict::kMoves },  // QF_MQ101_0003372B.Fragment_204: Game.DisablePlayerControls
		{ "MQ101", 54, StageVerdict::kMoves },  // QF_MQ101_0003372B.Fragment_209: Game.DisablePlayerControls
		{ "MQ101", 64, StageVerdict::kMoves },  // 70 -> QF_MQ101_0003372B.Fragment_227: Game.SetPlayerAIDriven
		{ "MQ101", 70, StageVerdict::kMoves },  // QF_MQ101_0003372B.Fragment_227: Game.SetPlayerAIDriven
		{ "MQ101", 95, StageVerdict::kMoves },  // QF_MQ101_0003372B.Fragment_287: Game.DisablePlayerControls
		{ "MQ101", 202, StageVerdict::kMoves },  // QF_MQ101_0003372B.Fragment_311: Game.DisablePlayerControls
		{ "MQ105", 270, StageVerdict::kMoves },  // QF_MQ105_000242BA.Fragment_83: Game.DisablePlayerControls
		{ "MQ201", 95, StageVerdict::kMoves },  // QF_MQ201_00035D5F.Fragment_104: Game.DisablePlayerControls
		{ "MQ201", 100, StageVerdict::kMoves },  // QF_MQ201_00035D5F.Fragment_15: MoveTo PartyArrivalMarker
		{ "MQ203", 5, StageVerdict::kMoves },  // QF_MQ203_00036192.Fragment_24: MoveTo temp11
		{ "MQ203", 6, StageVerdict::kMoves },  // 5 -> QF_MQ203_00036192.Fragment_24: MoveTo temp11
		{ "MQ206", 20, StageVerdict::kMoves },  // QF_MQ206_00036193.Fragment_4: Game.DisablePlayerControls
		{ "MQ206", 100, StageVerdict::kMoves },  // QF_MQ206_00036193.Fragment_25: MoveTo temp47
		{ "MQ302", 60, StageVerdict::kMoves },  // QF_MQ302_00045923.Fragment_17: Game.DisablePlayerControls
		{ "MQ303", 50, StageVerdict::kMoves },  // QF_MQ303_00046EF0.Fragment_5: Game.DisablePlayerControls
		{ "MQ303", 55, StageVerdict::kMoves },  // QF_MQ303_00046EF0.Fragment_43: Game.DisablePlayerControls
		{ "MQ303", 60, StageVerdict::kMoves },  // QF_MQ303_00046EF0.Fragment_10: MoveTo SkuldafnStartMarker
		{ "MQ305", 250, StageVerdict::kMoves },  // QF_MQ305_00046EF2.Fragment_15: Game.DisablePlayerControls
		{ "MQ305", 260, StageVerdict::kMoves },  // QF_MQ305_00046EF2.Fragment_35: MoveTo TamrielMarker
		{ "MS10", 0, StageVerdict::kMoves },  // QF_MS10_0001DBFC.Fragment_26: MoveTo temp21
		{ "MS12", 0, StageVerdict::kMoves },  // QF_MS12_0002C258.Fragment_11: MoveTo temp17
		{ "TG05", 60, StageVerdict::kMoves },  // QF_TG05_00021551.Fragment_15: MoveTo temp3
		{ "TG08A", 55, StageVerdict::kMoves },  // QF_TG08A_00057F99.Fragment_34: Game.DisablePlayerControls
		{ "TG08B", 1, StageVerdict::kMoves },  // QF_TG08_00021554.Fragment_34: MoveTo pTG08BPlayerDebugFinalStart
		{ "TG08B", 35, StageVerdict::kMoves },  // QF_TG08_00021554.Fragment_29: Game.DisablePlayerControls
	} };
}
