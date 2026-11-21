# Plan

Six phases, in order. Each one ends with something that has to be shown on two real games before the next one starts. There are no dates.

**Where it is today: phase 0, not yet run in a game.** The repository builds, the game-free parts are unit-tested, and the plugin's start-up is written but has never been loaded by a game.

## What counts as verified

The same rules as the sister project, which learned each one the hard way.

- **In a game, on a rig.** A phase is done when its proof was seen in two running games (a host PC and a second machine, each with its own Steam account), read from the plugins' logs by a script. Unit tests show the logic is right; they never show the game does it.
- **Both directions.** Everything is tested with the host doing it and the friend seeing it, and then the other way round. A thing that works one way is half done.
- **Real input.** A test moves a player by sending keys to the game, never by setting a position. Placing a character with the console to reach a scene is fine if the report says so.
- **Function, not looks.** "The other player's character is there" is not the test; "it stands where the other log says its player stands, within a stated distance, for the whole run" is.
- **Over time.** A thing that works for a minute is tested for an hour before it is called done: the long test (the soak) of a release candidate is part of the gate ([RELEASING.md](RELEASING.md)).
- **Nothing echoes.** What one game sends must not come back to it as news and be sent again. Every shared system gets a check for that.
- **Say what was not tested.** A report names what was run and what was not. "Not tested in a game" is an honest answer and today it applies to everything.

Bugs are sorted by what they do to a session:

| | Meaning |
| --- | --- |
| S0 | A crash, a lost or damaged save, a player stuck with no way out. Never shipped as a known issue. |
| S1 | The session breaks or goes wrong for good (a quest that can't finish, players who no longer see each other). |
| S2 | Wrong but playable (a pose, a sound, a number that differs between the two games). |
| S3 | Cosmetic. |

## Phase 0: the plugin loads

The plugin is loaded by SKSE into the current Steam version of Skyrim Special Edition and does nothing but report.

To write: nothing new is required for the first load; reading the load order (`src/game/LoadOrder`) belongs here so the log's plugin count is real.

Proof, on one game, then on the second machine:

- `SkyrimCoop.log` exists and starts with the version line, the game's version, the `Launch:` lines and the settings read from the ini.
- "Game data loaded (N plugins read)" with N matching the load order.
- Loading a save, saving and starting a new game each write their line, and the game plays for ten minutes with the plugin in, with no new warning in the log and no crash.
- The log's Steam line and the output of `tools\steamcheck.ps1` are written down: they are the first input to phase 1 ([GETTING-STARTED.md](GETTING-STARTED.md), "The first thing to test").
- The same on game version 1.5.97 if that version is to stay loadable (a decision to confirm).

## Phase 1: a Steam lobby and an invite between two games

Two games find each other through Steam and exchange messages. Nobody sees anybody yet.

To write: the frame hook (`src/game/GameThread`), so Steam's callbacks and the session run every frame; whatever the Steam check of phase 0 showed is missing; a first way to host and to invite without a menu (an ini setting and a key, or console commands); the session's start (hello, welcome, the load-order comparison) wired to the carried `common/Server` and `src/net/NetClient`.

Proof, on the rig, both as host:

- The host opens a friends-only lobby. The friend's Steam friends list offers "Join Game"; the overlay invite arrives; accepting it joins.
- Both logs show the session: the host lists the friend as `player 2`, the friend is welcomed, and each side names the other's game and protocol version.
- Messages go both ways over Steam's relay for an hour: heartbeats answered, no disconnect, and the round trip time logged.
- Leaving is clean both ways: the friend leaves and the host's log says so; the host stops and the friend's log says so; each can start again without restarting the game.
- A wrong password, a full session, a different protocol version and a different load order are each refused with their own clear line in both logs.
- The same session over direct UDP (`sTransport = enet`) on the local network.
- If Steam cannot do this from inside Skyrim, the phase ends with that finding and a decision on the fallbacks, not with a workaround nobody chose.

## Phase 2: players see each other move

Each player sees the other's character walk, run, sneak, jump, swim and ride, in the same cell and across cell and worldspace changes.

To write: reading the local player (`src/game/LocalPlayer`); the other players' stand-ins (`src/game/RemotePlayers`: an actor that looks like the other player's character, placed and animated from their state, that the game's AI leaves alone); smoothing between states; what happens when players are in different cells; fast travel and doors; the stand-in's gear.

Proof, on the rig, both directions:

- One player walks a scripted route with real input; in the other game the stand-in's position, read from that game, stays within a stated distance of the sender's reported position for the whole route.
- Running, sneaking, jumping, drawing a weapon, swimming and mounting each show as that state in the other game's log.
- Going through a door to an interior, coming back, and fast travelling: the stand-in leaves and arrives; no stand-in is left behind, doubled or stuck.
- A thirty-minute run of both players moving ends with no drift, no crash and no warning that repeats.
- The stand-in is never attacked by guards or treated as a stranger by the world's AI, and never talks.

## Phase 3: combat

Players fight the same enemies and the fight comes out the same in both games.

To write: who owns an NPC's fight (the host, as in the sister project's design); hits, damage, staggers and deaths sent and applied; weapons, bows and spells shown on the stand-in; a player's own death and what follows it; friendly fire as a setting; shouts.

Proof, on the rig, both directions:

- A player kills an enemy with a sword, with a bow and with a spell; in the other game the same enemy dies at the same time, once, and stays dead after both leave and return.
- An enemy's health, read in both games during the fight, agrees within a stated margin.
- Hits by the friend count: the friend alone can kill an enemy the host is not near.
- A player who is killed sees what the design says (downed, respawn or reload) and the session goes on.
- An asserting check for cross-game hits is part of the gate from this phase on: a long test alone does not notice hits that stop counting.

## Phase 4: world state

What one player changes in the world is changed for everyone, and is still so after a save and a reload.

To write: containers and what is in them; picked-up and dropped items; doors, locks and activators; harvested plants and mined ore; dead bodies and their loot; time of day and weather from the host; what a joining friend is told about a world that has already changed; what the friend's own save keeps (their character, not the world).

Proof, on the rig, both directions:

- An item taken from a container by one player is gone for the other; an item put in is there. No item is ever doubled or lost in a scripted hundred transfers.
- A door opened, a lock picked and a lever pulled show the same in both games.
- The host saves, both leave, the host loads, the friend joins again: the world is as they left it.
- A friend who joins late sees the changes made before they joined.
- The friend's own save file, loaded alone afterwards, is not damaged by having been a guest.

## Phase 5: quests

The group does quests together: what any player does counts, and everyone sees the same stage.

To write: quest stages and objectives from the host to everyone; who may talk to a quest's NPC and what the others see; quest items; scenes that wait for "the player"; rewards for everyone; the list of quests that cannot be shared as they are, each with what happens instead.

Proof, on the rig, both directions:

- A scripted run of the first main-quest steps with the friend doing the steps: the stage advances in both games, and the host's save holds it.
- The same with the host doing the steps.
- A quest's reward reaches both players.
- A dialogue one player is in does not lock the other out of the world or leave them stuck.
- An audit lists every quest in the base game as shared, shared with limits, or not shared, with the reason: nothing is left as "unknown".

## After that

Not planned in detail: a menu in the game for hosting and joining, followers, houses and Hearthfire, the Creation Club content of the Anniversary Edition, other languages, larger groups, Proton and the Steam Deck, mods in the load order. The sister project's order of doing these is the first guess for this one.
