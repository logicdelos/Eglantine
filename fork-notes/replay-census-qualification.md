<!--
SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->
<!-- lang: en-GB -->

# Issue #5: Two-Title Graphics Census and Standalone Replay Qualification

**Public base (`fork-main`):** `dcdc82a4d6dd02fddc72efe69595ad99aa5ab817`
**Candidate from #1 (`acc/h-int54`):** `b5aef6b73b2229c9930668eccb395e9f894a2886` (`Eglantine-w23-s54-verified\MANIFEST.txt`, `eden-cli.exe` SHA-256 `03edae9b781bf437bba1ebb73d306626c5bf4cb80a8032ec9cb71a3c5dfc8326`)
**Working branch (`agy/issue-5`):** code fix + unit test + original standalone replay/census fixture and verifier.

> **Status boundary:** No live title GPU runs were executed in this offline qualification slice. All synthetic checks pass on original redistributable fixtures; two-title (`FE:FW` and `XC2`) census and replay promotion remains **uncertified / pending live title execution** until a clean GPU slot is reserved.

---

## 1. Inventory of Capture, Replay, and Census on Candidate `b5aef6b73b`

Candidate `b5aef6b73b` (`acc/h-int54`) already carries the `fork-0.6` (`d086a9ec1a`) capture/replay and `f06/census` (`fb869f99ac`) subsystems, plus the G0 fast-probe / effect-ledger extensions:

| Subsystem | Key Files on `b5aef6b73b` | Role |
|---|---|---|
| Capture recorder & checkpoint | `src/video_core/replay/{capture,checkpoint,memory_capture,format,format_io,events,library_call}.*` | Records `Session`, `Checkpoint` (via GPU-thread `GPU::RunQuiesced`), `MemoryRange`, `GpuEvent` (`Submit`/`Consume`/`Fence`), `Present`, `EndPicture`, `FrameEnd`, `CompleteRecord`. |
| Standalone replay & runner | `src/video_core/replay/replay_session.{h,cpp}`, `src/yuzu_cmd/replay_runner.{h,cpp}`, `replay_seams.{h,cpp}` | Replays `.egcap` via `System::InitializeReplayKernel()` without a guest process; offscreen Vulkan `Compose`, `DrainGpu()`, `read_picture` PNG readback, `PictureChecksum`, `CompareSemantics`, `frames.jsonl`. |
| Replay options & report | `src/video_core/replay/{options,result,report_json,report_sections}.*` | CLI flags `--capture`, `--capture-frames`, `--replay`, `--replay-images`, `--replay-benchmark`, `--nvn-census`, `--nvn-census-audit`. |
| Graphics call census | `src/core/hle/guest_call/{nvn_census,nvn_census_verdict}.*` | Dynarmic A64 branch audit (`OnAuditBranch`), resolver + export hooks (`OnCall`), return-site classification (`IsCallInstruction`), 11-condition `EvaluateCoverage`. |
| External image comparator | `src/fe-lab/tools/frame_compare.py`, `frame_compare_selftest.py` | Evaluates `live_<id>.png` vs `replay_<id>.png` + `frames.jsonl` (`imgdiff` and `g0-sdr` MAE/outlier/bias/alpha envelopes). |
| Unit tests on `b5aef6b73b` | `src/tests/core/{nvn_census_verdict,replay_kernel}.cpp`, `src/tests/video_core/replay_*.cpp` | Format, codec, memory staging, checkpoint, session refusals, kernel layout, and 11-condition census verdict tests. |

### Starting-Evidence Root Cause Located on `b5aef6b73b` (Before Adding Functionality)

Inspection of `b5aef6b73b` confirmed **both** defects described in Issue #5's starting evidence are still present on `b5aef6b73b` (even though `b5aef6b73b` already contains `bc58c0e2a9`'s `GPU::RunQuiesced` checkpoint cut and `fb869f99ac`'s `nvn_census` audit fixes):

