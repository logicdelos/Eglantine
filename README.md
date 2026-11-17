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
| Fire Emblem: Fortune's Weave | [fork-v0.1.0](https://github.com/logicdelos/Eglantine/releases/tag/fork-v0.1.0) | 2026-11-16 | Boots from a save into the reference scene at 1x and 2x resolution scale. 30.0 fps with the speed limit on, longest frame 76 ms at 1x and 69 ms at 2x, no error lines in the log, clean exit. |
| Xenoblade Chronicles 2 | [fork-v0.1.0](https://github.com/logicdelos/Eglantine/releases/tag/fork-v0.1.0) | 2026-11-16 | Boots to the title screen at 1x resolution scale. 60.0 fps with the speed limit on, longest frame 59 ms, no error lines in the log, clean exit. Play past the title screen was not part of this test. |
| Fire Emblem: Fortune's Weave | [fork-v0.2.0](https://github.com/logicdelos/Eglantine/releases/tag/fork-v0.2.0) | 2026-11-16 | Same test as fork-v0.1.0, same result: reference scene reached at 1x and 2x. 30.0 fps with the speed limit on, longest frame 57 ms at 1x and 72 ms at 2x, no error lines in the log, clean exit. End picture differs from the fork-v0.1.0 one by 0.019 at 1x and 0.009 at 2x (limit 0.06). |
| Xenoblade Chronicles 2 | [fork-v0.2.0](https://github.com/logicdelos/Eglantine/releases/tag/fork-v0.2.0) | 2026-11-16 | Same test as fork-v0.1.0, same result: title screen reached at 1x. 60.0 fps with the speed limit on, longest frame 41 ms, no error lines in the log, clean exit. End picture differs from the fork-v0.1.0 one by 0.002. |
| Fire Emblem: Fortune's Weave | [fork-v0.3.0](https://github.com/logicdelos/Eglantine/releases/tag/fork-v0.3.0) | 2026-11-16 | Same test as fork-v0.2.0: reference scene reached at 1x and 2x. 30.0 fps with the speed limit on, longest frame 64 ms at 1x and 73 ms at 2x, no error lines in the log, clean exit. End picture differs from the fork-v0.2.0 one by 0.007 at 1x and 0.009 at 2x (limit 0.06). |
| Xenoblade Chronicles 2 | [fork-v0.3.0](https://github.com/logicdelos/Eglantine/releases/tag/fork-v0.3.0) | 2026-11-16 | Same test as fork-v0.2.0: title screen reached at 1x. 60.0 fps with the speed limit on, longest frame 34 ms, no error lines in the log, clean exit. End picture differs from the fork-v0.2.0 one by 0.002. The unattended exit test also passed on this build: frame limit, time limit and a forced crash, on both tested games. |

Each run lasted about 3 minutes with Vulkan, GPU accuracy High, SMAA, the BUF scaling filter and docked mode. The release notes of each build carry the full table.

Test machine: Windows 11, AMD Ryzen 9 8940HX, NVIDIA GeForce RTX 5070 Ti Laptop GPU, 16 GB RAM.

Frame-rate figures in this repository always come with the scene, settings, hardware and method used. This project makes no general frame-rate promise.

### Baseline settings

There is one set of settings, the same for every game: the defaults. A game is tested on them, so you can load a game without changing any setting. This fork has no per-game settings, and the baseline never changes with the game's title. The Qt front end keeps a prompt inherited from upstream that offers recommended settings when you start a few other games; it applies only what you accept and is not part of the baseline. If a tested game needed a different value, that would be recorded as a failed result, not hidden behind a per-game setting.

| Setting | Baseline |
|---|---|
| Console mode | Docked |
| Graphics backend | Vulkan |
| GPU accuracy | High |
| Resolution | 1x |
| Anti-aliasing | SMAA |
| Scaling filter | BUF |
| Vertical sync | FIFO |
| Speed limit | on, 100 %; running faster than real time not allowed |
| Asynchronous shader building | on |
| Disk shader cache | on |

With "Allow running faster than real time" off (the baseline) the frame limit setting cannot make a game run fast: your speed limit cannot be switched off or exceed 100 %. What a game asks for itself is left alone. Frames above a game's own rate are a matter for frame generation, which this fork has not tested. Android is untested and keeps its own defaults.

Version 1 of the baseline made Docked, SMAA and BUF the defaults; the runs in the table above used them. Every game in the table is tested on the baseline, and a result on any other setting is labelled with the setting that differs: the 2x results above are the baseline with one declared variation, the resolution scale. Fire Emblem: Fortune's Weave and Xenoblade Chronicles 2 need no settings changes on the baseline. No promise is made for games that were not tested. The full list, its hash, its version history and the command-line options `--baseline`, `--baseline-allow`, `--print-baseline` and `--user-dir` are in [fork-notes/baseline.md](fork-notes/baseline.md).

## How this fork differs from Eglantine

- It tracks upstream daily builds; each release names the upstream build it is based on.
- It carries its own fixes for the two tested games and for stability, listed per release in [FORK_CHANGELOG.md](FORK_CHANGELOG.md).
- It ports selected fixes from other Eglantine forks, each credited to its author in the commit and in the release notes.
- It adds a log line that names what limits the frame rate, and a command-line build that can run a game unattended: `eden-cli.exe --run-seconds`, `--run-frames` and `--report`, with exit codes that say how the run ended. See the [guide](fork-notes/unattended-runs.md).
- From 0.4 (not released yet): one baseline of settings, the defaults, for every game; real frames by default, with "Allow running faster than real time" (`allow_speed_up`) off; no new game-specific code, with the existing exceptions frozen and listed ([accuracy policy](fork-notes/accuracy-policy.md)); and `eden-cli` options for scripted runs (`--version --json`, `--user-dir`, `--list-settings`, `--set`, `--control`). A speed change is kept only when a measurement on both tested games shows a gain; none of the nine 0.4 candidates did.
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
