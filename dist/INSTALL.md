# SkyrimCoop: install

**Nothing is playable yet.** This build is the project's first step: an SKSE plugin that loads into the game, checks the game's version, reads its settings and writes a log. It does not connect two games. [docs/PLAN.md](https://github.com/Jolley71717/SkyrimCoop/blob/main/docs/PLAN.md) says what comes next, and this page grows with it.

No build of this mod has been run in a game yet: the steps below are how SKSE plugins are installed, written before the first game test.

## What you need

- **The Elder Scrolls V: Skyrim Special Edition on Steam** (the Anniversary Edition is the same game with more content, and works the same). Not the 2011 game ("Legendary Edition"), not Skyrim VR, not the GOG or Microsoft Store versions.
- Windows.
- **SKSE64**, the build made for your game version, from [skse.silverlock.org](https://skse.silverlock.org/). The game's version is on its main menu's system page and in the properties of `SkyrimSE.exe`. Copy `skse64_loader.exe` and the `skse64_*.dll` next to `SkyrimSE.exe`, and the archive's `Data` folder into the game's `Data` folder.
- **Address Library for SKSE Plugins**, the file made for your game version, from [its page on Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files): "All in one (Anniversary Edition)" for game versions 1.6 and later, "All in one (Special Edition)" for 1.5.97.
- This mod: install the zip with a mod manager, or extract it into `Skyrim Special Edition\Data`.

The plugin loads into game version 1.5.97 and later. Testing will start at 1.6.1170; on anything older the log says so.

Always start the game with SKSE (`skse64_loader.exe`, or your mod manager's SKSE launcher).

## What the zip holds

| In the zip | What it is |
| --- | --- |
| `SKSE\Plugins\SkyrimCoop.dll` | the plugin |
| `SKSE\Plugins\SkyrimCoop.ini` | its settings (each one is explained in the file) |
| `SkyrimCoop\INSTALL.md` | this page |
| `SkyrimCoop\LICENSE` | the GPL-3.0 license |
| `SkyrimCoop\EXCEPTIONS` | the modding and linking exceptions that go with it |
| `SkyrimCoop\SkyrimCoopServer.exe` | the dedicated server (only in the zip from GitHub; a game does not need it) |

## Did it load?

Start the game through SKSE and get to the main menu, then open

`Documents\My Games\Skyrim Special Edition\SKSE\SkyrimCoop.log`

It should begin like this:

```
SkyrimCoop 0.0.1 (protocol 1)
Game version 1.6.1170
...
Config: host no, transport auto, port 7780, up to 4 players, password none, address none, log level info
...
Game data loaded (... plugins read)
Steam: the game's own Steam library has everything the mod needs
```

- No `SkyrimCoop.log` at all: SKSE did not load the plugin. `skse64.log` in the same folder names every plugin it tried and why one was refused (a missing Address Library file is the usual reason).
- `Steam: cannot be used yet: ...`: the line says what the mod did not find in the game's Steam library. That line is the most useful thing to send with a report right now.
- The logs of the last three launches are kept next to it (`SkyrimCoop.1.log` is the launch before this one).

## Settings

Edit `Data\SKSE\Plugins\SkyrimCoop.ini`. The file is read when the game starts: change it, then restart the game.

## Reporting a problem

Use the bug form at <https://github.com/Jolley71717/SkyrimCoop/issues/new/choose> and attach the log. The log is written to leave out player names, Steam ids and network addresses; check anything else you attach.

## License

GPL-3.0-or-later with the modding and linking exceptions in the repository's `EXCEPTIONS` file. The source is at <https://github.com/Jolley71717/SkyrimCoop>.
