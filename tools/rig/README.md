# The rig scripts

Scripts that drive two games of Skyrim on the rig: a host PC and a virtual machine, each signed in to its own Steam account that owns the game ([docs/GETTING-STARTED.md](../../docs/GETTING-STARTED.md)). They run from Git Bash on the host. Every script says at its top what it does and what it assumes.

| Script | What it does |
|---|---|
| `install.sh` | puts SKSE64, the Address Library, Crash Logger and a build of the plugin into both games (`install.sh undo` takes it out again; `--record <name>` keeps an install's record apart) |
| `firststart.sh` | the ini files the launcher would write on a first run, a window instead of the full screen (`check`, `undo`) |
| `start.sh` | starts both games through SKSE and waits for the plugin; `--cell <cell>` puts each player there with `coc`; `start.sh stop` closes them |
| `lobbytest.sh` | the first test: a Steam lobby and pings over Steam's relay, at the main menu |
| `smoke.sh` | **the functional test**: named steps with asserting checks, both directions, a verdict, and the restore |
| `fetchlogs.sh` | copies both machines' logs (the plugin's, SKSE's, Crash Logger's) into one folder |
| `assert.sh`, `crashlib.sh`, `steps.sh` | the checks, the crash logs and the steps `smoke.sh` is made of |
| `selftest.sh` | the harness's own tests, with no game (the Linux tests run it on every push) |

## Running the functional test

Hold the rig first: one user at a time, and never while Fallout 4 runs ([AGENTS.md](../../AGENTS.md)).

```bash
bash $STAGE/rigq.sh take <name>
export SKYRIMCOOP_RIG_OWNER=<name>
bash tools/rig/smoke.sh --build <folder holding SkyrimCoop.dll>    # a whole run: about ten minutes
bash tools/rig/smoke.sh --only join,see                             # some steps (the guard sets up what they need)
bash tools/rig/smoke.sh --only join,forms                           # a step that runs only when named (below)
bash tools/rig/smoke.sh --list                                      # the steps, one line each
bash $STAGE/rigq.sh free <name>
```

`--build` installs that build on both machines first (a CI build: `gh run download`), under an install record of the run's own (`install.sh --record`), so it can go over a build that is installed already, and takes it out again at the end, putting back what was there (also when the install stopped half way). Without it the run tests what is installed. The games are started if they are not running, with `coc` from the main menu (`--cell`, default Riverwood): a run never uses Continue or Load on the host, whose Saves folder may hold the maintainer's own saves. Every run, whatever steps it picks, stops in its set-up if the host's game has loaded a save since it started.

A run's folder is `$STAGE/checks/skyrim-smoke-<time>/`: `run.log` (everything it printed), `results.tsv` (one line per check), `summary.txt`, the movement samples, the plugin log lines of the run, and copies of any crash log. It holds the two machines' ini files as they were before the run, and the listings of both Saves folders, so it stays in the rig's folder: never attach it as it is.

### What the verdict says

Every check is one line: `PASS`, `FAIL` or `WARN`, with what was read. Four more words are about the rig, not the mod:

- `RIG`: a check the rig could not make this time (a scripted walk that stuck before it got anywhere, a button press the game never saw). A step that meets one runs once more; the lines of the first attempt from the fault on are set aside (`VOID`), and a check that fails in both attempts stays a `FAIL`.
- `HEAL`: the rig was found in a wrong state and put right before a step (a player at the main menu put in the world with `coc`, a walk left going stopped, the two games put in one session again). No verdict either way; listed.
- `RIGFAULT`: a wrong state that could not be put right; that step's checks were not made. A game drawing fewer than 20 frames a second (ticks a second, read twice 3 s apart before each step that times something: a display asleep, a starved machine) is one: its stand-ins and walks would lag, and the step would measure the rig. The harness says so and never changes a machine's power settings.
- `rig broken` (inside a `FAIL`): a game not answering, ssh to the VM down, the pad moving nobody. Two of them end the run at once, and a `RIGFAULT` counts toward those two as well (two steps skipped for a slow game end the run): the rest would measure the rig.

