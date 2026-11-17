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

There is no second table. The baseline is read from the settings themselves: the default of every saved setting in the categories audio, core, CPU (including debug and unsafe), renderer (including advanced, hacks, extensions, debug and memory), system (including system audio) and GPU driver. Left out on purpose: the selected user (`current_user`), the console's device name (`device_name`), the volume controls (`volume`, `audio_muted`), controls, paths, profiles, the interface and settings that are not saved. Of the debug settings only those that change what the emulator executes or what the game sees are in: `debug_knobs`, `disable_macro_hle`, `disable_macro_jit`, `disable_web_applet`, `enable_all_controllers`, `perform_vulkan_check`, `quest_flag`, `use_auto_stub` and `use_dev_keys`. Logging, dump, server, secret, path and per-install serial settings are not. In addition, every `EDEN_NX2_*` environment switch must be unset, except the four measurement harness variables below.

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
| Speed limit | `use_speed_limit`, `speed_limit`, `allow_speed_up` | on, 100 %, speed-up off |
| Asynchronous shader building | `use_asynchronous_shaders` | on |
| Disk shader cache | `use_disk_shader_cache` | on |
| Multicore emulation | `use_multi_core` | on |
| Memory layout | `memory_layout_mode` | 4 GiB |

The full list is printed by the command line (below) and pinned, key by key, by the test `src/tests/common/baseline_settings.cpp`. Android is untested: it keeps its own defaults for console mode, anti-aliasing and scaling filter (handheld, FXAA, ScaleForce), it has no setting screen for `allow_speed_up` (the setting is off there too), and nothing in this document or its hashes has been run on it.

Four defaults depend on the platform: `cpu_backend`, `cpuopt_unsafe_host_mmu`, `fullscreen_mode` and `dyna_state`, and the renderer backend on some systems. The pinned hash below is for Windows and Linux on x86-64.

## Version and hash

The baseline has a version number and a hash (SHA-256 of the sorted `key=value` lines). The hash changes whenever a baseline default changes, a baseline setting is added or removed, or the platform changes one of the four platform-dependent defaults. Raise the version and add a history line below whenever the list changes; the test fails until the pinned list and the pinned hash match.

| Platform | Version 1 hash (126 entries) |
|---|---|
| Windows x86-64 | `57f9c3b773893b948dacca41e57da5d81d7502f203cd95f7e9bab65c3f4648db` |
| Linux x86-64 | `a4caf374a41520be82f05bdca2f46b9e7db2c1cb7ff4e81e34477f1dcedb032c` |

## History

| Version | Change |
|---|---|
| 1 | Console mode Docked, anti-aliasing SMAA and scaling filter BUF become the defaults on desktop builds (before: handheld, FXAA and ScaleForce, which Android keeps). These are the settings the games in the README table were run on in releases 0.1 to 0.3. Config files that kept the old defaults follow the new ones; a value you chose yourself is kept. The debug settings that change a run (`debug_knobs`, `disable_macro_hle`, `disable_macro_jit`, `disable_web_applet`, `enable_all_controllers`, `perform_vulkan_check`, `quest_flag`, `use_auto_stub`, `use_dev_keys`) and `allow_speed_up` (off) are part of the list. Version 1 was never released, so these entries were added to it instead of raising the version. |

## Command line (`eden-cli`)

- `--print-baseline` prints the baseline version, the hash and every entry as `key=value`, then exits. It reads no configuration.
- `--baseline` runs on the baseline: after the configuration loads, every baseline setting is reset to its default. Controls, paths and profiles are not touched. It refuses to start, with exit code 64 and a message, if an `EDEN_NX2_*` environment variable is set that `--baseline-allow` does not name.
- `--baseline-allow KEY[,KEY...]` declares variations. An item is an `EDEN_NX2_*` switch name (the prefix and at least one of A-Z, 0-9 and underscore) or a setting key; any other bare item is refused. `KEY=VALUE` also sets the setting after the reset, for example `--baseline-allow resolution_setup=6` for the 2x reference scene. An unknown key, a key that is not in the baseline or a value the setting does not accept is refused with exit code 64, with or without `--report`. A declared value is applied before the driver shader cache is set up, so a declared `private_driver_shader_cache` or `driver_shader_cache_size` takes effect. It needs `--baseline`. An option that is missing its argument (for example `--baseline-allow` last on the line) is a bad argument too.
- `--user-dir <path>` (or `--user-dir=<path>`) makes `<path>` the user directory of the run (configuration in `config/`, cache in `cache/`, saves, keys, logs), created when missing; a directory that cannot be used is a bad argument (exit code 64). An empty directory is a fresh install: it holds no stored choices, so the run is on the baseline, and `--baseline` is not needed for that. The report names the directory in `user_dir`. The Qt front end ignores `--user-dir`, `--baseline`, `--baseline-allow` and `--print-baseline`.

