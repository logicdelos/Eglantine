#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise capture/replay boundaries with the built CLI, no guest and no external inputs."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


class ReplayBoundaries(unittest.TestCase):
    def replay(self, mode):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        stream, profile = root / "input.egcap", root / "empty-profile"
        images, report = root / "images", root / "report.json"
        profile.mkdir()
        subprocess.run([self.producer, stream, mode], check=True, timeout=30)
        rc = subprocess.run([self.cli, "--replay", stream, "--user-dir", profile,
                             "--replay-images", images, "--report", report], timeout=180).returncode
        return rc, json.loads(report.read_text(encoding="utf-8"))["replay"], images

    @staticmethod
    def rows(images, name):
        return [json.loads(line) for line in (images / name).read_text(encoding="utf-8").splitlines()]

    def test_unmet_present_fence(self):
        rc, result, images = self.replay("--unmet-present-fence")
        self.assertNotEqual(rc, 0)
        self.assertFalse(result["run_complete"])
        self.assertIn("PRESENT guest syncpoint fence was not satisfied", result["error"])
        self.assertEqual(result["frames_replayed"], 1)
        self.assertFalse((images / "replay_00000002.png").exists())

    def test_guest_readbacks_survive_verification_probes(self):
        rc, result, images = self.replay("--guest-readback")
        self.assertEqual(rc, 0, result)
        self.assertTrue(result["run_complete"])
        self.assertEqual(result["frames_replayed"], 2)
        rows = self.rows(images, "effects.jsonl")
        self.assertEqual([row["kind"] for row in rows if row["frame_id"] == 1],
                         ["submit", "readback_visibility", "readback_visibility", "consume",
                          "fence_signal", "present_compose"])
        readbacks = [row for row in rows if row["kind"] == "readback_visibility"]
        self.assertEqual([row["frame_id"] for row in readbacks], [1, 1])
        self.assertTrue(all(row["classification"] == "unobservable" for row in readbacks))
        rc, result, _ = self.replay("--lost-guest-readback")
        self.assertNotEqual(rc, 0)
        self.assertFalse(result["run_complete"])
        self.assertEqual(result["frames_replayed"], 0)
        self.assertIn("on frame 1: expected 6, observed 4", result["error"])

    def test_preroll_is_accounted_separately(self):
        rc, result, images = self.replay("--preroll")
        self.assertEqual(rc, 0, result)
        self.assertTrue(result["run_complete"])
        self.assertEqual(result["replay_guest_call_count"], 0)
        self.assertEqual(result["preroll_frames_executed"], 1)
        self.assertEqual(result["frames_replayed"], 1)
        (warmup,) = self.rows(images, "preroll_frames.jsonl")
        (selected,) = self.rows(images, "frames.jsonl")
        self.assertEqual((warmup["frame_id"], warmup["output_id"], selected["frame_id"]), (1, 1, 2))
        self.assertFalse(warmup["has_live_picture"])
        self.assertEqual((warmup["width"], warmup["height"]), (32, 18))
        self.assertRegex(warmup["replay_checksum"], r"^[0-9a-f]{16}$")
        self.assertNotEqual(warmup["replay_checksum"], selected["replay_checksum"])
        expected = Path(__file__).resolve().parent / "fixtures" / "gpu_witness" / "expected_effects.jsonl"
        live = self.rows(expected.parent, expected.name)
        for frame, name in [(warmup, "preroll_effects.jsonl"), (selected, "effects.jsonl")]:
            self.assertEqual(frame["replay_guest_call_count"], 0)
            effects = self.rows(images, name)
            self.assertEqual(frame["effect_count"], len(effects))
            self.assertEqual(effects, [row for row in live if row["frame_id"] == frame["frame_id"]])
        self.assertFalse((images / "replay_00000001.png").exists())
        self.assertTrue((images / "replay_00000002.png").is_file())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--producer", type=Path, required=True)
    args = parser.parse_args()
    ReplayBoundaries.cli = args.cli.resolve()
    ReplayBoundaries.producer = args.producer.resolve()
    unittest.main(argv=[__file__])
