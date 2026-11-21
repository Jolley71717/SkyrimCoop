# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

## Project rules

The maintainer's standing rules for every agent are in AGENTS.md ("Project rules"). Read them before any change,
merge, release, rig test or message to anyone.

@AGENTS.md

## Build & Test

See CONTRIBUTING.md. In short:

```bash
git submodule update --init
xmake config -m releasedbg --skyrim_vr=n --skse_patch_safety=n
xmake build -y
build/windows/x64/releasedbg/SkyrimCoopTests.exe   # exit code = failed tests; a substring runs only matching tests
```

Anything that touches the game (`src/game`, `src/steam`, `src/main.cpp`) is only tested in a game, and no game test
has been run yet: say so in the pull request.

## Architecture Overview

See docs/DESIGN.md (how it is meant to work and the decisions so far), docs/PLAN.md (the phases and what each must
prove) and docs/PORTING.md (what came over from F4Multiplayer and what has to be redone for Skyrim).

Maintainer-local instructions, if present, are in CLAUDE.local.md (not committed).
