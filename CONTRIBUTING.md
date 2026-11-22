# Contributing

SkyrimCoop is at its very start: [docs/PLAN.md](docs/PLAN.md) says which phase it is in, and nothing has been run in a game yet. Help is welcome, and the most useful help right now is small and checkable: a build fix, a unit test, a correction to a doc, an engine function found and described.

Agents working on the project also follow [AGENTS.md](AGENTS.md).

## Where things go

| Folder | What it holds | Needs the game? |
| --- | --- | --- |
| `common/` | The protocol, the server's logic, the transports' interface, ini reading, name and log scrubbing. Plain C++, no game and no Windows headers. | no |
| `server/` | The dedicated server (`SkyrimCoopServer`), a thin shell around `common/Server`. | no |
| `bot/` | A game-free client (`SkyrimCoopBot`) that joins a server and walks in a circle, for tests without a game. | no |
| `tests/` | The unit tests (`SkyrimCoopTests`), with their own small test harness (`tests/Test.h`). | no |
| `src/` | The SKSE plugin: start-up, log, settings. | yes |
| `src/steam/` | Steam through the game's own Steam library: lobbies, invites, the relay transport. | yes |
| `src/net/` | The plugin's side of a connection. | partly |
| `src/game/` | Everything that reads or changes the game. Today these are stubs behind small interfaces; [docs/PORTING.md](docs/PORTING.md) says what each one has to become. | yes |
| `lib/commonlibsse-ng` | CommonLibSSE-NG, a git submodule pinned to a commit. | |
| `dist/` | What goes in the release zip beside the DLL: the ini, the install page, release notes, the texts in every language (`dist/lang`). | |
| `tools/` | Packaging, promotion, the Steam library check, the git hooks, and the rig scripts of the first test in a game (`tools/rig/`, [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md)). | |
| `docs/` | The design, the plan, the porting map, releasing, prior art, getting started. | |

## Building

