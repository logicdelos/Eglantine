# Frontends and common support (Qt GUI, SDL CLI, config, logging, MCP server)

**Purpose.** This slice is everything around the emulator core: the Qt GUI (`src/yuzu`, `src/qt_common`), the SDL3 command-line frontend (`src/yuzu_cmd`), the INI configuration layer (`src/frontend_common`), the embedded MCP debug and automation server (`src/mcp`), and the `src/common` utilities (settings, logging, filesystem, threads, containers). It starts and stops `Core::System`, turns config files into `Settings::values`, writes the log, and gives outside tools (agents, scripts) a way to drive the emulator. The GUI is the owner's normal frontend; the CLI is mostly used for unattended Switch 2 (NX2) test runs.

## Files

| file | role | read depth |
|---|---|---|
| `src/common/logging.cpp`, `log_ring.h` | log filter, backends, `FmtLogMessageImpl` | full |
| `src/common/settings.cpp`, `settings_setting.h` | `Settings::values`, `LogSettings`, `SwitchableSetting` global/custom | partial (lines 1-303 and 290-439) |
| `src/common/nx2_sampler.cpp` | Windows sampling profiler and stall watchdog (fork) | full except the report text assembly |
| `src/common/nx2_bottleneck.cpp` | what limits the game: one `Report` log line every 10 s from wall-time meters at the wait sites (fork) | full |
| `src/common/fs/file.cpp`, `file.h` | `IOFile` | partial |
| `src/common/driver_shader_cache.cpp`, `host_memory.cpp`, `thread.cpp`, `env_gate.h` | env switches only | skimmed (grep) |
| `src/common/assert.cpp`, `bounded_threadsafe_queue.h` | `Log::Stop` caller; queues (not used by logging) | partial |
| `src/common/fs/fs.cpp`, `path_util.cpp`, `thread_worker.h`, `swap.h`, `fixed_point.h`, `tree.h`, other containers | utilities | not opened |
| `src/frontend_common/config.cpp` | `Config` base: INI load/save, per-game/global/profile | partial (1-703, 978-1004) |
| `src/qt_common/qt_common.cpp`, `config/qt_config.cpp`, `render/emu_thread.cpp` | Qt init, QtConfig, emulation thread | full |
| `src/yuzu/main.cpp`, `main_window.cpp` | Qt entry, boot/shutdown, MCP hosting | main.cpp full; main_window.cpp partial (boot, shutdown, config-dialog paths) |
| `src/yuzu/bootmanager.cpp`, `breakpad.cpp`, `mcp_host_qt.cpp` | render window, crash dumps, MCP host | partial |
| `src/yuzu/configuration/*` (~60 files), `qt_software_keyboard.cpp`, `configure_input_player_widget.cpp` | settings dialogs and applets | not opened except `configure_performance.cpp` (grep) |
| `src/yuzu_cmd/yuzu.cpp` | CLI start-up, loop, key scripts, crash dump | full |
| `src/yuzu_cmd/emu_window/*`, `sdl_config.cpp`, `mcp_host_cli.cpp` | SDL window, config, MCP host | skimmed |
| `src/mcp/server.cpp`, `protocol.cpp` (auth part), `server.h` | HTTP server, routes, auth, log sink | server.cpp full; protocol partial |
| `src/mcp/tools/*.cpp` (~17 files, about 9000 lines), `context.cpp`, `control.cpp`, `timeline.cpp` | tools | not opened except `artifact_tools.cpp` path checks (grep) |
| `src/android/*` | Android only | not opened |

## Main types and ownership

- `Core::System` is created by `QtCommon::Init` (Qt) or on the stack of `EmuState` in `SDL_AppInit` (CLI). The frontend owns it for the whole process; games are loaded and unloaded inside it.
- `MainWindow` owns `config` (a global `QtConfig`), `EmuThread` (`QtCommon::emu_thread`), `GRenderWindow`, and `mcp_server` (`unique_ptr<Mcp::Server>`).
- `Settings::values` is one global `Values` object. Each `Setting` holds a value; each `SwitchableSetting` also holds a `custom` value and a `use_global` flag that selects which one `GetValue`/`SetValue` touch.
- `Config` (INI wrapper, `CSimpleIniA`) has three types: Global (`<ConfigDir>/qt-config.ini` or `sdl2-config.ini`), PerGame (`<ConfigDir>/custom/<title>.ini`), InputProfile (`<ConfigDir>/input/`).
- Logging is a static `std::optional<Impl>` with five backends (console, ring, file, debugger, logcat) plus one atomic external sink pointer.
- `Mcp::Server` owns an httplib server with its 8-thread pool, a `Protocol`, a `ToolRegistry`, and a reference to a process-lifetime `Timeline` that is deliberately never freed.

## Threads and locking

