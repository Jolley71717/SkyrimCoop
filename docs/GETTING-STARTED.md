# Getting started: from an empty PC to a plugin in the game

This page is for whoever sets up the first game to develop against. It was written before any game was installed: nothing on it has been tried in Skyrim. Where it gives a version or a download, that is what the named page listed on 2026-11-21; check it again on the day.

The order matters. The first section is the one question that decides the project's shape, and it can be answered within the first hour of having the game.

## The first thing to test

**Do Steam lobbies, invites and the relay work from inside Skyrim Special Edition, the way they do from inside Fallout 4?**

The whole pitch (a friend joins from the Steam overlay, nobody opens a port) rests on it. F4Multiplayer does it by calling the Steam library that the game itself loaded (`steam_api64.dll` in the game's folder): it ships no Steam code of its own, and asks that library for lobbies, for the invite dialog and for Steam's relay network. SkyrimCoop's Steam layer (`src/steam/`) is that code carried over. Two things about Skyrim are not known yet:

1. **Whether Skyrim's Steam library has the functions.** A game ships the Steam library of the day it was built. The plugin needs 29 functions by name (`src/steam/SteamApi.cpp`), among them the newer relay messaging (`ISteamNetworkingMessages`) and five interface accessors of specific versions. If the game ships an older library, some of those names will not be there.
2. **Whether Steam lets Skyrim's app id (489830) use them.** Lobbies, rich presence ("Join Game" in the friends list) and the relay are things Steam offers to every game, and Fallout 4 never asked for them either; but "it worked for the other game" is not a test.

### Step 1: read the game's Steam library (five minutes, the game does not have to run)

```
tools\steamcheck.ps1 -Path "<your Steam library>\steamapps\common\Skyrim Special Edition"
```

It reads the export table of the game's `steam_api64.dll` and compares it with the names the plugin asks for. It loads and changes nothing.

- `RESULT: every function the plugin needs is here.` Go on to step 2.
- `RESULT: this Steam library lacks N function(s) the plugin needs.` The script lists which, which versions of the interface accessors the file does have, and whether the older ways to reach an interface are there. That list decides which fallback below applies. Keep the output.

The script was tried once, read only, on an older Steam library file from another game: it reported 17 of 29 present, with every versioned accessor, all of `ISteamNetworkingMessages` and both relay functions missing. That is what an old library looks like, and it is a likely enough result for Skyrim that the fallbacks below are worth reading first.

### Step 2: the plugin's own answer (the game runs once)

Install SKSE64, the Address Library and the plugin (the sections below), start the game through SKSE, reach the main menu and open `Documents\My Games\Skyrim Special Edition\SKSE\SkyrimCoop.log`. One of these lines is there:

- `Steam: the game's own Steam library has everything the mod needs`
- `Steam: cannot be used yet: <what was not found>`

The `Launch:` lines above it say which Steam app id the game was given and whether Steam's overlay is in the game. No overlay means no invite dialog, whatever else works.

### Step 3: two games (this is phase 1 of [PLAN.md](PLAN.md), and needs code that is not written)

The real answer takes two PCs, two Steam accounts that are friends and each own the game, and a plugin that runs every frame (the frame hook, [PORTING.md](PORTING.md), "The frame"). Then, in order, each read from the two logs:

1. The host's game opens a friends-only lobby; the log shows the lobby was made.
2. The friend's Steam friends list shows "Join Game" on the host, and the host's overlay invite reaches the friend.
3. The friend accepts; the friend's log shows the lobby joined and the host's log shows the member.
4. A message sent over Steam's relay from one game arrives in the other, both ways.

All four passing means Steam first, as designed. Any one failing is a finding to write down with both logs before anything else is built on top.

### If it does not work: the fallbacks, in the order to try them

1. **Other versions of the same interfaces.** If the library has `SteamAPI_SteamFriends_v0NN` with another number, the same calls may exist under it. Each changed version needs its functions checked one by one (an argument added between versions breaks a call silently).
2. **Ask the Steam client for a newer interface.** The Steam client on the PC is always current, even when a game's `steam_api64.dll` is old; the old library can hand out an interface by its version name (`SteamInternal_FindOrCreateUserInterface`, or `SteamClient()` and its generic-interface call). The plugin would then call the interface through its function table instead of through named functions. More work, and each table's layout has to be checked, but it does not depend on what the game shipped.
3. **Steam's older peer-to-peer messaging** (`ISteamNetworking`), which every Steam library of Skyrim's age has. It is deprecated but still relayed by Steam. The transport (`src/steam/SteamTransport.cpp`) would be rewritten on it; lobbies and invites stay as they are.
4. **Join by address only.** If Steam cannot be used from inside the game at all, the project still works over direct UDP (`sTransport = enet`): the host forwards a port or the group uses a LAN or a VPN. That is the design's second way in, already carried over and unit-tested, but it gives up the main reason for the project. This would be the moment to stop and decide whether to go on.

Never fix this by shipping a Steam library or the Steamworks SDK with the mod.

## What to buy and install

- **The Elder Scrolls V: Skyrim Special Edition on Steam** (app id 489830). The Anniversary Edition is the same game with the "Anniversary Upgrade" content added; either does. Not the 2011 game, not Skyrim VR, not the GOG or Microsoft Store versions.
- **Twice.** A two-game test needs two Steam accounts that each own the game: one for the host PC, one for the second machine. One copy shared through a Steam family cannot be played by two accounts at the same time. The two accounts must be Steam friends, since lobbies are friends-only.
- English, no mods, default install folder, to begin with. Start the game once from Steam so it writes its ini files and picks its graphics settings; then set it to windowed, at a low resolution on the second machine.
- Turn automatic updates down as far as Steam allows (update only when the game is started) and always start the game through SKSE. A game update needs a new SKSE and a new Address Library file before any plugin loads again.

Note the game's version: right-click `SkyrimSE.exe`, Properties, Details. On 2026-11-21 the newest SKSE build was made for game version 1.7.104, so that is what a fresh Steam install is expected to be. The plugin loads into 1.5.97 and later and warns below 1.6.1170 (`src/RuntimeSupport.h`); the pinned CommonLibSSE-NG knows the game up to 1.7.104.

## SKSE64

From [skse.silverlock.org](https://skse.silverlock.org/), the official page. As listed on 2026-11-21:

| The game | SKSE64 build |
| --- | --- |
| Steam, current ("Anniversary Edition build"), game 1.7.104 | 2.3.1 |
| Steam, 1.5.97 ("Special Edition build") | 2.0.20 |
| GOG | its own build; not supported by this project |

Take the build whose game version matches the game's exactly. From the archive: `skse64_loader.exe` and the `skse64_*.dll` go next to `SkyrimSE.exe`; the archive's `Data` folder goes into the game's `Data` folder. Start the game with `skse64_loader.exe` from then on.

Check: in the game's console (the `~` key), `getskseversion` prints the version.

## Address Library for SKSE Plugins

From [its page on Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files) (a Nexus account is needed to download). "All in one (Anniversary Edition)" for game 1.6 and later, "All in one (Special Edition)" for 1.5.97. It is a set of `versionlib-<game version>.bin` files (or `version-<game version>.bin` for 1.5.97) that go in `Data\SKSE\Plugins\`. Check that the set has a file for the installed game version; right after a game update it may not yet.

Without the matching file SKSE refuses the plugin, and `skse64.log` says so.

## The plugin

Build it ([CONTRIBUTING.md](../CONTRIBUTING.md), "Building"), or take the artifact of a `build.yml` run (`gh run download <run id>`). Then:

| File | Goes to |
| --- | --- |
| `build\windows\x64\releasedbg\SkyrimCoop.dll` | `<game>\Data\SKSE\Plugins\SkyrimCoop.dll` |
| `dist\SkyrimCoop.ini` | `<game>\Data\SKSE\Plugins\SkyrimCoop.ini` |

Keep `SkyrimCoop.pdb` from the same build: a crash report can only be read with it.

## Did it load?

Start the game through SKSE, reach the main menu, and open `Documents\My Games\Skyrim Special Edition\SKSE\SkyrimCoop.log`. A good first log has, in this order:

```
SkyrimCoop 0.0.1 (protocol 1)
Game version 1.7.104
Launch: ...
Config: host no, transport auto, port 7780, up to 4 players, ...
Game data loaded (0 plugins read)
Steam: ...
```

- **No `SkyrimCoop.log`.** SKSE did not load the plugin. `skse64.log` in the same folder lists every plugin it looked at and why one was refused: usually the Address Library file for this game version is missing, or the SKSE build does not match the game.
- **"0 plugins read" and nothing per frame.** Expected today: reading the load order and the frame hook are the first things to write ([PORTING.md](PORTING.md)).
- **The game does not start, or crashes at the main menu.** Take the plugin out and try again, to tell SKSE's own setup from the plugin. A crash with the plugin in is the first bug to file, with `SkyrimCoop.log`, `skse64.log` and the game version.

Loading a save and saving should add `Load:` and `Save:` lines. That is everything phase 0 asks for ([PLAN.md](PLAN.md)).

## Reusing the Fallout 4 test rig

F4Multiplayer is tested on two machines: the maintainer's PC as host and a virtual machine on the same PC as the friend, each signed in to its own Steam account, driven by scripts that send real keyboard and mouse input and read the plugins' logs. Nothing in that depends on which game runs. For Skyrim:

- **Machines.** The same two. Whether the virtual machine draws Skyrim well enough is to be seen on the day. Install the game, SKSE64 and the Address Library on both, the same versions.
- **Accounts.** The virtual machine's Steam account has to own Skyrim too (above).
- **One game at a time.** The rig runs one test of one project at a time, so Skyrim runs take the same lock the Fallout 4 tests take. Skyrim and Fallout 4 are never both running on a machine.
- **Scripts.** The rig's tools live in the sister project (`tools/rig/`, `tools/deploy.ps1`, `tools/input.ps1`, `tools/soak.ps1`) and are written around Fallout 4's paths, window and log names. What carries over is the method: copy the build to both machines, start both games through the script extender, send scripted input, wait for lines in both logs, and fail the run on any line that should not be there. They are to be ported when phase 1 needs two games ([PORTING.md](PORTING.md), "Tools"); until then phase 0 is checked by hand with the steps above.
- **What a rig run must never do** is in [AGENTS.md](../AGENTS.md): no teleporting to make a test pass, no steering by screenshots, back up and restore the ini files, put `main`'s build back afterwards.

## Decisions to confirm

The full list is in [DESIGN.md](DESIGN.md). The ones this page touches:

- The tested line starts at game 1.6.1170; if the fresh install is 1.7.104, raising the line to that is the obvious change.
- 1.5.97 loads with a warning. Whether it is ever tested is open.
- Proton and the Steam Deck: the start-up log reports them, nothing more is planned yet.