You need Windows, Visual Studio 2022 (the "Desktop development with C++" workload) and [xmake](https://xmake.io) 3.1.1 or later.

```
git clone https://github.com/Jolley71717/SkyrimCoop
cd SkyrimCoop
git submodule update --init
xmake config -m releasedbg --skyrim_vr=n --skse_patch_safety=n
xmake build -y
```

- `git submodule update --init`, not `--recursive`: the library's own submodule is only for Skyrim VR, which this project does not build for.
- The first build compiles CommonLibSSE-NG from source and takes a while. xmake downloads the packages the build needs (ENet, spdlog, DirectXMath, DirectXTK) as source and builds them too.
- The results are in `build/windows/x64/releasedbg/`: `SkyrimCoop.dll` (the plugin), `SkyrimCoopServer.exe`, `SkyrimCoopBot.exe` and `SkyrimCoopTests.exe`.
- Building needs no game, no SKSE and no Steamworks SDK. Running the plugin needs all of the game's side: [docs/GETTING-STARTED.md](docs/GETTING-STARTED.md).

The game-free targets (the tests, the server, the bot) also build on Linux with gcc 14, which is how GitHub runs the tests for every pull request:

```
xmake f -y -p linux -m releasedbg --cc=gcc-14 --cxx=g++-14 --ld=g++-14
xmake build -y
build/linux/x86_64/releasedbg/SkyrimCoopTests
```

`tools/coverage.sh` does the same with line coverage (it also needs gcovr 7 and python3): it builds into `build/coverage` with its own configuration, runs the tests and writes the reports to `build/coverage/report` (`coverage.md` is the short table, `summary.txt` has one line per file). It measures `common/` and `src/`; the tests themselves, the server and the bot are not counted. This is what GitHub runs; it works the same in WSL's Ubuntu 24.04.

## Tests

```
build/windows/x64/releasedbg/SkyrimCoopTests.exe            # all of them; the exit code is the number that failed
build/windows/x64/releasedbg/SkyrimCoopTests.exe Protocol   # only tests whose name contains "Protocol"
```

- A change to `common/` comes with a test. The tests are plain functions registered with `TEST(...)` in `tests/Test.h`; `tests/ServerFixture.h` runs a whole server and several clients in memory through `tests/FakeTransport.h`.
- Anything that touches the game (`src/game`, `src/steam`, `src/main.cpp`) can only be tested in a game. Say in the pull request how the change was tested. "Not tested in a game" is a fine answer when it is true, and today it is true of everything.
- A test that checks behaviour says so in its name, and a check asserts what happened (a value arrived, a count changed), not that a function ran.

## What GitHub runs

| Workflow | When | What |
| --- | --- | --- |
| `tests.yml` | every pull request into `main`; the repository owner's pushes to `main`, and to a branch with no open pull request | Linux: builds the game-free targets with line coverage (`tools/coverage.sh`), runs the unit tests, and puts the coverage in the run's summary and in one comment on the pull request, kept up to date, with the change against `main` |
| `build.yml` | the repository owner's pushes to a branch, and by hand | Windows: builds everything, the plugin included, and runs the unit tests |
| `vet.yml` | by hand, for an outside pull request | reads and builds the pull request's code on a throwaway machine and reports what it found |
| `review-label.yml`, `vet-label.yml` | pull request events | keep the `reviewed` and `vetted` labels honest: a new commit removes them |
| `package.yml` | by hand | builds the release zips from a commit |
| `nexus-upload.yml` | by hand | switched off until the mod has a Nexus page |

A pull request from a fork gets the Linux tests, with its coverage in the run's summary only: the coverage comment is written by a job that holds write access to pull requests, and it trusts only the reports of this repository's own branches. Its plugin build happens in `vet.yml` after the maintainer has looked at it.

## Code style

- C++23. Tabs. Braces on the same line. Names as in the code around the change.
- Parameters start with `a_` (`a_player`, `a_message`), as in CommonLibSSE-NG.
- Comments say why, not what. A comment that explains a rule of the game or of Steam is worth its lines; one that repeats the code is not.
- The game is single-threaded where it matters: everything that reads or changes game data runs on the game's main thread, from the frame callback. Network threads hand their work over; they never touch the game.
- `common/` stays free of the game and of Windows headers, so it builds and is tested on Linux.
- No personal data in logs (below).
- Every text a player reads (a notice, a menu label, a refusal, Steam's status line) is a key in `src/TextEnglish.h`, shown with `Text::Get` or `Text::Format`, and has a line in every `dist/lang` file. The tests fail on a key missing from a language, on placeholders that differ from the English, on a text left in English, and on a literal handed to a call that shows text. A new key's translations are a machine first pass until a speaker checks them; say which in the pull request. The log, the dedicated server's console and the dev channel stay English.

## The protocol

`common/Protocol.h` is the wire format. Any change to what goes on the wire (a new message, a field added, a size changed) raises `Protocol::VERSION` in the same change, and comes with a test that writes and reads the message. Two games with different protocol versions refuse each other with a clear message; they never guess.

## Branches and releases

- Work on a branch and open a pull request against `main`. `main` always builds and passes its tests.
- Every pull request gets a code review at its current head before it merges: a comment that starts "Code review at <sha>" and the `reviewed` label. A new commit removes the label.
- Releases are made from `main` by the maintainer: a release candidate first, a long two-game test of exactly that build, then the release ([docs/RELEASING.md](docs/RELEASING.md)). Until two games can be run there are no releases.

## Binaries and new dependencies

The repository holds no binaries: no DLLs, no compiled menu files, no archives, no Steam library. Everything that ships is built from source by the project's workflows.

A pull request gets the `needs-vetting` label when it comes from outside, or when it adds or changes any of these:

- a dependency (a submodule, an xmake package, a pinned commit or version),
- a URL that the build or a tool fetches,
- a workflow, a script the workflows run, or a git hook,
- a file that is not text.

For such a pull request the maintainer runs `vet.yml` on it. The vet reads the change, builds it on a throwaway GitHub machine that holds no secrets, and writes a report; only the summary of its "Trusted: report" job counts. When the report is clean and the maintainer has read it, the pull request gets the `vetted` label and a "Vetted at `<head>`" comment from the label workflow. A new commit removes the label, and the vet is run again. An outside pull request's code is not built or run anywhere else before that.

`vet.yml` runs from the `vet-runner` branch, which the maintainer sets to `main`'s commit first:

```
git push --force origin origin/main:refs/heads/vet-runner
gh workflow run vet.yml --ref vet-runner -f pr=<number> -f sha=<the pull request's head>
```

A new dependency must be source, pinned to a commit or a version, under a license compatible with the GPL-3.0, and the pull request says which license and where it was read. The ones in use:

| Dependency | How it is pinned | License |
| --- | --- | --- |
| CommonLibSSE-NG | submodule `lib/commonlibsse-ng`, a commit | GPL-3.0 with modding and linking exceptions (parts under MIT and a BSD-style license, in its `licenses/` folder) |
| ENet | xmake package, v1.3.18 | MIT |
| spdlog (and fmt) | xmake package, the version CommonLibSSE-NG asks for | MIT |
| DirectXMath, DirectXTK | xmake packages, the versions CommonLibSSE-NG asks for | MIT |

Pin CommonLibSSE-NG to a commit that is not one of its release tags. On GitHub's machines its build file looks for a release tag on the pinned commit and, when it finds one, downloads that release's prebuilt library instead of compiling the source. This project builds every dependency from source, so the pin stays between tags; a bump of the submodule checks that with `git -C lib/commonlibsse-ng describe --tags --exact-match`, which must find no tag.

The Steamworks SDK is not a dependency: it is not open source, and its terms do not fit a GPL repository. The plugin declares the few functions it calls by hand (`src/steam/SteamApi.h`) and finds them at run time in the Steam library the game itself loaded. Never add the SDK's headers or `steam_api64.dll` to the repository.

Code from other multiplayer mods is not copied in, whatever their license says, without the maintainer deciding it first: [docs/PRIOR-ART.md](docs/PRIOR-ART.md). Code from [F4Multiplayer](https://github.com/Jolley71717/F4Multiplayer), the sister project under the same license, is carried over freely; [docs/PORTING.md](docs/PORTING.md) keeps track.

## No personal data

The repository, its history and the players' logs are public or meant to be shared, so none of them holds anything that identifies a person or a machine.

- In logs, players are `player N`. `Redact::Alias` turns a name or an id into that; `Redact::MaskAddress` hides a network address; `Redact::Scrub` is a last net over every log line. A bug report's log should be safe to attach as it is.
- In commits, pull requests, comments and docs: no real names, handles, emails, Windows user names, machine names, Steam ids or IP addresses. Example addresses are names like `host.example`.
- The git hooks refuse a commit or a push that holds one of your own private strings. Install them once per clone:

  ```
  git config core.hooksPath tools/git-hooks
  ```

  They read `~/.skyrimcoop-private-patterns`: one case-insensitive regular expression per line (your real name, your Windows user name, your machine's name, your email). The file lives outside the repository and is never committed. The hooks refuse to run without it.

## License

SkyrimCoop is GPL-3.0-or-later with the modding and linking exceptions in [EXCEPTIONS](EXCEPTIONS). By sending a change you agree that it is under the same terms.
