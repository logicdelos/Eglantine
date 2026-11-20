# Regression Checker Qualification (Issue #6)

Statistical regression checker (`regress_stats.py`) and frozen contract (`regress_contract.py`) for detecting real-frame median and tail (`1 %` low) slowdowns beyond measured run-to-run noise.

## Statistical Design & Protocol

1. **Pinned inputs:** Every run in a manifest pins the scene, true-present real-frame metric (`host_present` or `replay_frame` via `--frame-times`, or PresentMon `MsBetweenPresents`), `cache_state` (`cold` or `warm`), `settings`, `device` (hardware/driver), and `build` identity.
2. **A/A first:** Unchanged-vs-unchanged A/A pairs establish sample count, paired ordering (`accounting.paired_ordering`), observed spread `s`, and whether the run has power to detect `DELTA = -ln(0.95) = 0.051293` (a 5 % frame-rate loss) at family-wise `alpha = 0.01` (Bonferroni over `K = scenes * 2 metrics * looks`) and planning `beta = 0.05`.
3. **Reversible 5 % slowdown positive control (`self_proof` mode):** Separate from the A/A arm (`aa: baseline vs baseline_repeat`), a reversible delay `delay_for_loss(T) = T / 19` is injected (`injection: baseline vs injected`) to produce a true 5 % throughput drop (valid when achieved loss lies in `4.5–5.5 %`, or 95 % CI within `4–6 %` covering `5 %`).
   - In `self_proof` mode: exit `0` (`PASS`) requires **both** A/A `PASS` (no false alarm) and the 5 % injection `FAIL` (caught and valid). A false alarm on A/A exits `1` (`FAIL`).
   - In `check` mode: exit `0` (`PASS`) requires ruling out a 5 % loss across all scenes; exit `1` (`FAIL`) flags a significant slowdown in `median_fps` or `inverse_p99`.
4. **Integrity & rejection guards:**
   - **Incomplete metrics or shutdown:** Rejects truncated/lossy `--frame-times` logs, `metrics_complete=false`, and `shutdown_complete=false` (inline or in `--report` JSON `exit` block) -> exit `3` (`INCONCLUSIVE`) or `2` (`INVALID`).
   - **Generated-frame inflation:** Rejects `frame_gen=true` (inline or in `--report` JSON `settings`), `generated_frames > 0`, `frame_gen_multiplier > 1`, or generated-frame sources -> exit `3` (`INCONCLUSIVE`).
   - **Mixed cold/warm samples:** Rejects mismatched `cache_state` within any pair, mismatched `cache_state` against a manifest-level pinned `cache_state`, and mixing `cold` and `warm` pairs within a scene -> exit `3` (`INCONCLUSIVE`).
   - **Synthetic vs measured separation:** Inline `frame_ms` and synthetic runs are always labelled `evidence="synthetic"` and rejected if mixed with measured runs (`EXIT_BAD_INPUT = 2`).

## Synthetic CI Fixture & Invocations

Run unit tests (92 tests) and the checked-in synthetic CI fixture suite (`tools/qualification/regression/fixtures/`):

```sh
python -B tools/qualification/regression/test_regress_stats.py
python -B tools/qualification/regression/verify_ci_fixtures.py
```

Direct `regress_stats.py` invocations against individual synthetic CI fixtures:

```sh
# 1. A/A unchanged control -> exit 0 (PASS)
python -B tools/qualification/regression/regress_stats.py --runs tools/qualification/regression/fixtures/aa_control_runs.json

# 2. Self-proof (A/A + reversible 5% slowdown positive control) -> exit 0 (PASS, proof caught)
python -B tools/qualification/regression/regress_stats.py --runs tools/qualification/regression/fixtures/self_proof_5pct_runs.json

# 3. Check mode against 5% slowed positive control -> exit 1 (FAIL)
python -B tools/qualification/regression/regress_stats.py --runs tools/qualification/regression/fixtures/check_5pct_regression_runs.json

# 4. Negative controls (incomplete metrics/shutdown, generated frames, mixed cold/warm, truncated log)
python -B tools/qualification/regression/regress_stats.py --runs tools/qualification/regression/fixtures/neg_incomplete_metrics_runs.json   # exit 3
python -B tools/qualification/regression/regress_stats.py --runs tools/qualification/regression/fixtures/neg_incomplete_shutdown_runs.json  # exit 3
python -B tools/qualification/regression/regress_stats.py --runs tools/qualification/regression/fixtures/neg_generated_frames_runs.json     # exit 3
python -B tools/qualification/regression/regress_stats.py --runs tools/qualification/regression/fixtures/neg_mixed_cache_runs.json          # exit 3
python -B tools/qualification/regression/regress_stats.py --runs tools/qualification/regression/fixtures/neg_truncated_log_runs.json        # exit 2
```

