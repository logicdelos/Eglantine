<!--
SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
SPDX-License-Identifier: GPL-3.0-or-later
-->
<!-- lang: en-GB -->

# Issue #5: Two-Title Graphics Census and Standalone Replay Qualification

**Public base (`fork-main`):** `dcdc82a4d6dd02fddc72efe69595ad99aa5ab817`
**Candidate from #1 (`acc/h-int54`):** `b5aef6b73b2229c9930668eccb395e9f894a2886` (`Eglantine-w23-s54-verified\MANIFEST.txt`, `eden-cli.exe` SHA-256 `03edae9b781bf437bba1ebb73d306626c5bf4cb80a8032ec9cb71a3c5dfc8326`)
**Working branch (`agy/issue-5`):** code fix + unit test + original standalone replay/census fixture and verifier.

> **Status boundary:** This offline fixture exercises the verifier, not a title or the standalone replay engine. Two-title (`FE:FW` and `XC2`) census and replay promotion remains **uncertified** without complete live evidence.

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

- `src/video_core/engines/engine_upload.{h,cpp}`: a transfer remains pending until its declared bytes are flushed. Legacy bulk transfers advance the same cursor as scalar writes; short or mixed packets cannot count unstaged bytes as written.
- `src/video_core/engines/kepler_compute.cpp`: guards `uploads.push_back(info)` with `if (!native_mode)`.
- `src/video_core/engines/kepler_compute.h`: adds `KeplerCompute::CleanCutBlocker()` with distinct per-condition blockers.
- `src/tests/video_core/compute_clean_cut.cpp` (registered in `src/tests/CMakeLists.txt`): Catch2 tests verifying:
  1. Native mode (`native_mode = true`) running 200 inline uploads stays at a clean cut (`CleanCutBlocker() == nullptr`).
  2. Unfinished inline upload (`data_upload` incomplete) reports `"an inline compute upload is unfinished"` until complete.
  3. Legacy mode (`!native_mode`) reports `"a compute upload waits for its launch"` until `launch` executes and drains `uploads`.
  4. Whole and split legacy bulk packets (including last-word padding) drain after launch; a staged but unflushed native word still blocks a checkpoint.

---

## 3. Census Denominator & Call/Effect Reconciliation Contract

For a title reference scene to pass Issue #5, **all** calls and effects must reconcile against an independent denominator with zero silent omissions and zero per-game hooks:

1. **Independent Call Denominator (`nvn_census_verdict.h` / `EvaluateCoverage`):**
   - `discovered = named_targets + audit_targets + unknown_targets`, where `audit_targets` are entry points discovered by Dynarmic's A64 external-branch audit (`OnAuditBranch`), excluding return sites (`EntryClass::ReturnSite`: a `RET` landing immediately after `BL`/`BLR`/`BLRAA*`/`BLRAB*` in library code).
   - Completeness requires all 11 conditions: `bootstrap_found_before_first_request`, `every_target_resolved` (`unknown_targets == 0`), `every_target_instrumented`, `audit_found_entries_counted_at_entry`, `no_unknown_dispatch` (`unknown_calls == 0`, `unaudited_entries == 0`), `audit_matches_counts` (`audited == known_entries`, `shortfall == 0`, reported shortfall equal, `audited <= executed`; the older one-entry tolerance is gone), `frame_accounting_exact` (`executed == startup + frames + late + tail`, `late == 0`, `mismatched == 0`), `one_present_stream`, `presents_consistent` (`frames_closed + 1 == accepted_presents`), `enough_frames`, `no_dropped_or_lost_events`.
   - Frame ids (census report schema 3, `frame_id_basis: "closing_accepted_present"`): census frames close at the same admitted-present seam the recorder numbers `FRAME_END` from (`VideoCore::Replay::NoteAcceptedPresent`, then `Core::NvnCensus::NoteAcceptedPresent` with the same id). A `frame_rows` entry's frame is the id of the accepted present that closed it. Start-up, the calls before the first present, is frame 1, so census frame N's calls equal capture `FRAME_END(N).call_count`. Refresh-only and dropped presents close no frame in either. `frames.presents` counts every stream present and `frames.accepted_presents` the admitted ones. `frames.rows_joinable` is false, with `rows_unjoinable_reason` set, unless one unambiguous stream produced every present, each of them was admitted, and no row was dropped. A verifier must refuse per-frame qualification when it is false and must apply no offset mapping.
