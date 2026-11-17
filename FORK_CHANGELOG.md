<!--
SPDX-FileCopyrightText: Copyright 2026 logicdelos
SPDX-License-Identifier: GPL-3.0-or-later
-->
<!-- lang: en-GB -->

# Fork changelog

This file lists the changes made in this fork. Upstream's [CHANGELOG.md](CHANGELOG.md) is left as it is, so that upstream merges stay clean.

Where a fix is described as a "likely fix", it was written to remove a cause found in the code or in a log. It has not been confirmed in the game. Where a fix says "checked" or "measured", the run is named in the line.

All runs were made on one Windows 11 laptop: AMD Ryzen 9 8940HX, NVIDIA GeForce RTX 5070 Ti Laptop GPU, 16 GB RAM. Only Fire Emblem: Fortune's Weave and Xenoblade Chronicles 2 were run. Fixes marked "Orchid" or "Lichii" were ported from those forks, with the original author named in the commit message.

## 0.4.0 (unreleased)

Milestone 0.4, "Measured speed work": every speed change ships off by default behind an `EDEN_NX2_*` switch and is measured on and off before it is recommended. None of the switches below has been measured yet; the results are to be filled from the measurement table. Each is described in `dist/nx2_switches.txt`; `=1` turns it on.

### Speed experiments (measurement build; the release keeps only those with a measured gain)

- `EDEN_NX2_PIN_GPU_THREADS`: pins the front and back GPU threads to physical performance cores of their own and keeps the emulated CPU cores off them (Windows only). Results: to be filled from the measurement table.
- `EDEN_NX2_SKIP_EMPTY_FLUSH`: a fence flush skips its Vulkan queue submit when nothing was recorded since the last submit. Results: to be filled from the measurement table.
- `EDEN_NX2_HIRES_TIMER`: the host timer thread waits on a high resolution waitable timer (Windows 10 1803 or later only). Results: to be filled from the measurement table.
- `EDEN_NX2_ASTC_ONE_SUBMIT`: the GPU ASTC to BC transcode records the decode and the encode in one submit. Results: to be filled from the measurement table.
- `EDEN_NX2_SPLIT_AT_HIGH`: the GPU front/back thread split also runs at GPU accuracy High and with safe DMA. Results: to be filled from the measurement table.
- `EDEN_NX2_ZERO_PAGE_TRIM`: a background thread takes all-zero guest memory pages out of the process working set (Windows only). Results: to be filled from the measurement table.
- `EDEN_NX2_PROTECT_NO_LOCK`: guest memory protection changes first try the range without the map/unmap lock (Windows only). Results: to be filled from the measurement table.
- `EDEN_NX2_WARM_ADDON`: the shader precache build also reads the add-on packages of a `.nup`. Results: to be filled from the measurement table.
- `EDEN_NX2_ASTC_CHEAP_UPLOAD`: cheaper per-block set-up in the Vulkan GPU ASTC decoder, and the opaque-ASTC scan reads guest data in place. Results: to be filled from the measurement table.

### Command line

Additions for scripted runs of `eden-cli`. Not built or run yet at the time of writing.

