#pragma once

#include "StageRule.h"

#include <array>

// Written by tools/questscan.ps1 -Write from the game's own files (Skyrim.esm, Update.esm, Dawnguard.esm,
// HearthFires.esm, Dragonborn.esm and the scripts in Skyrim - Misc.bsa); do not edit by hand, run it again. Each row is a
// stage of a quest a session may share whose fragment moves the player, or takes their screen, controls or saving away,
// itself or through what it calls, sets or starts (tools/questscan.cs, MoveScan): made only with the friend who did it
// near (StageEffect: what it does, for the log).
// The comment after each row is the chain the scan followed to it. A row ending in true is its quest's start-up stage:
// any stage made while the quest is not started starts it and runs that stage too (QuestRules::StageRowHere). Sorted by
// editor ID (any case), then stage.
namespace QuestRules
{
	inline constexpr std::array<StageRule, 195> STAGE_RULES{ {
		{ "CWAttackCity", 35, StageVerdict::kMoves, StageEffect::kControls },  // QF_CWAttackCity_0004F8BF.Fragment_17: Game.DisablePlayerControls
		{ "CWFunctions", 0, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWFunctions_0002BFAE.Fragment_10: MoveTo temp26
		{ "CWFunctions", 100, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWFunctions_0002BFAE.Fragment_15: MoveTo temp39
		{ "CWFunctions", 101, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWFunctions_0002BFAE.Fragment_23: MoveTo temp96
		{ "CWFunctions", 102, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWFunctions_0002BFAE.Fragment_27: MoveTo temp84
		{ "CWFunctions", 105, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWFunctions_0002BFAE.Fragment_19: MoveTo temp62
		{ "CWFunctions", 106, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWFunctions_0002BFAE.Fragment_25: MoveTo temp151
		{ "CWFunctions", 110, StageVerdict::kMoves, StageEffect::kMove },  // 101 -> QF_CWFunctions_0002BFAE.Fragment_23: MoveTo temp96
		{ "CWFunctions", 300, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWFunctions_0002BFAE.Fragment_21: MoveTo temp17
		{ "CWSiegeQuickStart", 1110, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 1120, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 1210, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 1220, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 1225, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2110, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2120, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2210, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2220, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 2225, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWSiegeQuickStart_0009726A.Fragment_33: MoveTo temp79
		{ "CWSiegeQuickStart", 3220, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4110, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4120, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4210, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4215, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWSiegeQuickStart_0009726A.Fragment_35: MoveTo temp8
		{ "CWSiegeQuickStart", 4220, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4221, StageVerdict::kMoves, StageEffect::kMove },  // 4220 -> cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 4225, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWSiegeQuickStart_0009726A.Fragment_32: MoveTo temp58
		{ "CWSiegeQuickStart", 5220, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 6110, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 7110, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8110, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8115, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8120, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8210, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 8220, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 9110, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 9115, StageVerdict::kMoves, StageEffect::kMove },  // QF_CWSiegeQuickStart_0009726A.Fragment_34: MoveTo temp28
		{ "CWSiegeQuickStart", 9120, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 9210, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "CWSiegeQuickStart", 9220, StageVerdict::kMoves, StageEffect::kMove },  // cwsiegequickstartscript.QuickStartSiege -> CWSiegeQuickStartScript.QuickStartSiege: MoveTo FieldCO
		{ "DA01", 70, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA01_00028AD6.Fragment_13: MoveTo StarPlayerMarker
		{ "DA01", 75, StageVerdict::kMoves, StageEffect::kControls },  // QF_DA01_00028AD6.Fragment_28: Game.DisablePlayerControls
		{ "DA01", 85, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA01_00028AD6.Fragment_46: MoveTo StarPlayerMarker
		{ "DA01", 90, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA01_00028AD6.Fragment_48: MoveTo pShrineReturnMarker
		{ "DA01", 95, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA01_00028AD6.Fragment_49: MoveTo pNelacarReturnMarker
		{ "DA02", 11, StageVerdict::kMoves, StageEffect::kScreen },  // da02script.resurrectCounduit -> DA02Script.resurrectCounduit: ImageSpaceModifier.Apply DA02ResurrectBlurAnimated
		{ "DA02", 17, StageVerdict::kMoves, StageEffect::kScreen },  // da02script.resurrectCounduit -> DA02Script.resurrectCounduit: ImageSpaceModifier.Apply DA02ResurrectBlurAnimated
		{ "DA02", 20, StageVerdict::kMoves, StageEffect::kScreen },  // 30 -> 40 -> da02boethiahsconduitscript.startIMODs -> DA02BoethiahsConduitScript.StartIMODs: ImageSpaceModifier.ApplyCrossFade temp8
		{ "DA02", 30, StageVerdict::kMoves, StageEffect::kScreen },  // 40 -> da02boethiahsconduitscript.startIMODs -> DA02BoethiahsConduitScript.StartIMODs: ImageSpaceModifier.ApplyCrossFade temp8
		{ "DA02", 31, StageVerdict::kMoves, StageEffect::kScreen },  // 40 -> da02boethiahsconduitscript.startIMODs -> DA02BoethiahsConduitScript.StartIMODs: ImageSpaceModifier.ApplyCrossFade temp8
		{ "DA02", 40, StageVerdict::kMoves, StageEffect::kScreen },  // da02boethiahsconduitscript.startIMODs -> DA02BoethiahsConduitScript.StartIMODs: ImageSpaceModifier.ApplyCrossFade temp8
		{ "DA02", 100, StageVerdict::kMoves, StageEffect::kScreen },  // da02script.resurrectCounduit -> DA02Script.resurrectCounduit: ImageSpaceModifier.Apply DA02ResurrectBlurAnimated
		{ "DA03", 0, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA03_0001BFC4.Fragment_0: MoveTo DA03TEMPMoveMarker
		{ "DA08", 1, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA08_0004A37B.Fragment_32: MoveTo StartMarker
		{ "DA09", 1, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA09_0004E4E1.Fragment_13: MoveTo temp3
		{ "DA09", 200, StageVerdict::kMoves, StageEffect::kMove },  // da09script.movePlayerToSky -> DA09Script.movePlayerToSky: TranslateToRef DA09FloatMarker1
		{ "DA09", 300, StageVerdict::kMoves, StageEffect::kMove },  // da09script.movePlayerToEarth -> DA09Script.movePlayerToEarth: MoveTo DA09StatueMarker
		{ "DA09", 450, StageVerdict::kMoves, StageEffect::kMove },  // da09script.TeleportPlayerToSky -> DA09Script.TeleportPlayerToSky: MoveTo DA09SkyMarker
		{ "DA09", 500, StageVerdict::kMoves, StageEffect::kMove },  // da09script.movePlayerToEarth -> DA09Script.movePlayerToEarth: MoveTo DA09StatueMarker
		{ "DA10", 110, StageVerdict::kMoves, StageEffect::kScreen },  // QF_DA10_00022F08.Fragment_72: ImageSpaceModifier.Apply DA10ResurrectBlurAnimated
		{ "DA10HauntingController", 0, StageVerdict::kMoves, StageEffect::kScreen, true },  // da10hauntingcontrollerscript.startRunningHaunt -> DA10HauntingControllerScript.GotoState -> DA10HauntingControllerScript.onBeginState -> DA10HauntingControllerScript.onBeginState: ImageSpaceModifier.applyCrossFade DA10HauntingISMDIn
		{ "DA11", 20, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA11_0002C358.Fragment_39: the player through dunReachcliffSecExitDoor
		{ "DA14", 0, StageVerdict::kMoves, StageEffect::kMove },  // 5 -> QF_DA14_0001BB9B.Fragment_3: MoveTo temp8
		{ "DA14", 5, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA14_0001BB9B.Fragment_3: MoveTo temp8
		{ "DA14", 200, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA14_0001BB9B.Fragment_37: MoveTo temp23
		{ "DA15", 0, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA15_0002AC68.Fragment_0: MoveTo temp13
		{ "DA15", 55, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA15_0002AC68.Fragment_31: MoveTo temp10
		{ "DA15", 190, StageVerdict::kMoves, StageEffect::kMove },  // 200 -> DA15Return 0 -> 10 -> QF_DA15Return_0010FF8F.Fragment_5: MoveTo temp2
		{ "DA15", 200, StageVerdict::kMoves, StageEffect::kMove },  // DA15Return 0 -> 10 -> QF_DA15Return_0010FF8F.Fragment_5: MoveTo temp2
		{ "DA16", 145, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA16_000242AF.Fragment_118: MoveTo temp87
		{ "DA16", 155, StageVerdict::kMoves, StageEffect::kMove },  // QF_DA16_000242AF.Fragment_120: MoveTo temp66
		{ "DA16", 190, StageVerdict::kMoves, StageEffect::kControls },  // QF_DA16_000242AF.Fragment_113: Game.DisablePlayerControls
		{ "DB09", 75, StageVerdict::kMoves, StageEffect::kScreen },  // DB10 1 -> dbsanctuarydb10loadcheck.RunLoadCheck -> DBSanctuaryDB10LoadCheck.RunLoadCheck: ImageSpaceModifier.Apply FireIM
		{ "DB10", 1, StageVerdict::kMoves, StageEffect::kScreen },  // dbsanctuarydb10loadcheck.RunLoadCheck -> DBSanctuaryDB10LoadCheck.RunLoadCheck: ImageSpaceModifier.Apply FireIM
		{ "DB10", 10, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> dbsanctuarydb10loadcheck.RunLoadCheck -> DBSanctuaryDB10LoadCheck.RunLoadCheck: ImageSpaceModifier.Apply FireIM
		{ "DB10", 55, StageVerdict::kMoves, StageEffect::kScreen },  // dbsanctuarydb10loadcheck.RunLoadCheck -> DBSanctuaryDB10LoadCheck.RunLoadCheck: ImageSpaceModifier.Apply FireIM
		{ "DLC1LD", 1, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1LD_01005817.Fragment_77: MoveTo DLC1LD1
		{ "DLC1LD", 2, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1LD_01005817.Fragment_79: MoveTo DLC1LD2
		{ "DLC1LD", 3, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1LD_01005817.Fragment_80: MoveTo DLC1LD3
		{ "DLC1LD", 4, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1LD_01005817.Fragment_81: MoveTo DLC1LD4
		{ "DLC1LD", 5, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1LD_01005817.Fragment_60: MoveTo DLC1LD5
		{ "DLC1LD", 6, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1LD_01005817.Fragment_83: MoveTo DLC1LD6
		{ "DLC1LD", 7, StageVerdict::kMoves, StageEffect::kMove },  // 6 -> DLC1_QF_DLC1LD_01005817.Fragment_83: MoveTo DLC1LD6
		{ "DLC1VQ02", 0, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1VQ01COPY0000_01002F65.Fragment_7: MoveTo DLC1VQ02PlayerStart
		{ "DLC1VQ02", 8, StageVerdict::kMoves, StageEffect::kControls },  // DLC1_QF_DLC1VQ01COPY0000_01002F65.Fragment_23: Game.DisablePlayerControls
		{ "DLC1VQ03Vampire", 0, StageVerdict::kMoves, StageEffect::kControls },  // DLC1Radiant 10 -> dlc1vampireturnscript.ReceiveHarkonsGift -> playervampirequestscript.VampireChange -> PlayerVampireQuestScript.VampireChange: Game.DisablePlayerControls
		{ "DLC1VQ03Vampire", 67, StageVerdict::kMoves, StageEffect::kScreen },  // playervampirequestscript.VampireProgression -> PlayerVampireQuestScript.VampireProgression: ImageSpaceModifier.applyCrossFade VampireTransformIncreaseISMD
		{ "DLC1VQ04", 0, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_VQ04Hunter_0100284F.Fragment_0: MoveTo pDLC1PlayerStartXMarker
		{ "DLC1VQ04", 1, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_VQ04Hunter_0100284F.Fragment_31: MoveTo pDLC1VQ04VampirePlayerStart
		{ "DLC1VQ04", 110, StageVerdict::kMoves, StageEffect::kControls },  // dlc1vq04questscript.SeranaBite -> dlc1vampireturnscript.ReceiveSeranasGift -> DLC1VampireTurnScript.ReceiveHarkonsGift -> playervampirequestscript.VampireChange -> PlayerVampireQuestScript.VampireChange: Game.DisablePlayerControls
		{ "DLC1VQ05", 0, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1VQ05_01002850.Fragment_0: MoveTo pDLC1VQ05PlayerTempStartMarker
		{ "DLC1VQ06", 0, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1VQ06_01002852.Fragment_0: MoveTo pDLC1VQ06PlayerStartMarker
		{ "DLC1VQ06", 61, StageVerdict::kMoves, StageEffect::kScreen },  // dlc1vq06readingtriggerscript.PlayerReadsScroll -> DLC1VQ06ReadingTriggerScript.PlayerReadsScroll: ImageSpaceModifier.applyCrossFade FadeToWhiteHoldImod
		{ "DLC1VQ06", 65, StageVerdict::kMoves, StageEffect::kScreen },  // dlc1vq06readingtriggerscript.PlayerGoesBlind -> DLC1VQ06ReadingTriggerScript.PlayerGoesBlind: ImageSpaceModifier.Apply FXReadScrollsBlindImod
		{ "DLC1VQ07", 0, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1VQ07_01002853.Fragment_0: MoveTo pDLC1VQ07PlayerStart
		{ "DLC1VQ08", 0, StageVerdict::kMoves, StageEffect::kMove },  // DLC1_QF_DLC1VQ08_01007C25.Fragment_4: MoveTo PlayerStart
		{ "DLC2dunKolbjornQST", 0, StageVerdict::kMoves, StageEffect::kMove },  // DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_12: MoveTo DLC2dunKolbjornQSTStage0Marker
		{ "DLC2dunKolbjornQST", 1, StageVerdict::kMoves, StageEffect::kMove },  // DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2dunKolbjornQST", 2, StageVerdict::kMoves, StageEffect::kMove },  // 1 -> DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2dunKolbjornQST", 3, StageVerdict::kMoves, StageEffect::kMove },  // 2 -> 1 -> DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2dunKolbjornQST", 4, StageVerdict::kMoves, StageEffect::kMove },  // 3 -> 2 -> 1 -> DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2dunKolbjornQST", 5, StageVerdict::kMoves, StageEffect::kMove },  // 4 -> 3 -> 2 -> 1 -> DLC2_QF_DLC2dunKolbjornQST_0201810B.Fragment_59: MoveTo DLC2dunKolbjorn_PlayerOutsideKolbjornMarker
		{ "DLC2MQ02", 50, StageVerdict::kMoves, StageEffect::kChargen },  // DLC2_QF_DLC2MQ02_02017F8F.Fragment_16: Game.SetInChargen
		{ "DLC2MQ05", 320, StageVerdict::kMoves, StageEffect::kControls },  // DLC2_QF_DLC2MQ04_020179DE.Fragment_42: Game.DisablePlayerControls
		{ "DLC2MQ06", 330, StageVerdict::kMoves, StageEffect::kControls },  // DLC2_QF_DLC2MQ07_020179D7.Fragment_6: Game.DisablePlayerControls
		{ "DLC2RR01", 0, StageVerdict::kMoves, StageEffect::kMove },  // DLC2_QF_DLC2RR01_02018B13.Fragment_1: MoveTo pDLC2RR01StartMarker
		{ "MG02", 50, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MG02_0001F252.Fragment_9: ImageSpaceModifier.PopTo LoopFX
		{ "MG04", 40, StageVerdict::kMoves, StageEffect::kScreen },  // MG04MonkSceneQuest 200 -> QF_MG04MonkSceneQuest_00076F39.Fragment_2: ImageSpaceModifier.PopTo LoopFX
		{ "MG04MonkSceneQuest", 0, StageVerdict::kMoves, StageEffect::kScreen, true },  // QF_MG04MonkSceneQuest_00076F39.Fragment_0: ImageSpaceModifier.Apply IntroFX
		{ "MG04MonkSceneQuest", 200, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MG04MonkSceneQuest_00076F39.Fragment_2: ImageSpaceModifier.PopTo LoopFX
		{ "mg06", 99, StageVerdict::kMoves, StageEffect::kMove },  // QF_MG06_0001F256.Fragment_18: MoveTo MG05PlayerStartMarker
		{ "MG07", 200, StageVerdict::kMoves, StageEffect::kMove },  // MG08 0 -> QF_MG08_0001F258.Fragment_0: MoveTo MG07TolfdirBridgeMarker
		{ "MG08", 0, StageVerdict::kMoves, StageEffect::kMove, true },  // QF_MG08_0001F258.Fragment_0: MoveTo MG07TolfdirBridgeMarker
		{ "MG08", 1, StageVerdict::kMoves, StageEffect::kMove },  // QF_MG08_0001F258.Fragment_15: MoveTo temp1
		{ "MQ101", 0, StageVerdict::kMoves, StageEffect::kMove, true },  // 10 -> QF_MQ101_0003372B.Fragment_4: MoveTo temp214
		{ "MQ101", 5, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ101_0003372B.Fragment_56: MoveTo PlayerMarker4
		{ "MQ101", 6, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ101_0003372B.Fragment_57: MoveTo PlayerImperialKeepMarker1
		{ "MQ101", 7, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ101_0003372B.Fragment_59: MoveTo PlayerSonKeepMarker1
		{ "MQ101", 10, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ101_0003372B.Fragment_4: MoveTo temp214
		{ "MQ101", 30, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ101_0003372B.Fragment_204: Game.DisablePlayerControls
		{ "MQ101", 54, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ101_0003372B.Fragment_209: Game.DisablePlayerControls
		{ "MQ101", 64, StageVerdict::kMoves, StageEffect::kControls },  // 70 -> QF_MQ101_0003372B.Fragment_227: Game.SetPlayerAIDriven
		{ "MQ101", 65, StageVerdict::kMoves, StageEffect::kChargen },  // QF_MQ101_0003372B.Fragment_318: Game.SetInChargen
		{ "MQ101", 70, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ101_0003372B.Fragment_227: Game.SetPlayerAIDriven
		{ "MQ101", 75, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MQ101_0003372B.Fragment_13: Game.ShowRaceMenu
		{ "MQ101", 80, StageVerdict::kMoves, StageEffect::kChargen },  // QF_MQ101_0003372B.Fragment_11: Game.SetInChargen
		{ "MQ101", 95, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ101_0003372B.Fragment_287: Game.DisablePlayerControls
		{ "MQ101", 98, StageVerdict::kMoves, StageEffect::kFrozen },  // QF_MQ101_0003372B.Fragment_308: SetRestrained on the player
		{ "MQ101", 202, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ101_0003372B.Fragment_311: Game.DisablePlayerControls
		{ "MQ105", 70, StageVerdict::kMoves, StageEffect::kScreen },  // fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ105", 110, StageVerdict::kMoves, StageEffect::kScreen },  // fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ105", 255, StageVerdict::kMoves, StageEffect::kScreen },  // fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ105", 270, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ105_000242BA.Fragment_83: Game.DisablePlayerControls
		{ "MQ105", 280, StageVerdict::kMoves, StageEffect::kScreen },  // mq105questscript.GreybeardSpeakingEffect -> MQ105QuestScript.GreybeardSpeakingEffect: ImageSpaceModifier.Apply GreybeardOutroIMOD
		{ "MQ105", 282, StageVerdict::kMoves, StageEffect::kScreen },  // mq105questscript.GreybeardSpeakingEffect -> MQ105QuestScript.GreybeardSpeakingEffect: ImageSpaceModifier.Apply GreybeardOutroIMOD
		{ "MQ105", 284, StageVerdict::kMoves, StageEffect::kScreen },  // mq105questscript.GreybeardSpeakingEffect -> MQ105QuestScript.GreybeardSpeakingEffect: ImageSpaceModifier.Apply GreybeardOutroIMOD
		{ "MQ106", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ106", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ201", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ201", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ201", 95, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ201_00035D5F.Fragment_104: Game.DisablePlayerControls
		{ "MQ201", 100, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ201_00035D5F.Fragment_15: MoveTo PartyArrivalMarker
		{ "MQ202", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ202", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ203", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ203", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ203", 5, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ203_00036192.Fragment_24: MoveTo temp11
		{ "MQ203", 6, StageVerdict::kMoves, StageEffect::kMove },  // 5 -> QF_MQ203_00036192.Fragment_24: MoveTo temp11
		{ "MQ204", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ204", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ204", 80, StageVerdict::kMoves, StageEffect::kScreen },  // fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ204", 81, StageVerdict::kMoves, StageEffect::kScreen },  // fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ204", 82, StageVerdict::kMoves, StageEffect::kScreen },  // fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ204", 125, StageVerdict::kMoves, StageEffect::kScreen },  // fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ205", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ205", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ206", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ206", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ206", 5, StageVerdict::kMoves, StageEffect::kScreen },  // mq206script.CleanUpTimeTravelEffects -> MQ206Script.CleanUpTimeTravelEffects: ImageSpaceModifier.Apply FadeToWhiteInOutImod
		{ "MQ206", 20, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ206_00036193.Fragment_4: MoveTo temp61
		{ "MQ206", 42, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MQ206_00036193.Fragment_68: ImageSpaceModifier.ApplyCrossFade FXTimeTravelImodStatic01
		{ "MQ206", 48, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MQ206_00036193.Fragment_71: ImageSpaceModifier.ApplyCrossFade FXTimeTravelImodStatic03
		{ "MQ206", 60, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MQ206_00036193.Fragment_8: ImageSpaceModifier.ApplyCrossFade FXTimeTravelImodStatic04
		{ "MQ206", 70, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MQ206_00036193.Fragment_12: ImageSpaceModifier.ApplyCrossFade FXTimeTravelImodStatic03
		{ "MQ206", 88, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MQ206_00036193.Fragment_69: ImageSpaceModifier.ApplyCrossFade FXTimeTravelImodStatic01
		{ "MQ206", 89, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MQ206_00036193.Fragment_73: ImageSpaceModifier.ApplyCrossFade FXTimeTravelImodStatic03
		{ "MQ206", 90, StageVerdict::kMoves, StageEffect::kScreen },  // QF_MQ206_00036193.Fragment_23: ImageSpaceModifier.ApplyCrossFade FXTimeTravelImodStatic
		{ "MQ206", 100, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ206_00036193.Fragment_25: MoveTo temp47
		{ "MQ301", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ301", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ302", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ302", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ302", 5, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ302", 6, StageVerdict::kMoves, StageEffect::kScreen },  // 5 -> 1 -> MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ302", 60, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ302_00045923.Fragment_17: Game.DisablePlayerControls
		{ "MQ303", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ303", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ303", 50, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ303_00046EF0.Fragment_5: Game.DisablePlayerControls
		{ "MQ303", 55, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ303_00046EF0.Fragment_43: Game.DisablePlayerControls
		{ "MQ303", 60, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ303_00046EF0.Fragment_10: MoveTo SkuldafnStartMarker
		{ "MQ304", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ303 1 -> MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ304", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ303 1 -> MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ305", 0, StageVerdict::kMoves, StageEffect::kScreen },  // 1 -> MQ304 1 -> MQ303 1 -> MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ305", 1, StageVerdict::kMoves, StageEffect::kScreen },  // MQ304 1 -> MQ303 1 -> MQ301 1 -> MQ206 1 -> MQ205 1 -> MQ204 1 -> MQ203 1 -> MQ202 1 -> MQ201 1 -> MQ106 1 -> MQ105 70 -> fxgreybeardshoutscript.GoAway -> FXGreybeardShoutScript.GoAway: ImageSpaceModifier.ApplyCrossfade LearnWordFadeLoop02
		{ "MQ305", 250, StageVerdict::kMoves, StageEffect::kControls },  // QF_MQ305_00046EF2.Fragment_15: Game.DisablePlayerControls
		{ "MQ305", 260, StageVerdict::kMoves, StageEffect::kMove },  // QF_MQ305_00046EF2.Fragment_35: MoveTo TamrielMarker
		{ "MS02", 0, StageVerdict::kMoves, StageEffect::kControls },  // crimeguardsscript.SetupCidhnaMine -> CidhnaMineJailEventScene 10 -> QF_CidhnaMineJailEventScene_000CD13F.Fragment_0: Game.DisablePlayerControls
		{ "MS10", 0, StageVerdict::kMoves, StageEffect::kMove },  // QF_MS10_0001DBFC.Fragment_26: MoveTo temp21
		{ "MS12", 0, StageVerdict::kMoves, StageEffect::kMove },  // QF_MS12_0002C258.Fragment_11: MoveTo temp17
		{ "TG05", 60, StageVerdict::kMoves, StageEffect::kMove },  // QF_TG05_00021551.Fragment_15: MoveTo temp3
		{ "TG08A", 55, StageVerdict::kMoves, StageEffect::kControls },  // QF_TG08A_00057F99.Fragment_34: Game.DisablePlayerControls
		{ "TG08B", 1, StageVerdict::kMoves, StageEffect::kMove },  // QF_TG08_00021554.Fragment_34: MoveTo pTG08BPlayerDebugFinalStart
		{ "TG08B", 35, StageVerdict::kMoves, StageEffect::kControls },  // QF_TG08_00021554.Fragment_29: Game.DisablePlayerControls
		{ "TG09", 45, StageVerdict::kMoves, StageEffect::kScreen },  // QF_TG09_00021555.Fragment_17: ImageSpaceModifier.applyCrossfade pTG09imod
		{ "TG09", 50, StageVerdict::kMoves, StageEffect::kControls },  // QF_TG09_00021555.Fragment_8: Game.DisablePlayerControls
	} };
}
