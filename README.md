<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

# Eglantine (logicdelos fork)

An experimental fork of [Eglantine](https://github.com/Eglantine-Community/Eglantine), a Nintendo Switch 2 emulator. Its aim is to run a game about as fast as a hand-made PC port of that game, without any per-game work.

> **Status: experimental.** This fork has been tested with two games only, **Fire Emblem: Fortune's Weave** and **Xenoblade Chronicles 2**, on one Windows laptop. Other games, other operating systems and other graphics cards are untested here. Expect crashes, glitches and slowdowns.

This is an independent fork. It is not an official Eglantine release, and it is not affiliated with or endorsed by Nintendo.

[What is tested](#what-is-tested) | [How it differs](#how-this-fork-differs-from-eglantine) | [Download](#download) | [Building](#building) | [Roadmap](#roadmap) | [Legal](#legal) | [Credits](#credits) | [Licence](#licence)

## What is tested

| Game | Fork build | Date | Result |
|---|---|---|---|
| Fire Emblem: Fortune's Weave | to be filled from the 0.1 test run | | |
| Xenoblade Chronicles 2 | to be filled from the 0.1 test run | | |

Test machine: Windows 11, AMD Ryzen 9 8940HX, NVIDIA GeForce RTX 5070 Ti Laptop GPU, 16 GB RAM.

Frame-rate figures in this repository always come with the scene, settings, hardware and method used. This project makes no general frame-rate promise.

## How this fork differs from Eglantine

- It tracks upstream daily builds; each release names the upstream build it is based on.
- It carries its own fixes for the two tested games and for stability, listed per release in [FORK_CHANGELOG.md](FORK_CHANGELOG.md).
- It ports selected fixes from other Eglantine forks, each credited to its author in the commit and in the release notes.
- It adds tools for measuring: a log line that names what limits the frame rate, and command-line runs that end by themselves and write a result file.
- Long term, it adds a fast graphics path beside the existing one. See the [roadmap](ROADMAP.md).

Everything else behaves as upstream does. The [user handbook](https://github.com/Eglantine-Community/docs/blob/38e8d2a962a6137731e67fc8e89163691a4521cd/content/user/README.mdx) applies unchanged.

## Download

Windows builds are attached to this repository's [releases](https://github.com/logicdelos/Eglantine/releases). Releases before 1.0 are pre-releases. Each release carries a `SHA256SUMS` file.

For anything other than the two tested games, upstream Eglantine is the safer choice.

## Building

See the [general build guide](https://github.com/Eglantine-Community/docs/blob/38e8d2a962a6137731e67fc8e89163691a4521cd/content/Build.mdx). This fork is built and tested with Clang on Windows only.

## Roadmap

[ROADMAP.md](ROADMAP.md) lists the milestones. Each one ends in a release with at least one significant change to try, and each states how it is judged done.

## Reporting problems

Open an issue with the game's full name, the release tag, your processor, graphics card and driver version, and the log file. Do not upload game files, keys, firmware or account information.

## Legal

The purpose of this project is not to enable illegal activity. No games, keys, firmware or system files are included or linked. Only use games and system files you have obtained legally from hardware and games you own.

Nintendo Switch is a trademark of Nintendo. Fire Emblem and Xenoblade Chronicles are trademarks of their owners. Game names are used only to say what was tested.

## Credits

This fork builds on [Eglantine](https://github.com/Eglantine-Community/Eglantine), started by Camille LaVey, which builds on Eden, yuzu and Sudachi. Their copyright notices and third-party licences are retained.

Fixes are ported from these Eglantine forks, with thanks to their authors: [Lichii](https://github.com/zortos293/Lichii), [Orchid](https://gitlab.com/MythosOfs/Orchid), [Fritchii](https://github.com/Frizti/Fritchii), [Anka](https://github.com/EddizEge/Anka) and [notsapinho's fork](https://github.com/notsapinho/Eglantine). Each ported change names its original author and source.

## Licence

GPL-3.0-or-later. See [LICENSE.txt](LICENSE.txt) and the [LICENSES](LICENSES) directory.
