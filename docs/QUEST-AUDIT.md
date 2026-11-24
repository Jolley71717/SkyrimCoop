# Quest audit

Written by `tools/questscan.ps1 -Audit` from the game's own files; do not edit by hand, run it again. The code reads the same list (`src/game/QuestAudit.h`): each game checks its records against it at load, and the unit tests check every row against the rules, so nothing here is left unknown. How each kind is shared is in docs/DESIGN.md, "Quests". Not tested in a game yet.

| Verdict | Why | Quests |
| --- | --- | --- |
| limited | hostRun | 80 |
| limited | nearStages | 46 |
| notShared | misc | 337 |
| notShared | ownList | 1 |
| notShared | untyped | 1608 |
| shared | whole | 202 |
| | all | 2274 |

## Shared

| Quest | Form ID | Tab | Why |
| --- | --- | --- | --- |
| CreatureDialogueAtronachFrost | 0004E848 | main | every stage as it is |
| CreatureDialogueChaurus | 0006399F | main | every stage as it is |
| CreatureDialogueDeer | 0007C727 | main | every stage as it is |
| CreatureDialogueDwarvenSphere | 0007A732 | main | every stage as it is |
| CreatureDialogueFalmer | 0006759B | main | every stage as it is |
| CreatureDialogueGargoyle | 0200F8B1 | main | every stage as it is |
| CreatureDialogueGiant | 0006ABD7 | main | every stage as it is |
| CWDialogueSoldiers | 0006B18F | main | every stage as it is |
| DA02Post | 0001A4EA | main | every stage as it is |
| DialogueGenericUniqueVoices | 0006F453 | main | every stage as it is |
| dunPinewatchQST | 000786B9 | main | every stage as it is |
| MG02MonkSceneQuest | 000FBFDD | main | every stage as it is |
| MQ101DragonAttack | 000D0593 | main | every stage as it is |
| MQ102 | 0004E50D | main | every stage as it is |
| MQ102A | 0002BF9C | main | every stage as it is |
| MQ102B | 0002610A | main | every stage as it is |
| MQ103 | 000D0800 | main | every stage as it is |
| MQ104 | 0002610C | main | every stage as it is |
| MQ105Ustengrav | 000713DC | main | every stage as it is |
| MQ106 | 00032926 | main | every stage as it is |
| MQ201Party | 00063B57 | main | every stage as it is |
| MQ202 | 00036191 | main | every stage as it is |
| MQ204 | 0002D515 | main | every stage as it is |
| MQ205 | 0002D516 | main | every stage as it is |
| MQ302FillAliases | 000876E6 | main | every stage as it is |
| MQPaarthurnax | 0003FA16 | main | every stage as it is |
| MS05KingOlafsFestival | 0006C0EA | main | every stage as it is |
| MS08ArgueQuest | 00055BFD | main | every stage as it is |
| MG01 | 0001F251 | College | every stage as it is |
| MG02 | 0001F252 | College | every stage as it is |
| MG03 | 0001F253 | College | every stage as it is |
| MG04 | 0001F254 | College | every stage as it is |
| MG05 | 0001F255 | College | every stage as it is |
| MGMainQuestBridge | 000C9F08 | College | every stage as it is |
| MGR21 | 000E0E04 | College | every stage as it is |
| MGR30 | 000C1E72 | College | every stage as it is |
| MGRAppJzargo01 | 000958B3 | College | every stage as it is |
| MGRAppOnmund01 | 0006BC33 | College | every stage as it is |
| MGRArniel01 | 0006A086 | College | every stage as it is |
| MGRArniel02 | 0006A08D | College | every stage as it is |
| MGRArniel03 | 0006A097 | College | every stage as it is |
| MGRArniel04 | 0006A119 | College | every stage as it is |
| MGRejoinQuest | 000C9A0E | College | every stage as it is |
| MGRitual01 | 000A4E82 | College | every stage as it is |
| MGRitual02 | 000B3233 | College | every stage as it is |
| MGRitual03 | 00099F27 | College | every stage as it is |
| MGRitual04 | 000CD987 | College | every stage as it is |
| MGRRogue | 0005A609 | College | every stage as it is |
| TG00 | 00021556 | Thieves Guild | every stage as it is |
| TG00SP | 00020BE7 | Thieves Guild | every stage as it is |
| TG01 | 0001F326 | Thieves Guild | every stage as it is |
| TG02 | 0002154E | Thieves Guild | every stage as it is |
| TG02SP | 0007EB37 | Thieves Guild | every stage as it is |
| TG03 | 0002154F | Thieves Guild | every stage as it is |
| TG04 | 00021550 | Thieves Guild | every stage as it is |
| TG06 | 00021552 | Thieves Guild | every stage as it is |
| TG07 | 00021553 | Thieves Guild | every stage as it is |
| TG09Post | 000E1782 | Thieves Guild | every stage as it is |
| TGBan | 000B03A2 | Thieves Guild | every stage as it is |
| TGCrown | 0009DD77 | Thieves Guild | every stage as it is |
| TGDialogueKarliah | 0004DBC5 | Thieves Guild | every stage as it is |
| TGLeadership | 000D7D69 | Thieves Guild | every stage as it is |
| TGTQ01 | 000799DF | Thieves Guild | every stage as it is |
| TGTQ02 | 000799E0 | Thieves Guild | every stage as it is |
| TGTQ03 | 000799E1 | Thieves Guild | every stage as it is |
| TGTQ04 | 000799E2 | Thieves Guild | every stage as it is |
| DarkBrotherhood | 0001EA5C | Dark Brotherhood | every stage as it is |
| DarkBrotherhoodadditionalNPCDialogue | 00051417 | Dark Brotherhood | every stage as it is |
| DarkBrotherhoodSanctuaryConversation1 | 0002E7B0 | Dark Brotherhood | every stage as it is |
| DarkBrotherhoodSanctuaryDialogue | 0002E7AF | Dark Brotherhood | every stage as it is |
| DarkBrotherhoodSanctuaryRepair | 00087BA2 | Dark Brotherhood | every stage as it is |
| DarkSideContractDialogue | 00024151 | Dark Brotherhood | every stage as it is |
| DB01 | 0001EA50 | Dark Brotherhood | every stage as it is |
| DB01Misc | 0001EA4F | Dark Brotherhood | every stage as it is |
| DB02 | 0001EA51 | Dark Brotherhood | every stage as it is |
| DB02a | 00022F47 | Dark Brotherhood | every stage as it is |
| DB03 | 0001EA52 | Dark Brotherhood | every stage as it is |
| DB04 | 0001EA53 | Dark Brotherhood | every stage as it is |
| DB04a | 0002AD16 | Dark Brotherhood | every stage as it is |
| DB05 | 0001EA54 | Dark Brotherhood | every stage as it is |
| DB06 | 0001EA55 | Dark Brotherhood | every stage as it is |
| DB07 | 0001EA56 | Dark Brotherhood | every stage as it is |
| DB08 | 0001EA57 | Dark Brotherhood | every stage as it is |
| DB11 | 0001EA59 | Dark Brotherhood | every stage as it is |
| DBDestroy | 000934FB | Dark Brotherhood | every stage as it is |
| DBEntranceQuest | 00050F2E | Dark Brotherhood | every stage as it is |
| DBEviction | 0006F9A5 | Dark Brotherhood | every stage as it is |
| DBrecurring | 0001EA5A | Dark Brotherhood | every stage as it is |
| DBRecurringControlQuest | 00097256 | Dark Brotherhood | every stage as it is |
| DBSideContract01 | 0001EA5B | Dark Brotherhood | every stage as it is |
| DBSideContract02 | 0001EA5E | Dark Brotherhood | every stage as it is |
| DBSideContract03 | 0001EA5F | Dark Brotherhood | every stage as it is |
| DBSideContract04 | 0001EA60 | Dark Brotherhood | every stage as it is |
| DBSideContract05 | 0001EA61 | Dark Brotherhood | every stage as it is |
| DBSideContract06 | 0001EA62 | Dark Brotherhood | every stage as it is |
| DBSideContract07 | 0001EA63 | Dark Brotherhood | every stage as it is |
| DBSideContract08 | 0001EA64 | Dark Brotherhood | every stage as it is |
| DBSideContract09 | 0001EA65 | Dark Brotherhood | every stage as it is |
| DBSideContract10 | 0001EA66 | Dark Brotherhood | every stage as it is |
| DBSideContract11 | 0001EA67 | Dark Brotherhood | every stage as it is |
| DBSideContract12 | 0001EA68 | Dark Brotherhood | every stage as it is |
| C00 | 0004B2D9 | Companions | every stage as it is |
| C00GiantAttack | 000C97D9 | Companions | every stage as it is |
| C01 | 0006E803 | Companions | every stage as it is |
| C02 | 0006E804 | Companions | every stage as it is |
| C02Kicker | 000A3EF8 | Companions | every stage as it is |
| C03 | 0001CEF4 | Companions | every stage as it is |
| C04 | 0001CEF5 | Companions | every stage as it is |
| C05 | 0006E805 | Companions | every stage as it is |
| C06 | 0001CEF6 | Companions | every stage as it is |
| CompanionsRejoinQuest | 000880E1 | Companions | every stage as it is |
| CR00 | 0001CEEF | Companions | every stage as it is |
| CR03 | 00025230 | Companions | every stage as it is |
| CR12PostQuest | 0010B10F | Companions | every stage as it is |
| DA02KillObj | 00052795 | Companions | every stage as it is |
| DA04 | 0002D512 | Daedric | every stage as it is |
| DA05 | 0002A49A | Daedric | every stage as it is |
| DA06 | 0003B681 | Daedric | every stage as it is |
| DA07 | 000240B8 | Daedric | every stage as it is |
| DA13 | 0008998D | Daedric | every stage as it is |
| BardsCollegePoeticEdda | 000E7814 | side | every stage as it is |
| DLC2BlackBook03Quest | 0403216C | side | every stage as it is |
| DLC2BlackBook04Quest | 0403216E | side | every stage as it is |
| DLC2BlackBook05Quest | 0403216D | side | every stage as it is |
| DLC2BlackBook06Quest | 0403216F | side | every stage as it is |
| DLC2BlackBook07Quest | 04032170 | side | every stage as it is |
| DLC2dunHaknirTreasureQST | 04024002 | side | every stage as it is |
| DLC2EbonyWarriorQuest | 0402C49E | side | every stage as it is |
| DLC2MH01 | 0401A50B | side | every stage as it is |
| DLC2MH02 | 0401A50C | side | every stage as it is |
| DLC2RR02 | 04018B14 | side | every stage as it is |
| DLC2RR03 | 04018B15 | side | every stage as it is |
| DLC2SV01 | 04019B4A | side | every stage as it is |
| DLC2SV02 | 0401CAF1 | side | every stage as it is |
| DLC2TT1 | 04017E8D | side | every stage as it is |
| DLC2TT1b | 0401844E | side | every stage as it is |
| DLC2TT2 | 040195A1 | side | every stage as it is |
| DLC2TTF1 | 0401AAB0 | side | every stage as it is |
| DLC2TTF2 | 0401AAC8 | side | every stage as it is |
| DLC2TTR2 | 0401C06E | side | every stage as it is |
| DLC2TTR3a | 0401B65D | side | every stage as it is |
| DLC2TTR3b | 0401D8D1 | side | every stage as it is |
| DLC2TTR4a | 0401B664 | side | every stage as it is |
| DLC2TTR5 | 0401C05C | side | every stage as it is |
| DLC2WB01 | 040209AE | side | every stage as it is |
| dunDarklightQST | 0003FA89 | side | every stage as it is |
| dunHunterQST | 00018601 | side | every stage as it is |
| dunTrevasWatchQST | 0004B2A1 | side | every stage as it is |
| dunValthumeQST | 0003ED82 | side | every stage as it is |
| dunVolunruudQST | 0008ADFA | side | every stage as it is |
| MS01 | 00018B4B | side | every stage as it is |
| MS02EscapeMadanachEnding | 00054880 | side | every stage as it is |
| MS02EscapeThonarEnding | 0006A9A0 | side | every stage as it is |
| MS03 | 0001DA3C | side | every stage as it is |
| MS04 | 000209CA | side | every stage as it is |
| MS05 | 00053511 | side | every stage as it is |
| MS06 | 00026C4D | side | every stage as it is |
| MS07 | 00023A64 | side | every stage as it is |
| MS08 | 0001CF25 | side | every stage as it is |
| MS09 | 0001CF26 | side | every stage as it is |
| MS11 | 0001F7A3 | side | every stage as it is |
| MS11b | 00020556 | side | every stage as it is |
| MS12b | 0002499C | side | every stage as it is |
| MS13 | 00039645 | side | every stage as it is |
| MS14 | 00025F3E | side | every stage as it is |
| NN01 | 000C9BA0 | side | every stage as it is |
| VC01 | 0005C625 | side | every stage as it is |
| CW | 00019E53 | Civil War | every stage as it is |
| CW01A | 000D517A | Civil War | every stage as it is |
| CW01B | 000E2D29 | Civil War | every stage as it is |
| CW02A | 0002D75C | Civil War | every stage as it is |
| CW02B | 0002610B | Civil War | every stage as it is |
| CW03 | 000D661C | Civil War | every stage as it is |
| CWAllies | 0008F307 | Civil War | every stage as it is |
| CWCampaignObj | 00026F45 | Civil War | every stage as it is |
| CWDialogue | 0005CBAA | Civil War | every stage as it is |
| CWFin | 00065C8D | Civil War | every stage as it is |
| CWFortDialogue | 00037B71 | Civil War | every stage as it is |
| CWJarlThanks | 000CB1CB | Civil War | every stage as it is |
| CWMerchants | 00024E1A | Civil War | every stage as it is |
| CWObj | 00029940 | Civil War | every stage as it is |
| CWSharedCombatInfos | 000770C0 | Civil War | every stage as it is |
| CWSharedInfos | 0002A3F2 | Civil War | every stage as it is |
| CWSiegeAttackObj | 0004E857 | Civil War | every stage as it is |
| CWSiegeDefendObj | 0002FF95 | Civil War | every stage as it is |
| CWSiegeObj | 00096E71 | Civil War | every stage as it is |
| DLC1HunterBaseIntro | 02004E24 | Dawnguard | every stage as it is |
| DLC1HunterBaseStage2 | 02013F1F | Dawnguard | every stage as it is |
| DLC1RV08 | 0200CE06 | Dawnguard | every stage as it is |
| DLC1RV09 | 0200CE0B | Dawnguard | every stage as it is |
| DLC1RV10 | 02019938 | Dawnguard | every stage as it is |
| DLC1VampireBaseIntro | 0200594C | Dawnguard | every stage as it is |
| DLC1VQ01 | 0200352A | Dawnguard | every stage as it is |
| DLC1VQ01MiscObjective | 0200D06C | Dawnguard | every stage as it is |
| DLC1VQ03Hunter | 020098CB | Dawnguard | every stage as it is |
| DLC1VQElder | 02005DA9 | Dawnguard | every stage as it is |
| DLC1VQElderHandler | 020181BF | Dawnguard | every stage as it is |
| DLC1VQSaint | 0201400B | Dawnguard | every stage as it is |
| DLC2MQ01 | 04017F8E | Dragonborn | every stage as it is |
| DLC2MQ03 | 04017F90 | Dragonborn | every stage as it is |
| DLC2MQ03B | 04035440 | Dragonborn | every stage as it is |
| DLC2MQ04 | 04016E1F | Dragonborn | every stage as it is |

