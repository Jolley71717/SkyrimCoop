# SkyrimCoop: install

**Nothing is playable yet.** This build is the project's first step: an SKSE plugin that loads into the game, checks the game's version, reads its settings and writes a log. It does not connect two games. [docs/PLAN.md](https://github.com/Jolley71717/SkyrimCoop/blob/main/docs/PLAN.md) says what comes next, and this page grows with it.

No build of this mod has been run in a game yet: the steps below are how SKSE plugins are installed, written before the first game test.

## What you need

- **The Elder Scrolls V: Skyrim Special Edition on Steam** (the Anniversary Edition is the same game with more content, and works the same). Not the 2011 game ("Legendary Edition"), not Skyrim VR, not the GOG or Microsoft Store versions.
- Windows.
- **SKSE64**, the build made for your game version (2.3.1 for game 1.7.104), from [skse.silverlock.org](https://skse.silverlock.org/) or [its Nexus page](https://www.nexusmods.com/skyrimspecialedition/mods/30379). The game's version is in the properties of `SkyrimSE.exe`. Copy `skse64_loader.exe` and the `skse64_*.dll` next to `SkyrimSE.exe`, and the archive's `Data` folder into the game's `Data` folder.
- **Address Library for SKSE Plugins**, the file made for your game version, from [its page on Nexus Mods](https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files): "All in One (1.7.104.0)" for the current game. Without it the plugin does not load, and its log says why.
- This mod: install the zip with a mod manager, or extract it into `Skyrim Special Edition\Data`.

The mod is tested on game version 1.7.104 (the current Steam version). Older Anniversary Edition versions (1.6.317 and later) load with a warning in the log; 1.5.97 and older are not supported.

Always start the game with SKSE (`skse64_loader.exe`, or your mod manager's SKSE launcher).

## What the zip holds

| In the zip | What it is |
| --- | --- |
| `SKSE\Plugins\SkyrimCoop.dll` | the plugin |
| `SKSE\Plugins\SkyrimCoop.ini` | its settings (each one is explained in the file) |
| `SKSE\Plugins\SkyrimCoop\lang` | the mod's texts, one file per language |
| `SkyrimCoop\INSTALL.md` | this page |
| `SkyrimCoop\LICENSE` | the GPL-3.0 license |
| `SkyrimCoop\EXCEPTIONS` | the modding and linking exceptions that go with it |
| `SkyrimCoop\SkyrimCoopServer.exe` | the dedicated server (only in the zip from GitHub; a game does not need it) |

## Did it load?

Start the game through SKSE and get to the main menu, then open

`Documents\My Games\Skyrim Special Edition\SKSE\SkyrimCoop.log`

It should begin like this:

```
SkyrimCoop 0.0.1 (protocol 3)
Game version 1.7.104.0 (the tested one)
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

## Language

The mod's texts follow the language the game is set to (English, French, German, Italian, Spanish, Polish, Russian,
Japanese or Traditional Chinese). Set `sLanguage` in `SkyrimCoop.ini` to pick another: `en-gb` (English with UK
spelling), `es-mx` (Latin American Spanish), `pt-br` (Brazilian Portuguese), `uk` (Ukrainian) or `zh-cn` (Simplified
Chinese), which the game itself doesn't have. Steam shows your status to your friends in your language.

The texts are in `Data\SKSE\Plugins\SkyrimCoop\lang`, one file per language; each file's first line says whether it
is a first machine translation or has been checked against the game's own words. Those files are the mod's: every
update brings its own, so a change made in them is gone after the next update. To correct a language or add one, put
your texts in a file of your own beside the mod's log, `Documents\My Games\Skyrim Special Edition\SKSE\SkyrimCoop_lang_<code>.txt`
(for Russian, `SkyrimCoop_lang_ru.txt`; for a language the mod doesn't have, pick a short code and set `sLanguage` to
it): the same `key = text` lines, only the ones you change, keeping the `{placeholders}`. Your texts go over the mod's
and no update touches the file. Corrections are welcome on the mod's GitHub page.

## Reporting a problem

Use the bug form at <https://github.com/Jolley71717/SkyrimCoop/issues/new/choose> and attach the log. The log is written to leave out player names, Steam ids and network addresses; check anything else you attach.

## License

GPL-3.0-or-later with the modding and linking exceptions in the repository's `EXCEPTIONS` file. The source is at <https://github.com/Jolley71717/SkyrimCoop>.
