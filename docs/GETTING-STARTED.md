# Getting started: from an empty PC to the first test in a game

This page is for whoever sets up the games to develop against. Nothing on it has been tried in Skyrim yet. Versions and downloads are what the named pages listed on 2026-11-21; check them again on the day.

## The first test, and what it proves

**Do Steam lobbies and the relay work from inside Skyrim Special Edition, the way they do from inside Fallout 4?** The whole pitch (a friend joins through Steam, nobody opens a port) rests on it. The plugin calls the Steam library the game itself loaded (`steam_api64.dll` in the game's folder) and ships no Steam code of its own.

The first test runs on the rig (the host PC and the VM, each signed in to its own Steam account that owns the game), with both games at the main menu and nobody at the keyboard. `tools/rig/lobbytest.sh` drives both games through the plugin's developer channel and ends with one PASS or FAIL line per point:

| Point | What it shows | Read from |
| --- | --- | --- |
| 0 | Each game is 1.7.104, the Address Library file is there and resolved an address, and the plugin's tick runs on the game's main thread | `Game version`, `Address Library:`, `Game thread: the first tick ran` |
| 1 | Steam is usable from the plugin: the library has every function, the user is logged on, the app id is 489830 | `Steam: ready (...); app 489830, logged on yes, relay ...` |
| 2 | The host's game opens a friends-only lobby, and how long it took | `First test: lobby open (lobby L) N ms after it was asked for` |
| 3 | The VM's game joins that lobby by its id, and both see two members | `First test: joined lobby L (2 members) ...`, `First test: a player joined our lobby ...` |
| 4 | Messages cross Steam's relay both ways (the VM to the host first, then back), each answered, with round trips | `First test: relay report: sent=5 answered=5 ... rttMs min= median= max=` |

All five passing means "Steam first" holds and phase 1 ([PLAN.md](PLAN.md)) can be built on it. Any one failing is the finding: write it down with both machines' logs before anything else is built.

What it does not prove: the friends list's "Join Game" and the overlay invite (they need a person to click; `--join url` gets close by handing the VM's Steam client the same `steam://joinlobby` address a "Join Game" click uses), the frame hook (the tick is a stand-in, below), and anything about players, movement or the world.

### Already known without the game

`tools\steamcheck.ps1` was run on the host's installed `steam_api64.dll` (2026-11-21): every function the plugin needs is there, and it is the same file Fallout 4 ships. So point 1 should pass; if it does not, the problem is Steam's side (the app id, the account, the client), not a missing function.

### The tick is a stand-in

SKSE has no "every frame" call. Until the frame hook is written ([PORTING.md](PORTING.md), "The frame"), a small thread asks SKSE's task queue every 16 ms to run the plugin's tick once on the game's main thread. That is enough to run Steam's callbacks and the test, but it is not the real thing: whether SKSE's queue runs at the main menu at all is one of the things point 0 finds out (the log line says which thread the tick ran on).

## What to buy and download, in this order

