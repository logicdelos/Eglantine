## What the log says when a run goes wrong

Always-on lines that say what happened without a trace. They cost nothing per draw beyond relaxed atomic adds, and each is written at most once per 10 s window or once per event. They change no emulation behaviour. Code: `src/common/nx2_diagnostics.*` (state machines, summaries), `src/common/nx2_bottleneck.cpp` (the 10 s loop that drives them), `src/video_core/renderer_vulkan/vk_driver_build_report.*` (skips).

### Skipped draws

`Skipped work: 1204 draws skipped (stage-rejected 1204; last 10 s); most skipped programs: vs=0x21dfc4d000 ps=0x21dfc4d300 900, vs=... ps=... 200` (Render.Vulkan, Info). Per reason, one line per window when non-zero, and the same text ends the `Shader builds:` line when that is written instead. The reasons are the `miss=` values of the `Draw skipped` warning plus `worker-pipeline-missing` (the pipeline handle was null when the draw was recorded), `vertex-count-limit` and `indirect-mesh-draw`. The programs are the 3 vertex/fragment pairs with the most skipped draws in the window; a draw skipped on the worker thread names none. The session total is logged at shutdown without programs.

### Native shader rejections

`Shader rejections: 3 new in the last 10 s (all-zero code 2, unsupported opcode 1); 17 this session` (HW.GPU, Warning), written when a window had new rejections. Classes: `all-zero code` (the program reads as zeros), `branch outside capture`, `unsupported opcode`, `other`. Only a first-seen rejection message counts: the per-program error line is unchanged.

### Scene changes

`Scene change at t=3346.2 s: render targets 1920x1080 fmt 0xd2 -> 1280x720 fmt 0xd2 (the old set lasted 1840 frames)` (Render.Vulkan, Info) and a `SceneChange` event in the system event ring, so a Ctrl+Alt+F8 bundle shows it. Triggers: the set of colour-target (width, height, format) keys of the draws changes and stays changed for 5 frames, or 10 or more new pipelines are built (not duplicates, not from the disk cache) within 2 s. At most one line per 5 s; suppressed ones are counted in the next line.

### Stop summary

Written once per process by the first stop: a force stop of the emulation thread, a normal stop, the command line's stop, or the crash handler (after the minidump, from fixed buffers; kind `crash`).

```
Stop summary (force stop): ran 13204.5 s, 790112 frames presented, 41 errors and 0 critical messages logged this session
Stop summary: last full 10 s report, written at 13194 s: Last 10 s: ...
Stop summary: memory: host RAM free 0.9 of 15.9 GiB, host commit 91% of its limit; emulator 9.8 GiB committed, 7.1 GiB in RAM
Stop summary: native shaders rejected this session: 120 (all-zero code 40, branch outside capture 80)
Stop summary: error 1 of 3 (the last distinct ones, newest last): [13100.2] file.cpp:123 x4: message
```

### Low memory

`LOW MEMORY: host RAM free below 1.0 GiB for 30 s (host RAM free 0.9 of 15.9 GiB, host commit 91% of its limit); emulator 9.8 GiB committed, 7.1 GiB in RAM. Expect stutter, long loads or a crash.` (Core, Warning): host RAM free under 1 GiB, or host commit over 95%, continuously for 30 s. Once; again only after `Memory recovered: ...` (Info) has been logged. Event kind `LowMemory`.

### No progress

`No progress (suspected): core 0 ran game code 99% for 20 s with 0 file reads while 1200 frames kept presenting in the last window; SleepThread calls that yielded to nobody were 97% of the 5000000 SVCs. ...` (Core, Warning), once per episode: one guest core at 95% or more, no file reads, and empty-yield SleepThread calls at least half of all SVCs, over 2 whole 10 s windows with frames presenting. The frame-stall watchdog (`EDEN_NX2_STALL_SECONDS`) is unchanged and stays silent while frames present. The warning starts a 1.5 s probe of that core (at most 3 per process): `No progress sample of core 0 (1.5 s): guest thread 0x.. "name" ran at: main+0x1234 80%, ... (700 polls); host: ...; full report: <log dir>/no_progress_<stamp>.txt`. Event kind `NoProgress`.

### Sampler

`EDEN_NX2_SAMPLE=CPUCore` wrote no file because the core threads are named `CPUCore_<n>` and a bare name matched only the exact name, the Qt frontend never started the sampler (only the command line did), the default output file was relative to the working directory, and the default start delay is 60 s (`EDEN_NX2_SAMPLE_START`). A bare name now also matches `Name_<anything>`, the Qt frontend starts it, the default file is `<log dir>/nx2_samples.txt`, and a sample that matches no thread writes the file saying so, with the thread names seen.
