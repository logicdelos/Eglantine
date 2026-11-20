#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the original witness twice on the actual GPU and reject both corrupt controls."""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--producer", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="new directory outside the worktree")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    fixture = here / "fixtures" / "gpu_witness"
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    capture = out / "original.egcap"
    subprocess.run([args.producer.resolve(), capture], check=True)
    if capture.read_bytes() != (fixture / "witness.egcap").read_bytes():
        raise ValueError("generated capture is not byte-identical to the committed stream")
    expected = fixture / "expected_effects.jsonl"
    verifier = [sys.executable, "-X", "utf8", "-I", "-S", "-B", here / "replay_qualify.py"]

    def replay(label, path, rejection=None, completed=0):
        profile, images, report = (out / (label + suffix) for suffix in ("-profile", "-images", "-report.json"))
        profile.mkdir()  # Always new; no inherited game, firmware, save or keys.
        rc = subprocess.run([args.cli.resolve(), "--user-dir", profile, "--replay", path,
                             "--replay-images", images, "--report", report], timeout=180).returncode
        result = json.loads(report.read_text(encoding="utf-8"))["replay"]
        if rejection:
            if (rc == 0 or result["run_complete"] or rejection not in result["error"]
                    or result["frames_replayed"] != completed):
                raise ValueError(f"{label}: wrong rejection: {rc}, {result}")
        else:
            if rc or not result["run_complete"] or result["frames_replayed"] != 2:
                raise ValueError(f"{label}: replay did not finish two frames: {rc}, {result}")
            shutil.copyfile(fixture / "manifest.json", images / "manifest.json")
            shutil.copyfile(report, images / "report.json")
            subprocess.run([*verifier, "--verify-replay", images, "--expected-effects", expected,
                            "--capture", path, "--require-equivalent"], check=True)
        return images

    a = replay("a", capture)
    b = replay("b", capture)
    subprocess.run([*verifier, "--compare-runs", a, b, "--witness-only", "--expected-effects",
                    expected, "--capture", capture, "--require-equivalent"], check=True)
    for label, reason, completed in (
        ("missing-page", "lacks captured backing", 0),
        ("lost-effect", "observed effect count differs from FRAME_END on frame 2: expected 5, observed 4", 1),
        ("unmet-present-fence", "PRESENT guest syncpoint fence was not satisfied", 1),
    ):
        path = out / (label + ".egcap")
        subprocess.run([args.producer.resolve(), path, "--" + label], check=True)
        replay(label, path, rejection=reason, completed=completed)


if __name__ == "__main__":
    main()
