# Porting map: from F4Multiplayer to SkyrimCoop

[F4Multiplayer](https://github.com/Jolley71717/F4Multiplayer) is the sister project for Fallout 4, under the same license. This page goes through its source and says, for each part, one of three things:

- **Carried as is**: copied with only names changed (the project's name, paths, the log's name). Already here.
- **Carried with changes**: the same idea and much of the same code, cut down or adapted. Here in a first form, or to be brought over when its phase needs it.
- **Redo for Skyrim**: the idea carries, the code does not, because it is written against Fallout 4's engine, data or content. Each such part names what to look for in Skyrim.

Sizes are lines of `.cpp` and `.h` in F4Multiplayer's `main` at the time of the copy (version 0.21.0), rounded where a group is summed. They say how much thinking went into a part, not how much typing a port needs.

Nothing here has been run in a game. The "Skyrim equivalent" entries name classes and events of CommonLibSSE-NG and of the engine as starting points to look at; each has to be found and checked on an installed game before any code relies on it.

## Summary

| Part of F4Multiplayer | Lines | Here |
| --- | --- | --- |
| `common/` (protocol, server, transports, helpers) | 20,100 | about 2,100 carried: the helpers as is, the protocol and server as a small first version |
| `src/` top level (start-up, log, config, crash and hang watch, text, developer channel) | 11,200 | about 870: log, launch report, platform as is; start-up and config written new and small |
| `src/net/` (the plugin's side of a session) | 3,200 | about 350 carried as is; the session itself not yet |
| `src/steam/` | 1,600 | all 1,600 carried, with the app id and names changed |
| `src/game/` (everything that touches the game) | 137,400 | four stubs, about 170 lines; all of it is to be redone |
| `server/`, `bot/` | 930 | about 330, cut down to the small protocol |
| `tests/` | 120,300 | about 2,900: the tests of everything carried |
| `ui/` (the in-game menu, Flash) | | nothing yet |
| `tools/`, `.github/`, `dist/`, docs | | packaging, promotion, hooks and all workflows carried; the first test's rig scripts written new (`tools/rig/`, below) |

## `common/`: no game, no Windows

| File | Lines | Status | Notes |
| --- | --- | --- | --- |
| `Transport.h` | 52 | as is | The interface between a session and how its bytes travel. |
| `EnetTransport.h/.cpp` | 167 | as is | UDP through ENet. |
| `Net.h/.cpp` | 91 | as is | Starting and stopping the network library. |
| `TokenBucket.h` | 41 | as is | Rate limits. |
| `IniFile.h` | 98 | as is | The ini reader. |
| `Name.h` | 164 | as is | What a player's name may hold. |
| `Redact.h/.cpp` | 187 | as is | `player N`, masked addresses, the last scrub over log lines. |
| `FormIds.h` | 260 | as is | Form id arithmetic: full plugins, light plugins, ids made at run time. Skyrim Special Edition uses the same layout (light plugins since 1.5.x); to confirm on a real load order. |
| `Protocol.h` | 7,407 | with changes | Here 720 lines: the reader and writer, the hello with its load order, welcome and refusal, join and leave, player state, a player's looks (race, sex, weight, worn armor: protocol 2), heartbeat. The other 6,800 lines are messages for Fallout 4's systems; each Skyrim system adds its own messages when its phase is built. |
| `Server.h/.cpp` | 5,704 | with changes | Here 434 lines: admission (version, load order, password, room), player ids, passing states and looks on. The rest (who runs which NPC, the shared world, the session file) comes back phase by phase. |
| `BanList.h` | 223 | with changes, later | Game-free. Bring over with the first menu that can remove a player. |
| `FormTranslate.h` | 844 | with changes, later | Translating form ids between load orders that differ. Only needed if differing load orders are ever allowed ([DESIGN.md](DESIGN.md)). |
| `SessionFile.h`, `SessionBuilds.h`, `SessionLevel.h`, `SaveRetry.h` | 1,025 | with changes, later | What the host keeps beside its save about the session. The idea carries (SKSE has co-saves, like F4SE); the contents are per system. |
| `DropStore.h`, `StorageBook.h`, `ObjectMovers.h`, `NpcHealths.h` | 1,384 | with changes, later | Game-free books for dropped items, containers, moved objects, NPC health. Mostly reusable in phases 3 and 4, after checking each against Skyrim's rules. |
| `AttackBook.h`, `RewardBook.h`, `CompanionBook.h`, `CatchUpXp.h`, `GroupDifficulty.h`, `RulesSummary.h`, `TimeScaleBook.h` | 1,837 | redo | Rules of Fallout 4's content (settlement attacks, quest rewards, companions, XP). Skyrim's followers, levelling by skill use and quest rewards work differently; take the shape, rewrite the rules. |
| `WorkshopStore.h`, `SettlerRoster.h`, `BenchNumbersBook.h` | 642 | not needed | Settlements. Skyrim has no workshop mode. |

## `src/` top level

| File | Lines | Status | Notes |
| --- | --- | --- | --- |
| `Log.h/.cpp`, `LogRotation.h` | 177 | as is | The log and keeping the last three. Path changed to `My Games\Skyrim Special Edition\SKSE`. |
| `LaunchReport.h`, `Platform.h/.cpp` | 238 | as is | What the log says about Windows or Proton, the Steam app id and the overlay. Its wording about Proton comes from Fallout 4 experience; to check under Skyrim. |
| `pch.h` | 34 | with changes | CommonLibSSE-NG's headers instead of CommonLibF4's. |
| `main.cpp` | 1,288 | with changes | Here 201 lines, written new: the same start-up order (log, version check, platform report, ini, messages, co-save) with nothing after it. F4SE's `kGameDataReady` is SKSE's `kDataLoaded`; `kPreSaveGame` is `kSaveGame`. |
| `Config.h/.cpp` | 585 | with changes | Here 54 lines plus `Settings.h` (127, new and unit-tested): the nine settings that mean something today. The other settings come with their features. |
| `RuntimeSupport.h` | | new | Which game versions the plugin loads into. |
| `CrashWatch.*`, `CrashReport.h` | 993 | with changes, later | Writes a readable crash report beside the log. Mostly Windows, little game; bring over before phase 2, when crashes start to be the plugin's. Skyrim has well-known crash loggers that may make part of it unnecessary. |
| `HangWatch.*`, `HangDetector.h`, `NestedPump.h` | 1,105 | with changes, later | Notices a frozen game. Needs the frame hook. |
| `Background.h`, `MainThread.h` | 78 | with changes, part carried | `Background.h` (bRunInBackground: the game's bAlwaysActive on in a session) is carried from commit a7bddd24b with its tests, and also forced on while the developer channel is on, so the rig never stalls unfocused. Running work off and on the main thread: the idea of `MainThread.h` (a network thread hands work to the main thread and waits for the answer) is in `src/MainQueue.h` and `src/DevChannel.cpp`, written new after F4Multiplayer commit 1d29178b1. The rest comes with the frame hook. |
| `Text.*`, `TextEnglish.h` | 886 | with changes, later | Every line shown to a player, for translation. Comes with the first on-screen message. |
| `StorySetting.h` | 67 | redo | A Fallout 4 quest setting. |
| `DevChannel.*`, `DevCommands.*` | 5,706 | with changes, part carried | The rig's way to ask a running game questions and give it orders. The channel is carried from F4Multiplayer commit 1d29178b1: loopback only, a token file beside the log, one client at a time, commands run on the main thread; port 7791, off unless the ini turns it on. Its line handling is in `src/DevLine.h` (unit-tested). Three commands are new, for the first test: `status`, `steam` (host, join a lobby by id, leave) and `probe` (pings over Steam's relay, `src/net/RelayProbe.h` and `src/steam/SteamProbe`). Fallout 4's other commands are about its own systems and are not carried. |

## `src/net/`

| File | Lines | Status | Notes |
| --- | --- | --- | --- |
| `ClientConnection.h` | 118 | as is | One connection to a server over any transport. |
| `NetClient.h/.cpp` | 102 | as is | |
| `Identity.h/.cpp` | 72 | as is | The random number that lets a returning player keep their id. |
| `NameChoice.h` | 71 | as is | Which name a player shows. |
| `Session.h/.cpp`, `SessionSteps.h` | 2,269 | written new, part | The plugin's session: host or join, each frame's send and receive, what happens on a drop. `src/net/Session` is a small new one on the same shape (host: the server in the game, Steam for friends, UDP on this machine for the host's own connection; friend: a Steam connection to the lobby's owner; states 20 a second, looks when they change, heartbeats). Reconnects, the menu and what F4Multiplayer's `SessionSteps.h` orders come later. The lobby on Steam's launch command line is read by `src/net/LaunchArgs.h` (new, unit-tested). |
| `ModDiff.h` | 439 | with changes, later | Tells a refused player which mods differ, in words. Game-free; bring with the menu. |
| `PlayerHitRules.h`, `Refusals.h` | 119 | with changes, phase 3 | Rules for hits between players and refused actions. |

## `src/steam/`

| File | Lines | Status | Notes |
| --- | --- | --- | --- |
| `SteamApi.h/.cpp` | 408 | as is | The hand-declared Steam functions and how they are found in the game's own Steam library. **The names it asks for are the ones Fallout 4's library has. Whether Skyrim's has them is the first thing to test** ([GETTING-STARTED.md](GETTING-STARTED.md)); `tools\steamcheck.ps1` reads it off the file. |
| `Steam.h/.cpp` | 812 | as is | Lobbies, invites, "Join Game", friends who host. The app id is Skyrim Special Edition's. |
| `SteamTransport.h/.cpp` | 390 | as is | The transport over Steam's relay messaging. If Skyrim's library lacks that interface, this file is what gets rewritten. |

## `src/game/`: everything that touches the game

137,400 lines in 361 files, all written against Fallout 4. None of it is copied. What carries is the list of problems and the order they were solved in; many headers there are game-free rule books with their own unit tests, and those are the first place to look when the same problem comes up here.

The four parts the plugin's start-up already calls are stubs with an interface:

### The frame

`src/game/GameThread`. F4Multiplayer registers a permanent task with F4SE and gets called every frame on the main thread. SKSE's task interface runs a task once, so Skyrim needs a hook: a call inside the game's main update loop replaced by a function that calls the original and then the plugin. Many open SKSE plugins hook the same place; the Address Library ids and the offset are to be looked up for the current game version and checked in a disassembler, not copied on trust. CommonLibSSE-NG's trampoline (`SKSE::GetTrampoline`, `write_call`) does the patching. This is the first engine work of phase 1, and everything else waits for it. It also decides whether the library's "patch safety" option gets turned on. Until then `GameThread` runs a stand-in: a thread that asks SKSE's task queue every 16 ms to run the tick once ([DESIGN.md](DESIGN.md), "The frame"). It is enough for the first test and logs whether the tick ran on the main thread.

### The load order

`src/game/LoadOrder`. F4Multiplayer reads the data handler's compiled file lists (full and light plugins). Skyrim's `TESDataHandler` has the same two lists (`compiledFileCollection.files` and `.smallFiles`) and they are filled when SKSE sends `kDataLoaded`. Written: it reads them through CommonLibSSE-NG's `GetLoadedMods` and `GetLoadedLightMods`.

### The local player

`src/game/LocalPlayer`. Where the player is and what they are doing, once a frame: `PlayerCharacter::GetSingleton()`, its position and angle, parent cell and worldspace, and the actor state (sneaking, sprinting, weapon drawn, swimming, mounted). Written: the speed and direction are the player's graph's own (`Speed`, `Direction`), with the ground covered when the graph has none; its looks are race, sex, weight and the armor worn.

### The other players

`src/game/RemotePlayers` (F4Multiplayer: `RemotePlayers`, `Puppets`, the `StandIn*` headers, `StateBuffer`, the gait and prediction headers, `Equipment`, `Face*`, `Body*`, `Looks*`: about 16,000 lines). The stand-in actor for each other player: made, dressed, moved, animated, and kept out of the AI's and the guards' way. The largest single piece of phase 2 and the one with the most engine work. To find in Skyrim: creating an actor from a copy of the player's base (`TESNPC`: race, sex, head parts, tints, weight), placing it (`PlaceObjectAtMe`), moving it without the AI fighting back (the character controller, or packages), driving its animation graph (`NotifyAnimationGraph`, graph variables), equipping it (`ActorEquipManager`), and making factions and crime ignore it. `StateBuffer.h` (smoothing between received states) and the prediction headers are game-free and carry: `src/game/Prediction.h` is carried as is and `src/game/StateBuffer.h` cut down (the sender's clock, a fixed delay, no blend across a door, a short carry-on past the newest state unless it says they stopped), both from F4Multiplayer commit a7bddd24b, with their tests.

The first stand-in is written ([DESIGN.md](DESIGN.md), "Players"): the idea of F4Multiplayer's `Puppets` (the Character class's actor update hooked, a stand-in placed and run with the no-AI update, its graph driven), written new for Skyrim. Its base is a character-creation preset of the friend's race and sex, not a copy of their base; `src/game/Gait.h` (new, unit-tested) says which of the graph's events start and stop its movement. The save guard ([DESIGN.md](DESIGN.md), "Nothing of the session in a save") is new: F4Multiplayer's `SaveGuard` drops records from the engine's change map, which CommonLibSSE-NG does not lay out.

