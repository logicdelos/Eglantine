#!/usr/bin/env python3
"""Offline unit tests for tools/first_minutes_gate/first_minutes_gate.py using synthetic fixtures."""

from __future__ import annotations

from contextlib import redirect_stderr
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import first_minutes_gate as fg  # noqa: E402

ORIGIN = 1_000_000_000
MS = 1_000_000
SYNTHETIC_TID = "00000000deadbeef"


def make_frame(idx: int, duration_ms: int, **causes_ms: int) -> dict:
    cause = dict.fromkeys(fg.ALL_CAUSES, 0)
    used = 0
    for name, value in causes_ms.items():
        cause[name] = value * MS
        used += cause[name]
    cause["waiting"] += duration_ms * MS - used
    start = ORIGIN + (idx + 1) * 5_000_000_000
    return {
        "id": 100 + idx,
        "start_ns": start,
        "end_ns": start + duration_ms * MS,
        "duration_ns": duration_ms * MS,
        "cause_ns": cause,
        "primary_cause": "waiting",
        "instrumentation_complete": True,
    }


def make_report(
    frames: list[dict],
    *,
    baseline_hash: str = "a" * 64,
    on_baseline: bool = True,
    lost_events: int = 0,
    unknown_extra_ms: int = 0,
    exit_block: dict | None = None,
    build: dict | None = None,
    title_id: str = SYNTHETIC_TID,
    warm: bool = False,
) -> dict:
    if unknown_extra_ms and frames:
        frames[0]["cause_ns"]["unknown"] += unknown_extra_ms * MS
        frames[0]["cause_ns"]["waiting"] -= unknown_extra_ms * MS
    doc = {
        "schema_version": 2,
        "title_id": title_id,
        "build": build if build is not None else {"revision": "revA", "build_id": "idA"},
        "user_dir": "isolated/user",
        "settings": {"initial": {"cpu_accuracy": 0}, "final": {"cpu_accuracy": 0}},
        "tunables": {},
        "cache": {
            "status": "reported",
            "identity": {
                "build_id": "a" * 16, "title_id": title_id.lower(), "game_version": 0,
                "content_stamp": "b" * 16, "settings_hash": "c" * 16,
                "envelope_version": 1, "shader_format_version": 37,
                "device": {"vendor_id": 1, "device_id": 2, "driver_id": 3,
                           "driver_version": 4, "pipeline_cache_uuid": "d" * 32},
            },
            "files": [{"kind": "transferable", "status": "accepted" if warm else "missing",
                       "reason": "none" if warm else "missing"}],
        },
        "baseline": {
            "version": 1,
            "hash": baseline_hash,
            "requested": True,
            "on_baseline": on_baseline,
            "deviations": [{"key": "memory_layout_mode", "effective": "4",
                            "baseline": "0", "source": "program requirement"}],
        },
        "startup": {
            "first_host_present_ns": ORIGIN,
            "interactive_scene": {"id": "scene", "origin": "lab", "status": "complete",
                                  "scene_check": "passed", "marker_ns": ORIGIN + MS},
            "process_to_first_present": {"status": "complete"},
            "first_present_to_interactive": {"status": "complete"},
        },
        "first_120s": {
            "elapsed_ns": fg.WINDOW_NS,
            "anchor": "first_real_host_present",
            "scene_check": "passed",
            "frames_over_100ms": len(frames),
            "complete": True,
        },
        "attribution": {
            "source": "host_present_submission",
            "threshold_ns": fg.LONG_FRAME_NS,
            "enabled": True,
            "complete": True,
            "lost_events": lost_events,
            "dropped_boundaries": 0,
            "sink_write_errors": 0,
            "producer_unknown_wait_ns": 0,
            "long_frames": frames,
        },
        "exit": exit_block
        if exit_block is not None
        else {
            "code": 0,
            "reason": "requested",
            "metrics_complete": True,
            "shutdown_complete": True,
        },
    }
    return doc


