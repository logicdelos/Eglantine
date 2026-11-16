<!--
SPDX-FileCopyrightText: Copyright 2026 logicdelos
SPDX-License-Identifier: GPL-3.0-or-later
-->
<!-- lang: en-GB -->

# Fork changelog

This file lists the changes made in this fork. Upstream's [CHANGELOG.md](CHANGELOG.md) is left as it is, so that upstream merges stay clean.

Where a fix is described as a "likely fix", it was written to remove a cause found in the code or in a log. It has not been confirmed in the game. Where a fix says "checked" or "measured", the run is named in the line.

All runs were made on one Windows 11 laptop: AMD Ryzen 9 8940HX, NVIDIA GeForce RTX 5070 Ti Laptop GPU, 16 GB RAM. Only Fire Emblem: Fortune's Weave and Xenoblade Chronicles 2 were run. Fixes marked "Orchid" or "Lichii" were ported from those forks, with the original author named in the commit message.

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
