# SkyrimCoop

**Coming soon: nothing playable yet.**

SkyrimCoop will be a co-op mod for The Elder Scrolls V: Skyrim Special Edition on Steam, from the makers of [F4Multiplayer](https://github.com/Jolley71717/F4Multiplayer), the co-op mod for Fallout 4. The idea is the same one:

- **Join through Steam.** The host invites friends from the Steam overlay. No server to set up, no ports to open.
- **The host's world is the world.** One player hosts from their own save; friends play in it with their own characters.
- **Quests are shared.** What the group does counts for the group, not only for the player who started it.
- **Tested for hours before each release.** Every release candidate is played by two scripted games for a long session before it becomes a release.

## Where it is today

The project is at its very start. What exists is the foundation: an SKSE plugin that loads into the game and writes a log, the network and session code carried over from F4Multiplayer, a dedicated server, and their unit tests. It has not been run in a game yet, and it does not connect two games.

[docs/PLAN.md](docs/PLAN.md) lists the phases in order, from "the plugin loads" to shared quests, and what each one has to prove before the next begins. There are no dates.

## For the curious

- [docs/DESIGN.md](docs/DESIGN.md): how it is meant to work, and the decisions made so far.
- [docs/PORTING.md](docs/PORTING.md): what comes over from F4Multiplayer and what has to be written again for Skyrim.
- [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md): setting up a game to develop against.
- [CONTRIBUTING.md](CONTRIBUTING.md): building, testing and sending a change.

## License

GPL-3.0-or-later, with the modding and linking exceptions in [EXCEPTIONS](EXCEPTIONS). See [LICENSE](LICENSE).

SkyrimCoop is a fan project. It is not affiliated with or endorsed by Bethesda Softworks, ZeniMax Media or Valve. The Elder Scrolls and Skyrim are trademarks of their owners.