## Shared, with limits

| Quest | Form ID | Tab | Why |
| --- | --- | --- | --- |
| CWDialogueSoldiersHellos | 0006B190 | main | a radiant job: shared while it runs as the host's, with the host's targets |
| DA10HauntingController | 0002BB38 | main | a radiant job: shared while it runs as the host's, with the host's targets; 1 near-only stage |
| FreeformWinterholdCFin | 0006531D | main | a radiant job: shared while it runs as the host's, with the host's targets |
| MG04MonkSceneQuest | 00076F39 | main | 1 stage made only with the friend who did it near |
| MQ105 | 000242BA | main | 1 stage made only with the friend who did it near |
| MQ201 | 00035D5F | main | 2 stages made only with the friend who did it near |
| MQ203 | 00036192 | main | 2 stages made only with the friend who did it near |
| MQ206 | 00036193 | main | 9 stages made only with the friend who did it near |
| MQ301 | 00044C56 | main | 2 stages made only with the friend who did it near |
| MQ302 | 00045923 | main | 5 stages made only with the friend who did it near |
| MQ303 | 00046EF0 | main | 5 stages made only with the friend who did it near |
| MQ304 | 00046EF1 | main | 2 stages made only with the friend who did it near |
| MQ305 | 00046EF2 | main | 4 stages made only with the friend who did it near |
| MQGreybeardCall | 0010EC61 | main | a radiant job: shared while it runs as the host's, with the host's targets |
| WIChangeLocation05 | 00070E30 | main | a radiant job: shared while it runs as the host's, with the host's targets |
| WISabotage | 0006D1E1 | main | a radiant job: shared while it runs as the host's, with the host's targets |
| mg06 | 0001F256 | College | 1 stage made only with the friend who did it near |
| MG07 | 0001F257 | College | 1 stage made only with the friend who did it near |
| MG08 | 0001F258 | College | 2 stages made only with the friend who did it near |
| MGSuspensionQuest | 0005B5DC | College | a radiant job: shared while it runs as the host's, with the host's targets |
| TG05 | 00021551 | Thieves Guild | 1 stage made only with the friend who did it near |
| TG08A | 00057F99 | Thieves Guild | 1 stage made only with the friend who did it near |
| TG08B | 00021554 | Thieves Guild | 2 stages made only with the friend who did it near |
| TG09 | 00021555 | Thieves Guild | 2 stages made only with the friend who did it near |
| TGRBE | 0002893B | Thieves Guild | a radiant job: shared while it runs as the host's, with the host's targets |
| TGRDU | 00015D24 | Thieves Guild | a radiant job: shared while it runs as the host's, with the host's targets |
| TGRFO | 0006098E | Thieves Guild | a radiant job: shared while it runs as the host's, with the host's targets |
| TGRGF | 00028922 | Thieves Guild | a radiant job: shared while it runs as the host's, with the host's targets |
| TGRHC | 00028936 | Thieves Guild | a radiant job: shared while it runs as the host's, with the host's targets |
| TGRNT | 00060990 | Thieves Guild | a radiant job: shared while it runs as the host's, with the host's targets |
| TGRSL | 0002893E | Thieves Guild | a radiant job: shared while it runs as the host's, with the host's targets |
| DarkBrotherhoodSanctuaryConvSystem | 0003444D | Dark Brotherhood | a radiant job: shared while it runs as the host's, with the host's targets |
| DB03GetArrestedQuest | 0008D5DA | Dark Brotherhood | a radiant job: shared while it runs as the host's, with the host's targets |
| DB09 | 0001EA58 | Dark Brotherhood | 1 stage made only with the friend who did it near |
| DB10 | 0003CEDA | Dark Brotherhood | 3 stages made only with the friend who did it near |
| DBSuspensionQuest | 000A2490 | Dark Brotherhood | a radiant job: shared while it runs as the host's, with the host's targets |
| C00LocationMonitoring1 | 000A3EC5 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| C00LocationMonitoring2 | 000A3EE4 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| C00LocationMonitoring3 | 000A3EE5 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR01 | 0001CEEE | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR02 | 00025185 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR04 | 00025231 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR05 | 0002522F | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR06 | 000C18E1 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR07 | 00025250 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR08 | 00025251 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR08EditorMonitoring | 0005478C | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR09 | 00025252 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR10 | 0009D6FC | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR11 | 0009D700 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR12 | 000E3145 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR13 | 000E3163 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| CR14 | 000E3156 | Companions | a radiant job: shared while it runs as the host's, with the host's targets |
| DA01 | 00028AD6 | Daedric | 5 stages made only with the friend who did it near |
| DA02 | 0004D8D6 | Daedric | 7 stages made only with the friend who did it near |
| DA03 | 0001BFC4 | Daedric | 1 stage made only with the friend who did it near |
| DA05LocationCheck | 0005D330 | Daedric | a radiant job: shared while it runs as the host's, with the host's targets |
| DA08 | 0004A37B | Daedric | 1 stage made only with the friend who did it near |
| DA09 | 0004E4E1 | Daedric | 5 stages made only with the friend who did it near |
| DA10 | 00022F08 | Daedric | a radiant job: shared while it runs as the host's, with the host's targets |
| DA11 | 0002C358 | Daedric | 1 stage made only with the friend who did it near |
| DA14 | 0001BB9B | Daedric | 3 stages made only with the friend who did it near |
| DA15 | 0002AC68 | Daedric | 4 stages made only with the friend who did it near |
| DA16 | 000242AF | Daedric | 3 stages made only with the friend who did it near |
| DLC1LD | 02005817 | side | 7 stages made only with the friend who did it near |
| DLC2dunKolbjornQST | 0401810B | side | 6 stages made only with the friend who did it near |
| DLC2RR01 | 04018B13 | side | 1 stage made only with the friend who did it near |
| DLC2TTR1 | 0401B65F | side | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC2TTR7 | 0401BD18 | side | a radiant job: shared while it runs as the host's, with the host's targets |
| dunFrostmereCryptQST | 000CF915 | side | a radiant job: shared while it runs as the host's, with the host's targets |
| MS02 | 00040A5E | side | 1 stage made only with the friend who did it near |
| MS10 | 0001DBFC | side | 1 stage made only with the friend who did it near |
| MS12 | 0002C258 | side | 1 stage made only with the friend who did it near |
| CWAttackCity | 0004F8BF | Civil War | a radiant job: shared while it runs as the host's, with the host's targets; 1 near-only stage |
| CWCampaign | 0001C4A1 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWEscapeCity | 0007EA5D | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWFinale | 000D1444 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWFortObserver | 000D5F08 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWFortSiegeCapital | 00087C21 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWFortSiegeFort | 00083042 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWFunctions | 0002BFAE | Civil War | 8 stages made only with the friend who did it near |
| CWGovernment | 00036626 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWMission03 | 0003BCC4 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWMission04 | 00035A23 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWMission07 | 000504F0 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWMissionCatchEventBottomA | 0005A82D | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWMissionCatchEventBottomB | 0005A830 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWMissionCatchEventTopA | 0005A82C | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWMissionCatchEventTopB | 0005A82F | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResetGarrison1 | 000236A3 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResetGarrison2 | 000548D5 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResetGarrison3 | 000548D6 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResetGarrison4 | 000548D7 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResetGarrison5 | 000548D8 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResetGarrison6 | 000548D9 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResetGarrison7 | 000548DA | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResetGarrison8 | 000548DB | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResolution01 | 0002B272 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWResolution02 | 0001FA52 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWSiege | 000954E1 | Civil War | a radiant job: shared while it runs as the host's, with the host's targets |
| CWSiegeQuickStart | 0009726A | Civil War | 31 stages made only with the friend who did it near |
| DLC1RH01 | 02005E4A | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RH02 | 02005E53 | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RH03 | 02005E58 | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RH04 | 02006952 | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RH05 | 02006BAC | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RH06 | 02007201 | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RH07 | 02007EC5 | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RH08 | 02015017 | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RV01 | 02004C1B | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RV02 | 020050B9 | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RV03 | 020050BD | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RV04 | 020056FD | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RV05 | 02005702 | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RV06 | 020058CA | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1RV07 | 0200A29C | Dawnguard | a radiant job: shared while it runs as the host's, with the host's targets |
| DLC1VQ02 | 02002F65 | Dawnguard | 2 stages made only with the friend who did it near |
| DLC1VQ03Vampire | 02004C3D | Dawnguard | 1 stage made only with the friend who did it near |
| DLC1VQ04 | 0200284F | Dawnguard | 3 stages made only with the friend who did it near |
| DLC1VQ05 | 02002850 | Dawnguard | 1 stage made only with the friend who did it near |
| DLC1VQ06 | 02002852 | Dawnguard | 3 stages made only with the friend who did it near |
| DLC1VQ07 | 02002853 | Dawnguard | 1 stage made only with the friend who did it near |
| DLC1VQ08 | 02007C25 | Dawnguard | 1 stage made only with the friend who did it near |
| DLC2MQ02 | 04017F8F | Dragonborn | 1 stage made only with the friend who did it near |
| DLC2MQ05 | 040179DE | Dragonborn | 1 stage made only with the friend who did it near |
| DLC2MQ06 | 040179D7 | Dragonborn | 1 stage made only with the friend who did it near |