The rig's virtual gamepad, `src/game/VirtualPad.cpp`, is carried from F4Multiplayer commit a7bddd24b without its muting of the desktop's keyboard and mouse; `src/game/PadPlan.h` is carried as is but for one variable renamed (`near`, a Windows macro, is `slowAt`). `src/game/Console` runs console commands for the rig's `console` command.

### The rest, by system

| System in F4Multiplayer | Lines (about) | Status | The Skyrim equivalent to find |
| --- | --- | --- | --- |
| NPCs: who runs them, spawns, encounters, health, deaths (`NpcSync`, `Spawns`, `SpawnBook`, `Copy*`, `Npc*`, `Encounter*`, `HandOff*`, `*Hold`, `DeathReports`) | 17,000 | redo, phase 3 | Levelled actor spawns (`TESLevCharacter`), `Actor` AI packages and combat state (`CombatController`), death (`TESDeathEvent`), health as an actor value. Skyrim's encounter zones and levelled lists scale with the player's level: whose level counts is a design question. |
| Combat: weapon fire, projectiles, throwables, hits, staggers (`WeaponFire`, `InertShot*`, `MuzzleFlash*`, `Shot*`, `AttackKind`, `HandThrowable`, `GroupHits`, `Rolls`, `RollHook`) | 9,000 | redo, phase 3 | Fallout 4's fights are guns; Skyrim's are melee swings (`HitData`, `TESHitEvent`, weapon-swing animation events), bows (`ArrowProjectile`), spells and shouts (`MagicCaster`, `ActiveEffect`, `TESSpellCastEvent`), blocking and staggers. Mostly new. |
| Downed players and death (`Downed`, `DownRules`) | 2,600 | with changes, phase 3 | The rules are game-free and carry. Skyrim has a bleed-out state for essential actors that may serve. |
| VATS, power armor, Pip-Boy, terminals (`Vats*`, `RealTime*`, `ArmorFrames`, `Frame*` loans, `StandInPipboy`, `Terminal*`) | 7,000 | not needed | No such things in Skyrim. Its own special cases take their place: kill moves, mounts, werewolf and vampire-lord forms, slow-time shouts. |
| The shared world: objects, doors, containers, loot, drops, moved things (`WorldSync`, `ObjectShare`, `MovedObjects`, `SharedMove`, `Drops`, `DropBook`, `Loot*`, `ContainerLedger`, `Storage*`, `DoorShare`, `ScriptDoors`, `Enable*`, `DisabledParents`, `Gifts`, `GiftBook`, `Rack*`) | 20,000 | redo, phase 4 | `TESContainerChangedEvent`, `TESActivateEvent`, `TESOpenCloseEvent`, lock data on references (`REFR_LOCK`), enable state and enable parents, harvested flags on flora, `TESObjectREFR` extra data. The books (ledgers) are game-free and largely carry. |
| Destructibles and traps (`Destructibles`, `DestructionRules`, `Traps`, `TrapRules`) | 2,300 | redo, phase 4 | Skyrim's traps are activators and scripted hazards; destructible objects are few. |
| Time and weather (`WorldClock`, `GameClock`, `ClockMath`) | 300 | with changes, phase 4 | `Calendar`, `Sky`. |
| Quests (`QuestSync`, `StageRules`, `StageLog`, `QuestFilter`, `QuestStageLayout`, `QuestStart*`, `QuestActors`, `QuestRewards`, `RewardLines`) | 17,500 | redo, phase 5 | `TESQuest` stages and objectives, `TESQuestStageEvent`, aliases (`BGSRefAlias`). `StageRules.h` alone is 10,200 lines of knowledge about Fallout 4's quests, one quest at a time; Skyrim needs its own, built from an audit of its quests. |
| Radiant quests and placed scenes (`RadiantSync`, `RadiantRules`, `PlacedScenes`, `SceneHold`) | 6,000 | redo, phase 5 | The story manager (`BGSStoryManager`) and scenes (`BGSScene`). Skyrim leans on radiant quests more than Fallout 4 does. |
| Dialogue and voice lines (`Conversations`, `Dialogue`, `StrandedDialogue`, `Line*`, `SubtitleTime`) | 1,900 | redo, phase 5 | Skyrim's dialogue menu and topic system (`MenuTopicManager`), which stops time for the speaker only in some cases. |
| Companions (`Companions`, `CompanionRules`) | 850 | redo, later | Followers: a faction and a quest alias rather than Fallout 4's companion system. |
| Settlements and the workshop (`WorkshopSync`, `SharedWorkshop`, `Workshop*`, `ResourceMirror`, `Settler*`, `SettlementAttacks`, `SupplyLines`, `RemoteEdits`) | 19,000 | not needed | Skyrim has no workshop mode. Hearthfire's houses are built through scripted menus; if they are ever shared, it is a quest-and-object problem, not this code. |
| Party, the menu, the HUD, the map (`Party`, `Ui*`, `Menu*`, `Compass`, `FriendMap*`, `MapShare`, `Hud`, `Hotkeys`, `KillFeed`, `PingRules`) | 9,000 | redo, later | Skyrim's menus are Scaleform with ActionScript 2 (Fallout 4's are ActionScript 3), reached through `UI` and `IMenu`. Party rules and ping rules are game-free and carry. |
| Scripted input for the rig (`VirtualPad`, `PadPlan`, `InjectInput`, `InputGate`, `KeyWatch`, `MouseCapture`, `WindowFocus`) | 1,800 | with changes, phase 1 | Mostly Windows input, little game. The rig needs it before phase 2's proofs. `PadPlan.h` (the virtual gamepad's state, its command and the walkto steering; game-free) is carried from commit a7bddd24b with its tests, only its button comments changed for Skyrim's layout. |
| Voice chat (`Voice`, `VoiceGate`, `VoicePath`, `VoiceRamp`) | 570 | with changes, later | Steam voice; game-free apart from where the sound plays. |
| Saves (`SaveGuard`, `SaveStrays`, `SavedCopies`, `SavedScraps`, `SaveLoadHold`, `AutoSave`, `AutoSaveRules`) | 3,300 | redo, phase 4 | Keeping stand-ins and other session-only things out of the save, and holding the session across a load. SKSE's serialization interface is the co-save; `BGSSaveLoadManager` is the game's. The rules carry; the engine side is new. |
| Engine guards and watches (`HavokGuard`, `HavokNan`, `TextureWait`, `FastLoad*`, `Fade*`, `Gpu*`, `Frame*` watch and stats, `Perf`, `MemoryLounger`, `LoungerRescue`) | 6,000 | redo, as needed | Each one is the fix for a Fallout 4 crash or stall found in testing. Skyrim will have its own; none is ported ahead of the problem it answers. |
| Small shared helpers (`RetryBudget`, `SendPacer`, `LogPace`, `RollingLog`, `NetSim`, `DeferredDelete`, `TeleportRules`, `Papyrus`, `EngineClock`) | 1,400 | with changes | Game-free or nearly. Bring each over when first needed. `Papyrus.h` (calling scripts) maps to Skyrim's `BSScript` virtual machine. |
| Achievements (`Achievements`, `AchievementGate`, `AchievementStages`) | 970 | redo, later | Steam achievements for friends in a host's world. |

