## Unattended runs

How to let the command-line build run a game by itself, get a machine-readable report of the run, and read the exit code. Written for the Windows x64 pre-release in this repository on branch `fork-0.3`. The options, exit codes and report forms below were checked against the code in the release, and the unattended exit test that ran on the as-shipped 0.3.0 files covered bad arguments, a frame limit, a time limit and a forced crash on both tested games. The examples on this page were not all run one by one.

## Options

| Option | Effect |
|---|---|
| `--report <file.json>` | Write one JSON report when the run ends. The file must not exist, and its folder must exist. |
| `--run-seconds <n>` | End the run after n seconds, a positive number. Reaching the limit exits with 124. |
| `--run-frames <n>` | End the run after n presented frames, a positive number. Reaching the limit exits with 0. |

- Using any of the three makes the run unattended. Without them the build behaves as it did before this release: no run clock, no report, and the exit codes an ordinary launch always had.
- Both limits can be given. Whichever is reached first ends the run. If the frame limit is reached first, the time limit never fires.
- `--run-frames` alone also gets a safety limit of 900 seconds, so a stuck game cannot hang a script. Reaching it is a timeout, exit code 124.
- The run clock starts right after the command line is checked, before the configuration file is loaded, and `--run-seconds` therefore includes configuration load, GPU start-up and shader warm-up.
- `--report` on its own sets no limit: the run continues until the game exits or the window is closed, and then writes the report.
- The three options cannot be combined with `--help`, `--version`, `--pack-nup`, `--unpack-nup` or `--edit-nup`; that is 64.
- In an unattended run an unknown option, or an option missing its argument, is also 64, where an ordinary launch ignores it.
- The report destination is checked before anything is launched: the file must not exist, and its folder must be there. Both are 64. A temporary file `<name>.<pid>.tmp` is created next to the destination before the launch, so a destination that cannot be written is refused now rather than after a long run; that is 74. A second temporary file, `<name>.<pid>.crash.tmp`, is kept empty for a crash report and is removed when the run ends normally.
- The report is written to the temporary name, then renamed to the destination in one step. It is never written over an existing file, and nothing is written while the run is going.

### Reading JIT prewarm outcomes

On x86-64 with prewarm enabled, look for `JIT prewarm outcomes:` in the `Core_ARM` info log at orderly shutdown. Each core reports how its `loaded` hints ended: newly `compiled`, `already_present`, `rejected_mismatch` after the last retry, `no_space` or `unsupported` for the single hint that stopped the sweep, and `pending` for the rest. These six counts sum to `loaded`. The separate `mismatch_attempts` includes retries; do not add it to that total. `list=not_published` means the helper had not handed that core a list when the CPU stopped.

The counts describe precompilation, not blocks demanded or executed by the guest. If a crash or forced exit bypasses teardown, a missing line is not a zero.

## Exit codes

| Code | Reason | Meaning |
|---|---|---|
| 0 | `normal_end` | Normal end: the game exited, the window was closed, a quit was requested from the live keys file or the MCP server, or the frame limit was reached. |
| 1 | `startup_error` | The game did not load, or a later step of the start-up failed: the window could not be created, a room could not be joined. |
| 64 | `bad_arguments` | Bad arguments, as listed above. |
| 70 | `guest_crash` | The game broke (an unhandled CPU exception, a break without the notification flag, a fatal error with a screen) or hit a fatal error. |
| 71 | `device_lost` | The Vulkan device was lost. A loss on the scheduler worker or the present thread ends the run in order; a loss surfaced as an exception on another thread may still end the process as it did before. |
| 72 | `shader_failure` | A graphics or compute pipeline the game needs failed to build. |
| 73 | `host_crash` | The emulator itself crashed. The crash report is written first, then the log line and the minidump; if that report cannot be written, the code is 74 instead. Windows only, through the crash handler. |
| 74 | `report_io_error` | The report could not be written: the destination became unusable during the run, or a crash report could not be written. |
| 75 | `restart` | The game asked to be restarted again. The launcher boots the same game again. |
| 124 | `timeout` | The time limit, or the 900 second safety limit of a frame-only run, was reached. |
| 130 | `interrupted` | Ctrl+C, Ctrl+Break or SIGTERM. |

Without the three options, only codes 0, 1 and 75 exist; a bad argument also exits with 64 in an ordinary launch. The rest apply only to an unattended run. The process exit code is the number a script sees.