1. **Checkpoint cut blocker (`"a compute upload waits for its launch"`):**
   - In `src/video_core/engines/kepler_compute.cpp` (`CallMethod`, `case KEPLER_COMPUTE_REG_INDEX(exec_upload)`), `uploads.push_back(info);` executes unconditionally even when `native_mode` is `true` (confirmed also on `fork-main` `dcdc82a4d6` at line 584).
   - Switch 2 native compute engines launch dispatches via QMD control methods (`ProcessNativeMethod`) and **never** write the legacy `launch` register (`0xAF`), which is the only path that drains `uploads`.
   - `KeplerCompute::CleanCutBlocker()` in `b5aef6b73b:src/video_core/engines/kepler_compute.h` then checks `if (!uploads.empty() || upload_state.HasPendingData() || indirect_compute.has_value() || i2m_release.pending) return "a compute upload waits for its launch";` — causing every quiescent checkpoint cut on a native compute channel to refuse with `capture_failed`!
   - **Unmerged fix branch:** `f06/capture-cut` (`f3a6061505` + `9e9db5abdc`) guards `if (!native_mode) { uploads.push_back(info); }` and splits `CleanCutBlocker()` into distinct messages (`"a compute upload waits for its launch"`, `"an inline compute upload is unfinished"`, `"an indirect compute launch is in progress"`). Note: when integrating onto `b5aef6b73b`, `i2m_release.pending` (added in `1bdf350a08` on `acc/h-int54`) must keep its own distinct blocker check so it is neither dropped nor conflated.

2. **Completeness checker rejecting `capture_failed` runs (`run_not_complete`):**
   - In `b5aef6b73b:src/core/hle/guest_call/nvn_census.cpp`, shutdown calls `TakeSnapshot(s, true, IsCleanStop(reason), std::string{StopReasonName(reason)})` using `Common::RunOutcome::CurrentReason()` directly.
   - When a capture fails (`Reason::CaptureFailed`), `RunOutcome` overwrites the clean exit reason (`frame_limit` / `time_limit`), so `IsCleanStop(reason)` returns `false` and `EvaluateCoverage` fails with `run_not_complete` even when all 11 census coverage conditions hold.
   - **Unmerged fix branch:** `f06/capture-cut` (`1d7115b7ed` + `394b0c9905`) preserves the clean stop reason in `Common::RunOutcome` alongside `replay_capture.failed` / `.detail`, so census completeness is judged independently of capture failure.

3. **Additional unmerged census slice relative to `b5aef6b73b`:**
   - `f06/census-live` (`4ae68009c7`) adds `census_stop` live hook withdrawal (`StopLive`, `ClearInstructionCacheAndWait`, `StopTrackingRuns`), which is absent from `b5aef6b73b`.

---

## 2. Code Fix & Unit Verification Added on `agy/issue-5` (`dcdc82a4d6` base)

Because `fork-main` (`dcdc82a4d6`) already shares the unconditional `uploads.push_back(info)` bug in `src/video_core/engines/kepler_compute.cpp:584`, `agy/issue-5` applies the clean-cut fix and regression test directly on `fork-main`:

- `src/video_core/engines/engine_upload.h`: adds `State::HasPendingData() const noexcept` (`write_offset < copy_size`).
- `src/video_core/engines/kepler_compute.cpp`: guards `uploads.push_back(info)` with `if (!native_mode)`.
- `src/video_core/engines/kepler_compute.h`: adds `KeplerCompute::CleanCutBlocker()` with distinct per-condition blockers.
- `src/tests/video_core/compute_clean_cut.cpp` (registered in `src/tests/CMakeLists.txt`): Catch2 tests verifying:
  1. Native mode (`native_mode = true`) running 200 inline uploads stays at a clean cut (`CleanCutBlocker() == nullptr`).
  2. Unfinished inline upload (`data_upload` incomplete) reports `"an inline compute upload is unfinished"` until complete.
  3. Legacy mode (`!native_mode`) reports `"a compute upload waits for its launch"` until `launch` executes and drains `uploads`.

---

## 3. Census Denominator & Call/Effect Reconciliation Contract

For a title reference scene to pass Issue #5, **all** calls and effects must reconcile against an independent denominator with zero silent omissions and zero per-game hooks:

1. **Independent Call Denominator (`nvn_census_verdict.h` / `EvaluateCoverage`):**
   - `discovered = named_targets + audit_targets + unknown_targets`, where `audit_targets` are entry points discovered by Dynarmic's A64 external-branch audit (`OnAuditBranch`), excluding return sites (`EntryClass::ReturnSite`: a `RET` landing immediately after `BL`/`BLR`/`BLRAA*`/`BLRAB*` in library code).
   - Completeness requires all 11 conditions: `bootstrap_found_before_first_request`, `every_target_resolved` (`unknown_targets == 0`), `every_target_instrumented`, `audit_found_entries_counted_at_entry`, `no_unknown_dispatch` (`unknown_calls == 0`, `unaudited_entries == 0`), `audit_matches_counts` (`audit_shortfall_calls <= audit_tolerance`), `frame_accounting_exact` (`executed == startup + frames + late + tail`, `late == 0`, `mismatched == 0`), `one_present_stream`, `presents_consistent` (`frames_closed + 1 == presents`), `enough_frames`, `no_dropped_or_lost_events`.