- `eden-cli --version --json` prints one JSON object on one line on standard output and exits with 0: `name`, `fork_version`, `flavour`, `revision`, `branch`, `description`, `upstream_base`, `build_id`, `build_date`, `compiler`, `report_schema_version` and an `options` object (`discord_presence`, `mcp_server`, `opengl`, `update_checker`, `web_service`). `fork_version`, `flavour` and `upstream_base` are `null` when the build recorded none. It answers before the log, the crash handler, the configuration and the user directory are touched, and creates no window. Plain `--version` is unchanged. `--json` without `--version` is a bad argument.
- New CMake variable `EGLANTINE_UPSTREAM_BASE` (empty by default): the upstream commit a build is based on, reported as `upstream_base`. Nothing computes it; a release build passes it with `-DEGLANTINE_UPSTREAM_BASE=<commit>`.
- `eden-cli --user-dir <path>` (or `--user-dir=<path>`) uses that directory as the user directory for this run: configuration (`config/`), saves and NAND, keys, shader cache (`cache/shader/`), logs, screenshots, the same layout as the portable `user` directory. It is applied before the log, the crash handler, the system or the configuration read or create anything in the default directory, so the default directory is left alone. A missing directory is created. One that cannot be created or written to is a bad argument (exit 64, nothing else started, so no report is written). An abbreviation of the option is read the way getopt reads it, and the run is refused if the early scan and the full parse would read it differently. It can be given once. The report has a new top-level `user_dir` with the directory the run used. The paths of other emulators that Eglantine can import from are not moved.
- `eden-cli --list-settings` prints every setting of the settings registry as one JSON object per line on standard output, then exits with 0: `name`, `category`, `type` (`bool`, `string`, `enum`, `integer`, `float`), `value`, `default`, `saved`, for a ranged number `min` and `max`, for an enum `value_name`, `default_name` and `choices` (number and name of each). The values are those after the configuration file, `--set` and the other command line options.
- `eden-cli --set name=value` (repeatable) sets a setting for this run only. It is applied right after the configuration file is read, before the driver shader cache and everything else reads a setting, and it is never written back: the configuration file is saved before the override. The other command line options (`-n`, `-s`, `--user` and so on) still have the last word. Booleans take `true`, `false`, `1` or `0`; an enum takes its number or its name, in any letter case; whole numbers and floating point numbers are checked against the range of the setting; an empty string sets the default. An unknown name or an invalid value is a bad argument (exit 64, the message names the setting and what is accepted), and so is the same name twice. The report lists the overridden settings with their value in `settings.overrides`. The command line has no per-game configuration, so nothing else is loaded after the configuration file.
- Credentials are never shown by these options: a setting whose name contains `token`, `password`, `passwd` or `secret`, or ends in `_key` (today `mcp_token` and `eden_token`), appears in `--list-settings` as its name, category, type and `"redacted":true` without a value or default, in `settings.overrides` of the report as `"<redacted>"`, and its value is not repeated in an error message. The settings log already hid the two tokens; it now uses the same rule (`common/settings_secret.h`).
- `eden-cli --control <name>` opens a control channel for one local client: the named pipe `\\.\pipe\<name>` on Windows, `<name>.sock` in `XDG_RUNTIME_DIR` (or a private directory under `/tmp`) elsewhere. A request is one JSON line `{"id", "cmd", "args"}` and the reply is one line `{"id", "ok", "result"}` or `{"id", "ok": false, "error": {"code", "message"}}`. Commands: `status` (state, frames, shader builds in progress, script progress), `press` and `release` (SDL scancode number or name), `screenshot` (writes a PNG file) and `stop` (ends the run through the normal exit path, so the report is written). Off unless the option is given; no network port. The Windows pipe refuses remote clients and is open to the user of the process only. The channel has a thread of its own and never waits for the emulation or GPU threads: commands that act on the emulator run at the next iteration of the main loop, and a request the main loop does not take within 10 s is answered with a `busy` error. When a client disconnects, the keys it pressed without `hold_ms` and did not release are released, and nothing a later client pressed is touched. A press with `hold_ms` (up to 60 s) is a tap that ends at its deadline whether or not the client is still connected. A screenshot is written to a new temporary file and moved into place, so an existing file is replaced only with `"overwrite": true`.
- The control channel has two more commands, `get` and `set`, on the settings registry that `--list-settings` and `--set` use. `get` takes `name` and returns the setting as one line of `--list-settings` does, plus `runtime_modifiable`. `set` takes `name` and `value` (a boolean, a number or a string, checked like `--set`) and changes the setting in memory for the rest of the run, then applies it to the running game as the settings dialog does; the channel does not write the configuration file. Only a setting that the registry marks as changeable while the game runs is accepted, anything else is refused with `not_modifiable`. A credential is neither shown nor changed: `get` returns its name without a value, and `set` refuses it.
- `EDEN_NX2_KEY_SCRIPT` steps can wait for an event instead of a time: `frame>=600:4` (frames presented), `after first_frame+3:4`, `after shaders_idle+5:4` (the shader build queue stayed empty for 5 s) and `after prev+2:-4` (after the previous step). `@120` at the end of a step gives up after 120 s (300 s for an event step without it). A step that gives up ends an unattended run with reason `timeout`, exit code 124, and a detail naming the step; the remaining steps are not run. Steps still run in order. The timed forms `<seconds>:<code>` and `f<N>:<code>` work as before.
- The report gets a `steps` list when `EDEN_NX2_KEY_SCRIPT` is set: for each step its `when`, `key`, `action`, `status` (`fired`, `timed_out`, `skipped` or `pending`) and when it fired, in script seconds, run seconds and frames. The crash report does not include it. The report schema stays at version 1: `steps` and `user_dir` are optional fields.
- Because of the new option names, some abbreviations of long options are now ambiguous and getopt refuses them: `--s` (`--singlecore`, `--set`), `--co` and `--con` (`--config`, `--control`), `--us` and `--use` (`--user`, `--user-dir`), `--pr` (`--program`, `--print-baseline`), and `--b` to `--baselin` (`--baseline`, `--baseline-allow`). The full names, and `--conf`, `--pro`, `--baseline-` and longer, work as before. Every other abbreviation that names one option is read as that option, by getopt and by the early scan of `--user-dir` and `--version --json` alike.
- A script in `EDEN_NX2_KEY_SCRIPT` with an item that has no colon or a number that cannot be read is now dropped as a whole, and an unattended run ends with exit code 1 (startup error); before, such items were skipped or ended the process with an uncaught exception.
- Presented frames are now counted in every run, not only in unattended ones, so `frame>=` works without `--report`.