## Report file

`schema_version` is 2 (it was 1 up to 0.4; the members that schema 2 adds are listed under 0.5.0 in `FORK_CHANGELOG.md`). Indented JSON (two spaces), with these members:

- `build`: `revision`, `branch`, `description`, `build_id`, `build_date`, `compiler`, `name`, `full_name`, `version`, `display_version`, `flavour`.
- `title_id`: the loaded game's program id as 16 upper-case hex digits, or `null` when no game was loaded.
- `limits`: `seconds`, `frames` — the limits the run was armed with, including the 900 second safety value of a frame-only run, `null` for a limit that was not given.
- `elapsed_seconds`: wall clock seconds from the moment the run was armed to the end, `0` when the process ended before the run started, as with bad arguments.
- `summary`: the whole run, as the object described below.
- `windows`: one entry per 10 seconds of running time: `start_seconds`, `end_seconds`, `partial`, and `metrics` in the same shape as `summary`. `partial` is true for a window that does not cover a whole 10 seconds, as at a pause or at the end of the run. About 2 KB per entry, kept in memory for the whole run.
- `logs`: `scope`, `error`, `critical` — `scope` is `filtered_emitted`, the Error and Critical messages that passed the log filter and were logged.
- `exit`: `reason`, `code`, `shutdown_complete`, `metrics_complete`, `detail`.
- `settings`: `initial` and `final`. The settings after the configuration file and the command line were applied, and those at the end of the run. They cover these categories: Audio, Core, Cpu, CpuDebug, CpuUnsafe, Renderer, RendererAdvanced, RendererHacks, RendererDebug, RendererDevkit, RendererMemory, System, SystemAudio, GpuDriver. A value is a JSON boolean or number when its text is exactly one; anything else (an enum name, a string, a hex value) stays text. Paths, window and control settings, and the UI are left out on purpose.
- `tunables`: the `EDEN_*`, `NX2*` and `EGLANTINE_*` environment variables that were set when the run started. Each entry records `raw` (the text after the first `=`) and `effective`; `effective` is a JSON object and is `{}` in this release, because this tree has no table of defaults. A variable whose name or value contains a slash is not listed, and a listed one is from this process's environment.

### The summary object

| Field | Meaning |
|---|---|
| `active_seconds` | Time the guest actually ran. A paused guest does not count. |
| `frames` | Completed frames counted by the emulator's frame counter. |
| `fps` | `frames` divided by `active_seconds`. |
| `frame_time_ms` | Frame times: `samples` (intervals measured), `mean`, `p50`, `p95`, `p99`, `max`, `method` (`nearest_rank_log_histogram_us`), `relative_bucket_width` (0.015625; a bucket is at most 1/64 of its value), and `over_25_ms`, `over_50_ms`, `over_100_ms`. |
| `threads.gpu`, `threads.gpu_back`, `threads.vulkan_worker` | One object per thread, or `null` when that thread was never observed: `observed_seconds`, `work_ms`, `idle_ms`, `waits_ms`, with the waits split into `host_gpu`, `shader_build`, `vulkan_worker`, `present` and `gpu_back`. |
| `guest_gpu_wait` | `total_ms`, `longest_ms`, `count`, and `fraction`, the total wait as a fraction of `active_seconds`. |
| `guest_cpu_core_busy_ms` | Busy milliseconds per guest core; empty when the guest never ran. |
| `shader_wait` | `total_ms`, `longest_ms`, `count` — time the guest waited for shader work. |
| `limiter` | Text naming what limited the frame rate, or `not_running`. |

A run that never reached a game has an empty `summary` — `active_seconds` and `frames` at zero, empty arrays and zeroed waits — and no windows. The crash report writes a shorter `summary` with only `frames`.

## What cannot produce a report

A process killed outright (End task, Task Manager, `taskkill /F`, a power cut or a machine reset) writes no report: nothing can write afterwards, and the temporary file `<name>.<pid>.tmp`, or `<name>.<pid>.crash.tmp` when a crash report was armed, stays behind. Delete it before reusing the name.

A report destination whose folder disappeared, or whose file cannot be renamed, during the run: the run still ends with the reason it ended with, but the code becomes 74, an Error line is logged and one line is printed on stderr.

Two cases that do produce a report, but not a complete one:

