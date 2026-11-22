# Agent Instructions

## Project rules (every agent, every tool)

These are the maintainer's standing wishes. They hold for Claude Code, Codex, cloud sessions, API harnesses and
subagents alike, and a task brief can narrow them but never loosen them. When a task seems to need breaking one, stop
and ask.

### Where the project is

- SkyrimCoop is at its start (docs/PLAN.md, phase 0). The plugin has not been run in a game. Never write, in a commit,
  a pull request, a doc or a report, that something works in the game unless it was seen there; "unit tests only" and
  "not tested in a game" are the honest answers today.
- It is the sister project of [F4Multiplayer](https://github.com/Jolley71717/F4Multiplayer), by the same maintainer,
  under the same license. Its code may be carried over (docs/PORTING.md says what has been and what has not).

### Changes and merges

- Work on a branch, open a pull request against `main`, never push to `main` directly. A PR that is reviewed (below)
  and tested may be merged without asking the maintainer: keep things moving. Releases still follow the rc-and-soak
  rule below.
- Every PR gets a code review at its current head before it merges: a PR comment starting "Code review at <sha>" and
  the `reviewed` label. New commits remove the label (`.github/workflows/review-label.yml`); review again.
- Every outside PR, and every `needs-vetting` PR, merges only with `vetted` at its head and a "Vetted at `<that head>`"
  comment from the label workflow (CONTRIBUTING.md, "Binaries and new dependencies"). Only the "Trusted: report" job's
  summary of a vet counts; the untrusted jobs' summaries can say anything. An outside PR's code is never built or run
  on the maintainer's PC or the rig before it is vetted and reviewed, and never with `build.yml`.
- Merge locally (`git merge --no-ff origin/<branch>`, then push `main`), not with `gh pr merge`.
- Say how a change was tested. "Not tested in a game" is a fine answer when it's true; never imply a game test that
  didn't happen.
- Every edit, build and merge happens in its own git worktree, not in the main checkout. A fresh worktree needs
  `git submodule update --init` before it builds (not `--recursive`: the library's own submodule is only for Skyrim
  VR, which this project does not build for).
- Whoever makes a git worktree removes it once its branch is merged (`git worktree remove <path>`, then
  `git branch -d <branch>`). Each holds a build of a few GB. Never remove one with uncommitted changes, untracked
  source files or unmerged commits: report it instead.
- The Windows build on GitHub (`build.yml`) costs the most Actions minutes of anything here. It runs for the owner's
  branch pushes only; don't push a branch commit by commit when one push would do.

### Releases (docs/RELEASING.md)

- A GitHub pre-release `vX.Y.Z-rcN` first, then the gate and the soak docs/RELEASING.md sets for that rc's zip (the
  zip's DLL, not a rebuild), then `tools/promote.ps1 -Rc vX.Y.Z-rcN`. Never publish a release whose rc hasn't soaked.
  Until the rig can run two games of Skyrim there is no soak, so there are no releases: only pre-releases, marked as
  untested in a game.
- Release notes go to the maintainer before anything is published. Notes are for players: no issue-tracker ids or
  internal tooling.
- Nexus uploads only through `gh workflow run nexus-upload.yml -f tag=vX.Y.Z`, never by hand, and only when the
  maintainer says so. The Nexus zip never holds `SkyrimCoopServer.exe`. The workflow is switched off until the mod
  has a Nexus page (it refuses to run while its two ids are empty); creating that page is the maintainer's to do.

### Secrets, privacy and other people

- Never read, print, log, store or ask for a secret: `NEXUSMODS_API_KEY` (a repository secret the maintainer sets),
  tokens, passwords, API keys of any service.
- No real names, handles, emails, Windows usernames, machine names, Steam ids or IP addresses in commits, PRs,
  comments, logs or docs. Players in logs are `player N`. The repository's history is public: a private detail in a
  commit message is as public as one in a file.
- Never post, reply or message anyone (Nexus, GitHub issues of other projects, Discord, email) on the maintainer's
  behalf without their approval of that exact text.
- Never copy code, assets or data from a project without a GPL-compatible license, and never from another Skyrim or
  Fallout multiplayer mod without the maintainer deciding it first: docs/PRIOR-ART.md lists each project's license
  and what it allows. Comparing features is fine. Carrying code over from F4Multiplayer is fine.
- Never commit Steamworks SDK headers and never ship `steam_api64.dll`: the plugin calls the game's own Steam library
  through the hand-declared functions in `src/steam/SteamApi.cpp`.
- No binaries in the repository (no DLLs, no compiled menu files, no archives): what ships is built from source by
  the project's workflows. A new dependency is source, pinned to a commit or a version, with a GPL-compatible license
  that the change names.
- Never download the game, SKSE, the Address Library or any other mod onto the maintainer's machines: the maintainer
  downloads them and puts them in the rig's `skyrim-downloads` folder. Agents may install from those files only, and
  only with `tools/rig/install.sh`, which checks their hashes, backs up what it replaces and is undone with
  `install.sh undo` (docs/GETTING-STARTED.md). Buying and installing the game itself stays the maintainer's.
- Never permanently delete the maintainer's own files outside the repo, and never change system or security settings.
- Never change the repository's visibility or its settings.

### The test rig (when the rig exists)

Skyrim is not installed on the rig yet; docs/GETTING-STARTED.md says how the rig the sister project uses (a host PC
and a virtual machine, each with its own Steam account that owns the game) is meant to be reused. From the day it is:

- One user at a time: take and free the rig's lock. The same machines run the sister project's tests, and the two
  projects never use the rig at once. Back up the ini and session files first and restore them byte for byte after;
  put main's build back on both machines.
- Exercise real movement. Never teleport or inject movement to make a test pass. Console placement to reach a scene
  is fine if the report says so; injecting the host's attacks for combat tests is fine.
- Don't steer the game by screenshots (too slow and unreliable); script input and read the plugin log.
- Never run two traces or soaks at once on the same machines.
- Reproduce a bug on the rig before fixing it, and run the same reproduction again to show the fix.

### Cost and scale

- Routine work (merges, builds, gates, rig batches, soaks, releases, replies) runs on the smaller model. The larger
  one is for engine reverse engineering, crashes, and changes that could crash the game or touch the protocol or
  saves.
- A few agents at a time, not a fleet, unless the maintainer asks for one.
- GitHub Actions minutes are counted while the repository is private, and Windows runners count double. Prefer the
  Linux tests for anything that needs no plugin build.

### Product constraints

- Skyrim Special Edition and Anniversary Edition on Steam, through SKSE64, CommonLibSSE-NG and the Address Library.
  Not the 2011 game ("Legendary Edition"), not Skyrim VR, not the GOG or Microsoft Store versions, for now.
- Played over Steam (lobbies, invites, the relay), with join by address second. Not through a VPN service. No
  features for one mod manager only. The project is GPL.
- The host's save is the one world; friends follow. docs/DESIGN.md records the decisions.

### New bugs and requests go on the tracker

- Every new bug, crash or feature request, wherever it comes from (a player's report, a rig run, a review, something
  noticed in passing), gets an issue on the maintainer's tracker, labelled with its source, the moment it is found,
  not at the end of the task.
- Mark an item as taken the moment work on it starts, and move its stage as the work moves (looked into, PR,
  reviewed, rig-tested, merged, rc, shipped). The maintainer follows the work there.
- The tracker is private. Its ids, its name and its data never appear in commits, PRs, docs, release notes or
  comments.
- An agent that can't reach the tracker (a cloud session) lists the new bugs in its PR or hand-off so they get filed.
- Close or update the matching item when work lands; a GitHub issue that a fix covers gets a comment and is closed.

### Reporting

- Status updates are short: 2-4 lines, plain words, no tables or internals unless asked.

## Non-Interactive Shell Commands

**ALWAYS use non-interactive flags** with file operations to avoid hanging on confirmation prompts.

Shell commands like `cp`, `mv`, and `rm` may be aliased to include `-i` (interactive) mode on some systems, causing the agent to hang indefinitely waiting for y/n input.

**Use these forms instead:**
```bash
# Force overwrite without prompting
cp -f source dest           # NOT: cp source dest
mv -f source dest           # NOT: mv source dest
rm -f file                  # NOT: rm file

# For recursive operations
rm -rf directory            # NOT: rm -r directory
cp -rf source dest          # NOT: cp -r source dest
```

**Other commands that may prompt:**
- `scp` - use `-o BatchMode=yes` for non-interactive
- `ssh` - use `-o BatchMode=yes` to fail instead of prompting
- `apt-get` - use `-y` flag
- `brew` - use `HOMEBREW_NO_AUTO_UPDATE=1` env var