- **Logging has no queue and no thread.** `FmtLogMessageImpl` formats on the calling thread (any emulation, GPU, service or UI thread) and calls every backend synchronously. Only `RingBackend` has a mutex. `FileBackend`, `ColorConsoleBackend` and the `Filter` have none; `fwrite` is serialised by the CRT per call.
- Qt: GUI thread; `EmuControlThread` (`EmuThread::run`, holds `m_should_run_mutex`/condvar); render window thread; `QtConcurrent` pool for loose-RomFS reads and the game-list scan.
- CLI: SDL main-callback thread runs `SDL_AppIterate` at about 240 Hz; the emulator runs on its own threads started by `system.Run()`.
- MCP: one `http_thread` running `listen_after_bind`, plus 8 httplib workers. Long waits (`wait_for`, `act`, `/events/wait`, SSE) each hold a worker. Tools that need the UI marshal to the GUI thread (`QtMcpHost`) or to `DrainMainThread` (CLI). `StopMcpServer` runs the server destructor on a helper thread while the GUI thread keeps pumping events, because in-flight requests may wait for the GUI thread.
- Nx2 sampler: detached threads (`EDEN_NX2_SAMPLE` thread, `StallWatch`). They suspend other threads one at a time with `SuspendThread`, walk the stack without allocating, and resume.
- Nx2 bottleneck: one detached `Nx2Bottleneck` thread that sleeps 10 s, swaps the atomic counters to zero and logs. The metered threads only add to atomics; no lock.
- Lock order: not documented. The only visible nesting is log call to ring mutex or external sink to `Timeline::Push` (timeline.cpp has no `LOG_` calls, so no cycle there).

## Flow

**Qt start-up.** `main()`: `LoadNx2Switches` (reads `nx2_switches.txt`, then `_putenv_s`) -> `Config::ReadGlobalSettingEarly` (private driver cache, size) -> `ConfigureDriverShaderCache` -> `StartupChecks` -> optional Breakpad -> `MainWindow` constructor: `QtCommon::Init` (creates System, VFS, `CreateEdenPaths`, `system->Initialize()`, **then** `Log::Initialize()`, `Network::Init`) -> `QtConfig` (global) -> `SetGlobalFilter(log_filter)` -> widgets, hotkeys, HID, content providers, game list -> `ParseLaunchParams` -> `MaybeStartMcpServer` -> `BootGame` if a game was given. Log calls before `Log::Initialize()` are dropped (`logging_instance` is empty).

**CLI start-up.** `SDL_AppInit`: crash filter, `Nx2Sampler::StartFromEnvironment`, console attach, `Log::Initialize` + colour console on, `ParseLaunchParams`, `SdlConfig`, `ApplyLaunchParams` (re-applies log filter), NUP pack/unpack/edit sub-commands, `system.Initialize`, `ApplySettings`, window choice (Vulkan / null / GL), `system.Load`, `GPU().Start()`, disk shader cache load, callbacks, `system.Run()`, optional MCP (`--mcp`).

**Settings to core.** The core reads `Settings::values.X.GetValue()` directly, at use time or in `System::ApplySettings()`. Global values come from the INI. For a game, `BootGame` runs `config->SaveAllValues()`, builds `QtConfig per_game_config(title, PerGameConfig)` (which reads `custom/<title>.ini`, flips each setting with `\use_global=false` to its custom value, then immediately rewrites the file), calls `ReloadInputDevices` and `ApplySettings`, and logs everything with `LogSettings`. `Settings::RestoreGlobalState` (after stop) flips all `use_global` back to true. Global saves write `ToStringGlobal()`, so per-game values do not leak into `qt-config.ini`.

**Boot** (`BootGame`, main_window.cpp ~2429): guards (`boot_in_progress`, `close_pending`, shader-warm dialog) -> config -> `LoadROM` (`InitRenderTarget`, applets, `system->Load`) -> `EmuThread` start -> `emulation_running = true`.

**Shutdown.** `OnShutdownBegin` -> `RequestGameExit` + 1 s timer (5 s if exit locked, 0 with debugger) -> `ForceStop` + second timer -> `TerminateEmulationThread` (`QThread::terminate`) -> `OnEmulationStopped` (reset thread, `RestoreGlobalState`, reload input). `EmuThread::run` ends with `DetachDebugger` + `ShutdownMainProcess`; these are skipped if the thread is terminated. `closeEvent` stops MCP, saves window state and hotkeys, then `~QtConfig` of the global config saves all values. CLI: `SDL_AppQuit` stops MCP, detaches debugger, `ShutdownMainProcess` if powered on, `Network::Shutdown`; restart requests exit with code 75 after writing `restart-handoff.bin`.

**Logging pipeline.** Filter check (`class_levels` array) -> format into a stack buffer of `BUFSIZ` (longer messages are formatted a second time into a `std::string`) -> backends in order: console (CLI only), ring (off unless MCP/debug tools set a capacity), file (`<LogDir>/eden_log.txt`, old one renamed `.old.txt`; 4 KiB CRT buffer; flushed at Error and above, or every line with `log_flush_line`; stops after 100 MiB, 1 GiB with `extended_logging`; `censor_username` replaces the user name with `user`), debugger (`OutputDebugStringW`, always on Windows) -> optional external sink (MCP timeline, key hex runs redacted). `Log::Stop` (flush) is only called from `assert.cpp`.

## Connections