- A stop that cannot finish within 30 seconds, as when a graphics driver call never returns. The report is written by the watchdog on a timer thread with `shutdown_complete` and `metrics_complete` false, and the process exits with the code of the reason.
- A crash of the emulator itself. The report is written from the crash handler before the log line and the minidump, with reason `host_crash`, code 73, `shutdown_complete` and `metrics_complete` false, an empty `windows` array, and a `summary` that holds only the frame count. If that report cannot be written, the process exits with 74 instead.

A crash before the report is prepared: with no `--report`, or a destination that is refused, the crash handler has nothing to write, so it takes the ordinary crash path: the log line and the minidump, no report.

A crash inside a graphics driver call that never returns: nothing can be written there and then, and with `--report` alone no stop ever comes. When the 900 second safety limit or a signal brings a stop, the 30 second watchdog writes the report and the process exits with 124 or 130.

A run that ends before it was armed, as with bad arguments, or an unattended option mixed with `--help`, writes a report only if exactly one valid `--report` destination was given. In that report `elapsed_seconds` is 0 and both limits are `null`.

## Log files

- Log folder: `log` inside the user folder (the folder beside the program when there is one, otherwise `%APPDATA%\Eden`).
- `eden_log.txt`: this session's log. The file is flushed to disk every second, so lines below Error no longer wait in the buffer until a hang or a kill.
- `eden_log.txt.old.txt`: the previous session's log, moved over its predecessor in one step, so the old log is not lost when another process still holds `eden_log.txt`.
- `eden_log_2.txt` appears only when another process still holds `eden_log.txt`, and the same one-step move applies to it.
- `eden_crash.dmp`: the minidump, written next to the log and overwritten on each crash. `EDEN_NX2_CRASH_DUMP=<dir>` only chooses where it goes; it is written on every crash.

## Crash information

On Windows, an unhandled exception, an `abort()` or a CRT parameter failure writes one Critical `Host crash` line to the log with the exception code (or the fail-fast code), the faulting module and offset, the faulting address and the thread id, then writes `eden_crash.dmp` next to the log and logs `Host crash: minidump of process <pid>` with `written` or `failed` and the error. The minidump is written on every crash and is not behind a switch any more. A crash before the handler is installed leaves no crash line and no report.

## Test switch

`EDEN_NX2_TEST_CRASH_AFTER_FRAMES=<n>` crashes the emulator on purpose when the nth frame is presented. `n` is a positive whole number counted from the start of the run; unset or 0 turns it off. It exists to test the crash path and the report written from it, and it does nothing else. It is listed, commented out, under "Test switches (for unattended-run tests; never set these in normal use)" in `dist/nx2_switches.txt`, and it does nothing unless it is set.

## Running from a script

To check the report and the exit code:

```
eden-cli.exe -g "game.nup" --run-frames 600 --report frames.json
eden-cli.exe -g "game.nup" --run-seconds 120 --report seconds.json
eden-cli.exe -g "game.nup" --run-seconds 600 --run-frames 1800 --report both.json
```

The program is a Windows-subsystem executable. As the windowed build is, a shell does not wait for it by default: the prompt comes back while the game is still running, and the exit code you read is not the run's. A script has to wait for the process and read its exit code. In PowerShell, in the folder you unzipped:

```powershell
$p = Start-Process .\eden-cli.exe -ArgumentList '-g', "game.nup", '--run-seconds', "120", '--report', "run.json" -Wait -PassThru
$p.ExitCode
```

In Python, with the program on your `PATH`:

```python
import subprocess
code = subprocess.run(["eden-cli.exe", "-g", "game.nup", "--run-seconds", "120", "--report", "run.json"]).returncode
```

If `eden-cli.exe` is not on `PATH`, give its path. A bad argument or a destination that already exists is refused at once with 64, so the call returns quickly.

The test switch is a separate block:

```powershell
$env:EDEN_NX2_TEST_CRASH_AFTER_FRAMES = 300
$p = Start-Process .\eden-cli.exe -ArgumentList '-g', "game.nup", '--run-frames', "900", '--report', "crash.json" -Wait -PassThru
$p.ExitCode
Remove-Item Env:EDEN_NX2_TEST_CRASH_AFTER_FRAMES
```

Expected: `crash.json` with `exit.reason` `host_crash` and exit code 73, `windows` empty, and a `Host crash` line at the end of the log and `eden_crash.dmp` in the log folder. The switch is test-only; it was used to test this path on Fire Emblem: Fortune's Weave and Xenoblade Chronicles 2 only.
