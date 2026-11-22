# Design

How SkyrimCoop is meant to work, and why. This is a skeleton: the decisions that are made, the parts that exist, and for everything else a heading that says what has to be decided there. A section is filled in when its phase of [PLAN.md](PLAN.md) is built and tested, with what the game turned out to do, not before.

Nothing here has been run in a game yet.

## The idea

A small group of friends plays through Skyrim together in one world.

- **One host, whose save is the world.** The host loads their own save and plays. Friends join it. Quests, the state of the world, the time of day: the host's game decides, and the host's save keeps it.
- **Friends follow.** A friend plays their own character, with their own gear and skills, kept in their own save. Their save keeps the character, not the world: what the group did lives in the host's save.
- **Steam first.** The host's game opens a friends-only Steam lobby. A friend joins from the Steam overlay or the friends list. The games talk through Steam's relay network: no server to rent, no port to open, no address to pass around.
- **By address second.** A direct UDP connection to an address and port (a LAN, a forwarded port, a VPN of the group's own choosing), and a dedicated server for those who want one. The same session either way.
- **Tested for hours before it ships.** A release is a release candidate that two scripted games played for a long session without a failure ([RELEASING.md](RELEASING.md)).

It is the design of [F4Multiplayer](https://github.com/Jolley71717/F4Multiplayer), the sister project for Fallout 4, moved to another game by the same engine family. Where that project has an answer that does not depend on the game, this one starts from it.

## Decisions made

| Decision | Why |
| --- | --- |
| The game is Skyrim Special Edition on Steam, the current version (the Anniversary Edition update line). Not the 2011 game, not Skyrim VR, not the GOG or Microsoft Store versions, for now. | One game version to reverse-engineer and test. Steam is how players join, so a version without Steam has no way in. |
| The plugin is an SKSE64 plugin built on CommonLibSSE-NG, finding the game's functions through the Address Library. | The same stack as the sister project's (F4SE, CommonLibF4, Address Library), so the same habits apply. Address Library ids survive most game updates. |
| The host owns the world; friends follow. | One source of truth. Two games that both decide drift apart, and no save can hold two worlds. |
| Steam lobbies, invites and the relay first; join by address second. | The reason for the project: joining should be an invite, not network administration. |
| Steam is reached through the game's own Steam library, by hand-declared functions found at run time. The Steamworks SDK is never in the repository and no Steam library ships with the mod. | The SDK's terms do not fit a GPL repository, and the game already has Steam loaded under its own app id. |
| GPL-3.0-or-later with the modding and linking exceptions, as F4Multiplayer. | The same license lets code move between the two projects, and CommonLibSSE-NG's license asks for it. |
| No code from other multiplayer mods. | Their licenses differ and some allow nothing ([PRIOR-ART.md](PRIOR-ART.md)); whether to build on any of them is the maintainer's decision, not a contributor's. |
| Game-free code is kept apart (`common/`) and unit-tested on every pull request; anything that touches the game is tested on two real games. | The split the sister project relies on: most protocol bugs are found without a game, and no game behaviour is taken on trust. |
| A release candidate first, then a long two-game test of exactly that build, then the release. | What is tested is what ships. |
| Logs name no person and no machine. | A bug report's log is attached in public. |

## What exists today

```
        game A (host)                                   game B (friend)
  +--------------------------+                   +--------------------------+
  | SkyrimCoop.dll           |                   | SkyrimCoop.dll           |
  |  src/main.cpp  start-up  |                   |                          |
  |  src/game/*  stand-ins   |                   |  src/game/*  stand-ins   |
  |  src/net/NetClient ------+--- transport -----+-- src/net/NetClient      |
  |  common/Server (thread)  |   Steam relay or  |                          |
  |  src/steam/*             |   UDP (ENet)      |  src/steam/*             |
  +--------------------------+                   +--------------------------+
```

- **Start-up** (`src/main.cpp`): keeps the last three logs, starts the log, checks the game's version (`src/RuntimeSupport.h`), reports how the game was started (`src/LaunchReport.h`, `src/Platform.cpp`), reads the ini (`src/Settings.h`, `src/Config.cpp`), and listens for SKSE's messages: data loaded, before and after a load, new game, save, delete. Its co-save ("SKCO") holds one record: the stand-ins taken out of the world for that save (below, "Nothing of the session in a save").
- **The protocol** (`common/Protocol.h`): a first, small one. A hello with the mod's protocol version, the player's name, a password and the load order; a welcome or a refusal with its reason; players joining and leaving; each player's state (cell or worldspace, position, heading, speed, a few flags, the sender's clock); each player's looks (race, sex, weight and the armor worn, as form ids), which the server keeps and gives to whoever joins later; a heartbeat. Two channels: reliable, and unreliable-sequenced for state.
- **The server** (`common/Server.cpp`): lets players in (same protocol version, same load order, the right password, room left), gives each an id, and passes each player's state and looks to the others. It runs as a thread inside the host's game, or alone as `SkyrimCoopServer.exe`.
- **Transports** (`common/Transport.h`): the session does not know how bytes travel. `common/EnetTransport` is UDP through ENet; `src/steam/SteamTransport` is Steam's relay messaging; `tests/FakeTransport.h` is memory, for tests.
- **Steam** (`src/steam/`): the lobby, invites, "Join Game", friends who are hosting, carried from the sister project. Not yet shown to work under Skyrim ([GETTING-STARTED.md](GETTING-STARTED.md), "The first thing to test").
- **The session in the game** (`src/net/Session`): hosting runs the server in the game, reached by friends through Steam and by the host's own connection over UDP on this machine only; a friend joins from Steam itself (an invite, "Join Game", a `steam://joinlobby` address, or the lobby on the command line Steam starts the game with, `src/net/LaunchArgs.h`). Once in, each game sends its player's state 20 times a second and its looks when they change.
- **The game side** (`src/game/`): the load order (`LoadOrder`), the local player (`LocalPlayer`), the other players' stand-ins (`RemotePlayers`, with `StateBuffer` and `Gait`), the frame (`GameThread`, still the stand-in tick) and, for the rig, a virtual gamepad (`VirtualPad`) and console commands (`Console`). [PORTING.md](PORTING.md) has a section for each.
- **Privacy** (`common/Redact.h`): players are `player N` in every log line; addresses are masked; a last scrub runs over every line.

## Sessions

Carried from the sister project and unit-tested:

- A friend is let in only with the same protocol version, the same load order and the host's password. Each refusal has its own reason, shown to the player.
- The load order is compared by plugin names in order, full and light plugins apart. The first player in the session sets it; on a hosted game that is the host.
- Each game has a random identity kept on its PC; a player who drops and returns gets the same player id. It is not a Steam id and says nothing about the person.
- Up to 8 players by protocol, 4 by default.

To design when phase 1 is built: what a friend sees while joining, joining in the middle of a fight or a scene, the host leaving, a friend's game crashing, bans.

## Form ids across load orders

Every object in the game is named by a form id whose first byte (or, for light plugins, bits) depends on the load order. Two games with the same plugins in the same order agree; the session refuses any other pair, so ids go over the wire as they are. `common/FormIds.h` holds the arithmetic (full and light plugins, ids created at run time).

To decide later: whether to allow load orders that differ in ways that do not matter (a texture pack, a UI mod), which needs translating ids by plugin name. The sister project has that (its FormTranslate); it is not carried yet.

## The frame

Everything that reads or changes the game happens on the game's main thread, once per frame; network threads only queue. The stand-ins' movement does not wait for the tick: they are placed inside their own actor update, which the game runs every frame. SKSE offers no "every frame" call, so the plugin needs a hook in the game's main update. Not written yet ([PORTING.md](PORTING.md), "The frame"). Until it is, a stand-in tick serves the first test: a small thread asks SKSE's task queue every 16 ms to run the plugin's tick once on the main thread, one request at a time. It is not tied to the game's frames and may not run in every game state; it is replaced, not built on.

## Players

The first stand-in (`src/game/RemotePlayers`), kept simple on purpose:

- **What it is**: a plain NPC of the friend's race and sex, wearing the armor they wear and named as they are. Its base is one of the game's own character-creation presets of that race and sex (the lowest form id, so every game picks the same), an NPC of the load order: never a form made at run time. Faces, hair and body come later.
- **How it is moved**: the game never runs its AI. Every Character's actor update goes through one hook (the class's vtable, `Actor::Update`); for a stand-in it places the actor where its friend was 120 ms ago (blended between the two states either side, `StateBuffer`), turns it, runs the engine's no-AI update and tells its animation graph what its friend's legs do (`Gait`: the graph's Speed and Direction, and its move, sprint, sneak and jump events). Any other actor gets the game's own update.
- **When it is there**: while both players are in the same interior cell or the same worldspace and within about two cells of each other.
- Open, for later: guards, factions and crime; mounts, shouts and transformed forms; two hands; how far apart the group may be.

## Nothing of the session in a save

A friend keeps their own save, and nothing of the session goes into it. The sister project learned that a stand-in written into a save comes back broken, and crashes the game when its base was made at run time. Here:

- A stand-in's base is an NPC of the load order, so even one that a save did keep would load as a plain actor, not a crash.
- SKSE tells the plugin a save is about to be written (`kSaveGame`), before the game writes it. Every stand-in is then disabled, so none stands in the world the save is made of; each is placed again a moment later. It is deleted a second later, not at once: a stand-in disabled and deleted in the same frame crashed the game (the disable's own task, run later, found the actor already out of its cell). Every way a stand-in leaves (its player leaves, the session ends, it goes out of range, its looks change, a save) takes the same two steps (`src/game/Retire.h`), so a save may still hold a disabled stand-in; the next point removes it.
- The co-save lists the stand-ins taken out. After a load, any of them the save brought back is removed, and so is any other placed actor of a preset base (a sweep, for a save written before this guard ran).
- The rig checks it: a friend saves mid-session, loads that save alone, and the plugin's `strays` count is 0. The load's log line says whether anything had to be removed, and by which of the two.

## Combat

Not designed yet (phase 3). The starting rule is the sister project's: the host's game runs every NPC and decides every hit's result; a friend's game reports its hits and shows what the host decided. To settle: melee timing over a connection, blocking and staggers, bows and spells in flight, kill moves, a player's death, friendly fire, levelled enemies when players differ in level.

## The world

Not designed yet (phase 4). The host's world is the world: containers, items lying about, doors and locks, bodies, harvested and mined things, time and weather. To settle: what a friend who joins late is told, respawning cells, what a friend's save keeps, houses and Hearthfire.

## Quests

Not designed yet (phase 5). The aim is that every quest is shared: the stage advances for everyone whoever did the step, and the host's save keeps it. To settle: dialogue (who talks, what the others see), scenes written for one player, quest items, rewards, radiant quests, and the quests that cannot be shared as they are.

## The menu

Not designed yet. Phase 1 uses the ini and a key or console commands. A menu in the game (host, invite, join a friend, who is here) comes later. Skyrim's menus are Flash files in ActionScript 2, built from source like everything else; no compiled menu file is committed.

## Decisions to confirm

Choices made to keep moving. Each is the obvious option, cheap to change now, and the maintainer's to confirm or overturn.

| Choice made | The alternative |
| --- | --- |
| The name is SkyrimCoop, for the repository, the DLL and the folders. Other GitHub repositories carry the same or a similar name (unrelated projects); no Nexus mod with exactly this name was found. | ShieldSiblings, SkyrimSteamCoop or SkyrimParty; none had a GitHub repository when checked. |
| CommonLibSSE-NG (`alandtse/CommonLibSSE-NG`), pinned to a commit of its `ng` branch, built from source with Skyrim VR support off. | Another maintained fork of CommonLibSSE (for example `libxse/commonlibsse`), if it follows new game versions sooner. |
| The plugin is tested on game 1.7.104 (a fresh Steam install on 2026-11-21), loads with a warning into 1.6.317 and later, and refuses anything older, 1.5.97 included. | Keep 1.5.97 loadable (its own SKSE build and Address Library file, and a second test line). |
| CommonLibSSE-NG's "patch safety" option is off (it would pull in one more dependency to guard writes to game memory), since the plugin patches nothing yet. | Turn it on with the first hook, after reading that dependency's license. |
| The protocol and the server are a small first version (join, leave, player state, heartbeat), not the sister project's whole protocol. | Carry more of it at once. Most of the rest describes Fallout 4's systems. |
| The first player in the session sets the load order. | Only ever the host. |
| Up to 4352 plugin names in a hello (the longest load order, 254 full plugins and 4096 light ones, with one to spare) in at most 480 KB, so a hello is one Steam message (512 KB at most). That holds names of up to 110 bytes on average; a longer list is cut at a whole name, and `Protocol::PluginsFit` lets a game tell its player before it joins. | Send a longer list in parts, if real load orders ever need it. |
| Up to 8 players by protocol, 4 by default; port 7780; ini and log named SkyrimCoop. | Other numbers. |
| On GitHub, the Windows build runs only for the owner's branch pushes and by hand; pull requests get the Linux unit tests, with a line coverage report (the run's summary, and a comment on pull requests from this repository's branches). | Build the plugin for every pull request once the repository is public and minutes are free. |
| The Nexus upload workflow is present but switched off: it refuses to run until the mod has a Nexus page and its ids are filled in. | Remove it until then. |
| The vetting workflow still knows how to build Flash menu sources, as in the sister project, though this project has none yet. | Trim it, and add it back with the first menu. |
| The git hooks read `~/.skyrimcoop-private-patterns`, and fall back to the sister project's file. | A file of its own only. |
| The ban list, load-order translation, the mod comparison shown to a refused player, and the log-privacy test over all sources are not carried yet ([PORTING.md](PORTING.md)). | Carry them now. |
| Whether to ever build on another project's GPL code. | [PRIOR-ART.md](PRIOR-ART.md) sets out what that would and would not allow. Nothing has been taken. |
| Proton and the Steam Deck are reported in the log and otherwise untested. | Test them from phase 1. |
