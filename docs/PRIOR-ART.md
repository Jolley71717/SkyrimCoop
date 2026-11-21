# Prior art: other Skyrim multiplayer projects and their licenses

Other people have built multiplayer for Skyrim, some of it over many years. This page records what each project's own files say about its license, as read on 2026-11-21, so that nobody working on SkyrimCoop has to guess what may be looked at, learned from or reused.

It is a record of licenses, not a review of anyone's work, and it is not legal advice. Licenses change: read the file again before relying on a line here.

**The rule today: SkyrimCoop contains no code, assets or data from any of these projects, and none may be added without the maintainer deciding it first.** Comparing features and reading public documentation is fine.

## What was read

For each project: the license files in its public repository and what its own pages state. Source code of these projects was not read.

| Project | What it is | License, as its own files state it |
| --- | --- | --- |
| Skyrim Together Reborn (repository `tiltedphoques/TiltedEvolution`) | Co-op for Skyrim Special Edition with a separate server program | GPL-3.0 or later for the project; one component under LGPL 2 (details below) |
| SkyMP (repository `skyrim-multiplayer/skymp`) | A framework for multiplayer servers | A mix per subproject: AGPL-3.0, GPL-3.0 and MIT (details below) |
| Servers built on SkyMP (for example Keizaal Online) | Public roleplay servers | A fork of SkyMP carries SkyMP's licenses; the server's own site states "All rights reserved" for its content |
| Aurbis | A public roleplay server | Not stated where we looked |
| Tamriel Online | Co-op for the 2011 game | Nexus Mods permissions only; no source repository found |
| Skyrim Online (2011 to 2015, archived) | An early attempt for the 2011 game | Creative Commons BY-NC-ND 3.0 |

## Skyrim Together Reborn

**What its files say.** The repository's `LICENSE` file is a notice: the project is free software under the GNU General Public License, "either version 3 of the License, or (at your option) any later version". The same file says the launcher component (`code/launcher`) is under the LGPL version 2, because it uses code derived from another project (CitizenFX), under that project's terms. The README has a license section naming the GPLv3. The repository pulls in several libraries of the same organisation as git submodules; their license files were not read for this page. The mod's page on Nexus Mods carries Nexus's usual permission settings (no re-upload, ask before modifying); those describe the files hosted there.

**What building on its GPL code would allow.** SkyrimCoop is GPL-3.0-or-later, so GPL-3.0-or-later code can be combined with it. The GPL would allow:

- reading the code and learning how it solves a problem;
- copying or adapting parts of it into SkyrimCoop, provided the copyright notices and license notices stay, the changed files say that and when they were changed, and the source of the combined work stays available under the GPL;
- distributing the result, source and binaries, under the GPL.

**What it would not allow, or would change.**

- **The exceptions do not travel.** SkyrimCoop's `EXCEPTIONS` file gives extra permissions (for modding and for linking with the game and its tools). Those are granted by SkyrimCoop's copyright holders for SkyrimCoop's own code. Code taken from another GPL project comes under the plain GPL, without them, unless that project grants the same. Every file holding such code would have to say so, and the project as a whole could no longer be described as simply "GPL with these exceptions".
- **No relicensing.** Code taken in stays GPL for good. It could not later move to F4Multiplayer under different terms, or be offered under another license.
- **The LGPL launcher is a separate matter** with its own upstream terms; nothing about the GPL parts covers it.
- **The submodules are separate works.** Each has its own license, not read here; nothing may be assumed about them.
- **Names, logos, the Nexus page's text and images, and the project's servers** are not covered by a code license at all.
- **The GPL covers copying, not ideas.** Describing publicly how a feature behaves, or which engine function a problem comes down to, is not something a license grants or withholds; but writing code "from memory" of someone's source is copying in practice. Whoever reads another project's source for a problem should not be the one to write SkyrimCoop's code for it, unless the decision to build on that code has been made.

**The decision is the maintainer's.** The choice is between a project that is entirely its own and F4Multiplayer's (one author, one license with its exceptions, code moving freely between the two sister projects) and one that saves work on the hardest engine problems at the price of mixed terms and a standing duty of attribution. Until that is decided, the rule at the top holds.

## SkyMP

**What its files say.** `TERMS.md` states the project is "primarily distributed under the GPLv3 and AGPLv3 licenses" and that each subproject has its own license in its folder. The license files read:

| Subproject | License file says |
| --- | --- |
| `skymp5-server` | AGPL-3.0 |
| `skymp5-client`, `skymp5-front`, `skyrim-platform`, `savefile`, `skymp5-scripts` | GPL-3.0 |
| `papyrus-vm`, `libespm`, `skymp5-functions-lib`, `viet` | MIT |
| `serialization` | no license file |

The repository also has a contributor agreement (`CLA.md`) and a file of third-party licenses. Its README sums the terms up as a duty to disclose the source of forks.

**What that would and would not allow.** The MIT parts could be used in a GPL project with their notice kept. The GPL-3.0 parts are in the same position as described above. The AGPL-3.0 server code adds a duty to offer the source to users of a network service running it; AGPL code can be combined with GPL-3.0 code, but the combination carries the AGPL's network clause for that part, which would reach SkyrimCoop's dedicated server. The folder without a license file grants nothing. The same rule applies: nothing is taken without a decision.

## Servers built on SkyMP

A public fork of a GPL or AGPL repository carries those licenses for the code in it. A server project's own content (its site, its world, its art, its launcher if not published as source) is its own; where a site says "All rights reserved", that is what applies to it. Nothing here is of use to a co-op mod's code.

## Tamriel Online, Aurbis

No source repository with a license was found for either. Nexus Mods permission settings are not a source license. Without a license there is no permission to copy anything.

## Skyrim Online (2011 to 2015)

The archived repository's `LICENSE` file places the work under Creative Commons Attribution-NonCommercial-NoDerivs 3.0. That license forbids derivative works and commercial use, and is not compatible with the GPL. Nothing from it may be used.

## Projects this one does build on

For contrast, the code SkyrimCoop does use, each with a license read before it was added ([CONTRIBUTING.md](../CONTRIBUTING.md), "Binaries and new dependencies"): CommonLibSSE-NG (GPL-3.0 with modding and linking exceptions), ENet, spdlog with fmt, DirectXMath and DirectXTK (all MIT), and F4Multiplayer (GPL-3.0-or-later with the same exceptions, same maintainer). SKSE64 itself and the Address Library are not part of the repository or the release: players install them from their own pages.
