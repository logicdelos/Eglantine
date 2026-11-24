<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->

# Eglantine (logicdelos fork)

[![Latest release](https://img.shields.io/github/v/release/logicdelos/Eglantine?label=release)](https://github.com/logicdelos/Eglantine/releases/latest)
[![Licence](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue)](LICENSE.txt)

A performance-focused fork of [Eglantine](https://github.com/Eglantine-Community/Eglantine), the Nintendo Switch 2 emulator for PC.

The aim is that a game runs the way a PC game does from a launcher: high frame rate, steady frame pacing, and no setup per game.

> **Experimental.** This fork is developed and tested on one Windows 11 laptop (AMD Ryzen 9 8940HX, NVIDIA GeForce RTX 5070 Ti Laptop GPU, 16 GB RAM) with the four games named below. Expect bugs. For anything else, upstream Eglantine is the safer choice.

## Goals

- **Speed.** Use the PC's processor and graphics card efficiently, and remove work the emulator does not need to do.
- **Smoothness.** Find what causes a late frame and fix that cause.
- **Compatibility.** No game-specific hacks. A change has to be correct for every game, and the fork stays close to upstream.
- **Measured changes.** A speed change is kept only when a measurement shows a gain, and each one has a setting that restores the old behaviour.

## Progress

| Change | Measured result | Release |
|---|---|---|
| Render target copy cuts | Splatoon Raiders: image copies per frame 626 → 8, graphics card time per frame 9.7 → 4.0 ms | 0.7 |
| Precise reactive flushing | Xenoblade Chronicles 2: late frames in one test scene 11–14 → 0–3 | 0.7 |
| Graphics memory re-mapping fix | Xenoblade Chronicles 2: skipped draws in a 3-minute run 353,746 → 0 | 0.6 |
| Pipeline cache keeps what is in use | Xenoblade Chronicles 2: no pipelines evicted in a 3-minute run (before: evicted after 175 s) | 0.6 |
| Controller start-up crash fix | An access violation during early controller events no longer occurs | 0.6 |

Also in the fork:

- **Unattended runs.** `eden-cli.exe` can run a game for a set time or frame count and report how the run ended ([guide](fork-notes/unattended-runs.md)).
- **Diagnostics.** A log line that names what limits the frame rate, a crash dump on host crashes, an incident log ([guide](fork-notes/log-diagnostics.md)), and the last session's log zipped ready for a bug report.
- **Correctness fixes** ported from other Eglantine forks (see [Credits](#credits)).

Not every attempt worked. The fast graphics preview in 0.7 gave no measurable gain in Fire Emblem: Fortune's Weave and is off by default, and none of the nine speed candidates tried for 0.4 was kept.

Games used for testing: Fire Emblem: Fortune's Weave, Xenoblade Chronicles 2, Donkey Kong Bananza and Splatoon Raiders. Details per release are in [FORK_CHANGELOG.md](FORK_CHANGELOG.md); plans are in [ROADMAP.md](ROADMAP.md).

## Download

Windows x64 builds are on the [releases page](https://github.com/logicdelos/Eglantine/releases). Unzip anywhere and run `eglantine.exe`. Each release carries a `SHA256SUMS` file.

## Building

Follow the upstream [build guide](https://github.com/Eglantine-Community/docs/blob/38e8d2a962a6137731e67fc8e89163691a4521cd/content/Build.mdx). This fork is built and tested with Clang on Windows only.

## Reporting problems

Open an [issue](https://github.com/logicdelos/Eglantine/issues/new/choose) and pick the form that fits. Attach `eglantine-last-session.zip` from the log folder (File > Open Eglantine Folders > Log Folder); the emulator writes it when it closes. Do not upload game files, keys, firmware or account information.

## Credits

This fork builds on [Eglantine](https://github.com/Eglantine-Community/Eglantine), started by Camille LaVey, which builds on Eden, yuzu and Sudachi. Their copyright notices and third-party licences are retained.

It also borrows from other Eglantine forks. Each ported change names its original author and source in its commit message.

| Fork | What this fork took from it |
|---|---|
| [Lichii](https://github.com/zortos293/Lichii) | A shader fix for texture arrays, parts of its frontend, and several optional speed experiments |
| [Orchid](https://gitlab.com/MythosOfs/Orchid) | Correctness fixes in shader translation, render target copies, upscaling regions and service bounds checks |
| [Fritchii](https://github.com/Frizti/Fritchii) | The high-resolution host timer option and the empty-flush skip |
| [Anka](https://github.com/EddizEge/Anka) | The controller vibration range fix |

Thanks to their authors and contributors.

## Legal

This project is not affiliated with or endorsed by Nintendo. Its purpose is not to enable illegal activity. No games, keys, firmware or system files are included or linked. Only use games and system files you have obtained legally from hardware and games you own.

## Licence

GPL-3.0-or-later. See [LICENSE.txt](LICENSE.txt) and the [LICENSES](LICENSES) directory.