The run report (`--report`) has a `baseline` object: `version`, `hash`, `requested` (whether `--baseline` was given), `on_baseline`, `deviations` (key, baseline value, effective value, source), `declared` and `harness`. A run is on the baseline when every deviation is declared. Sources are `global config`, `per-game config`, `command line`, `environment switch`, `hardware fallback` and `program requirement`. A program requirement is a value the emulator sets for the program by itself (the 12 GiB memory layout of a Switch 2 program); it is listed but does not take the run off the baseline. The value of an environment switch is written to the report only when it is a plain number (at most 24 characters) or one of on, off, true, false, yes and no; any other value is written as `set`. A variable whose value is a path, a file name or a script (the name decides, before the value is looked at: a relative path can be `123` or `on`) is always written as `set`, in the deviations and in the `tunables` list alike. Shader ablation by the live control file is a deviation named `shader_ablation` (an environment switch, off by default) and cannot be declared.

### Measurement harness variables

Four environment variables belong to the measurement harness and are not baseline switches: `EDEN_NX2_DIAG_DIR`, `EDEN_NX2_FRAME_CAPTURE`, `EDEN_NX2_HIDE_WINDOW` and `EDEN_NX2_KEY_SCRIPT`. They are allowed without a declaration (they decide where evidence is written, whether frames are captured, whether the window is shown and which keys are sent; none changes what the emulator computes). A run names the ones that were set in `baseline.harness`, by name only, never by value. Every other `EDEN_NX2_*` variable is an ordinary switch.

The live control file can change the GPU accuracy (`gpu low|high`) while a game runs; the run then reports `gpu_accuracy` as a deviation with the source `live control file`, and the crash report snapshot is refreshed.

## Things that change a setting without you

- The shipped `dist/nx2_switches.txt` has no active line. Earlier it set `EDEN_NX2_SPLIT=2` and `EDEN_NX2_FENCE_BATCH=1`; both are what the code does when the switch is unset, so nothing changes except that the environment of a fresh install holds no `EDEN_NX2_*` variable. The Performance tab of the Qt front end writes switches to your own copy of the file only when you press Apply with a feature ticked.
- Switch 2 programs run with the 12 GiB memory layout (a program requirement, above).
- On the OpenGL backend the BUF scaling filter is drawn as bilinear; BUF itself is a Vulkan pass.
- If Vulkan is unusable, the Qt front end falls back to another backend (a hardware fallback).
- The game's own language and region choices are written to the system settings by the game and show up as deviations.

## Real frames by default

`allow_speed_up` (off in the baseline; "Allow running faster than real time" in the general settings) decides whether your speed limit may speed the game up. With it off the speed limit cannot be switched off and cannot exceed 100 %: a frame limit that is off, above 100 %, set to 0 or the turbo speed all give 100 %, and a limit below 100 % still slows the game down. What is held is your limit; what a game asks for itself is left alone: a game that queues a frame for presentation without waiting for the display (swap interval 0) gets the doubled composition rate it asks for, as upstream does, and the run log has one Info line when that multiplier changes. With it on, a limit that is off or above 100 % sends frames to the game as fast as the emulator can; a game that times itself by the clock draws more frames, a game that advances one step per frame runs fast.

So with `allow_speed_up` off the frame limit setting cannot make a game run fast. Frames above a game's own rate are a matter for frame generation; this fork has not tested it. `allow_speed_up` is a general setting, never per game. A run that turns it on is a declared variation: `--baseline-allow use_speed_limit=false,allow_speed_up=true` is the run with the frame limit off and speed-up allowed. The status bar shows the speed limit in force, not the value you chose. The pacing arithmetic of the display conductor is one function (`src/core/hle/service/vi/conductor_pace.h`) with tests that pin its results.

The pacing code (frame pacing, the vblank tick scale, the present mode, the frame limiter, the status bar) reads `Settings::SpeedLimited()` and `Settings::SpeedLimit()` and no longer reads `use_speed_limit` itself; the interface keeps the setting you chose.