## `server/` and `bot/`

| File | Lines | Status | Notes |
| --- | --- | --- | --- |
| `server/main.cpp` | 198 | with changes | Here 101 lines: port, player count, password. |
| `bot/main.cpp` | 735 | with changes | Here 233 lines: joins and walks in a circle. F4Multiplayer's bot can also shoot, report quests, go down, talk and more, one switch per system; each returns with its system. |

## `tests/`

262 files and 120,300 lines there; 23 files and about 4,900 lines here. Carried: the harness (`Test.h`, `main.cpp`), the in-memory transport and the server fixture, and the tests of everything carried (`FormIds`, `IniFile`, `LaunchReport`, `LogRotation`, `NameChoice`, `Name`, `Protocol`, `Redact`, `Server`, `ServerEnet`, `TokenBucket`), with the protocol and server tests cut to the small protocol. New: `SettingsTests`.

`LogPrivacyTests` and its `SourceTree.h` are carried with changes: the server's log over an evening of players (its tests of ignored plugins and of the mod list a joiner is sent are left out until those return, and the flood test became one of a stray message), and the guard on the source, whose list of log calls is the plugin's own (`logs::`, `SKSE::log::`, `spdlog::`) and which also knows Skyrim's ways to a player's name (`Steam::NameOf`, `NameChoice::Choose`, the player character's `GetName`). With it the server, like F4Multiplayer's, runs every log line through `Redact::Scrub`.

Not carried yet:

- The simulations (`tests/sim/`): game-free stand-ins for a game, for testing whole exchanges without one. They return when there is a session to simulate.
- The tests of each rule book in `src/game`: each comes with its book.
- The coverage report on pull requests (`tools/coverage.sh` and its workflow steps).

## The menu (`ui/`)

F4Multiplayer's menu is Flash, built from ActionScript 3 source by an open-source compiler in its workflows; no compiled file is committed. Skyrim's menus use ActionScript 2, which needs a different open-source compiler. Nothing is carried; the rule (source only, built by the workflows, vetted like code) is. `.github/scripts/vet.py` still knows the ActionScript 3 toolchain and does nothing with it here.

## Tools

| In F4Multiplayer | Status | Notes |
| --- | --- | --- |
| `tools/package.ps1`, `tools/promote.ps1`, `tools/release-lib.ps1` | carried with changes | Names, the SKSE folder layout, the submodule's name. Not yet run: no release has been packaged. |
| `tools/git-hooks/` | carried as is | The pre-commit and pre-push privacy checks. |
| `tools/steamcheck.ps1` | new | Reads which Steam functions a game's Steam library exports. |
| `tools/addrcheck.ps1` | redo, phase 1 | Checks that every Address Library id the plugin uses exists for a game version. Needed from the first hook. |
| `tools/devctl.ps1` | with changes, phase 1 | Talks to a running game's developer channel. Not carried as a file: the first test's scripts talk to the channel themselves (`dc_one` in `tools/rig/env.sh`, after F4Multiplayer commit b82f950ce). |
| `tools/deploy.ps1`, `tools/input.ps1`, `tools/restart-game.ps1`, `tools/screenshot.ps1`, `tools/hangcatch.ps1` | with changes, phase 1 | Copy a build to both machines, send input, restart a game. Paths, process and window names change. For the first test, `tools/rig/install.sh` and `tools/rig/start.sh` (new) do the copying and the start. |
| `tools/rig/`, `tools/soak.ps1`, `tools/botsoak.ps1`, `tools/stress.ps1`, `tools/testloop.sh`, `tools/menustress.sh` | redo, from phase 1 | The gate's checks and the soak. The framework (take the lock, back up, run, read both logs, restore, verdict) carries; every check is about a Fallout 4 scene and is written again for Skyrim. |
| `tools/rig/env.sh` | with changes | Carried from F4Multiplayer commit 5b535694c (with `guard.sh`, `backup.sh` and `dcfast.sh` of the same commit as models): the same rig, rig folder and lock; Skyrim's paths, process, ports and file names. The first test's scripts beside it (`install`, `firststart`, `start`, `lobbytest`, `fetchlogs`) are new. Not yet run. |
| `tools/questscan.*`, `tools/rewardscan.ps1`, `tools/menumodescan.ps1`, `tools/rollprobes.ps1` | redo, phase 5 | Scans of Fallout 4's data files for the quest audit. Skyrim's plugin format is close; the questions differ. |
| `tools/coverage.sh`, `tools/mutate.sh` | with changes, later | Coverage and mutation testing of the game-free code. |
| `tools/flash/`, `tools/svg-to-as.lua`, `tools/xwin.lua`, `tools/crossovermac.sh` | not carried | The ActionScript 3 toolchain, and building and running on a Mac. |

## Workflows and repository files

| In F4Multiplayer | Status | Notes |
| --- | --- | --- |
| `.github/workflows/build.yml` | with changes | Windows build and tests for the owner's branch pushes and by hand. |
| `.github/workflows/tests.yml` | with changes | Linux build and tests of the game-free targets for every pull request; no coverage comment yet. |
| `.github/workflows/review-label.yml`, `vet-label.yml` | as is | |
| `.github/workflows/vet.yml`, `.github/scripts/vet.py` | with changes | Names and the submodule. |
| `.github/workflows/package.yml` | with changes | |
| `.github/workflows/nexus-upload.yml` | with changes, switched off | Refuses to run until the mod has a Nexus page and its two ids are filled in. |
| Issue forms, the pull request template | with changes | |
| `LICENSE`, `EXCEPTIONS` | as is | |
| `AGENTS.md`, `CLAUDE.md`, `CONTRIBUTING.md`, `docs/RELEASING.md` | with changes | The same rules, for this game and this stage. |
| `docs/DESIGN.md`, `docs/PLAN.md` | new | F4Multiplayer's design document (6,000 lines) is the record of how each Fallout 4 system was synced and why. Read its section before designing the same system here. |
| `dist/` (ini, install page) | with changes | Language files and the Nexus page texts are not carried. |