- In: `Core::System` and everything else call `LOG_*` and read `Settings::values`; `launch_params.cpp` (`ParseLaunchParams`/`ApplyLaunchParams`) is shared by both frontends.
- Out: `System::Initialize/Load/Run/Pause/ApplySettings/ShutdownMainProcess/RequestShutdown`, `HIDCore().ReloadInputDevices`, `GPU().Start/ObtainContext`, `Network::Init/Shutdown`, `ShaderWarm::*`, `InitializeDebugger/DetachDebugger`, `Core::g_nx2_present_count` (read by the MCP timeline and CLI key script), `Nx2Sampler::NoteGuestFrame/SetGuestRunning` (called by the core).

## Settings and switches

Settings keys: `log_filter`, `flush_line` (`log_flush_line`), `extended_logging`, `censor_username`, `enable_mcp_server`, `mcp_port` (6544), `mcp_allow_writes`, `mcp_token`, `private_driver_shader_cache`, `driver_shader_cache_size`, `use_disk_shader_cache`, `use_gdbstub`. MCP binds 127.0.0.1 only; port taken with `SO_EXCLUSIVEADDRUSE`; Host and Origin must be loopback; bearer token optional (empty by default); write tools need `mcp_allow_writes`; a discovery file `mcp-server.json` (no token) is written in `EdenDir`.

Environment switches and where they are read:

| variable | read in | effect |
|---|---|---|
| `EDEN_NX2_SPLIT`, `_FENCE_BATCH`, `_HOST_IMPORT`, `_BANK_REUPLOAD` | `configure_performance.cpp:104-125` (launch-arg table; GUI shows if set), read by the GPU code | NX2 GPU options |
| `nx2_switches.txt` | `yuzu/main.cpp:151-168` (exe dir, then user dir) | sets `EDEN_NX2_*` if not already set |
| `EDEN_NX2_CRASH_DUMP` | `yuzu_cmd/yuzu.cpp:116` | CLI crash filter writes a MiniDump |
| `EDEN_CLI_RESTART_HANDOFF` | `yuzu.cpp:688` | restart handoff file |
| `EDEN_NX2_DIAG_DIR` | `yuzu.cpp:846` | live `keys.txt` command channel |
| `EDEN_NX2_KEY_SCRIPT` / `_KEY_RECORD` | `yuzu.cpp:912`, `emu_window_sdl3.cpp:142` | scripted or recorded key input |
| `EDEN_NX2_HIDE_WINDOW` | `emu_window_sdl3.h:36` | hidden SDL window |
| `EDEN_NX2_SAMPLE`, `_SAMPLE_OUT`, `_SAMPLE_START`, `_SAMPLE_SECONDS` | `nx2_sampler.cpp:463-469` | sampling profiler |
| `EDEN_NX2_STALL_SECONDS` | `nx2_sampler.cpp:487` | stall watchdog (default 60 s; writes `stall_*.txt`) |
| `EDEN_NX2_PRIVATE_DRIVER_CACHE`, `__GL_SHADER_DISK_CACHE_*` | `driver_shader_cache.cpp:44-71` | NVIDIA driver shader cache location and size |
| `EDEN_NX2_COLD_HINT`, `EDEN_PUNCH_HOLE` | `host_memory.cpp:818`, `:994` | host memory behaviour |
| `EDEN_HYBRID_CORES`, `EDEN_NX2_VCACHE` | `thread.cpp:475`, `:632`, `:801` | thread placement |
| `EnvGate(name)` (`env_gate.h`) | many call sites | true unless the value is "0" |

## Invariants

- `Log::Initialize` runs before any log that must reach the file (assumed; not enforced, early lines are dropped).
- `players.SetGlobal` must match the config type being saved or loaded (enforced only inside `SaveControlValues`/`ReadControlValues`; callers restore it in two places only).
- The global `QtConfig` is the only writer of `qt-config.ini` (assumed).
- `Settings::RestoreGlobalState` does nothing while the system is powered on (enforced, `settings.cpp:425`).
- `Mcp::Timeline` outlives all producers (enforced by never freeing it).
- Only one MCP server per port per machine (enforced by `SO_EXCLUSIVEADDRUSE`).

## Hazards and improvement candidates

See `findings/frontends-common.json`. Summary: `Config::WriteToIni` truncates `qt-config.ini` in place; no log flush on a crash (the file backend buffers about 4 KiB); `QThread::terminate` skips `ShutdownMainProcess`; a failed `LoadROM` leaves the per-game overrides active; the log file backend has unsynchronised counters; every log line is formatted by each backend separately, and `OutputDebugStringW` runs without a debugger check; the global-config save flips `players` to global during a per-game session; `StopMcpServer` spins the GUI thread; CLI ignores a guest exit; unchecked `std::stoi` on the key-script env variable. The stall watchdog (`WatchStalls`) starts on the first `Run()` with a 60 s default and is paused correctly by `Pause()` and `ShutdownMainProcess`; the sampler keeps allocation out of the suspended-thread window.

Not covered: the tool bodies in `src/mcp/tools`, most dialogs under `src/yuzu/configuration`, `sdl_config.cpp`, and `fs`/`path_util`.