1. **The Elder Scrolls V: Skyrim Special Edition on Steam** (app id 489830), on each machine, with each machine's own account (one copy shared through a Steam family can't run on two accounts at once). The two accounts are Steam friends: lobbies are friends-only. Anniversary Edition is the same game; not the 2011 game, not VR, not GOG or the Microsoft Store. English, no mods, default settings.
2. In Steam, for Skyrim on both machines: Properties, Updates, "Only update this game when I launch it". A game update needs a new SKSE and a new Address Library file before any plugin loads again.
3. **SKSE64 2.3.1** for game 1.7.104, from [its Nexus page](https://www.nexusmods.com/skyrimspecialedition/mods/30379) (the same build as [skse.silverlock.org](https://skse.silverlock.org/)). The archive's `skse64_2_03_01` folder holds `skse64_loader.exe`, `skse64_1_7_104.dll` and `Data\Scripts`.
4. **Address Library for SKSE Plugins**, "All in One (1.7.104.0)", from [its Nexus page](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files). Only `SKSE\Plugins\versionlib-1-7-104-0.bin` is used.
5. **Crash Logger** 1.27.0 (`CrashLogger_1.27.0.7z`, MIT) from its GitHub releases. Not needed for the test; it writes `crash-*.log` next to SKSE's logs if the game crashes, which is the only useful report of a crash.
6. **The plugin**: the artifact of a `build.yml` run (`gh run download <run id>`), or a local build ([CONTRIBUTING.md](../CONTRIBUTING.md)). The `.pdb` of the same build is kept with it.

The three downloads go into the rig's folder, one folder each: `skyrim-downloads/skse64/`, `skyrim-downloads/addresslibrary/`, `skyrim-downloads/crashlogger/`. `tools/rig/install.sh` checks their hashes against the ones it was written for (the files of 2026-11-21) and stops on any other file; a newer download is a new hash in the script, checked first.

## The first run, step by step

Everything in `tools/rig/` is written but has never been run. Each script says at its top what it does, and what it assumes. They run from Git Bash on the host, with the rig held (`rigq.sh take <name>`, then `export SKYRIMCOOP_RIG_OWNER=<name>`): Skyrim and Fallout 4 never run at the same time on the rig.

**The maintainer, once, by hand:**

1. Buy the game on both accounts, install it on both machines, set updates as above, and put the three downloads in the rig's folder.
2. Start Steam on both machines, signed in.
3. Add `HOST_GAME=` and `VM_GAME=` to the rig's `rig.conf` if the games are not in the folders `tools/rig/env.sh` expects.

**Then the scripts, in order (an agent can run them):**

| Step | Script | What it does |
| --- | --- | --- |
| 1 | `install.sh` | Puts SKSE, the Address Library file, Crash Logger and the plugin (with the developer channel on) into both game folders; backs up anything it replaces; keeps a record. `install.sh undo` takes out exactly what it put in. |
| 2 | `firststart.sh` | Writes the ini files the launcher would on a first start (windowed, 1280x720, no intro video), and checks Steam's update setting and the C++ runtime. `firststart.sh check` changes nothing; `undo` puts them back. |
| 3 | `start.sh` | Starts both games through `skse64_loader.exe` and waits for the plugin's tick on both; then reaches the VM's developer channel through an ssh tunnel. |
| 4 | `lobbytest.sh` | The test above. Writes `run.log`, `result.txt` and both machines' logs to `checks/skyrim-lobby-<time>/`. `lobbytest.sh --join url` is the second run, joining through Steam's own path. |
| 5 | `start.sh stop` | Closes both games. |
| 6 | `fetchlogs.sh` | Copies both machines' logs again, any time. SKSE's and Crash Logger's logs hold the Windows user's paths: never attach or commit them as they are. |

If a game does not reach the plugin's tick in step 3, read `skse64.log` and `SkyrimCoop.log` (the script shows their last lines), then try the same game once with the plugin taken out (`install.sh undo`, then SKSE alone) to tell SKSE's own setup from the plugin.

## Did it load?

`Documents\My Games\Skyrim Special Edition\SKSE\SkyrimCoop.log`. A good first log has, in this order:

```
SkyrimCoop 0.0.1 (protocol 1)
Game version 1.7.104.0 (the tested one)
Address Library: versionlib-1-7-104-0.bin (2263132 bytes) resolved an address (...)
Launch: ...
Config: host no, ..., dev channel on (port 7791)
Dev channel: listening on 127.0.0.1:7791
Game data loaded (0 plugins read)
Game thread: the first tick ran N ms after the pump started (...); on the game's main thread: yes
Steam: ready (~...); app 489830, logged on yes, relay ...
```

- **No `SkyrimCoop.log`.** SKSE did not load the plugin. `skse64.log` lists every plugin it looked at and why it refused one.
- **`Address Library: ... is not there`.** The file for this game version is not in `Data\SKSE\Plugins`. The plugin says so and stays out, instead of the library's own error box.
- **"0 plugins read".** Expected: reading the load order comes later ([PORTING.md](PORTING.md)).
- **No `Game thread:` line.** SKSE's task queue never ran the tick: the stand-in does not work at the main menu, and the frame hook moves up to first.
- **A crash.** `crash-*.log` from Crash Logger, with `SkyrimCoop.log`, `skse64.log` and the game version, is the first bug to file.

## If Steam does not work from inside Skyrim: the fallbacks, in order

1. **Other versions of the same interfaces**, if the library has them under another number (each one's functions checked one by one).
2. **Ask the Steam client for a newer interface** by its version name, and call it through its function table.
3. **Steam's older peer-to-peer messaging** (`ISteamNetworking`): deprecated, still relayed. Lobbies and invites stay; the transport is rewritten on it.
4. **Join by address only** (`sTransport = enet`): works, but gives up the main reason for the project. The moment to stop and decide.

Never fix this by shipping a Steam library or the Steamworks SDK with the mod.

## The rig, and what it must never do

The same two machines and the same lock as F4Multiplayer's tests ([AGENTS.md](../AGENTS.md)): one game at a time, back up and restore the ini files, put `main`'s build back afterwards, no teleporting, no steering by screenshots. The Skyrim scripts are their own (`tools/rig/`), ported from the sister project's ([PORTING.md](PORTING.md), "Tools"), and share only the rig's folder, its `rig.conf`, the lock and the VM's desktop helper `in-vm-run.ps1`.

## Decisions to confirm

The full list is in [DESIGN.md](DESIGN.md). The ones this page touches:

- The tested version is now 1.7.104. Older Anniversary Edition builds (1.6.317 and later) load with a warning; 1.5.97 is refused.
- Proton and the Steam Deck: the start-up log reports them, nothing more is planned yet.
