# Roadmap

Eglantine (logicdelos fork) is a fork of [Eglantine](https://github.com/Eglantine-Community/Eglantine), a Nintendo Switch 2 emulator.

**Tested games.** This fork has been tested with two games only: **Fire Emblem: Fortune's Weave** and **Xenoblade Chronicles 2**. Other games may work as they do upstream, may work better, or may work worse; nobody has checked. Reports for other games are welcome.

**Baseline.** Every game is tested on one set of settings, the defaults, so you can load a game without changing any setting; there are no per-game settings. A result on any other setting is labelled with the setting that differs. See [fork-notes/baseline.md](fork-notes/baseline.md).

**Tested hardware.** One Windows 11 laptop: AMD Ryzen 9 8940HX, NVIDIA GeForce RTX 5070 Ti Laptop GPU, 16 GB RAM. Linux, macOS, AMD and Intel graphics are untested in this fork.

## Where this is going

The long-term goal is an emulator that runs a game about as fast as a hand-made PC recompilation of that game, without any per-game work: you give it the game you own and press start. No per-game build, patch or table may be needed for a game to run.

The plan is to keep the existing emulator as it is and add a fast graphics path beside it. Games on Switch hardware draw through one graphics library. Today the emulator imitates the graphics chip underneath that library. The fast path answers the library's calls directly with Vulkan, and falls back to the existing path for anything it does not cover.

Milestones 0.1 to 0.6 improve the emulator as it exists today. Milestones 0.7 onwards build the fast path. Whether 0.7 onwards goes ahead depends on measurements taken before it; if they show no worthwhile gain on real hardware, this roadmap will say so and change.

## How releases work

- Every milestone ends in a GitHub release with a Windows build, and every milestone contains at least one significant change you can try.
- Tags are `fork-vX.Y.Z`, so they cannot be confused with upstream's tags. Everything before 1.0 is marked as a pre-release.
- Each release note lists what changed, what was measured and how, known problems, and the upstream build the release is based on.
- Speed figures are published only with the scene, settings, hardware and method used to get them. A change meant to make things faster is kept only if a before-and-after measurement shows it does.

## Milestones

Status values: **planned**, **in progress**, **released**.

### 0.1: Fork baseline
Status: released as [fork-v0.1.0](https://github.com/logicdelos/Eglantine/releases/tag/fork-v0.1.0) on 2026-11-16. The test table is in the release notes.

**Significant change:** the fork's own fixes on top of upstream build `daily-20261116`.

- Xenoblade Chronicles 2: the game no longer aborts at boot (a larger alias region, 4 KiB window buffer alignment, add-on content packages mounted from the game container); the title screen colours, textures and font are decoded correctly; corner noise is removed (an upscaling step now turns invalid values into zero).
- Fire Emblem: Fortune's Weave: dialogue scenes no longer draw a black 3D background with the present queue on (a likely fix, not confirmed in the game); a scene that stayed at about 23.5 fps with GPU accuracy High and Sync Memory Operations on now measures 30.0 fps on the tested laptop.
- Four waits that could hang the emulator for ever are now bounded; the log is flushed on a crash.
- Audio: sample count kept across stop and start, volume applied once, idle pacing.
- Memory: guest memory committed as it is used on Windows; a failed image creation at high resolution scales should no longer end the session (a likely fix for a crash seen in Fire Emblem: Fortune's Weave at 4x, not confirmed).
- A log line every 10 seconds that names what currently limits the frame rate.

Done when: both tested games boot to their reference scenes on a clean build.

### 0.2: Fixes from sibling forks
Status: released as [fork-v0.2.0](https://github.com/logicdelos/Eglantine/releases/tag/fork-v0.2.0) on 2026-11-16. The exit test below passed; the individual fixes were not each confirmed in a game, as the release notes state.

**Significant change:** correctness fixes ported from other Eglantine forks, each credited to its author.

- Fire Emblem: Fortune's Weave battle scenes: shader fix for texture arrays (from Lichii).
- Xenoblade Chronicles 2: wrong 2x zoom in handheld mode (from Orchid).
- Audio decoding sign fix, render-target and depth-target fixes, file read bounds, controller vibration value checks.

Done when: both games reach their reference scenes with no new error lines and pictures within the comparison limit.

### 0.3: Unattended runs
Status: released as [fork-v0.3.0](https://github.com/logicdelos/Eglantine/releases/tag/fork-v0.3.0) on 2026-11-16. The release gate passed against `fork-v0.2.0`, and the unattended exit test passed 10 of 10 cases: bad arguments, a frame limit, a time limit and a forced crash, on both tested games.

**Significant change:** the command-line build can run a game for a set time or number of frames and write a machine-readable result file.

- `--report`, `--run-seconds`, `--run-frames`, and exit codes that say how the run ended. See [fork-notes/unattended-runs.md](fork-notes/unattended-runs.md).
- Crash handler, log flush thread and log rotation.

Done when: a scripted run of each tested game ends with the right exit code and a complete report, including after a forced crash.

### 0.4: Measured speed work
Status: in progress. The measurement is done and the release build is being checked; not released yet.

**Significant change:** one general baseline of settings and real frames by default. Every speed candidate was measured on and off on both games, and only a measured gain would have been kept.

- Baseline: every game is tested on the defaults of every emulation setting, one set for all games, with a version and a hash. See [fork-notes/baseline.md](fork-notes/baseline.md).
- Real frames by default: the new setting "Allow running faster than real time" (`allow_speed_up`) is off, so your speed limit cannot be switched off or raised above 100 %. Turning it on gives the old behaviour.
- Nine candidates were measured: the GPU thread split at high accuracy, pinned GPU threads, fewer empty submits, texture decode and encode in one submit, a high-resolution timer, zero-page trimming, protection changes without a lock, add-on packages in the shader precache, and a cheaper texture decoder set-up. None showed a gain beyond the measured spread without a loss or a broken picture, so none is in the release; the release notes list each with its numbers and the reason.
- Accuracy policy: no new game-specific code; the existing exceptions are frozen and listed. See [fork-notes/accuracy-policy.md](fork-notes/accuracy-policy.md).

Done when: every candidate has a recorded result on both games, the release notes carry the table, and the release gate passes against 0.3.0.

### 0.5: Smoother first minutes
Status: in progress; not released.

**Significant change:** shaders and pipelines are recorded on the first run and prepared ahead on later runs, so the second launch has far fewer long frames.

- Every long frame and the cold start are attributed to a cause (disk, shader, pipeline, texture, decompression, waiting, code translation).
- The cache carries a version stamp and is rejected cleanly when it does not match.

Done when: frames over 100 ms in the first two minutes are at least halved on the second launch in both games.

### 0.6: Replay and regression check
Status: planned.

**Significant change:** a frame's graphics calls can be recorded and replayed without the game, and pictures compared automatically.

- A census of the graphics library calls each game makes per frame, built into the emulator.
- A check that fails a change when median or low-end frame rate drops beyond measured noise.

Done when: an injected 5 % slowdown is caught, and the call census is complete for both games with no per-game code.

### 0.7: Fast graphics path, preview
Status: planned. Depends on the measurements above.

**Significant change:** an experimental switch that sends a game's graphics library calls straight to Vulkan.

- Direct calls from translated game code into the emulator, with no round trip through the system call path.
- Data that changes often is kept in video memory and updated by write tracking.

Done when: Fire Emblem: Fortune's Weave reference scenes match the existing path in the replay comparison, and the frame-rate comparison is published.

### 0.8: Fast graphics path, integrated
Status: planned.

**Significant change:** the fast path works with the emulator's own presentation (upscaling, HDR, screenshots) and falls back to the existing path automatically.

- Xenoblade Chronicles 2 on the fast path.
- Fallback at start-up and at a frame boundary when a game uses something not yet covered.

Done when: both games run their reference scenes on the fast path, and an untested game still boots on the existing path.

### 0.9: No per-game work
Status: planned.

**Significant change:** a third game runs on the fast path with no code written for it.

- Frame pacing and frame generation on the fast path.
- Translated game code cached on disk, so later launches start already translated.

Done when: the third game boots and plays its first scene on the fast path with zero per-game code.

### 1.0: Fast path by default
Status: planned.

**Significant change:** the fast path is the default for games that pass the replay comparison; the existing path remains for the rest.

- Background re-optimisation of hot game code, if earlier measurements show code speed limits the frame rate.
- The fast path offered to upstream as an optional back end.

Done when: both tested games and the third game meet the speed target below from the second launch on.

## Speed target

A target, not a result: from the second launch on, within 1 % of a hand-made recompilation of the same game in scenes limited by graphics, and within 5 to 10 % in scenes limited by the processor. The first launch is excluded. This has not been measured yet; the first comparison will be published when it has.

## Not planned

- No games, keys, firmware or system files are provided or linked. Use games you own.
- No per-game speed patches as a requirement. They may exist as optional data.
- No rewrite of the emulator core.