class FirstMinutesGateTests(unittest.TestCase):
    def test_halving_pass_with_game_gaps_excluded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_p = root / "cold.json"
            sec_p = root / "second.json"
            # Cold: 4 emulator-caused + 2 game-gap frames (6 raw)
            cold_frames = [
                make_frame(0, 180, shader=120),
                make_frame(1, 200, pipeline=150),
                make_frame(2, 160, code_translation=100),
                make_frame(3, 190, texture=140),
                make_frame(4, 600, shader=20),  # remainder 580 ms -> game gap
                make_frame(5, 500, disk=10),    # remainder 490 ms -> game gap
            ]
            # Second: 2 emulator-caused + 4 game-gap frames (6 raw -> raw didn't halve, attributed 4->2 did)
            sec_frames = [
                make_frame(0, 170, texture=120),
                make_frame(1, 160, code_translation=90),
                make_frame(2, 550, shader=5),
                make_frame(3, 520, pipeline=10),
                make_frame(4, 480, disk=10),
                make_frame(5, 450, decompression=10),
            ]
            cold_p.write_text(json.dumps(make_report(cold_frames)), encoding="utf-8")
            sec_p.write_text(json.dumps(make_report(sec_frames, warm=True)), encoding="utf-8")

            res = fg.score_pair(cold_p, sec_p, label="title_a")
            self.assertEqual(res["verdict"], "PASS")
            self.assertEqual(res["cold"]["raw_count"], 6)
            self.assertEqual(res["cold"]["emulator_attributed_count"], 4)
            self.assertEqual(res["cold"]["game_gap_count"], 2)
            self.assertEqual(res["second"]["raw_count"], 6)
            self.assertEqual(res["second"]["emulator_attributed_count"], 2)
            self.assertEqual(res["second"]["game_gap_count"], 4)

    def test_miss_reports_largest_residual_cause_and_keeps_open(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_p = root / "cold.json"
            sec_p = root / "second.json"
            cold_p.write_text(
                json.dumps(make_report([make_frame(0, 220, shader=160), make_frame(1, 200, texture=150)])),
                encoding="utf-8",
            )
            sec_p.write_text(
                json.dumps(make_report([make_frame(0, 320, texture=260), make_frame(1, 180, shader=100)], warm=True)),
                encoding="utf-8",
            )
            pair = fg.score_pair(cold_p, sec_p, label="title_b")
            self.assertEqual(pair["verdict"], "FAIL")
            self.assertEqual(pair["largest_residual_cause"], {"cause": "texture", "total_ms": 260.0})

            gate = fg.score_gate({"title_b": [pair]})
            self.assertEqual(gate["verdict"], "INCOMPLETE")  # only one title supplied
            self.assertEqual(gate["titles"]["title_b"]["verdict"], "FAIL")
            self.assertTrue(gate["milestone_open"])
            self.assertTrue(gate["titles"]["title_b"]["milestone_open"])

    def test_zero_cold_is_nothing_to_halve(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_p = root / "cold.json"
            sec_p = root / "second.json"
            cold_p.write_text(json.dumps(make_report([make_frame(0, 500, shader=10)])), encoding="utf-8")
            sec_p.write_text(json.dumps(make_report([], warm=True)), encoding="utf-8")
            pair = fg.score_pair(cold_p, sec_p)
            self.assertEqual(pair["verdict"], "NOTHING_TO_HALVE")
            gate = fg.score_gate({"t": [pair]})
            self.assertEqual(gate["verdict"], "INCOMPLETE")
            self.assertTrue(gate["milestone_open"])

    def test_unknown_over_10_percent_and_baseline_mismatch_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_p = root / "cold.json"
            sec_p = root / "second.json"
            cold_p.write_text(
                json.dumps(make_report([make_frame(0, 200, shader=120)], unknown_extra_ms=25)),
                encoding="utf-8",
            )
            sec_p.write_text(json.dumps(make_report([], warm=True)), encoding="utf-8")
            res = fg.score_pair(cold_p, sec_p)
            self.assertEqual(res["verdict"], "INCOMPLETE")
            self.assertIn("unknown share", res["reasons"][0])

            cold_p.write_text(json.dumps(make_report([make_frame(0, 200, shader=120)])), encoding="utf-8")
            sec_p.write_text(
                json.dumps(make_report([], baseline_hash="b" * 64, warm=True)),
                encoding="utf-8",
            )
            res2 = fg.score_pair(cold_p, sec_p)
            self.assertEqual(res2["verdict"], "INCOMPLETE")
            self.assertIn("baseline mismatch", res2["reasons"][0])

    def test_cache_dir_discovery_includes_semantics_revision_subdirs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            user_dir = Path(tmp) / "user"
            direct = user_dir / "cache" / "shader" / SYNTHETIC_TID
            rev6 = direct / "semantics_r6"
            rev6.mkdir(parents=True)
            (direct / "semantics_r6_backup").mkdir()
            (direct / "jit_blocks.mythos.bin").write_bytes(b"jit")
            (rev6 / "vulkan.mythos.bin").write_bytes(fg.build_synthetic_envelope(1))
            (rev6 / "vulkan_used.mythos.bin").write_bytes(b"used")

            dirs = fg.discover_title_cache_dirs(user_dir, SYNTHETIC_TID)
            self.assertEqual(dirs, [direct, rev6])
            self.assertEqual(fg.discover_title_cache_dirs(user_dir, "0xdeadbeef"), dirs)
            with self.assertRaises(ValueError):
                fg.discover_title_cache_dirs(user_dir, "../other")
            files = {p.relative_to(direct).as_posix() for p in fg.find_title_cache_files(user_dir, SYNTHETIC_TID)}
            self.assertEqual(
                files,
                {"jit_blocks.mythos.bin", "semantics_r6/vulkan.mythos.bin", "semantics_r6/vulkan_used.mythos.bin"},
            )

    def test_synthetic_envelope_stamp_and_rejection_verification(self) -> None:
        hdr = fg.build_synthetic_envelope(999)
        self.assertEqual(len(hdr), fg.CACHE_ENVELOPE_BYTES)
        self.assertTrue(hdr.startswith(fg.CACHE_ENVELOPE_MAGIC))
        self.assertEqual(struct.unpack_from("<I", hdr, fg.CACHE_VERSION_OFFSET)[0], 999)

        v1 = fg.build_synthetic_envelope(1)
        stamped = fg.stamp_rejected_envelope(v1, 999)
        self.assertIsNotNone(stamped)
        self.assertEqual(struct.unpack_from("<I", stamped, fg.CACHE_VERSION_OFFSET)[0], 999)

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rep = root / "report.json"
            log = root / "eden_log.txt"
            rep.write_text(
                json.dumps(
                    {
                        "schema_version": 2,
                        "exit": {"code": 0, "reason": "frame_limit", "metrics_complete": True,
                                 "shutdown_complete": True},
                        "cache": {
                            "status": "reported",
                            "files": [
                                {
                                    "kind": "transferable",
                                    "status": "rejected",
                                    "reason": "format_version",
                                    "expected": 1,
                                    "found": 999,
                                }
                            ]
                        },
                    }
                ),
                encoding="utf-8",
            )
            log.write_text(
                "[0.12] Render.Vulkan <Warning> Nx2Cache reject kind=transferable "
                "reason=format_version expected=1 found=999 action=ignore\n",
                encoding="utf-8",
            )
            ok = fg.verify_cache_rejection(rep, log)
            self.assertEqual(ok["verdict"], "PASS")

            rep_bad = root / "report_bad.json"
            rep_bad.write_text(json.dumps({"exit": {"code": 1}, "cache": {"files": []}}), encoding="utf-8")
            bad = fg.verify_cache_rejection(rep_bad, log)
            self.assertEqual(bad["verdict"], "FAIL")

    def test_header_generation_preserves_existing_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "existing.bin"
            path.write_bytes(b"original")
            with self.assertRaises(SystemExit) as error, redirect_stderr(io.StringIO()):
                fg.main(["make-reject-header", "--out", str(path)])
            self.assertEqual(error.exception.code, 2)
            self.assertEqual(path.read_bytes(), b"original")

    def test_exit_completeness_and_build_title_mismatch_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            cold_p = root / "cold.json"
            sec_p = root / "second.json"
            for bad_exit, expected_msg in (
                ({"code": 124, "reason": "timeout", "metrics_complete": True, "shutdown_complete": True}, "exit code"),
                ({"code": 0, "reason": "host_crash", "metrics_complete": True, "shutdown_complete": True}, "exit reason"),
                ({"code": 0, "reason": "requested", "metrics_complete": False, "shutdown_complete": True}, "metrics_complete"),
                ({"code": 0, "reason": "requested", "metrics_complete": True, "shutdown_complete": False}, "shutdown_complete"),
            ):
                cold_p.write_text(
                    json.dumps(make_report([make_frame(0, 200, shader=120)], exit_block=bad_exit)),
                    encoding="utf-8",
                )
                sec_p.write_text(json.dumps(make_report([], warm=True)), encoding="utf-8")
                res = fg.score_pair(cold_p, sec_p)
                self.assertEqual(res["verdict"], "INCOMPLETE")
                self.assertIn(expected_msg, res["reasons"][0])

            cold_p.write_text(
                json.dumps(
                    make_report(
                        [make_frame(0, 200, shader=120)],
                        build={"revision": "revA", "build_id": "idA"},
                        title_id=SYNTHETIC_TID,
                    )
                ),
                encoding="utf-8",
            )
            sec_p.write_text(
                json.dumps(
                    make_report(
                        [],
                        build={"revision": "revB", "build_id": "idA"},
                        warm=True,
                        title_id=SYNTHETIC_TID,
                    )
                ),
                encoding="utf-8",
            )
            res_build = fg.score_pair(cold_p, sec_p)
            self.assertEqual(res_build["verdict"], "INCOMPLETE")
            self.assertIn("build.revision mismatch", res_build["reasons"][0])

            sec_p.write_text(
                json.dumps(
                    make_report(
                        [],
                        build={"revision": "revA", "build_id": "idA"},
                        title_id="00000000feedface", warm=True,
                    )
                ),
                encoding="utf-8",
            )
            res_tid = fg.score_pair(cold_p, sec_p)
            self.assertEqual(res_tid["verdict"], "INCOMPLETE")
            self.assertIn("title_id mismatch", res_tid["reasons"][0])

    def test_inferred_is_a_subset_not_extra_work_or_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "report.json"
            frame = make_frame(0, 200, shader=90, unknown=20)
            doc = make_report([frame])
            path.write_text(json.dumps(doc), encoding="utf-8")
            self.assertIsNone(fg.score_report(path)["game_gap_frames"][0]["inferred_ns"])

            inferred = dict.fromkeys(fg.ALL_CAUSES, 0)
            inferred["shader"] = 85 * MS
            frame["inferred_ns"] = inferred
            path.write_text(json.dumps(doc), encoding="utf-8")
            run = fg.score_report(path)
            self.assertEqual((run["raw_count"], run["emulator_attributed_count"]), (1, 0))
            gap = run["game_gap_frames"][0]
            self.assertEqual(gap["emulator_ms"], 90)
            self.assertEqual(gap["unknown_ns"], 20 * MS)
            self.assertEqual(gap["inferred_ns"], inferred)

            inferred["shader"] = frame["cause_ns"]["shader"] + 1
            path.write_text(json.dumps(doc), encoding="utf-8")
            invalid = fg.score_report(path)
            self.assertIn("inferred_ns.shader", invalid["reason"])
            self.assertEqual(invalid["raw_count"], 1)
            self.assertNotIn("emulator_attributed_count", invalid)

    def test_window_threshold_and_exact_unknown_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cold.json"
            f = make_frame(0, 200, shader=100, unknown=20)
            f["end_ns"] = ORIGIN + fg.WINDOW_NS  # right endpoint included
            f["start_ns"] = f["end_ns"] - f["duration_ns"]
            doc = make_report([f])
            tail = dict(f, end_ns=f["end_ns"] + f["duration_ns"], start_ns=f["end_ns"])
            doc["attribution"]["long_frames"].append(tail)
            path.write_text(json.dumps(doc), encoding="utf-8")
            score = fg.score_report(path)
            self.assertTrue(score["valid"], score)
            self.assertEqual((score["raw_count"], score["emulator_attributed_count"]), (1, 1))
            f["cause_ns"]["unknown"] += 1
            f["cause_ns"]["waiting"] -= 1
            path.write_text(json.dumps(doc), encoding="utf-8")
            self.assertIn("unknown share", fg.score_report(path)["reason"])

            for block, key, value, diagnostic in (
                ("attribution", "threshold_ns", 200 * MS, "threshold_ns"),
                ("first_120s", "elapsed_ns", 45_000 * MS, "incomplete"),
                ("first_120s", "frames_over_100ms", False, "frames_over_100ms"),
                ("first_120s", "frames_with_history_loss", 1, "history_loss"),
                ("attribution", "dropped_boundaries", False, "dropped_boundaries"),
                ("attribution", "sink_write_errors", 1, "sink_write_errors"),
            ):
                with self.subTest(key=key):
                    doc = make_report([])
                    doc[block][key] = value
                    path.write_text(json.dumps(doc), encoding="utf-8")
                    score = fg.score_report(path)
                    self.assertFalse(score["valid"])
                    self.assertIn(diagnostic, score["reason"])
                    self.assertNotIn("emulator_attributed_count", score)

    def test_invalid_attribution_never_becomes_a_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "report.json"
            for case in ("negative", "incomplete", "overlap", "short", "wrong sum", "missing counter"):
                with self.subTest(case=case):
                    frame = make_frame(0, 200, shader=190)
                    doc = make_report([frame])
                    if case == "negative":
                        frame["cause_ns"].update(shader=201 * MS, waiting=-MS)
                    elif case == "incomplete":
                        doc["attribution"]["complete"] = False
                    elif case == "overlap":
                        doc["attribution"]["long_frames"].append(dict(frame))
                        doc["first_120s"]["frames_over_100ms"] = 2
                    elif case == "short":
                        doc["attribution"]["long_frames"][0] = make_frame(0, 100, shader=100)
                    elif case == "wrong sum":
                        frame["cause_ns"]["shader"] += 1
                    else:
                        del doc["attribution"]["lost_events"]
                    path.write_text(json.dumps(doc), encoding="utf-8")
                    score = fg.score_report(path)
                    self.assertFalse(score["valid"])
                    self.assertEqual(score["raw_count"], doc["first_120s"]["frames_over_100ms"])
                    self.assertNotIn("emulator_attributed_count", score)

    def test_reject_incomparable_runs_even_if_counts_would_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cold, warm = (Path(tmp) / name for name in ("cold", "second"))
            cold.write_text(json.dumps(make_report([make_frame(0, 200, shader=120)])), encoding="utf-8")
            for path, value in (
                (("cache", "identity", "device", "driver_version"), 100),
                (("cache", "identity", "content_stamp"), "0" * 16),
                (("cache", "files"), [{"kind": "transferable", "status": "missing", "reason": "missing"}]),
                (("user_dir",), "other"), (("settings", "final", "cpu_accuracy"), 2),
                (("tunables",), {"NX2_DEBUG": {"raw": "1"}}),
                (("startup", "interactive_scene", "id"), "different scene"),
                (("startup", "interactive_scene", "scene_check"), "unknown"),
                (("steps",), [{"action": "press", "key": 4, "when": "42", "status": "fired"}]),
                (("baseline", "hash"), None), (("baseline", "version"), None),
                (("baseline", "deviations"), [{"source": "allowed override"}]),
                (("exit", "reason"), None), (("exit", "reason"), "capture_failed"),
                (("title_id",), None), (("build",), {}), (("build",), "invalid"),
            ):
                with self.subTest(path=path, value=value):
                    doc = make_report([], warm=True)
                    block = doc
                    for key in path[:-1]:
                        block = block[key]
                    block[path[-1]] = value
                    warm.write_text(json.dumps(doc), encoding="utf-8")
                    self.assertEqual(fg.score_pair(cold, warm)["verdict"], "INCOMPLETE")

    def test_aggregate_requires_both_target_ids_and_one_build_and_device(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            def pair(tid, driver=4, revision="revA"):
                paths = []
                for warm in (False, True):
                    path = Path(tmp) / f"{tid}-{warm}"
                    doc = make_report([] if warm else [make_frame(0, 200, shader=120)],
                                      title_id=tid, warm=warm, build={"revision": revision, "build_id": "idA"})
                    doc["cache"]["identity"]["device"]["driver_version"] = driver
                    path.write_text(json.dumps(doc), encoding="utf-8")
                    paths.append(path)
                return fg.score_pair(*paths)

            fe_id, xc2_id = "04007EF01F8AE000", "0400E95004038001"
            fe, xc2 = pair(fe_id), pair(xc2_id)
            gate = fg.score_gate({"title A": [fe], "title B": [xc2]})
            self.assertEqual(gate["verdict"], "PASS", gate)
            self.assertIsNone(gate["milestone_open"])
            for other in (fe, pair(SYNTHETIC_TID), pair(xc2_id, driver=9), pair(xc2_id, revision="revB")):
                self.assertEqual(fg.score_gate({"FE": [fe], "XC2": [other]})["verdict"], "INCOMPLETE")

    def test_legacy_reject_requires_matching_report_log_and_clean_exit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report, log = (Path(tmp) / name for name in ("report", "log"))
            expected = int.from_bytes(b"EGL05CCH", "little")
            found = int.from_bytes(b"yuzucach", "little")
            doc = make_report([])
            doc["cache"]["files"] = [{"kind": "transferable", "status": "rejected", "reason": "legacy_format",
                                     "expected": expected, "found": found}]
            message = (f"[Warning] Nx2Cache reject kind=transferable reason=legacy_format "
                       f"expected={expected} found={found} action=backup_legacy")
            for text, complete, verdict in ((message, True, "PASS"), (message, False, "FAIL"),
                                           (message.replace(str(found), "1"), True, "FAIL"),
                                           (message.replace("backup_legacy", "ignore"), True, "FAIL"),
                                           (message.replace(" expected", "\nexpected"), True, "FAIL")):
                with self.subTest(text=text, complete=complete):
                    doc["exit"]["shutdown_complete"] = complete
                    report.write_text(json.dumps(doc), encoding="utf-8")
                    log.write_text(text, encoding="utf-8")
                    self.assertEqual(fg.verify_cache_rejection(report, log, expected_reason="legacy_format")["verdict"], verdict)


if __name__ == "__main__":
    unittest.main()
