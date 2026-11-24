## 0.1.0-rc1 (Pre-alpha)

Release candidate: this build is being tested before 0.1.0 and may be replaced. It never goes to Nexus.

**SkyrimCoop 0.1.0 is a pre-alpha: the first build for other people to try.**

This is a test build. Very little of the world is shared yet, it has bugs, and it can crash. Back up your saves before you play with it, and do not use your only save.

It is for two players over Steam, on Skyrim Special Edition 1.7.104 (the current Steam version). Everyone in a session needs this same version of the mod.

What you need: SKSE64 2.3.1, Address Library for SKSE Plugins (All in One), and this zip. The steps are in `SkyrimCoop\INSTALL.md` inside the zip.

How to play: the host sets `bHost = true` in `Data\SKSE\Plugins\SkyrimCoop.ini`, starts the game with SKSE and loads a save. The friend leaves `bHost = false` and joins from the Steam overlay (Join Game on the host's entry in the friends list, or the host's invite). No server to set up and no ports to open.

What works:

- **Join through Steam.** The friend joins the host's game from the Steam overlay, and each player keeps their own character and save.
- **You see each other.** Each player sees the other's character, named and dressed as they are, walking and running where they go.
- **Doors are shared.** A door one player opens or closes opens or closes for both, and stays that way after a save and a rejoin.
- **Load order doesn't matter.** Games with the same mods loaded in a different order still agree on which door is which.
- **14 languages.** The mod's own texts are in every language Skyrim has.

Not shared yet (each game still keeps its own): enemies and other NPCs, combat and deaths, quests, items and containers, time and weather. Your friend's character doesn't draw a weapon or attack in your game yet. These are being built and tested and will come in the next versions.

Also known:

- Only two players have been tested. More are allowed in the settings but nobody has tried them.
- Other mods that change the world, the quests or the player are untested. Start with a vanilla game (the Anniversary content is fine).
- Game versions older than 1.7.104: Anniversary Edition 1.6.317 and later load with a warning and are untested; 1.5.97 and older do not load.
- Skyrim VR and the GOG and Microsoft Store versions are not supported.

How it was tested: Unit tests pass. This candidate is being tested on two PCs before release.

Reporting a problem: use the bug form at https://github.com/Jolley71717/SkyrimCoop/issues/new/choose and attach `SkyrimCoop.log` from `Documents\My Games\Skyrim Special Edition\SKSE`. The log leaves out player names, Steam ids and addresses; check anything else you attach.

Files: `SkyrimCoop-0.1.0-rc1.zip`.