2. **Effect Reconciliation (Supported / Fallback / Unsupported / Unobservable):**
   - Every captured frame's GPU/host operations (`submit`, `consume`, `draw`, `compute_dispatch`, `syncpoint_increment`, `fence_signal`, `readback_visibility`, `present_compose`) must appear in `effects.jsonl` with an explicit `classification` in `{"supported", "fallback", "unsupported", "unobservable"}` and `reconciled: true`, such that `supported + fallback + unsupported + unobservable == total_events`.
3. **Current Two-Title Status (Sanitized Historical Baseline vs. Candidate Gap):**
   - Historical pre-fix `fork-0.6.0-pre1` (`a62b6b2641`, `src/fe-lab/plans/wave7_Run06.md`):
     - `FE:FW` (300 s, `--nvn-census-audit`): `8,765` presents / `8,764` closed frames / `24,931,131` counted calls -> **INCOMPLETE** (`82` unknown entry points, `2,194,378` unknown calls, `4` audit mismatches, instrumented share `0.9008`); capture failed at checkpoint cut (`capture_failed`).
     - `XC2` (300 s, `--nvn-census-audit`): `17,590` presents / `17,589` closed frames / `134,723,191` counted calls -> **INCOMPLETE** (`76` unknown entry points, `186,292,179` unknown calls, `2` audit mismatches, instrumented share `0.9070`); capture failed at checkpoint cut (`capture_failed`).
   - Post-`f06/census` + `f06/capture-cut` two-title live capture/replay/census runs on candidate `b5aef6b73b` + `f3a6061505` + `1d7115b7ed`: **NOT YET EXECUTED** (awaiting GPU slot reservation).

---

## 4. Standalone Replay Runner Invocation & Original Redistributable Fixture

### Original Redistributable Fixture & Offline Verifier (`tools/replay_qualify/`)
- Fixture directory: `tools/replay_qualify/fixtures/synthetic_scene/` (pure procedural 32x32 RGBA8 2-frame scene: `manifest.json`, `frames.jsonl`, `effects.jsonl`, `census.json`, `live_00000001.png`, `replay_00000001.png`, `live_00000002.png`, `replay_00000002.png` — zero game data).
- Verifier (`tools/replay_qualify/replay_qualify.py`, Python 3 standard library only):
  - Image envelope: `max_rgb_mae <= 1.0`, `max_channel_delta <= 4`, `outlier_fraction <= 0.005` (threshold `2`), `max_alpha_delta == 0`, strict RGBA8 PNG CRC & dimension check.
  - Sync & observable effects: monotonic `frame_id`, `syncpoint_value`, `fence_id`; `submit`/`consume`/`syncpoint_increment`/`fence_signal` ordered before `present_compose`; `semantic_mismatches == 0`; 100% effect classification reconciliation.
  - Census check: validates all 11 schema-2 completeness conditions and rejects `capture_failed` stop pollution.

### Single-Command Reproduction

```sh
# 1. Original fixture verification (PASS, exit 0)
python -I -S -B tools/replay_qualify/replay_qualify.py --verify tools/replay_qualify/fixtures/synthetic_scene

# 2. Positive & intentional mismatch-rejection controls (8/8 checks, exit 0)
python -I -S -B tools/replay_qualify/replay_qualify.py --selftest

# 3. C++ compute clean-cut regression test (built in build/agy-issue-5)
tests.exe "[compute_clean_cut]"
```

### Candidate Runner Invocations (For Title-Owning Maintainer / Reserved Live Slot)

```sh
# Audited graphics call census (owned title, baseline settings, sanitized JSON only)
eden-cli --baseline -g <owned_title_path> --nvn-census <census.json> --nvn-census-audit --run-seconds 300 --report <census_report.json>

# Frame capture (requires f3a6061505 native compute clean-cut fix)
eden-cli --baseline -g <owned_title_path> --capture <scene.egcap> --capture-frames <FIRST>:<COUNT> --run-frames <STOP_FRAME> --report <capture_report.json>

# Standalone replay without the game running
eden-cli --replay <scene.egcap> --replay-images <replay_images_dir> --report <replay_report.json>
```
