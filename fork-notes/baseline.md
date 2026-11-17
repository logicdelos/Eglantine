<!--
SPDX-FileCopyrightText: Copyright 2026 logicdelos
SPDX-License-Identifier: GPL-3.0-or-later
-->
<!-- lang: en-GB -->

# Baseline settings

Eglantine (logicdelos fork) has one baseline: the defaults of every emulation setting. Every game is tested on it, and it is what a fresh install runs. You load a game and play; no setting needs to change.

## The rule

- The baseline is one general setting set. It is not per game. The fork ships no per-game configuration, the tests use none, the baseline does not change with a game's title id, and opening a game does not create a per-game configuration file. One thing from upstream remains: when the Qt front end starts one of a few other games (the title ids in `src/common/game_presets.h`, none of them a tested game), it offers recommended settings. It applies only what you accept, and a run that accepted them is not a baseline run.
- A per-game configuration that you made yourself keeps working. A run reports it as a deviation with the source `per-game config`, and `--baseline` ignores it.
- If a tested game needs a value other than the baseline, that is a failed result on the baseline. It is recorded as such and is not solved with a per-game setting.
- A result on any other setting is labelled with the setting that differs (a declared variation). The 2x results in the README are the baseline with the resolution scale declared as 2x.

## What is in the baseline

There is no second table. The baseline is read from the settings themselves: the default of every saved setting in the categories audio, core, CPU (including debug and unsafe), renderer (including advanced, hacks, extensions, debug and memory), system (including system audio) and GPU driver. Left out on purpose: the selected user (`current_user`), the console's device name (`device_name`), the volume controls (`volume`, `audio_muted`), controls, paths, profiles, the interface and settings that are not saved. In addition, every `EDEN_NX2_*` environment switch must be unset.

Settings you are most likely to look at:

| Setting | Key | Baseline |
|---|---|---|
| Console mode | `use_docked_mode` | Docked (1) |
| Graphics backend | `backend` | Vulkan |
| GPU accuracy | `gpu_accuracy` | High |
| Resolution | `resolution_setup` | 1x |
| Anti-aliasing | `anti_aliasing` | SMAA (2) |
| Scaling filter | `scaling_filter` | BUF (16) |
| Vertical sync | `use_vsync` | FIFO |
| Speed limit | `use_speed_limit`, `speed_limit` | on, 100 % |
| Asynchronous shader building | `use_asynchronous_shaders` | on |
| Disk shader cache | `use_disk_shader_cache` | on |
| Multicore emulation | `use_multi_core` | on |
| Memory layout | `memory_layout_mode` | 4 GiB |

The full list is printed by the command line (below) and pinned, key by key, by the test `src/tests/common/baseline_settings.cpp`. Android keeps its own defaults for console mode, anti-aliasing and scaling filter (handheld, FXAA, ScaleForce).

Four defaults depend on the platform: `cpu_backend`, `cpuopt_unsafe_host_mmu`, `fullscreen_mode` and `dyna_state`, and the renderer backend on some systems. The pinned hash below is for Windows and Linux on x86-64.

## Version and hash

The baseline has a version number and a hash (SHA-256 of the sorted `key=value` lines). The hash changes whenever a baseline default changes, a baseline setting is added or removed, or the platform changes one of the four platform-dependent defaults. Raise the version and add a history line below whenever the list changes; the test fails until the pinned list and the pinned hash match.

| Platform | Version 1 hash |
|---|---|
| Windows x86-64 | `23977f27b2859cb0013f008fedaa07c2590d4a52706bcd3085cbcb2750463557` |
| Linux x86-64 | `8e074e3912f682100c5a9eb64dd71c29755f054a154f16b2debc8520dbb8d3eb` |

## History

| Version | Change |
|---|---|
| 1 | Console mode Docked, anti-aliasing SMAA and scaling filter BUF become the defaults on desktop builds (before: handheld, FXAA and ScaleForce, which Android keeps). These are the settings the games in the README table were run on in releases 0.1 to 0.3. Config files that kept the old defaults follow the new ones; a value you chose yourself is kept. |

## Command line (`eden-cli`)

- `--print-baseline` prints the baseline version, the hash and every entry as `key=value`, then exits. It reads no configuration.
- `--baseline` runs on the baseline: after the configuration loads, every baseline setting is reset to its default. Controls, paths and profiles are not touched. It refuses to start, with exit code 64 and a message, if an `EDEN_NX2_*` environment variable is set that `--baseline-allow` does not name.
- `--baseline-allow KEY[,KEY...]` declares variations. An item is an `EDEN_NX2_*` switch name or a setting key. `KEY=VALUE` also sets the setting after the reset, for example `--baseline-allow resolution_setup=6` for the 2x reference scene. An unknown key, a key that is not in the baseline or a value the setting does not accept is refused with exit code 64. It needs `--baseline`.
- `--user-dir <dir>` keeps the data, config and cache directories under `<dir>` (`<dir>/config`, `<dir>/cache`) instead of the installed locations. An empty directory is a fresh install: with `--baseline`, or with a configuration that has no stored choices, the run is on the baseline. The Qt front end ignores `--user-dir`, `--baseline`, `--baseline-allow` and `--print-baseline`.

The run report (`--report`) has a `baseline` object: `version`, `hash`, `requested` (whether `--baseline` was given), `on_baseline`, `deviations` (key, baseline value, effective value, source) and `declared`. A run is on the baseline when every deviation is declared. Sources are `global config`, `per-game config`, `command line`, `environment switch`, `hardware fallback` and `program requirement`. A program requirement is a value the emulator sets for the program by itself (the 12 GiB memory layout of a Switch 2 program); it is listed but does not take the run off the baseline.

## Things that change a setting without you

- The shipped `dist/nx2_switches.txt` has no active line. Earlier it set `EDEN_NX2_SPLIT=2` and `EDEN_NX2_FENCE_BATCH=1`; both are what the code does when the switch is unset, so nothing changes except that the environment of a fresh install holds no `EDEN_NX2_*` variable. The Performance tab of the Qt front end writes switches to your own copy of the file only when you press Apply with a feature ticked.
- Switch 2 programs run with the 12 GiB memory layout (a program requirement, above).
- On the OpenGL backend the BUF scaling filter is drawn as bilinear; BUF itself is a Vulkan pass.
- If Vulkan is unusable, the Qt front end falls back to another backend (a hardware fallback).
- The game's own language and region choices are written to the system settings by the game and show up as deviations.