### Baseline settings

- The baseline: the defaults of every emulation setting are the settings every game is tested on, there is one set for all games, and it does not change with a game's title. It has a version (1) and a hash, listed with its history in [fork-notes/baseline.md](fork-notes/baseline.md). `eden-cli` gets `--print-baseline`, `--baseline` and `--baseline-allow KEY[,KEY...]` (also `KEY=VALUE`). With `--baseline`, a `--set` of a baseline setting that `--baseline-allow` does not declare is refused at start (exit 64, before the user directory is touched, also with `--version --json`), wherever the options stand on the command line. The run report has a `baseline` object with the version, the hash, whether the run is on the baseline and every deviation with its source.
- Default console mode is Docked, default anti-aliasing is SMAA and default scaling filter is BUF on desktop builds (they were handheld, FXAA and ScaleForce; Android keeps those). A configuration file that holds a value stored as the default follows the new default; a value chosen by the user is kept.
- `dist/nx2_switches.txt` ships with no active line. `EDEN_NX2_SPLIT=2` and `EDEN_NX2_FENCE_BATCH=1` were set at every start of the Qt front end; both are what the code does when the switch is unset, so only the environment changes.
- Opening a game in the Qt front end no longer writes an empty per-game configuration file.
- Real frames by default: the new setting `allow_speed_up` (off, general, never per game; "Allow running faster than real time") decides whether your speed limit may speed a game up. With it off the speed limit cannot be switched off and cannot exceed 100 %: a limit that is off, above 100 %, 0 or the turbo speed gives 100 %, and a limit below 100 % still slows it down. The multiplier a game asks for itself when it queues a frame without waiting for the display (swap interval 0, a doubled composition rate) is not capped, as in upstream; the log has one Info line when it changes. With it on, behaviour is what it was. The status bar shows the speed limit in force. With it off the frame limit setting cannot make a game run fast; frames above a game's own rate are a matter for frame generation, which this fork has not tested. Android keeps its own defaults, is untested, and has no screen for the setting.
- The baseline list gains the debug settings that change a run (`debug_knobs`, `disable_macro_hle`, `disable_macro_jit`, `disable_web_applet`, `enable_all_controllers`, `perform_vulkan_check`, `quest_flag`, `use_auto_stub`, `use_dev_keys`) and `allow_speed_up`; version 1 was never released, so it stays version 1 (126 entries).
- A declared value is applied before the driver shader cache is set up. A bare `--baseline-allow` item that is neither a baseline setting nor an `EDEN_NX2_` switch name is refused. A `eden-cli` option missing its argument is a bad argument with exit 64, with or without `--report`. The early scan for `--user-dir` and `--version` follows getopt (arguments of options, abbreviations, `--`), and `--version --json` prints only when the options the scan can see are in order.
- The report's `baseline` object has a new `harness` list: the measurement harness variables that were set (`EDEN_NX2_DIAG_DIR`, `EDEN_NX2_FRAME_CAPTURE`, `EDEN_NX2_HIDE_WINDOW`, `EDEN_NX2_KEY_SCRIPT`), by name, never by value. They do not need a declaration. The value of any other environment switch is written as `set` unless it is a plain number or on, off, true, false, yes, no. Shader ablation by the live control file shows as the deviation `shader_ablation`, and the live `gpu` command as a deviation of `gpu_accuracy`; the crash report snapshot is refreshed for both. A successful control channel `set` names its setting with the new source `control channel` and refreshes the crash report snapshot too; `settings.initial` still holds the values of the start.
- A variable that holds a path, a file name or a script is reported as `set` whatever its value looks like (the name decides first: a relative path can be `123` or `on`), in the deviations and in the `tunables` list. The crash report snapshot is now always refreshed when shader ablation begins: a request that arrives while the report is being updated is carried out when that update ends, and is no longer dropped.
- `eden-cli --version --json` with a mistake in one of the options of this fork (an option missing its argument, or a value given to `--baseline`, `--json`, `--print-baseline` or `--list-settings`) is now a bad argument with exit 64, like the same mistake without `--version --json`; it used to print the build identity and exit 0.
- With `--baseline` the configuration file is read and never written, whichever way the run ends, so it is byte-unchanged afterwards; when it does not exist, none is created and the defaults are used. A declared `--set` value that differs from the baseline is a deviation with the source `command line`.