## Sanitized Title A/A Result & Live Injection Status

Measured A/A control result is checked in as [`sanitized_title_aa_result.json`](sanitized_title_aa_result.json) (path-free, zero game data/keys/firmware).

- **Candidate build:** `b5aef6b73b2229c9930668eccb395e9f894a2886` (`acc/h-int54`, `Eglantine-w23-s54-verified\eden-cli.exe`, SHA-256 `03edae9b781bf437bba1ebb73d306626c5bf4cb80a8032ec9cb71a3c5dfc8326`).
- **Pinned configuration:** sanitized scene `sanitized_title_scene` (XC2 title screen), true-present `host_present` metric (`--frame-times`, 420 frames per run), `cache_state="warm"`, hardware/driver `NVIDIA GeForce RTX 5070 Ti Laptop GPU` (`621.14`, Windows 11), settings `--baseline --baseline-allow use_speed_limit=false,allow_speed_up=true,use_vsync=0` (`allow_speed_up=true`, `frame_gen=false`).
- **Exact per-run invocation (8 alternating AB/BA runs in isolated temp `--user-dir`):**

```powershell
eden-cli.exe -g "<local-title-path>" --user-dir "<isolated-temp-user-dir>" --baseline --baseline-allow use_speed_limit=false,allow_speed_up=true,use_vsync=0 --run-frames 420 --run-seconds 55 --report "<tag>.report.json" --frame-times "<tag>.jsonl"
python -B tools/qualification/regression/regress_stats.py --runs aa_live_runs.json --out sanitized_title_aa_result.json
```

- **A/A outcome (`n=4` alternating pairs, exit `0`, `evidence="measured"`):**
  - `sanitized_title_scene:median_fps` (`gating`): `loss = -0.10 %` (upper `+0.65 %`), spread `s = 0.0013`, `c = 11.45`, minimum detectable `0.85 %` (<= `5 %` threshold `delta = 0.051293`) -> **PASS**.
  - `sanitized_title_scene:inverse_p99` (`info`): `loss = -13.77 %` (upper `+91.86 %`), `s = 0.4606`, sensitivity `95.10 %` -> **INCONCLUSIVE** (non-gating; tail includes boot frames inside the 420-frame window).
  - All 8 runs exited `0` (`frame_limit`) with `metrics_complete=true` and `shutdown_complete=true`.
- **Live 5 % positive-control injection status on `b5aef6b73b` (open blocker):**
  - On `b5aef6b73b`, `EGLANTINE_TEST_FRAME_DELAY_NS` was removed (`src/common/test_frame_delay.h`), and `--g0-inject-delay-ns` requires `--g0-metrics` and `--replay` (`src/video_core/replay/options.cpp`).
  - Attempting `--capture` on `b5aef6b73b` exited `76` (`capture_failed: "the checkpoint could not be cut: channel 1: a compute upload waits for its launch (compute)"`, on both tested titles), blocking live-capture `--replay` delay injection until the Issue #5 compute clean-cut fix (`agy/issue-5`) is integrated.
  - On `Eglantine-fork-0.6.0-pre2` (`66c8d36731`), live runs reached only 9–11 frames in 35–40 s during cold/warm shader compilation.
  - The reversible 5 % positive-control proof is therefore verified end-to-end on the synthetic `--frame-times` CI fixture (`fixtures/self_proof_5pct_runs.json` exit `0` and `fixtures/check_5pct_regression_runs.json` exit `1`), while live title positive-control injection on candidate `b5aef6b73b` remains open pending Issue #5's compute clean-cut merge.