2. **Event Reconciliation (Supported / Fallback / Unsupported / Unobservable):**
   - A replay witness is an ordered pair of independent JSONL ledgers: `expected_effects.jsonl` (live/capture side, from the recorder) and `effects.jsonl` (replay/observed side). Each row is `{schema: 1, frame_id, ordinal, kind, classification, payload?, outcome?, reason?}`. Event identity is `(frame_id, ordinal, kind)`; `ordinal` is zero-based, unique and contiguous from 0 within a frame, and both ledgers must reproduce the same identity sequence in recorded order.
   - `frames.jsonl` schema 2 carries `call_count` and `effect_count` copied from the stream's `FRAME_END`, never recomputed from a sidecar. Per frame, `effect_count` must equal both ledgers' row counts. That is how an effect deleted from **both** sidecars is still rejected: the stream count disagrees. A missing or extra frame ID, a missing PNG, a changed pixel beyond the envelope, or a stale checksum also fails. `call_count` must match the live census's per-frame `frame_rows` total (see §Command Line Contract): the frame id is the closing accepted-present id on both sides, compared directly with no offset or epoch mapping, and a census report that does not declare that convention is rejected.
   - `supported` and `fallback` require equal `payload` and `outcome` (strict JSON equality, so `1` and `true` do not match). A live/expected row is `supported` when the capture saw a normal event; the replay/observed row may instead be `fallback` when the renderer takes a documented alternate path. That refinement is allowed only with equal payload/outcome and a non-empty observed reason. A live-supported effect that becomes `unsupported` or `unobservable` in replay is a loss and fails. `fallback` on both sides requires a reason on both. `unobservable` and `unsupported` require equal classification and a non-empty reason on both sides, and must not declare payload/outcome — a fabricated match is rejected.
   - Missing, extra, duplicate, reordered or mismatched effects, declared-count disagreement and frame gaps return exit 1. `unsupported`/`unobservable` rows are counted by class with their reasons published; while any remain the result carries `replay_equivalent: false` and must not be read as replay equivalence. `--require-equivalent` promotes that honest non-equivalence to exit 3 (INCOMPLETE).
3. **Current Two-Title Status (Sanitized Historical Baseline vs. Candidate Gap):**
   - Historical pre-fix `fork-0.6.0-pre1` (`a62b6b2641`, `src/fe-lab/plans/wave7_Run06.md`):
     - `FE:FW` (300 s, `--nvn-census-audit`): `8,765` presents / `8,764` closed frames / `24,931,131` counted calls -> **INCOMPLETE** (`82` unknown entry points, `2,194,378` unknown calls, `4` audit mismatches, instrumented share `0.9008`); capture failed at checkpoint cut (`capture_failed`).
     - `XC2` (300 s, `--nvn-census-audit`): `17,590` presents / `17,589` closed frames / `134,723,191` counted calls -> **INCOMPLETE** (`76` unknown entry points, `186,292,179` unknown calls, `2` audit mismatches, instrumented share `0.9070`); capture failed at checkpoint cut (`capture_failed`).
   - Post-`f06/census` + `f06/capture-cut` two-title live capture/replay/census runs on candidate `b5aef6b73b` + `f3a6061505` + `1d7115b7ed`: **NOT YET EXECUTED** (awaiting GPU slot reservation).

---

## 4. Standalone Replay Runner Invocation & Original Redistributable Fixture

### Original Redistributable Fixture & Offline Verifier (`tools/replay_qualify/`)
- Fixture directory: `tools/replay_qualify/fixtures/synthetic_scene/` — pure procedural 32x32 RGBA8, two frames, zero game data: `manifest.json` (schema 3, kind `standalone_replay_qualification_fixture`), `frames.jsonl` (schema 2, `call_count` + `effect_count` per frame), `expected_effects.jsonl`, `effects.jsonl`, `census.json`, `live_0000000N.png`, `replay_0000000N.png`. This is an accounting fixture, **not** a `.egcap` and not title evidence.
- Each frame declares nine effects: six `supported`, one `fallback`, one `unobservable` and one `unsupported`. The fixture therefore passes structural reconciliation but reports `replay_equivalent: false` with its opaque reasons — the honest non-equivalence path is exercised, and `--require-equivalent` exits 3. Do not read a PASS here as replay equivalence.
- Verifier (`tools/replay_qualify/replay_qualify.py`, Python 3 standard library only):
  - Image envelope: `max_rgb_mae <= 1.0`, `max_channel_delta <= 4`, `outlier_fraction <= 0.005` (threshold `2`), `max_alpha_delta == 0`, strict RGBA8 PNG CRC and dimension check. `frames.jsonl` checksums are 16-digit hexadecimal over **BGRA** before PNG conversion.
  - Reconciliation: event identity `(frame_id, ordinal, kind)`, both ledgers in recorded order, per-frame declared counts, payload/outcome equality for `supported`/`fallback`, classification-specific reason rules for opaque effects. Rejections cover missing, extra, duplicate, reordered, mismatched, deleted-from-both-sidecars, missing-frame, stale-checksum and changed-image cases.
  - Live guest calls: every frame's `call_count` (copied from `FRAME_END`, never recomputed) must equal the live census `frame_rows` total for that same frame id. Supply `--title-census <census.json>`; an in-directory `census.json` with `frame_rows` is used when the flag is absent. A frame missing from the census, an off-by-one total, or a non-zero `call_count` with no census is FAIL (fail closed, counts never rewritten). A zero-call witness needs no census.
  - Capture anchoring: a `standalone_replay_witness` (and any run given `--capture`) must supply a finalized EGLCAP v2 stream. The verifier reads only the 256-byte header, `INDEX`, `FRAME_END` and `COMPLETE`, checks CRC/header/length/`Complete`/required features, derives the trusted window from `preroll + finalized`, and requires it to equal `manifest.expected_frame_ids` with no gaps. Per selected frame, `frames.jsonl` `call_count`/`effect_count` must equal the stream `FRAME_END` counts. The adjacent `report.json` (or `--run-report`) must be complete, replay every selected frame, write every picture, execute the declared pre-roll, report zero guest calls, and carry the `COMPLETE.chain_digest`; a missing/truncated capture, missing report or wrong window is INVALID. Pre-roll effect counts come from `preroll_frames.jsonl` and are reconciled through `preroll_effects.jsonl` in frame order, so a dropped pre-roll effect is still caught.
  - Exit codes: `0` PASS, `1` reconciliation/image FAIL, `2` INVALID input (malformed JSON, schema mismatch, missing files, frame gap, truncated/partial capture, missing report), `3` INCOMPLETE (census gate failed, or `--require-equivalent` with opaque effects remaining).
  - `--reconcile-census` derives coverage from the emitted schema-2 `functions`, `bootstrap`, `audit`, `errors`, `accounting`, `guest_threads`, `frames` and `provenance` sections. Only a final, clean, independently complete census passes; a separate `replay_capture.failed` does not alter the census verdict.
  - `--verify-images` remains image/semantics-only: it cannot certify effect coverage or a complete capture.
  - `--write-fixture <empty-dir>` regenerates deterministic bytes and refuses a non-empty directory, so real evidence is never overwritten. Qualification manifest schema 3 and frame schema 2 are required; schema-2 manifests and format-1 streams are rejected rather than silently upgraded.