The last line is `VERDICT: PASS` (every check made and passed), `VERDICT: FAIL` (a check failed: the mod's), or `VERDICT: RIG` (nothing failed, but the rig kept some checks from being made: fix the rig and run again). The exit code is the number of failed checks, plus the checks the rig kept from being made and the rig faults it could not put right (so it is 0 only for `PASS`), at most 125; a run that made no check never passes.

### The steps

| Step | What it checks |
|---|---|
| `preflight` | both games run the same build of the plugin on 1.7.104 with the Address Library, Steam usable, the tick running and its rate (a `WARN` under 20 a second); the host's game loaded no save since it started (the run stops if it did; the set-up checks this on every run too) |
| `world` | both players in the world with no box open, near each other (`coc` from the main menu when not: console placement, said as a `HEAL`) |
| `join` | the host hosts; the VM joins over Steam through `steam://joinlobby` (the path an accepted invite takes; `--join dev` joins by lobby id); both say so; the join's time; nobody turned away |
| `see` | each game has a stand-in of the other: placed (a form id and a place, not the reply's `placed=` count), dressed, named as its friend, standing where its friend stands (within 60 u) and facing the way it faces (within 0.15 rad); its friend's states keep coming; no stray stand-ins |
| `move_vm` | the VM's player runs, sprints (`lb` held) and walks round a rectangle (the virtual pad; the sprint leg is 1400 u, sampled with no pause, and fewer than 4 sprinting samples is RIG, not judged); on the host the stand-in follows the whole way (mean and largest distance, the share of the way it covers), its legs move while its friend moves, it shows a sprint while its friend sprints and none while it does not, and after each leg it stops where its friend stopped, facing the way its friend faces |
| `move_host` | the same, the host's player moving and the VM watching |
| `gait` | sneak and stand up, then jump, each way: the watcher's stand-in shows it |
| `save_rejoin` | the VM saves mid-session (no stand-in written into the save, the game lives through it), leaves (its stand-in goes on the host, nothing stray), joins again, loads its own save in the session (no stand-in comes back from it); then the host ends the session (the VM's stand-in of it goes) and hosts again |
| `npcs` | both games share the NPCs near the players (the graph event hook in, `npcs` active); every NPC loaded in both has one runner (`owner=`) both games agree on; the other game shows it as a copy (`role=copy`) within 150 u of where its runner has it, for 80 % of them, both ways; people among the NPCs (a creature missing is a `WARN`); then each player in turn walks 9000 u away (the virtual pad) and the NPCs it ran near the other player go to the other's game in both books, and it walks back; no graph event lost (`steps/npcs.sh`) |
| `forms` | **only when named** (`--only ...,forms`; below). Two Creation Club masters swapped in the VM's load order (`Skyrim.ccc`, and `plugins.txt` when it lists both), backed up first; the VM's game restarted in the run's cell: it loads them in each other's place, joins, and translates forms by plugin name; an armor of one of them that each player puts on is on the other's stand-in actor (placed, dressed, in its `equippedIds`) as that game's own form ID, and leaves it when taken off, both ways; nothing skipped; then the files put back byte for byte (SHA-256) and the VM's game back in the session (`steps/forms.sh`) |
| `final` | both games still up, no stray stand-ins; each plugin log since the start (no error or assertion line; warnings counted per prefix; no Steam id, IP address or private name); the host never saved or loaded; no new crash log on either machine |

After the steps, always (also after the breaker or Ctrl-C: the run's log goes through a tee that Ctrl-C does not kill), the restore: virtual pads off, sessions left, the VM's test save moved aside into the project's folder on the VM (never deleted), any join task left on the VM removed; then both games closed (when one will not close, the rest waits for `--restore`), both machines' `Skyrim.ini` and `SkyrimPrefs.ini` put back byte for byte, `--build`'s install undone and both DLLs checked to be the ones from before, `start.ps1`'s game tasks removed from the VM, and both Saves folders compared with their listings from before (the host's must not have changed). `--keep` leaves the games running; once both are closed, `smoke.sh --restore <run folder>` does the rest of the restore (the ini files, the install undone and both DLLs checked, the game tasks, the Saves listings). A run folder is marked put back once its restore passes, and is not put back twice; a restore with a failed check (an undo that failed, a DLL that differs) is left unmarked, so `--restore` can try it again.

**Steps that run only when named** (`OPTIN_STEPS` in `steps.sh`: today `forms`) change a game's own files for a while, outside `install.sh`. Each keeps a backup with its hashes on the VM (`$VM_DIR/forms-backup`) and a copy in the run's folder, and puts the files back at its end and in the restore. A run that died in the middle (the harness killed, the ssh link or the host down) leaves the backup behind: the next run's set-up puts it back first (a `HEAL`, the VM's game closed and started again), or stops if it can't. Such a step joins the default run only once the maintainer agrees to it.

## Adding a step for a new feature

Every feature's pull request adds its own step, so the rig can test it the day it is free. A step is written before anyone has run it; the pull request says so ("Not tested in a game yet; rig step `<name>` is ready for it").

1. **One step per class of thing, not per object.** `doors` covers every kind of door, gate and activator it can reach; `quests` every way a quest stage moves. Name it after the class, in lower case.
2. **In `steps.sh`:** add the name to `STEPS` (before `final`), a one-line `STEP_INFO` entry, the name to `SESSION_STEPS` when it needs the two games in one session, and write `step_<name>`. In `smoke.sh`, add it to `RERUN_STEPS` only if running it twice in a row changes nothing (no save, no quest stage set for good).
3. **Both directions.** Do it on the VM with the host watching, then on the host with the VM watching, each in its own `section`. A check names the game it reads (`host: ...`, `vm: ...`).
4. **Real input only.** Players move with the virtual pad (`walkto <ref>` to walk up to a reference, `walkto <x> <y>`, `pad button a` to activate); nothing is placed or moved by a script. `coc` from the main menu (or to bring both players to a scene) is allowed and is said in the run's log. The host's attacks may be injected for combat steps.
5. **Read both games.** The feature adds a developer-channel command that answers in one line of `key=value` (no names, no Steam ids: `Redact.h`), and the step asserts on it in both games with `expect_*`: the two games agree, and they agree with what was done. Read the plugin log since the run's mark with `log_since_mark <who> '<regex>'` for lines that must (or must not) be there.
6. **Say whose fault it is.** A press that never reached the acting game (its own state did not change) is `rig_fault input`; a game that does not answer is `rig_broken game`. Only what the mod does wrong is a `fail`.
7. **Saves.** A persistence check saves and loads on the VM only, under `$SAVE_NAME` (moved aside at the end). The host's game never saves and never loads.
8. **Test the reading.** Add a case to the self-test at the end of `steps.sh` that fakes both games' replies (in the plugin's real format) and shows a good reply passes and a bad one fails. `bash tools/rig/selftest.sh` runs every self-test; CI runs it on every push. Add the step to the table above (`smoke.sh --selftest` checks every step is named here).
9. **Fast.** A step is a functional check of a minute or two, not a soak.

## What these scripts never do

They never download anything (the maintainer puts SKSE, the Address Library and Crash Logger in the rig's `skyrim-downloads` folder; `install.sh` checks their hashes), never use Continue or Load on the host, never save on the host, never delete a save (the VM's test save is moved aside), never teleport a player or move one by a script, and never steer a game by screenshots. They print no Steam id and no name: the VM's join address names the host's Steam id and is passed from one game to the other without being printed or kept.