## 0.3.0 (2026-11-16)

Milestone 0.3, "Unattended runs": a scripted run ends by itself, says why in its exit code, and leaves one report file. Verified on the as-shipped folder: the release gate passed against the `fork-v0.2.0` baseline (three runs, no new error lines, pictures within the limit), and the unattended exit test passed 10 of 10 cases (bad arguments, frame limit, time limit and forced crash on Fire Emblem: Fortune's Weave and Xenoblade Chronicles 2). Other error paths were read from the code, not provoked.

### New

- `eden-cli` options for unattended runs (the build target is called `yuzu-cmd`): `--report <file.json>` writes one JSON report when the run ends (the file must not exist, its directory must), `--run-seconds <n>` ends the run after n seconds, `--run-frames <n>` ends it after n presented frames. Any of the three makes the run unattended. `--run-frames` alone also gets a safety limit of 900 s.
- Exit codes of an unattended run: 0 normal end or frame limit, 1 startup failure, 64 bad arguments, 70 guest crash (a break without notification, a fatal error with a screen, an unhandled CPU exception), 71 Vulkan device lost, 72 pipeline build failure, 73 emulator crash, 74 report not written, 75 restart requested by the game, 124 `--run-seconds` reached, 130 interrupted by Ctrl+C, Ctrl+Break or SIGTERM. A run with none of the three options keeps its old exit codes. `--help` lists them.
- The report (schema version 1) holds the build (revision, branch, description, build id and date, compiler, product name, version, display version and flavour), the title id, the limits, the elapsed time, a summary of the whole run and one entry per 10 s window (frame rate, frame time mean, 50th, 95th and 99th percentile and maximum, frames over 25, 50 and 100 ms, time of the GPU thread, back thread and Vulkan worker split into work, idle and waits, guest waits for the GPU, shader build waits, busy time per guest core, and the limiter verdict of the log), the count of Error and Critical log lines, how the run ended (`exit.reason`, `exit.code`, `exit.detail`, `shutdown_complete`, `metrics_complete`), the `EDEN_*`, `NX2*` and `EGLANTINE_*` variables that were set, and the settings that affect speed, at start and at the end.
- An emulator crash during an unattended run with `--report` leaves a report with `exit.reason` `host_crash`, `shutdown_complete` false and `metrics_complete` false, and the process exits with 73. The crash handler publishes this report first, before it writes the log line and the minidump; the report's `exit.detail` holds only the exception code, the faulting address and the thread id (module, offset and stack are in the log line), the summary holds only the frame count, `windows` is empty and `settings.final` is null. The report writer takes no lock and allocates nothing: the file is opened and the text formatted before the run. If that report cannot be written, the process exits with 74 instead. A fault inside the log line or the minidump still ends with 73, and a watchdog ends the process 15 s after the crash if that optional part hangs.
- The crash report stays armed until the normal report is on disk, so a crash while the run is ending still leaves a report. A report is never replaced: the file must not exist when the report is published, on every platform.
- If a stop was asked for and the shutdown does not finish in 30 s, the report is written with `shutdown_complete` false and the process exits with the code of the reason. The reason of a closed window or a restart is recorded when the shutdown begins, so this limit also covers it.
- Startup failures of an unattended run (the window cannot be created, a room cannot be joined) now write the report with `exit.reason` `startup_error` and exit with 1. Ctrl+C and SIGTERM are handled from the start of the run, before the configuration is read.
- Test switch `EDEN_NX2_TEST_CRASH_AFTER_FRAMES=<N>` crashes the emulator on purpose after N presented frames, to test the crash path. Off unless set. See `dist/nx2_switches.txt`.

### Fixes

- Crash handler on Windows: an unhandled exception, `abort()` or a failed CRT parameter check writes one Critical "Host crash" line with the code, the faulting address and the stack as module and offset, and the thread, then writes `eden_crash.dmp` in the log directory (`EDEN_NX2_CRASH_DUMP=<dir>` moves it). The dump is now always written, and the earlier `eden-crash-<pid>.dmp` name and the stderr stack print are gone. Ported from Lichii #1453 (zortos293), with the later fixes of #1710 and #1728. The forced-crash cases of the unattended exit test ran this path on both tested games.
- Log file: a thread flushes it once a second, so lines below Error no longer wait in the buffer until a hang or kill. Rotation renames the log over the previous one in one step; if the log cannot be moved, the session writes to `eden_log_2.txt`, and if that cannot be moved either it appends. A failed flush no longer recurses. Ported from Lichii #1771 (zortos293) and #889 (Eduardo Gomes).

### Internal

- `Common::RunOutcome` keeps the reason a run ended, with a precedence so that a late "window closed" never hides a crash. It does nothing until an unattended run begins.
- The per-window figures of the log line are kept as numbers for the report. The text of the log line is unchanged.
- Not in this version: a registry of tunables with `--set` and `--list-tunables`, and a control channel. The report lists only the variables that were set.

## 0.2.0 (2026-11-16)

Fixes ported from other forks. The original author and the source are named in each commit message and in the release notes. No fix in this section was confirmed in a game on its own: the release test ran with all of them together, and its results are in the release notes.

Game-specific:

- Fire Emblem: Fortune's Weave: texture arrays indexed by a found bit are limited to 32 elements instead of 1024 (Lichii #1914).
- Fire Emblem: Fortune's Weave: a depth target padded to whole GOBs is created as the square texture the game samples, so one image serves both (Lichii #1868).
- Xenoblade Chronicles 2: in handheld mode the 3D scene is no longer zoomed in 2x; the upscale stand-in copies only the regions the game names (Orchid).

Graphics:

- Vulkan: a render target that got no memory is left out of the framebuffer instead of binding a null view (Lichii #1766).
- Vulkan: the zero fill of the null buffer is ordered before every read of it (Lichii-TGM, via Orchid).
- Resolution scaling: an image whose scaled size would pass the GPU's image or framebuffer limits stays at native resolution (Lichii #1890).
- Texture cache: a CPU write to a page without images no longer waits for the texture cache lock (Lichii #1789).
- Texture cache: a copy between depth and color formats that spans several layers is done one layer at a time (Orchid).
- Software 2D blit: Switch 2 surfaces use the Switch 2 tile order (Eglantine-steamdeck, via Orchid).
- Shaders: FFMA and HFMA2 apply FMZ before SAT, and half-precision FMZ translates (Lichii #745, via Orchid).

Audio and input:

- Audio: ADPCM samples are decoded with the correct sign (Lichii #1196).
- Input: vibration amplitudes are kept between 0 and 1, and a bad frequency becomes the default (Anka).

Core and services:

- Kernel: a 64-bit address arbiter read whose end wraps around is rejected (lichii-m5s, via Orchid).
- Kernel: a process joins the kernel object list only after it loads (Lichii #1574).
- Stop: guest threads are suspended before power-off, so stopping a game should no longer end in a Userspace PANIC (2010-0212) (Lichii #1450).
- JIT: on Windows the code space commit is retried for up to about 10 seconds instead of ignoring a failure (Lichii #310).
- File system: file and storage reads and writes longer than their IPC buffer return an error, as on the console (Orchid).
- Services: bytes a service handler does not write in an out buffer keep the guest's values (Lichii #1802).
- Services: an unknown command answered by the automatic stub is logged instead of asserting (Lichii #1919).

## 0.1.0 (2026-11-16)

Based on upstream build daily-20261116.

### Fixes

Game-specific:

- Fire Emblem: Fortune's Weave: dialogue scenes drew a black 3D background with the present queue on, because the queue's semaphore words did not hold the channel's sync point value. They do now. Likely fix, not confirmed in the game.
- Fire Emblem: Fortune's Weave: with GPU accuracy High and Sync Memory Operations on, one scene stayed at about 23.5 fps. A semaphore acquire that an already issued release will meet now continues at once, and a command-list sync bit no longer waits for the host GPU. Measured in one scene, 60 s windows, on the laptop above: 30.0 fps after, 23.5 to 23.8 fps before. Other scenes and other hardware were not measured.
- Fire Emblem: Fortune's Weave: the game crashed about 20 s in at 4x resolution scale. A failed image creation no longer ends the session: the emulator evicts, retries, then falls back to native resolution. Likely fix, not confirmed: the 4x run has not been repeated on this build.
- Xenoblade Chronicles 2: the game aborted with error 2010-0011 before the first frame. The alias region of Switch 2 processes is now 128 GiB (Orchid).
- Xenoblade Chronicles 2: the game aborted at boot with a null call in `NativeWindow::Acquire`. Window buffers are now aligned to 4 KiB for every title, which replaces a special case for Star Fox. Star Fox was not retested (Orchid).
- Xenoblade Chronicles 2: the game aborted with error 2162-0001 because its data sits in add-on packages of the container, and none were listed. The container's add-on and sibling program data are now mounted. Add-on listing for Switch 1 games with DLC was not retested (Orchid).
- Xenoblade Chronicles 2: title screen colours, cloud textures and font were scrambled. Textures that ask for the Maxwell layout now keep it (Orchid).
- Xenoblade Chronicles 2: block noise in the bottom right corner of the upscaled image. The upscaler lowering now turns a NaN into zero instead of passing it on.

Both tested games:

- `Unmapped Device ReadBlock` errors in the log are gone for uniform buffers that the guest maps only in part. Counted in a 96 s run: 16 to 0 in Fire Emblem: Fortune's Weave and 8 to 0 in Xenoblade Chronicles 2. Nothing was wrong on screen.
- Guest memory on Windows is committed as it is used. Before, 14336 MiB were charged against the system commit limit at boot. Measured 40 s into a run of each game: 9848 MiB committed.
- Sessions that the guest closes are now destroyed, with their threads. Three session classes are still kept until shutdown. Likely fix for a leak, not confirmed in the game.

Stability and correctness:

- Four waits that could hold the emulator for ever are bounded: swapchain recreation on a window with no area, the wait for the GPU thread at shutdown, a host sync point wait, and audio back-pressure. The log file is flushed from the crash handler of both frontends, so the lines before a crash reach the file.
- Bounds checks were added for these guest-chosen values: register index in the command pushers, Nvdec and Vic, event id in `nvhost_ctrl`, SSID length in `SetNetworkProfile`, and a rename across directories with a missing source. Keyblob lines in a keys file with an index or length outside the tables are skipped.
- Guest writes that cross a 2 KiB chunk are now reported to the GPU caches. Before, the second chunk was missed.
- `VK_EXT_vertex_input_dynamic_state` was removed on every NVIDIA and Intel Windows driver because the driver version was compared in the wrong packing. Checked on the RTX 5070 Ti: the warning is gone and both games render for 95 s as before.
- Audio: the played-sample count survives Stop and Start, so buffers are released again after a restart. The volume setting is applied once, not twice (50 % played at about a seventh). The volume curve above 100 % starts at 1.0. Muting a target mutes only the active output target. Two users of the null sink no longer share one sample counter. The render manager sleeps when idle and is paced when no running sink stream paces it. Likely fixes, not confirmed in the game: both games were only booted and played for 96 s, and these cases were not triggered on purpose.
- `InstallNCA` and `InstallNSP` fail on a short read or write instead of leaving a truncated file.
- Video: a pipeline built in the background is used instead of discarded. Query pools are locked against the fence thread while they grow. The anti-aliasing pass is replaced only after the frames that may use it have finished. Likely fixes, not triggered on purpose.
- Texture cache: garbage collection waits for the GPU once instead of once per image. An unswizzle whose image is deleted is cancelled. Not triggered on purpose.
- File system: a member of a Switch 2 container is decompressed once when several threads miss on it. A read-only file reads under its own lock. A solid NCZ is decompressed into a temporary file instead of memory.
- Core: a failed fallback allocation of guest memory now stops with a message instead of continuing with a null base. Code cache invalidation between cores no longer writes another core's state.
- Thread names are set on every Windows compiler, so profilers and debuggers see them in MinGW builds.
- `eden-cli` keeps a redirected stdout or stderr instead of reattaching it to the parent console.
- Switch 2 accuracy: macro arguments sent with an incrementing header are collected like the others, and the cube-array view assert that fired on valid image descriptors is removed. Both games ran without differences in the verify mode, with no macro assert.
- Switch 2 graphics classes follow the documented layouts for the host semaphore reduction, a DMA launch with no transfer, signed compute reductions and local memory sizes. The GPU characteristics block of the Switch 2 follows the documented layout. Not tested in either game. `EDEN_NX2_CLASS_FIXES=legacy` and `EDEN_NX2_GPU_CHARS=legacy` restore the old behaviour.
- Ported from other forks, no gain measured: guest reads of pages that no GPU write touched skip the cache locks (Lichii #1479; no change measurable in Fire Emblem: Fortune's Weave); NCA sections are opened on first use (Lichii #1784; untested, because both games are Switch 2 containers); multi-range buffers unused for 60 frames are retired (Orchid); sync point waits whose value moved are woken (Orchid; the freeze it fixes was not seen in either game).

### New

- Scaling filter "BasicUpscaler Formula (BUF)": a sharpening upscaler based on AMD FidelityFX Super Resolution 1.0 (MIT). Its sharpening strength is the setting `buf_strength`, 0 to 200 %, default 100 %. It sharpens only when the window is larger than the game's image.
- `EDEN_NX2_CAPTURE_SIZE=<w>x<h>` sets the size of a frame capture, so a capture can be larger than the game's image.

### Tools and diagnostics

- The log gets one line every 10 s of play that names what limits the game: frame rate, a verdict and the figures behind it (time of the GPU thread, Vulkan worker and guest cores, waits for the graphics card, shader builds, presentation, file reads, audio underruns). It is always on.
- A second line every 10 s reports memory use by owner: process private bytes and working set, guest RAM committed, JIT code, staging buffers, texture cache and GPU heaps.
- A "Shader builds" line reports where each pipeline build's time goes and what waited for it. It reports only and changes no behaviour.
- The sampler attributes time in JIT code to the guest block that ran. It is active only with `EDEN_NX2_SAMPLE`.
- Windows MinGW builds with Clang write a `.pdb` next to each executable (option `MINGW_PDB`, on by default). The executable stays stripped.
- `tools/nx2/host_threads.py` lists every host thread of a running or hung emulator from outside it: name, CPU use, priority and call stack.
- A stall watch writes every thread's call stack to a file in the log directory when a running game presents no frame for 60 s (`EDEN_NX2_STALL_SECONDS`, 0 turns it off). Checked with a 1 s limit, which fired during the boot of Fire Emblem: Fortune's Weave. The default stayed silent in normal play.
- Unimplemented service commands are counted per command and listed once at shutdown.
- Unmapped device reads log the host call stack once per start address, at most 16.

### Internal

- Lab-only tooling: a method census and spike recorder for graphics-class features, a live key and quit file for scripted runs, and environment switches named `EDEN_NX2_*` for each behaviour change above. They do nothing unless set.
- `docs`: a system map of the emulator in `fork-notes/system-map/`, with 146 defect candidates read from the source. A second reading found 14 confirmed and 43 confirmed under narrow conditions. Nothing in it was reproduced unless the page says so.
- `NX2_NOTES.md` gets rows 151 to 158 for the fixes above.
- `src/audio_core/out` is tracked. It was ignored by mistake before.
- Local builds: dependency downloads use `aria2c` when it is on `PATH`, and a hash mismatch is retried.
- MinGW executables no longer hold the absolute source and build directory in file names, assert text or the `.pdb` reference.