### Single-Command Reproduction

```sh
# 1. Positive & adversarial self-controls (73 checks; exit 0)
python -X utf8 -I -S -B tools/replay_qualify/replay_qualify.py --selftest

# 2. Original fixture verification (PASS, exit 0; replay_equivalent false by design)
python -X utf8 -I -S -B tools/replay_qualify/replay_qualify.py --verify tools/replay_qualify/fixtures/synthetic_scene

# 3. C++ compute clean-cut regression tests (when built)
tests.exe "[compute_clean_cut]"
```

### Real Witness and Dual-Run Usage

```sh
# One run: observed side next to the replay images, capture-side ledger supplied separately.
python -X utf8 -I -S -B tools/replay_qualify/replay_qualify.py \
  --verify <replay_dir> --expected-effects <capture_dir>/expected_effects.jsonl \
  [--capture <scene.egcap> [--run-report <replay_dir>/report.json]] [--require-equivalent]

# Game-free replay witness for a capture with executed guest titles: reconcile calls.
python -X utf8 -I -S -B tools/replay_qualify/replay_qualify.py \
  --verify-replay <replay_dir> --capture <scene.egcap> \
  --expected-effects <capture_dir>/expected_effects.jsonl \
  --title-census <census.json> [--require-equivalent]

# A zero-call witness still needs its finalized capture + report; only the census is absent.
python -X utf8 -I -S -B tools/replay_qualify/replay_qualify.py --verify-replay <replay_dir> \
  --capture <scene.egcap>

# Two runs: verify each independently, then require identical witnesses.
python -X utf8 -I -S -B tools/replay_qualify/replay_qualify.py \
  --compare-runs <run_a> <run_b> [--witness-only] [--title-census <census.json>] \
  [--capture <scene.egcap>]
```

`--compare-runs` compares per-frame ledger content and image checksums (not path names), so two runs from different directories compare cleanly; any divergence exits 1. `--verify-replay` reconciles every `call_count` against the live census's per-frame `frame_rows` total; a non-zero count without `--title-census` (or an in-directory census) is unreconciled and exits 1, never rewritten. The verifier never rewrites, migrates or deletes a witness or capture.

### Candidate Runner Invocations (For Title-Owning Maintainer / Reserved Live Slot)

```sh
# Audited graphics call census (owned title, baseline settings, sanitized JSON only)
eden-cli --baseline -g <owned_title_path> --nvn-census <census.json> --nvn-census-audit --run-seconds 300 --report <census_report.json>

# Frame capture (requires f3a6061505 native compute clean-cut fix)
eden-cli --baseline -g <owned_title_path> --capture <scene.egcap> --capture-frames <FIRST>:<COUNT> --run-frames <STOP_FRAME> --report <capture_report.json>

# Standalone replay without the game running
eden-cli --replay <scene.egcap> --replay-images <replay_images_dir> --report <replay_report.json>
# Full witness: observed ledgers + manifest + frames must sit in <replay_images_dir>.
python -X utf8 -I -S -B tools/replay_qualify/replay_qualify.py --verify <replay_images_dir> \
  --expected-effects <capture_dir>/expected_effects.jsonl --require-equivalent
# Image-only fallback; does not certify effect coverage.
python -X utf8 -I -S -B tools/replay_qualify/replay_qualify.py --verify-images <replay_images_dir>
python -X utf8 -I -S -B tools/replay_qualify/replay_qualify.py --reconcile-census <census.json>
```
