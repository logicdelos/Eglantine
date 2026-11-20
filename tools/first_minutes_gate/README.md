# First-120-second cache gate (0.5)

From the repository root, score one title's two original schema-2 reports. This command reads the reports, writes JSON to stdout and does not launch the emulator:

```sh
python tools/first_minutes_gate/first_minutes_gate.py score-pair --cold cold/report.json --second second/report.json
```

Use the default baseline check for release evidence. `--no-baseline` is diagnostic only. A comparable pair needs a clean exit with complete metrics, a verified scene, matching inputs, build, content, device/driver, effective settings and user directory. The transferable cache must be `missing` on cold and `accepted` on second launch. Independently retain the launcher evidence for the binary and seed hashes, save/input identity, genuine scene verification and isolated cache inventories; the reports alone cannot prove these. Scorer output contains local paths: keep it private.

| Pair exit | Meaning |
|---|---|
| 0 | `PASS`: cold attributed count was positive and twice the second attributed count was no greater than cold. |
| 1 | `FAIL` or `NOTHING_TO_HALVE` (zero cold attributed frames). |
| 2 | `INCOMPLETE`: read `reasons`. Do not substitute raw counts for missing attribution. |

`raw_count` counts real host frames over 100 ms ending in `(first_host_present, first_host_present + 120 s]`. `emulator_attributed_count` counts only those whose remainder after removing emulator causes is at most 100 ms. A larger remainder is a `game_gap` under this rule, not proof the game caused the delay. Each frame's `inferred_ns` identifies the producer-inferred **subset** of its cause times (`null` if not reported); do not add it again or call it measured work. `unknown_ns` is the unclassified cause time, not a synonym for inferred time. Over 10% unknown in any long frame, lost attribution or an incomplete window is not a passing count. A pair verdict is not a two-title gate result; even a two-title median pass does not prove incompatible-cache rejection.

For a separate rejection launch, compare the report with **that launch's** log:

```sh
python tools/first_minutes_gate/first_minutes_gate.py check-reject --report reject/report.json --log reject/eden_log.txt --reason format_version
```

Use `--reason legacy_format` for the legacy case. Exit 0 confirms the expected rejection; exit 1 means the evidence did not match. See `--help` for read-only cache discovery and synthetic header creation (new disposable path only).

Run only this tool's offline synthetic regression suite:

```sh
python -B tools/first_minutes_gate/test_first_minutes_gate.py -v
```