## Not shared (quests with a journal tab; the game's untyped machinery is only counted above)

| Quest | Form ID | Tab | Why |
| --- | --- | --- | --- |
| MQ101 | 0003372B | main | each player plays it for themselves |
| AchievementsQuest | 000FC85D | misc | the misc tab: each player's own in this version |
| BardsCollegeDrum | 000D944F | misc | the misc tab: each player's own in this version |
| BardsCollegeFlute | 000D9431 | misc | the misc tab: each player's own in this version |
| BardsCollegeLute | 000D93FA | misc | the misc tab: each player's own in this version |
| BQ01 | 00095125 | misc | the misc tab: each player's own in this version |
| BQ02 | 000BD77F | misc | the misc tab: each player's own in this version |
| BQ03 | 000BD78C | misc | the misc tab: each player's own in this version |
| BQ04 | 000CBA9A | misc | the misc tab: each player's own in this version |
| BYOHHouseBanditAttack2 | 030008A8 | misc | the misc tab: each player's own in this version |
| BYOHHouseBuilding | 0300305D | misc | the misc tab: each player's own in this version |
| BYOHHouseFalkreath | 0300305E | misc | the misc tab: each player's own in this version |
| BYOHHouseHjaalmarch | 0300BE09 | misc | the misc tab: each player's own in this version |
| BYOHHousePale | 03010BEE | misc | the misc tab: each player's own in this version |
| BYOHRelationshipAdoptable | 03004291 | misc | the misc tab: each player's own in this version |
| BYOHRelationshipAdoptableStewardCourier | 03003F7B | misc | the misc tab: each player's own in this version |
| BYOHRelationshipAdoption | 030042B4 | misc | the misc tab: each player's own in this version |
| BYOHRelationshipAdoptionCWSiegeHandler | 03003F7E | misc | the misc tab: each player's own in this version |
| BYOHRelationshipAdoptionNewAdoptionHandler | 03003F6E | misc | the misc tab: each player's own in this version |
| BYOHRelationshipAdoptionScheduler | 0300401C | misc | the misc tab: each player's own in this version |
| CompanionsMiscObjective | 000D19B9 | misc | the misc tab: each player's own in this version |
| CompanionsRadiantMiscObjective | 00109AAA | misc | the misc tab: each player's own in this version |
| CW00A | 000D3C5F | misc | the misc tab: each player's own in this version |
| CW00B | 000E1B5A | misc | the misc tab: each player's own in this version |
| CW01AOutfitImperial | 000C349B | misc | the misc tab: each player's own in this version |
| CWMiscObj | 00064F6C | misc | the misc tab: each player's own in this version |
| CWPostWhiterunObj | 001052F3 | misc | the misc tab: each player's own in this version |
| DA01dunIlinaltasDeepQST | 00089CD1 | misc | the misc tab: each player's own in this version |
| DA01Intro | 00092E25 | misc | the misc tab: each player's own in this version |
| DA03Start | 000D7939 | misc | the misc tab: each player's own in this version |
| DA05_dunBloatedMansGrottoQST | 000955CF | misc | the misc tab: each player's own in this version |
| DA07FIN | 000BA1CC | misc | the misc tab: each player's own in this version |
| DA07intro | 00094D89 | misc | the misc tab: each player's own in this version |
| DA08RumorPointer | 000DC9E0 | misc | the misc tab: each player's own in this version |
| DA10MiscObjective | 000DC53C | misc | the misc tab: each player's own in this version |
| DA11Intro | 0007D949 | misc | the misc tab: each player's own in this version |
| DA11IntroScene | 000AEC22 | misc | the misc tab: each player's own in this version |
| Da13Intro | 000BEDEC | misc | the misc tab: each player's own in this version |
| DA14Start | 0003FA17 | misc | the misc tab: each player's own in this version |
| DA15Rumor | 000D7897 | misc | the misc tab: each player's own in this version |
| DarkwaterCrossingDerkeethusRescueQuest | 00076B4A | misc | the misc tab: each player's own in this version |
| DB01MiscObjective | 000BC8B6 | misc | the misc tab: each player's own in this version |
| DB01Objective | 000BC8B7 | misc | the misc tab: each player's own in this version |
| DB11_dunKatariahQST | 000C6020 | misc | the misc tab: each player's own in this version |
| DBOlavaReadingMiscObjective | 0002E655 | misc | the misc tab: each player's own in this version |
| DBOlavaReadingTreasureObjective | 0002E656 | misc | the misc tab: each player's own in this version |
| DBRecurringIntroMisc | 00097255 | misc | the misc tab: each player's own in this version |
| DBTortureTreasureMiscObjective1 | 00037A2D | misc | the misc tab: each player's own in this version |
| DBTortureTreasureMiscObjective2 | 00037A51 | misc | the misc tab: each player's own in this version |
| DBTortureTreasureMiscObjective3 | 00037A53 | misc | the misc tab: each player's own in this version |
| DBTortureTreasureMiscObjective4 | 00037AE3 | misc | the misc tab: each player's own in this version |
| DialogueCidhnaMine | 000362AC | misc | the misc tab: each player's own in this version |
| DialogueFollower | 000750BA | misc | the misc tab: each player's own in this version |
| DialogueIvarsteadScriptedScene | 000DD5FA | misc | the misc tab: each player's own in this version |
| DialogueRiftenGateGuards | 000E66FC | misc | the misc tab: each player's own in this version |
| DialogueRiftenSS01 | 000AA0B3 | misc | the misc tab: each player's own in this version |
| DialogueRiftenSS02 | 000C19A8 | misc | the misc tab: each player's own in this version |
| DialogueRiftenSS04 | 000B668C | misc | the misc tab: each player's own in this version |
| DialogueRiftenSS05 | 0005CE25 | misc | the misc tab: each player's own in this version |
| DialogueShorsStone | 0002E11B | misc | the misc tab: each player's own in this version |
| DialogueSolitudeGuardIntro | 00034B58 | misc | the misc tab: each player's own in this version |
| DLC01SoulCairnHorseQuest2 | 020163CD | misc | the misc tab: each player's own in this version |
| DLC1LD_Misc | 0200B639 | misc | the misc tab: each player's own in this version |
| DLC1Radiant | 02004C1C | misc | the misc tab: each player's own in this version |
| DLC1Surgery | 0200697C | misc | the misc tab: each player's own in this version |
| DLC1VampireTutorial | 0201909F | misc | the misc tab: each player's own in this version |
| DLC1VQ00 | 0200D90E | misc | the misc tab: each player's own in this version |
| DLC1VQ00ChangeLocation | 0200D90B | misc | the misc tab: each player's own in this version |
| DLC1VQ05SoulGemHandler | 0200EA41 | misc | the misc tab: each player's own in this version |
| DLC1VQCastleGuard | 0201A415 | misc | the misc tab: each player's own in this version |
| DLC1VQDragon | 0200C73A | misc | the misc tab: each player's own in this version |
| DLC1VQDragonMisc | 0201119F | misc | the misc tab: each player's own in this version |
| DLC1VQFVBooks | 0201A3E4 | misc | the misc tab: each player's own in this version |
| DLC2CultistAmbush | 0403546D | misc | the misc tab: each player's own in this version |
| DLC2dunFrostmoonQSTMisc | 04035E2F | misc | the misc tab: each player's own in this version |
| DLC2dunHaknirQSTIncreaseLevel | 04038FB0 | misc | the misc tab: each player's own in this version |
| DLC2dunHaknirTreasureQSTMisc | 0403661B | misc | the misc tab: each player's own in this version |
| DLC2dunHrodulf | 04037560 | misc | the misc tab: each player's own in this version |
| DLC2dunKolbjornMiscQST | 0402667E | misc | the misc tab: each player's own in this version |
| DLC2KagrumezQST | 0403166D | misc | the misc tab: each player's own in this version |
| DLC2MHMisc | 0402BAB0 | misc | the misc tab: each player's own in this version |
| DLC2RR03Intro | 04020A1C | misc | the misc tab: each player's own in this version |
| DLC2RR03Priest | 0403332A | misc | the misc tab: each player's own in this version |
| DLC2RRFavor01 | 04024DDE | misc | the misc tab: each player's own in this version |
| DLC2RRFavor02 | 04024DE7 | misc | the misc tab: each player's own in this version |
| DLC2RRFavor03 | 04024DFC | misc | the misc tab: each player's own in this version |
| DLC2RRFavor04 | 04024E04 | misc | the misc tab: each player's own in this version |
| DLC2RRFavor05 | 04024FA8 | misc | the misc tab: each player's own in this version |
| DLC2RRFavor06 | 04024FAD | misc | the misc tab: each player's own in this version |
| DLC2RRFavor07 | 0402BAC6 | misc | the misc tab: each player's own in this version |
| DLC2SkaalVillageFreeform1 | 0402C8AA | misc | the misc tab: each player's own in this version |
| DLC2SkaalVillageFreeform2 | 04031787 | misc | the misc tab: each player's own in this version |
| DLC2SV02AncarionMerchant | 04027E15 | misc | the misc tab: each player's own in this version |
| DLC2SV02Misc | 0401E169 | misc | the misc tab: each player's own in this version |
| DLC2TGQuest | 0402AD26 | misc | the misc tab: each player's own in this version |
| DLC2ThirskFFElmus | 04038EBF | misc | the misc tab: each player's own in this version |
| DLC2ThirskFFElmusBack | 04038EC4 | misc | the misc tab: each player's own in this version |
| DLC2ThirskFFHalbarn | 04038FBC | misc | the misc tab: each player's own in this version |
| DLC2ThirskFFHilund | 04038FBB | misc | the misc tab: each player's own in this version |
| DLC2TTR4b | 0401CDE1 | misc | the misc tab: each player's own in this version |
| DLC2TTR8 | 0401F13B | misc | the misc tab: each player's own in this version |
| DLC2WE03 | 0401F989 | misc | the misc tab: each player's own in this version |
| DLC2WE06 | 04027A14 | misc | the misc tab: each player's own in this version |
| DLC2WE09 | 040331C0 | misc | the misc tab: each player's own in this version |
| DLC2WE12 | 0403923B | misc | the misc tab: each player's own in this version |
| dunAlftandQST | 0003B0FB | misc | the misc tab: each player's own in this version |
| dunAngarvundeQST | 0007EDE5 | misc | the misc tab: each player's own in this version |
| dunAnsilvundQST | 0003D721 | misc | the misc tab: each player's own in this version |
| dunBlindCliffQST | 00039B3B | misc | the misc tab: each player's own in this version |
| dunEmbershardQST | 000B6FEF | misc | the misc tab: each player's own in this version |
| dunFolgunthurQST | 0004CEDE | misc | the misc tab: each player's own in this version |
| dunFrostflowAbyssQST | 00048121 | misc | the misc tab: each player's own in this version |
| dunFrostmereToggleQST | 0002AA57 | misc | the misc tab: each player's own in this version |
| dunGeirmundQST | 000E738F | misc | the misc tab: each player's own in this version |
| dunHobsFallQST | 00082FBB | misc | the misc tab: each player's own in this version |
| dunIronbindQST | 0006CD54 | misc | the misc tab: each player's own in this version |
| dunLabyrinthian | 00091F1A | misc | the misc tab: each player's own in this version |
| dunLiarsRetreatQST | 000AA5D6 | misc | the misc tab: each player's own in this version |
| dunLostValleyRedoubtQST | 000B0F5B | misc | the misc tab: each player's own in this version |
| dunMidden01QST | 00055977 | misc | the misc tab: each player's own in this version |
| dunMistwatchQST | 00030B36 | misc | the misc tab: each player's own in this version |
| dunMossMotherQST | 000376EE | misc | the misc tab: each player's own in this version |
| dunNilheimQST | 0001BAEC | misc | the misc tab: each player's own in this version |
| dunRagnvaldQST | 0003763C | misc | the misc tab: each player's own in this version |
| dunRaldbtharQST | 000B7C01 | misc | the misc tab: each player's own in this version |
| dunRebelsCairnChangeLocationQST | 000EB9E2 | misc | the misc tab: each player's own in this version |
| dunRebelsCairnQST | 000C1ACC | misc | the misc tab: each player's own in this version |
| dunRobbersGorgeQST | 000B8C9F | misc | the misc tab: each player's own in this version |
| dunShriekwindQST | 000A2408 | misc | the misc tab: each player's own in this version |
| dunSleepingTreeCampQST | 000E1A9D | misc | the misc tab: each player's own in this version |
| dunSouthfringeQST | 0003A74A | misc | the misc tab: each player's own in this version |
| dunTalkingStoneQST | 0004BA6C | misc | the misc tab: each player's own in this version |
| Favor001 | 000403B0 | misc | the misc tab: each player's own in this version |
| Favor013 | 000CA209 | misc | the misc tab: each player's own in this version |
| Favor017 | 000CA210 | misc | the misc tab: each player's own in this version |
| Favor018 | 0003F46A | misc | the misc tab: each player's own in this version |
| Favor019 | 0003F4EC | misc | the misc tab: each player's own in this version |
| Favor104 | 0006FD35 | misc | the misc tab: each player's own in this version |
| Favor109 | 000940E0 | misc | the misc tab: each player's own in this version |
| Favor110 | 000CA439 | misc | the misc tab: each player's own in this version |
| Favor151 | 0006C824 | misc | the misc tab: each player's own in this version |
| Favor153 | 00090E36 | misc | the misc tab: each player's own in this version |
| Favor154 | 0007172A | misc | the misc tab: each player's own in this version |
| Favor157 | 000A3504 | misc | the misc tab: each player's own in this version |
| Favor158 | 000940D0 | misc | the misc tab: each player's own in this version |
| Favor204 | 0007105B | misc | the misc tab: each player's own in this version |
| Favor205 | 0006FE37 | misc | the misc tab: each player's own in this version |
| Favor250 | 000A2C86 | misc | the misc tab: each player's own in this version |
| Favor252 | 000A2C9B | misc | the misc tab: each player's own in this version |
| Favor253 | 000A2C9E | misc | the misc tab: each player's own in this version |
| Favor254 | 000A2CA6 | misc | the misc tab: each player's own in this version |
| Favor255 | 000A34CE | misc | the misc tab: each player's own in this version |
| Favor256 | 000A34D4 | misc | the misc tab: each player's own in this version |
| Favor257 | 000A34D7 | misc | the misc tab: each player's own in this version |
| Favor258 | 000A34DE | misc | the misc tab: each player's own in this version |
| FavorJarlsMakeFriends | 00087E24 | misc | the misc tab: each player's own in this version |
| FavorOrcsIntro | 00076335 | misc | the misc tab: each player's own in this version |
| FreeformCaravansA | 000D9393 | misc | the misc tab: each player's own in this version |
| FreeformCidhnaMineA | 000DB851 | misc | the misc tab: each player's own in this version |
| FreeformDragonBridge01 | 000555DD | misc | the misc tab: each player's own in this version |
| FreeformDushnikhYalA | 0009976E | misc | the misc tab: each player's own in this version |
| FreeformFalkreathQuest01 | 00093753 | misc | the misc tab: each player's own in this version |
| FreeformFalkreathQuest02 | 0009375A | misc | the misc tab: each player's own in this version |
| FreeformFalkreathQuest03 | 000B83C3 | misc | the misc tab: each player's own in this version |
| FreeformFalkreathQuest03B | 000D6986 | misc | the misc tab: each player's own in this version |
| FreeformHeartwoodMill | 000E3EA5 | misc | the misc tab: each player's own in this version |
| FreeformHighHrothgarA | 000E76DE | misc | the misc tab: each player's own in this version |
| FreeformHighHrothgarB | 000E8283 | misc | the misc tab: each player's own in this version |
| FreeformIvarstead01 | 000D9B63 | misc | the misc tab: each player's own in this version |
| FreeformIvarstead02 | 000D9B64 | misc | the misc tab: each player's own in this version |
| FreeformIvarstead03 | 000D9B65 | misc | the misc tab: each player's own in this version |
| FreeformIvarstead04 | 000D9B66 | misc | the misc tab: each player's own in this version |
| FreeformKarthwastenA | 000622E8 | misc | the misc tab: each player's own in this version |
| FreeformKolskeggrA | 0001FD72 | misc | the misc tab: each player's own in this version |
| FreeformKynesgroveA | 00069043 | misc | the misc tab: each player's own in this version |
| FreeformMarkarthD | 00075399 | misc | the misc tab: each player's own in this version |
| FreeformMarkarthE | 000753A1 | misc | the misc tab: each player's own in this version |
| FreeformMarkarthJ | 000790E1 | misc | the misc tab: each player's own in this version |
| FreeformMarkarthL | 0006099C | misc | the misc tab: each player's own in this version |
| FreeformMarkarthM | 00064798 | misc | the misc tab: each player's own in this version |
| FreeformMarkarthN | 000647B3 | misc | the misc tab: each player's own in this version |
| FreeformMarkarthO | 00053804 | misc | the misc tab: each player's own in this version |
| FreeformMerryfairFarm | 000E3E9B | misc | the misc tab: each player's own in this version |
| FreeformMorthalB | 00021896 | misc | the misc tab: each player's own in this version |
| FreeformOldHroldanB | 0006819A | misc | the misc tab: each player's own in this version |
| FreeformRiften01 | 00053305 | misc | the misc tab: each player's own in this version |
| FreeformRiften02 | 00053306 | misc | the misc tab: each player's own in this version |
| FreeformRiften03 | 00053307 | misc | the misc tab: each player's own in this version |
| FreeformRiften04 | 00053308 | misc | the misc tab: each player's own in this version |
| FreeformRiften05 | 00053309 | misc | the misc tab: each player's own in this version |
| FreeformRiften06 | 00053318 | misc | the misc tab: each player's own in this version |
| FreeformRiften07 | 0005331A | misc | the misc tab: each player's own in this version |
| FreeformRiften08 | 0005331B | misc | the misc tab: each player's own in this version |
| FreeformRiften09 | 0005331C | misc | the misc tab: each player's own in this version |
| FreeformRiften10 | 0005331D | misc | the misc tab: each player's own in this version |
| FreeformRiften11 | 00041F91 | misc | the misc tab: each player's own in this version |
| FreeformRiften12 | 00041FD2 | misc | the misc tab: each player's own in this version |
| FreeformRiften13 | 00043E1A | misc | the misc tab: each player's own in this version |
| FreeformRiften14 | 00043E25 | misc | the misc tab: each player's own in this version |
| FreeformRiften15 | 000557E9 | misc | the misc tab: each player's own in this version |
| FreeformRiften16 | 000557F3 | misc | the misc tab: each player's own in this version |
| FreeformRiften17 | 0005B2DE | misc | the misc tab: each player's own in this version |
| FreeformRiften18 | 0005B3A5 | misc | the misc tab: each player's own in this version |
| FreeformRiften19 | 00065C30 | misc | the misc tab: each player's own in this version |
| FreeformRiften20 | 00085D40 | misc | the misc tab: each player's own in this version |
| FreeformRiften21 | 0009F179 | misc | the misc tab: each player's own in this version |
| FreeformRiften22 | 000C1E07 | misc | the misc tab: each player's own in this version |
| FreeformRiftenThane | 00065BDF | misc | the misc tab: each player's own in this version |
| FreeformRiverwood01 | 00074A83 | misc | the misc tab: each player's own in this version |
| FreeformSalviusFarmA | 000931BF | misc | the misc tab: each player's own in this version |
| FreeformSarethiFarm | 000E3308 | misc | the misc tab: each player's own in this version |
| FreeformShipsDawnstar | 00094152 | misc | the misc tab: each player's own in this version |
| FreeformShipsWindhelm | 00094158 | misc | the misc tab: each player's own in this version |
| FreeformShorsStone01 | 000E49E7 | misc | the misc tab: each player's own in this version |
| FreeformShorsStone02 | 000E49E8 | misc | the misc tab: each player's own in this version |
| FreeformSkyhavenTempleA | 000E38C9 | misc | the misc tab: each player's own in this version |
| FreeformSkyHavenTempleB | 000E3E67 | misc | the misc tab: each player's own in this version |
| FreeformSkyHavenTempleC | 000E6DE3 | misc | the misc tab: each player's own in this version |
| FreeformSkyHavenTempleD | 000E6DF1 | misc | the misc tab: each player's own in this version |
| FreeformSoljundsSinkholeA | 0006136B | misc | the misc tab: each player's own in this version |
| FreeformStonehillsA | 00064958 | misc | the misc tab: each player's own in this version |
| FreeformValdDebt | 000C04BA | misc | the misc tab: each player's own in this version |
| FreeformWhiterunQuest01 | 000934F9 | misc | the misc tab: each player's own in this version |
| FreeformWhiterunQuest02 | 0009380B | misc | the misc tab: each player's own in this version |
| FreeformWhiterunQuest03 | 000946FD | misc | the misc tab: each player's own in this version |
| FreeformWhiterunQuest04 | 00094E3C | misc | the misc tab: each player's own in this version |
| FreeformWhiterunQuest05 | 00094E3F | misc | the misc tab: each player's own in this version |
| FreeformWindhelmA | 000CD882 | misc | the misc tab: each player's own in this version |
| FreeformWinterholdA | 00062F54 | misc | the misc tab: each player's own in this version |
| FreeformWinterholdC | 00064EA8 | misc | the misc tab: each player's own in this version |
| FreeformWinterholdCollegeA | 0005D2E9 | misc | the misc tab: each player's own in this version |
| FreeformWinterholdCollegeB | 000E357D | misc | the misc tab: each player's own in this version |
| GenericDialgoueWounded | 000E725D | misc | the misc tab: each player's own in this version |
| HousePurchase | 000A7B33 | misc | the misc tab: each player's own in this version |
| JailQuest | 0010D9F4 | misc | the misc tab: each player's own in this version |
| MG01Pointer | 00054D0A | misc | the misc tab: each player's own in this version |
| MGR01 | 000266F1 | misc | the misc tab: each player's own in this version |
| MGR02 | 000266F2 | misc | the misc tab: each player's own in this version |
| MGR10 | 00028A13 | misc | the misc tab: each player's own in this version |
| MGR11 | 00028A19 | misc | the misc tab: each player's own in this version |
| MGR12 | 00028A1E | misc | the misc tab: each player's own in this version |
| MGR20 | 00028A24 | misc | the misc tab: each player's own in this version |
| MGR20B | 000BC096 | misc | the misc tab: each player's own in this version |
| MGRAppBrelyna01 | 000C0416 | misc | the misc tab: each player's own in this version |
| MQ201Malborn | 0004CECE | misc | the misc tab: each player's own in this version |
| MQ201RecoverGearMISC | 000A7921 | misc | the misc tab: each player's own in this version |
| MQ306 | 00046EF3 | misc | the misc tab: each player's own in this version |
| MS01MiscObjectives | 000D1958 | misc | the misc tab: each player's own in this version |
| MS05Rumor | 000D789C | misc | the misc tab: each player's own in this version |
| MS05Start | 00042868 | misc | the misc tab: each player's own in this version |
| MS05_dunDeadMensRespiteQST | 00016C8D | misc | the misc tab: each player's own in this version |
| MS06Rumor | 000D789A | misc | the misc tab: each player's own in this version |
| MS07Rumor | 000AA9A9 | misc | the misc tab: each player's own in this version |
| MS07_dunIcerunnerQST | 000906D4 | misc | the misc tab: each player's own in this version |
| MS10MiscObjPointer | 0010D3BB | misc | the misc tab: each player's own in this version |
| MS13Intro | 000B6031 | misc | the misc tab: each player's own in this version |
| PerksQuest | 0005F596 | misc | the misc tab: each player's own in this version |
| RelationshipMarriage | 00074793 | misc | the misc tab: each player's own in this version |
| RelationshipMarriageBreakUp | 0007431B | misc | the misc tab: each player's own in this version |
| RelationshipMarriageFIN | 00021382 | misc | the misc tab: each player's own in this version |
| RelationshipMarriageWedding | 0007404E | misc | the misc tab: each player's own in this version |
| RelationshipOrcs | 0006819E | misc | the misc tab: each player's own in this version |
| RoriksteadFreeform | 0004EC52 | misc | the misc tab: each player's own in this version |
| SolitudeFreeform01 | 000E8BD7 | misc | the misc tab: each player's own in this version |
| SolitudeFreeform02 | 000C7D1C | misc | the misc tab: each player's own in this version |
| SolitudeFreeform03 | 000C7D42 | misc | the misc tab: each player's own in this version |
| SolitudeFreeform04 | 000D055D | misc | the misc tab: each player's own in this version |
| SolitudeFreeform06 | 000D69FB | misc | the misc tab: each player's own in this version |
| SolitudeFreeform07 | 000E77BA | misc | the misc tab: each player's own in this version |
| SolitudeOpening | 000B2FD9 | misc | the misc tab: each player's own in this version |
| T01DegaineFavor | 000D45F4 | misc | the misc tab: each player's own in this version |
| T01Intro | 000C5940 | misc | the misc tab: each player's own in this version |
| T03Intro | 0006A3D5 | misc | the misc tab: each player's own in this version |
| TG00MaulHandler | 00064A14 | misc | the misc tab: each player's own in this version |
| TG00MiscHandler | 000C99EF | misc | the misc tab: each player's own in this version |
| TG02B | 00084ABD | misc | the misc tab: each player's own in this version |
| TG04EECHandler | 001007FC | misc | the misc tab: each player's own in this version |
| TG04Post | 000D3AC6 | misc | the misc tab: each player's own in this version |
| TGCrownMisc | 0006D585 | misc | the misc tab: each player's own in this version |
| TGFenceCaravan | 000D8E40 | misc | the misc tab: each player's own in this version |
| TGLarceny | 0004C6C2 | misc | the misc tab: each player's own in this version |
| TGRShell | 00028923 | misc | the misc tab: each player's own in this version |
| TutorialAlchemy | 0010557C | misc | the misc tab: each player's own in this version |
| TutorialBlacksmithing | 00105385 | misc | the misc tab: each player's own in this version |
| TutorialEnchanting | 00105A60 | misc | the misc tab: each player's own in this version |
| WE100ObjA | 000A17AD | misc | the misc tab: each player's own in this version |
| WE100ObjB | 000A17B4 | misc | the misc tab: each player's own in this version |
| WE100ObjC | 000A17B5 | misc | the misc tab: each player's own in this version |
| WE100ObjD | 000A17B6 | misc | the misc tab: each player's own in this version |
| WE100ObjE | 000A17B7 | misc | the misc tab: each player's own in this version |
| WE100ObjF | 000A17B8 | misc | the misc tab: each player's own in this version |
| WE100ObjG | 000A17B9 | misc | the misc tab: each player's own in this version |
| WE100ObjH | 000A17BA | misc | the misc tab: each player's own in this version |
| WE100ObjI | 000A17BB | misc | the misc tab: each player's own in this version |
| WE100ObjJ | 000A17BC | misc | the misc tab: each player's own in this version |
| WE24 | 00062119 | misc | the misc tab: each player's own in this version |
| WE31 | 000753CA | misc | the misc tab: each player's own in this version |
| WEBountyCollectorQST | 000F8A80 | misc | the misc tab: each player's own in this version |
| WEDA02 | 0004A602 | misc | the misc tab: each player's own in this version |
| WEDL13 | 000BD757 | misc | the misc tab: each player's own in this version |
| WEDL14 | 000BD758 | misc | the misc tab: each player's own in this version |
| WEJS02 | 000B5D41 | misc | the misc tab: each player's own in this version |
| WEJS10 | 000B85B8 | misc | the misc tab: each player's own in this version |
| WEJS11 | 000B85BD | misc | the misc tab: each player's own in this version |
| WEJS12 | 000BA2FF | misc | the misc tab: each player's own in this version |
| WEJS13 | 000BC037 | misc | the misc tab: each player's own in this version |
| WEJS14 | 000BED92 | misc | the misc tab: each player's own in this version |
| WEJS15 | 000BEDAF | misc | the misc tab: each player's own in this version |
| WEJS16 | 000E8921 | misc | the misc tab: each player's own in this version |
| WEJS18 | 000E8929 | misc | the misc tab: each player's own in this version |
| WEJS20 | 000E892D | misc | the misc tab: each player's own in this version |
| WEJS21 | 000E892E | misc | the misc tab: each player's own in this version |
| WEJS24 | 000E8924 | misc | the misc tab: each player's own in this version |
| WEJS25 | 000E8941 | misc | the misc tab: each player's own in this version |
| WEJS26 | 00105D12 | misc | the misc tab: each player's own in this version |
| WEJS28 | 00105D19 | misc | the misc tab: each player's own in this version |
| WEMaiqTheLiar | 000BA0C3 | misc | the misc tab: each player's own in this version |
| WERoad06 | 00105D1D | misc | the misc tab: each player's own in this version |
| WERoad07 | 00105D15 | misc | the misc tab: each player's own in this version |
| weroad08 | 001065F4 | misc | the misc tab: each player's own in this version |
| WERoad09 | 001065FA | misc | the misc tab: each player's own in this version |
| WEThalmorDialogue | 000B5E0C | misc | the misc tab: each player's own in this version |
| WIAddItem07 | 000A0F43 | misc | the misc tab: each player's own in this version |
| WIAssault02 | 00039792 | misc | the misc tab: each player's own in this version |
| WICastMagic01 | 00070217 | misc | the misc tab: each player's own in this version |
| WICastMagic01ActorDialogueEvent | 00070223 | misc | the misc tab: each player's own in this version |
| WICastMagic02 | 000AB3FA | misc | the misc tab: each player's own in this version |
| WICastMagic03 | 000167D3 | misc | the misc tab: each player's own in this version |
| WICastMagic04 | 00023EE4 | misc | the misc tab: each player's own in this version |
| WICastMagicNonHostileSpell01 | 000A8A86 | misc | the misc tab: each player's own in this version |
| WIChangeLocation03 | 0006E028 | misc | the misc tab: each player's own in this version |
| WIChangeLocation04 | 0006F211 | misc | the misc tab: each player's own in this version |
| WIGamesHideAndSeek | 0004B869 | misc | the misc tab: each player's own in this version |
| WIGamesTag | 000AE004 | misc | the misc tab: each player's own in this version |
| WIKill04 | 00071441 | misc | the misc tab: each player's own in this version |
| WIKill04RivalDialgoue | 00071445 | misc | the misc tab: each player's own in this version |
| WITavern | 000DEE92 | misc | the misc tab: each player's own in this version |
| WITavernServerPatronSceneA | 000DEE9A | misc | the misc tab: each player's own in this version |
