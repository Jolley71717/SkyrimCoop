# Releasing

**There are no releases yet, and there will be none until two Skyrim games run on a test rig.** This page describes how a release will be made, with the tools that are already in the repository. The tools are carried from the sister project, [F4Multiplayer](https://github.com/Jolley71717/F4Multiplayer), where every release is made this way; here they have been written and read, never run.

The rule the whole page serves: **what was tested is what ships.** A version is first a release candidate; exactly that candidate's files are played for a long session by two scripted games; if it holds, the release is made from those same files without building anything again.

## Before the rig exists

Until phase 1 of [PLAN.md](PLAN.md) has two games talking, nothing can be gated or soaked. If a build is put on GitHub at all before then, it is a pre-release whose notes begin by saying it was not tested in a game, and it is never promoted to a release and never goes to Nexus Mods.

## Release candidates

A version is first published as a GitHub pre-release tagged `vX.Y.Z-rcN`. In this order:

1. Write `dist/RELEASE-X.Y.Z-rcN.md` and commit it, through a pull request like any change: the candidate's commit carries its notes. For a new version's first candidate, `set_version` in `xmake.lua` is bumped to X.Y.Z in the same commit.
2. Package that commit on GitHub with the Package workflow (`.github/workflows/package.yml`, owner only, run from `main`):

   ```
   gh workflow run package.yml -f ref=<commit> -f label=rcN
   ```

   The ref must be on `main`. On a Windows machine of GitHub's, with no build cache, it runs `tools/package.ps1 -Label rcN` on that commit: builds every target, runs the unit tests and stops if one fails, checks that nothing in the zip names the machine or its user, and produces an artifact named `SkyrimCoop-X.Y.Z-rcN-<commit>`. Put its files where the other tools expect them:

   ```
   gh run download <run id> -n SkyrimCoop-X.Y.Z-rcN-<commit> -D dist/out
   ```

   `dist/out/` then holds:
   - `SkyrimCoop-X.Y.Z-rcN.zip`, laid out like the game's `Data` folder;
   - `SkyrimCoop-X.Y.Z-rcN.manifest.txt`: the version, the commit, the SHA-256 of the zip and of the DLL, and the pinned dependencies;
   - `symbols/SkyrimCoop-X.Y.Z-rcN-<commit>.pdb`, for reading crash logs from exactly this build. It never goes on a release (it is full of build paths), and it is needed long after the artifact has expired: keep it.

   `-f trial=true` packages with any label and uncommitted notes, to try the workflow out; such a build calls itself `<commit>-dirty` and cannot be promoted.

   `tools\package.ps1 -Label rcN` on a development PC does the same and is the fallback. It refuses uncommitted changes to tracked files, untracked source files that would be compiled in, notes that are not committed, and notes in `dist/` for a newer version (that means `set_version` was not bumped).

   The plugin's log then says `SkyrimCoop build rcN <commit>` near its top: that is how a log names the build it came from.
3. Tag the commit the manifest names: `git tag vX.Y.Z-rcN <commit>`, and push the tag. The tag follows the artifact, not the other way round.
4. Publish the candidate:

   ```
   gh release create vX.Y.Z-rcN dist/out/SkyrimCoop-X.Y.Z-rcN.zip --prerelease --title X.Y.Z-rcN --notes-file dist/RELEASE-X.Y.Z-rcN.md
   ```

## The gate

Every candidate, every time, on the packaged zip installed on both machines (the zip's own DLL, not a rebuild of the same commit):

1. The unit tests pass (packaging runs them).
2. The standing set of rig checks passes once in both directions: host to friend and friend to host. The set grows with the plan: each phase adds the checks named under its "Proof" in [PLAN.md](PLAN.md), and a check, once in the set, stays.
3. No new crash on either machine during it.
4. Neither machine's plugin log from these sessions holds a name, an account id or an address: the same scan the git hooks run over commits, run over the logs.

A check the rig could not make (a machine that did not start, clocks that could not be lined up) is a fault of the rig, not a pass and not a failure of the mod: the rig is put right and the check is made again. Two rig faults in a row stop the run until the cause is found.

The scripts that do this do not exist for Skyrim yet ([PORTING.md](PORTING.md), "Tools").

## The soak

After the gate, the same candidate is played by the two scripted games for about 90 minutes: the gate's checks in a loop, with real input, both directions, while the logs are watched for errors, for warnings that repeat, for crashes and for the two games drifting apart. A long test does not notice everything: a thing that silently stops working (hits that no longer count) needs a check that asserts it, and that check belongs in the loop.

The soak ends with a verdict file, `dist/out/soak-verdict.txt`:

```
commit=<the candidate's commit, at least 7 characters>
dll_sha256=<the SHA-256 of the DLL that ran, as in the candidate's manifest>
verdict=PASS
```

Any failure is fixed on `main`, and a new candidate (`rcN+1`) starts again from step 1. A candidate is never patched.

A bug that crashes the game, harms a save or leaves a player stuck is never shipped as a known issue (S0 in [PLAN.md](PLAN.md)).

## Promotion

When a candidate has passed the gate and the soak:

1. Write `dist/RELEASE-X.Y.Z.md`, the release's notes, and send them to the maintainer before anything is published.
2. Run:

   ```
   tools\promote.ps1 -Rc vX.Y.Z-rcN
   ```

   It builds nothing. It checks that the candidate's zip is the one its manifest describes, that the tag points at the manifest's commit, that the candidate was not packaged from uncommitted changes, and that the soak verdict names the same commit and the same DLL with `verdict=PASS`. Then it writes, from the candidate's own files:
   - `SkyrimCoop-X.Y.Z.zip`, the full package with the dedicated server;
   - `SkyrimCoop-X.Y.Z-nexus.zip`, the same without `SkyrimCoopServer.exe`;
   - a manifest and a copy of the `.pdb` under the release's name.

   The release's plugin log still says `build rcN <commit>`: the release is the candidate, byte for byte.
3. It prints the commands that publish. Both zips go on the GitHub release:

   ```
   git tag vX.Y.Z <commit>
   git push origin vX.Y.Z
   gh release create vX.Y.Z "<zip>" "<nexus zip>" --title X.Y.Z --notes-file dist/RELEASE-X.Y.Z.md
   ```

## Release notes

- They go to the maintainer before anything is published.
- They are for players: what changed for someone playing, what to do when updating, what is known not to work. No issue-tracker ids, no internal tooling, no names of people.
- One line per paragraph or bullet, no hard wraps: GitHub and phones show a wrapped line as a break.
- They say how the build was tested, in plain words. They never imply a game test that did not happen.
- A change to the protocol means players on different versions cannot join each other: the notes say so at the top. A patch release (the last number) never changes the protocol or what a save holds.

## Nexus Mods

Not in use yet. The mod's page exists (page number 200124) but is unpublished and has no file.

The upload API only adds a new version to a file that already exists; it cannot create a page's first file. So, once, by hand: the maintainer uploads the first release's Nexus zip on the page's Manage files screen (as the main file), copies the file's id and the mod's API id from the Files tab's "Advanced" view, puts them in `NEXUS_FILE_ID` and `NEXUS_MOD_ID` at the top of `.github/workflows/nexus-upload.yml` through a pull request, and sets the repository secret `NEXUSMODS_API_KEY`. `NEXUS_MOD_ID` in the file now holds a computed value, `(1704 << 32) | 200124 = 7318624472508` (1704 is Skyrim SE's game id; the same formula gives F4Multiplayer's 4943507467508 from `(1151 << 32) | 109812`): it is the mod's API id, not the page number, and must be checked against the Advanced view after the first upload. From then on, uploads are rare, made only when the maintainer says so, and only through the workflow, never by hand in a browser:

```
gh workflow run nexus-upload.yml -f tag=vX.Y.Z
```

`-f check_only=true` checks the zip and the key and uploads nothing. With no tag (`gh workflow run nexus-upload.yml -f check_only=true`) it checks only that Nexus accepts the key, and works before any release or id exists. The workflow takes the Nexus zip from the GitHub release and the changelog from `dist/NEXUS-X.Y.Z.txt` on `main`, or at the tag when `main` lacks it, one line per entry. It refuses to run until the page's two ids are filled in at the top of `.github/workflows/nexus-upload.yml` and the repository secret it names is set; the maintainer does both. Nobody else reads, asks for or handles that secret.

The Nexus zip never holds `SkyrimCoopServer.exe`. In the sister project, uploads that held the dedicated server were held back for a manual virus review, and the first one without it passed the scan on its own. Players who want the dedicated server take the full zip from GitHub.

## Hotfixes

Releases are made from `main`, which is kept in a state that could ship. Only when `main` holds work that has not soaked and a fix cannot wait, a short-lived branch `hotfix/X.Y.Z` is made from the last release's tag, the fix goes there by pull request, the candidate is packaged from it, and the branch is merged back and deleted after the release. (`package.yml` takes commits on `main`; a hotfix candidate is packaged with `tools\package.ps1` on a PC.)

## After a release

- Keep `dist/out/symbols/` for every released build.
- Put `main`'s build back on both rig machines.
- Close what the release fixed, wherever it was reported, with the version that holds the fix.
