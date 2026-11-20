#!/usr/bin/env python3
"""Portable 0.5 first-120-second cache gate scorer, cache-dir helper, and rejection verifier.

Offline checks for the Issue #2 (Milestone 0.5) protocol, using the Python standard library:
- Anchors (0, 120 s] at startup.first_host_present_ns on schema-2 run reports.
- Reports raw >100 ms real-frame counts beside emulator-attributed counts and game-gap counts.
- Removes attributable emulator time (code_translation, shader, pipeline, texture, decompression,
  disk); frames with remainder <= 100 ms count as emulator-caused, >100 ms remainders are game
  gaps, and missing/incomplete/high-Unknown (>10%) attribution is INVALID.
- Checks 2 * second_attributed <= cold_attributed (cold == 0 -> NOTHING TO HALVE), verifies
  baseline parity between halves, and names the largest residual emulator cause on any miss.
- Discovers both legacy <tid>/ and versioned <tid>/semantics_r*/ Vulkan cache directories so
  isolated cold runs and wrong-version cache rejection checks work across 0.5 and 0.6+/s54 trees.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import re
import statistics
import struct
from pathlib import Path
from typing import Any

WINDOW_NS = 120 * 1_000_000_000
LONG_FRAME_NS = 100 * 1_000_000
UNKNOWN_SHARE_LIMIT_PERMILLE = 100  # 10% per long frame
# The two titles in the launch protocol (labels supplied by the caller are not identities).
GATE_TITLE_IDS = {"04007ef01f8ae000", "0400e95004038001"}

ALL_CAUSES = (
    "disk",
    "shader",
    "pipeline",
    "texture",
    "decompression",
    "waiting",
    "code_translation",
    "unknown",
)
EMULATOR_CAUSES = (
    "code_translation",
    "shader",
    "pipeline",
    "texture",
    "decompression",
    "disk",
)

CACHE_ENVELOPE_MAGIC = b"EGL05CCH"
CACHE_ENVELOPE_BYTES = 160
CACHE_VERSION_OFFSET = 8
LEGACY_MAGICS = (b"yuzucach", b"yuzuvkch")
CACHE_STEMS = (
    "vulkan",
    "vulkan_pipelines",
    "vulkan_pipelines.unoptimized",
    "vulkan_used",
    "vulkan_mesh",
    "jit_blocks",
)
CACHE_SUFFIXES = (".mythos.bin", ".bin")
WARM_FILES = ("warm-compute.bin", "warm-compute.count", "warm-compute.done")

CLEAN_EXIT_REASONS = ("none", "guest_exit", "window_closed", "requested", "frame_limit")


def build_synthetic_envelope(bad_version: int = 999) -> bytes:
    """Return a 160-byte schema-2 cache header stamped with an incompatible envelope version."""
    header = bytearray(CACHE_ENVELOPE_BYTES)
    header[: len(CACHE_ENVELOPE_MAGIC)] = CACHE_ENVELOPE_MAGIC
    struct.pack_into("<I", header, CACHE_VERSION_OFFSET, bad_version)
    return bytes(header)


def stamp_rejected_envelope(original: bytes, bad_version: int = 999) -> bytes | None:
    """Return a copy of an EGL05CCH cache with its envelope version stamped to bad_version."""
    if len(original) < CACHE_ENVELOPE_BYTES or not original.startswith(CACHE_ENVELOPE_MAGIC):
        return None
    mutated = bytearray(original)
    struct.pack_into("<I", mutated, CACHE_VERSION_OFFSET, bad_version)
    return bytes(mutated)


def discover_title_cache_dirs(user_dir: str | Path, title_id: str) -> list[Path]:
    """Return existing per-title shader cache dirs, including semantics_r* subdirectories."""
    tid = title_id.strip().lower()
    if tid.startswith("0x"):
        tid = tid[2:]
    if not re.fullmatch(r"[0-9a-f]{1,16}", tid):
        raise ValueError("title ID must be a hexadecimal 64-bit value")
    base = Path(user_dir) / "cache" / "shader" / tid.zfill(16)
    dirs: list[Path] = []
    if base.is_dir():
        dirs.append(base)
        for child in sorted(base.iterdir()):
            if child.is_dir() and re.fullmatch(r"semantics_r[0-9]+", child.name):
                dirs.append(child)
    return dirs


def find_title_cache_files(user_dir: str | Path, title_id: str) -> list[Path]:
    """Find transferable pipeline, mesh, used, JIT-hint, and warm-compute files for a title."""
    expected_names = {stem + suffix for stem in CACHE_STEMS for suffix in CACHE_SUFFIXES}
    expected_names.update(WARM_FILES)
    found: list[Path] = []
    for folder in discover_title_cache_dirs(user_dir, title_id):
        for entry in sorted(folder.iterdir()):
            if entry.is_file() and entry.name in expected_names:
                found.append(entry)
    return found


def _exit_error(doc: dict[str, Any]) -> str | None:
    block = doc.get("exit")
    if not isinstance(block, dict):
        return "missing or malformed exit block"
    if type(block.get("code")) is not int or block["code"] != 0:
        return f"report exit code {block.get('code')!r} (requires 0)"
    if block.get("reason") not in CLEAN_EXIT_REASONS:
        return f"report exit reason {block.get('reason')!r} is not clean"
    for key in ("metrics_complete", "shutdown_complete"):
        if block.get(key) is not True:
            return f"exit.{key} is not true"
    return None


def _baseline_summary(doc: dict[str, Any]) -> dict[str, Any]:
    base = doc.get("baseline")
    if not isinstance(base, dict):
        return {"present": False, "on_baseline": None, "version": None, "hash": None}
    return {
        "present": True,
        "on_baseline": base.get("on_baseline"),
        "version": base.get("version"),
        "hash": base.get("hash"),
        "deviations": base.get("deviations"),
    }


def _default_baseline(base: dict[str, Any]) -> bool:
    deviations = base.get("deviations")
    return (base.get("on_baseline") is True and type(base.get("version")) is int and base["version"] > 0
            and isinstance(base.get("hash"), str) and re.fullmatch(r"[0-9a-f]{64}", base["hash"]) is not None
            and isinstance(deviations, list)
            and all(isinstance(d, dict) and d.get("source") == "program requirement" for d in deviations))


def _pair_evidence(doc: dict[str, Any]) -> dict[str, Any]:
    """Keep deterministic run inputs, not measured times, for pairing and aggregation."""
    title = doc.get("title_id")
    if not isinstance(title, str) or not re.fullmatch(r"[0-9a-fA-F]{16}", title):
        raise ValueError("missing or malformed title_id")
    build = doc.get("build")
    if not isinstance(build, dict) or not all(
        isinstance(build.get(key), str) and build[key].strip() for key in ("revision", "build_id")
    ):
        raise ValueError("missing build.revision or build.build_id")
    user_dir = doc.get("user_dir")
    if not isinstance(user_dir, str) or not user_dir.strip():
        raise ValueError("missing user_dir")
    cache = doc.get("cache")
    if not isinstance(cache, dict) or cache.get("status") != "reported":
        raise ValueError("cache was not reported")
    identity = cache.get("identity")
    if not isinstance(identity, dict) or identity.get("title_id") != title.lower():
        raise ValueError("missing or mismatched cache.identity.title_id")
    for key in ("build_id", "content_stamp", "settings_hash"):
        if not isinstance(identity.get(key), str) or not re.fullmatch(r"[0-9a-fA-F]{16}", identity[key]):
            raise ValueError(f"missing or malformed cache.identity.{key}")
    device = identity.get("device")
    if not isinstance(device, dict) or not isinstance(device.get("pipeline_cache_uuid"), str) or not re.fullmatch(
        r"[0-9a-fA-F]{32}", device["pipeline_cache_uuid"]
    ):
        raise ValueError("missing or malformed cache.identity.device.pipeline_cache_uuid")
    for block, keys in ((identity, ("game_version", "shader_format_version", "envelope_version")),
                        (device, ("device_id", "vendor_id", "driver_id", "driver_version"))):
        for key in keys:
            if type(block.get(key)) is not int or block[key] < 0:
                raise ValueError(f"missing or malformed cache identity field {key}")
    files = cache.get("files")
    if not isinstance(files, list) or not all(isinstance(f, dict) for f in files):
        raise ValueError("malformed cache.files")
    transfers = [(f.get("status"), f.get("reason")) for f in files if f.get("kind") == "transferable"]
    if len(transfers) != 1:
        raise ValueError("expected one transferable cache.files entry")

    scene = doc["startup"].get("interactive_scene")
    if (not isinstance(scene, dict) or scene.get("status") != "complete"
            or scene.get("scene_check") != "passed" or not isinstance(scene.get("id"), str) or not scene["id"]):
        raise ValueError("interactive scene was not verified")
    steps = doc.get("steps", [])
    if not isinstance(steps, list) or not all(
        isinstance(s, dict) and s.get("status") == "fired" and s.get("action") in ("press", "release")
        and type(s.get("key")) is int and isinstance(s.get("when"), str) for s in steps
    ):
        raise ValueError("scripted input missing or not fired")
    tunables = doc.get("tunables")
    if not isinstance(tunables, dict):
        raise ValueError("missing effective environment tunables")
    settings = doc.get("settings")
    if not isinstance(settings, dict) or not all(
        isinstance(settings.get(k), dict) and settings[k] for k in ("initial", "final")
    ):
        raise ValueError("missing effective settings.initial or settings.final")
    # Compare effective settings without publishing local paths/settings in the result.
    effective = [settings["initial"], settings["final"], tunables]
    settings_digest = hashlib.sha256(json.dumps(effective, sort_keys=True).encode()).hexdigest()
    return {"title_id": title.lower(), "build": build, "user_dir": user_dir,
            "cache_identity": identity, "transferable": transfers[0], "settings_digest": settings_digest,
            "scene": scene["id"], "steps": [(s["when"], s["key"], s["action"]) for s in steps]}


def score_report(report_path: str | Path) -> dict[str, Any]:
    """Validate and score a single schema-2 run report against the 0.5 attribution rule."""
    path = Path(report_path)
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return {"valid": False, "reason": f"unreadable report: {exc}", "path": str(path)}

    if not isinstance(doc, dict):
        return {"valid": False, "reason": "report is not an object", "path": str(path)}
    if doc.get("schema_version") != 2:
        return {
            "valid": False,
            "reason": f"schema_version={doc.get('schema_version')!r} (requires 2)",
            "path": str(path),
        }

    exit_error = _exit_error(doc)
    if exit_error:
        return {"valid": False, "reason": exit_error, "path": str(path)}

    startup = doc.get("startup")
    first_120s = doc.get("first_120s")
    attribution = doc.get("attribution")
    if not isinstance(startup, dict) or not isinstance(first_120s, dict) or not isinstance(attribution, dict):
        return {"valid": False, "reason": "missing startup, first_120s, or attribution block", "path": str(path)}

    p0 = startup.get("first_host_present_ns")
    if type(p0) is not int or p0 <= 0:
        return {"valid": False, "reason": "invalid startup.first_host_present_ns", "path": str(path)}

    if first_120s.get("anchor") != "first_real_host_present":
        return {"valid": False, "reason": "invalid first_120s.anchor", "path": str(path)}
    if first_120s.get("complete") is not True or first_120s.get("elapsed_ns") != WINDOW_NS:
        return {"valid": False, "reason": "first_120s window is incomplete", "path": str(path)}

    if attribution.get("source") != "host_present_submission" or attribution.get("threshold_ns") != LONG_FRAME_NS:
        return {"valid": False, "reason": "requires host_present_submission with threshold_ns=100000000", "path": str(path)}
    strict_count = first_120s.get("frames_over_100ms")
    if type(strict_count) is not int or strict_count < 0:
        return {"valid": False, "reason": "invalid first_120s.frames_over_100ms", "path": str(path)}

    def invalid(reason: str) -> dict[str, Any]:
        return {"valid": False, "reason": reason, "path": str(path), "raw_count": strict_count}

    if attribution.get("enabled") is not True:
        return invalid("attribution is disabled")
    if attribution.get("complete") is not True:
        return invalid(f"attribution is incomplete (lost_events={attribution.get('lost_events')!r})")
    if first_120s.get("scene_check") != "passed":
        return invalid("first_120s.scene_check did not pass")

    for counter in ("lost_events", "dropped_boundaries", "sink_write_errors"):
        val = attribution.get(counter)
        if type(val) is not int or val != 0:
            return invalid(f"attribution.{counter}={val!r} (requires 0)")
    history_loss = first_120s.get("frames_with_history_loss", 0)  # not emitted by s54
    if type(history_loss) is not int or history_loss != 0:
        return invalid(f"first_120s.frames_with_history_loss={history_loss!r} (requires 0)")

    long_frames = attribution.get("long_frames")
    if not isinstance(long_frames, list):
        return invalid("attribution.long_frames missing")

    window_frames = []
    for frame in long_frames:
        if not isinstance(frame, dict):
            return invalid("malformed long_frame entry")
        end_ns = frame.get("end_ns")
        if type(end_ns) is not int:
            return invalid("long_frame missing end_ns")
        if p0 < end_ns <= p0 + WINDOW_NS:
            window_frames.append(frame)

    if len(window_frames) != strict_count:
        return invalid(f"first_120s.frames_over_100ms={strict_count} != window_frames={len(window_frames)}")

    emulator_frames = []
    game_gap_frames = []
    cause_totals_ns = {c: 0 for c in ALL_CAUSES}
    emulator_cause_totals_ns = {c: 0 for c in EMULATOR_CAUSES}
    worst_unknown_permille = 0
    previous_end = p0

    for frame in window_frames:
        if frame.get("instrumentation_complete") is not True or frame.get("history_loss", False) is not False:
            return invalid(f"frame {frame.get('id')} instrumentation incomplete or history loss")
        dur = frame.get("duration_ns")
        causes = frame.get("cause_ns")
        if type(dur) is not int or dur <= LONG_FRAME_NS or not isinstance(causes, dict):
            return invalid(f"frame {frame.get('id')} missing valid duration_ns/cause_ns")
        start = frame.get("start_ns")
        if type(start) is not int or start < previous_end or frame["end_ns"] - start != dur:
            return invalid(f"frame {frame.get('id')} overlapping or invalid interval")
        previous_end = frame["end_ns"]

        cause_sum = 0
        for key in ALL_CAUSES:
            val = causes.get(key)
            if type(val) is not int or val < 0:
                return invalid(f"frame {frame.get('id')} invalid cause {key}={val!r}")
            cause_sum += val
            cause_totals_ns[key] += val

        if cause_sum != dur:
            return invalid(f"frame {frame.get('id')} causes sum {cause_sum} != duration_ns {dur}")

        # The producer includes inferred time in cause_ns; it is not additional wall time.
        inferred = frame.get("inferred_ns")
        if inferred is not None:
            if not isinstance(inferred, dict) or inferred.keys() != causes.keys():
                return invalid(f"frame {frame.get('id')} inferred_ns is not a matching cause map")
            for key in ALL_CAUSES:
                value = inferred[key]
                if type(value) is not int or not 0 <= value <= causes[key]:
                    return invalid(f"frame {frame.get('id')} inferred_ns.{key}={value!r} outside cause_ns")

        unknown_ns = causes["unknown"]
        unknown_permille = (unknown_ns * 1000) // dur
        if unknown_permille > worst_unknown_permille:
            worst_unknown_permille = unknown_permille
        if unknown_ns * 1000 > dur * UNKNOWN_SHARE_LIMIT_PERMILLE:
            return invalid(f"frame {frame.get('id')} unknown share {unknown_ns}/{dur} exceeds 10%")

        emu_ns = sum(causes[c] for c in EMULATOR_CAUSES)
        remainder_ns = dur - emu_ns
        entry = {
            "id": frame.get("id"),
            "duration_ms": round(dur / 1_000_000.0, 3),
            "emulator_ms": round(emu_ns / 1_000_000.0, 3),
            "remainder_ms": round(remainder_ns / 1_000_000.0, 3),
            "unknown_ns": unknown_ns,
            "inferred_ns": inferred,
            "emulator_causes_ms": {c: round(causes[c] / 1_000_000.0, 3) for c in EMULATOR_CAUSES if causes[c] > 0},
        }
        if remainder_ns <= LONG_FRAME_NS:
            emulator_frames.append(entry)
            for c in EMULATOR_CAUSES:
                emulator_cause_totals_ns[c] += causes[c]
        else:
            game_gap_frames.append(entry)

    unclassified_intervals = []
    for interval_name in ("process_to_first_present", "first_present_to_interactive"):
        blk = startup.get(interval_name)
        if not isinstance(blk, dict) or blk.get("status") != "complete":
            status = blk.get("status") if isinstance(blk, dict) else "missing"
            unclassified_intervals.append({"interval": interval_name, "status": status})
    prod_unk = attribution.get("producer_unknown_wait_ns")
    if type(prod_unk) is not int or prod_unk < 0:
        unclassified_intervals.append({"interval": "producer_unknown_wait", "status": "not reported"})
    elif prod_unk > 0:
        unclassified_intervals.append({"interval": "producer_unknown_wait", "ns": prod_unk})

    try:
        evidence = _pair_evidence(doc)
    except ValueError as exc:
        return invalid(str(exc))

    return {
        "valid": True,
        "reason": None,
        "path": str(path),
        "raw_count": strict_count,
        "emulator_attributed_count": len(emulator_frames),
        "game_gap_count": len(game_gap_frames),
        "emulator_frames": emulator_frames,
        "game_gap_frames": game_gap_frames,
        "cause_totals_ms": {c: round(v / 1_000_000.0, 3) for c, v in cause_totals_ns.items()},
        "emulator_attributed_cause_totals_ms": {
            c: v / 1_000_000 for c, v in emulator_cause_totals_ns.items()
        },
        "worst_unknown_permille": worst_unknown_permille,
        "unclassified_intervals": unclassified_intervals,
        "baseline": _baseline_summary(doc),
        **evidence,
        "exit_code": doc["exit"]["code"],
    }


def largest_residual_cause(scored_second: dict[str, Any]) -> dict[str, Any] | None:
    """Identify the dominant emulator cause across emulator-attributed long frames in second launch."""
    totals = scored_second.get("emulator_attributed_cause_totals_ms") or {}
    nonzero = [(cause, ms) for cause, ms in totals.items() if ms > 0]
    if not nonzero:
        return None
    cause, ms = max(nonzero, key=lambda item: item[1])
    return {"cause": cause, "total_ms": ms}


def score_pair(
    cold_report: str | Path,
    second_report: str | Path,
    *,
    label: str = "pair",
    require_baseline: bool = True,
) -> dict[str, Any]:
    """Score one paired (cold vs. second launch) 0.5 gate run."""
    cold = score_report(cold_report)
    second = score_report(second_report)
    reasons: list[str] = []
    if not cold["valid"]:
        reasons.append(f"cold invalid: {cold['reason']}")
    if not second["valid"]:
        reasons.append(f"second invalid: {second['reason']}")

    if require_baseline and cold["valid"] and second["valid"]:
        cb, sb = cold["baseline"], second["baseline"]
        if not cb["present"] or not sb["present"]:
            reasons.append("missing baseline block in report")
        else:
            if not _default_baseline(cb) or not _default_baseline(sb):
                reasons.append("missing default baseline identity or non-default effective settings")
            if cb != sb:
                reasons.append(
                    f"baseline mismatch: cold=v{cb['version']}:{cb['hash']} vs "
                    f"second=v{sb['version']}:{sb['hash']}"
                )

    if cold["valid"] and second["valid"]:
        for field in ("title_id", "build", "cache_identity", "settings_digest", "user_dir", "scene", "steps"):
            if cold[field] != second[field]:
                if field == "build":
                    field = next((f"build.{key}" for key in ("revision", "build_id")
                                  if cold["build"][key] != second["build"][key]), "build")
                reasons.append(f"{field} mismatch between cold and second run")
        for half, run, expected in (("cold", cold, ("missing", "missing")),
                                    ("second", second, ("accepted", "none"))):
            if run["transferable"] != expected:
                reasons.append(f"{half} transferable cache {run['transferable']!r}; expected {expected!r}")

    if reasons:
        return {
            "label": label,
            "verdict": "INCOMPLETE",
            "reasons": reasons,
            "cold": cold,
            "second": second,
            "halved": None,
            "ratio": None,
            "largest_residual_cause": None,
        }

    c_cnt = cold["emulator_attributed_count"]
    s_cnt = second["emulator_attributed_count"]
    if c_cnt == 0:
        return {
            "label": label,
            "verdict": "NOTHING_TO_HALVE",
            "reasons": ["cold emulator-attributed count is 0; nothing to halve"],
            "cold": cold,
            "second": second,
            "halved": False,
            "ratio": None,
            "largest_residual_cause": largest_residual_cause(second),
        }

    halved = 2 * s_cnt <= c_cnt
    ratio = round(s_cnt / c_cnt, 4)
    residual = largest_residual_cause(second) if not halved else None
    out_reasons = [] if halved else [f"2 * second ({2 * s_cnt}) > cold ({c_cnt})"]
    return {
        "label": label,
        "verdict": "PASS" if halved else "FAIL",
        "reasons": out_reasons,
        "cold": cold,
        "second": second,
        "halved": halved,
        "ratio": ratio,
        "largest_residual_cause": residual,
    }


def score_gate(pairs_by_title: dict[str, list[dict[str, Any]]]) -> dict[str, Any]:
    """Aggregate comparable FE:FW and XC2 pairs. PASS here covers the count rule only."""
    title_summaries: dict[str, Any] = {}
    all_pass = True
    any_incomplete = False
    campaign = None
    title_ids: set[str] = set()
    problems: list[str] = []

    for title, pairs in pairs_by_title.items():
        valid_pairs = [p for p in pairs if p["verdict"] in ("PASS", "FAIL", "NOTHING_TO_HALVE")]
        problem_count = len(problems)
        if len(valid_pairs) == len(pairs) and valid_pairs:
            first = valid_pairs[0]["cold"]
            if first["title_id"] in title_ids:
                problems.append(f"{title}: same title_id already scored under another label")
            title_ids.add(first["title_id"])
            for pair in valid_pairs:
                run = pair["cold"]
                if any(run[k] != first[k] for k in ("cache_identity", "settings_digest", "scene", "steps")):
                    problems.append(f"{title}: incomparable repeated pairs")
                    break
                baseline = run["baseline"]
                if not _default_baseline(baseline) or baseline != pair["second"]["baseline"]:
                    problems.append(f"{title}: default baseline not established")
                    break
                setup = (run["build"], baseline["version"], baseline["hash"], run["cache_identity"]["device"])
                if campaign is None:
                    campaign = setup
                elif campaign != setup:
                    problems.append(f"{title}: different build, baseline or device/driver")
                    break
        else:
            problems.append(f"{title}: missing or invalid pair")
        if len(problems) != problem_count:
            any_incomplete = True
            title_summaries[title] = {
                "verdict": "INCOMPLETE", "valid_pairs": len(valid_pairs), "total_pairs": len(pairs),
                "cold_median": None, "second_median": None, "ratio": None, "milestone_open": True,
            }
            continue

        c_vals = [p["cold"]["emulator_attributed_count"] for p in valid_pairs]
        s_vals = [p["second"]["emulator_attributed_count"] for p in valid_pairs]
        c_med = float(statistics.median(c_vals))
        s_med = float(statistics.median(s_vals))
        if c_med == 0.0:
            verdict = "NOTHING_TO_HALVE"
            ratio = None
            all_pass = False
        elif 2.0 * s_med <= c_med:
            verdict = "PASS"
            ratio = round(s_med / c_med, 4)
        else:
            verdict = "FAIL"
            ratio = round(s_med / c_med, 4)
            all_pass = False

        residuals = collections.Counter()
        for p in valid_pairs:
            res = p.get("largest_residual_cause")
            if res:
                residuals[res["cause"]] += 1

        title_summaries[title] = {
            "verdict": verdict,
            "valid_pairs": len(valid_pairs),
            "total_pairs": len(pairs),
            "cold_median": c_med,
            "second_median": s_med,
            "raw_cold_counts": [p["cold"]["raw_count"] for p in valid_pairs],
            "raw_second_counts": [p["second"]["raw_count"] for p in valid_pairs],
            "attributed_cold_counts": c_vals,
            "attributed_second_counts": s_vals,
            "ratio": ratio,
            "residual_causes": dict(residuals),
            "milestone_open": True if verdict != "PASS" else None,
        }

    overall = "INCOMPLETE" if any_incomplete else ("PASS" if all_pass and title_summaries else "FAIL")
    if title_ids != GATE_TITLE_IDS:
        problems.append("requires both FE:FW and XC2 title IDs")
    if problems:
        overall = "INCOMPLETE"
    return {
        "verdict": overall,
        "reasons": problems,
        # Counts alone cannot close the milestone: incompatible-cache proof is independent.
        "milestone_open": True if overall != "PASS" else None,
        "titles": title_summaries,
    }


def verify_cache_rejection(
    report_path: str | Path,
    log_path: str | Path,
    *,
    expected_reason: str = "format_version",
    expected_version: int = 1,
    found_version: int = 999,
) -> dict[str, Any]:
    """Verify clean startup rejection of an incompatible transferable cache file."""
    rpath = Path(report_path)
    lpath = Path(log_path)
    reasons: list[str] = []

    try:
        doc = json.loads(rpath.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        return {"verdict": "FAIL", "reasons": [f"unreadable report: {exc}"]}

    if not isinstance(doc, dict) or doc.get("schema_version") != 2:
        return {"verdict": "FAIL", "reasons": [f"{rpath}: requires a schema-2 report object"]}
    exit_error = _exit_error(doc)
    if exit_error:
        reasons.append(exit_error)
    cache = doc.get("cache")
    if not isinstance(cache, dict) or cache.get("status") != "reported" or not isinstance(cache.get("files"), list):
        return {"verdict": "FAIL", "reasons": reasons + [f"{rpath}: missing reported cache.files"]}
    if expected_reason == "format_version" and expected_version != found_version:
        want = expected_version
        allowed = (found_version,)
        action = "ignore"
    elif expected_reason == "legacy_format":
        want = int.from_bytes(CACHE_ENVELOPE_MAGIC, "little")
        allowed = tuple(int.from_bytes(magic, "little") for magic in LEGACY_MAGICS)
        action = "backup_legacy"
    else:
        return {"verdict": "FAIL", "reasons": ["requires an incompatible format version or legacy format"]}
    matched_entry = None
    for item in cache["files"]:
        if (
            isinstance(item, dict)
            and item.get("kind") == "transferable"
            and item.get("status") == "rejected"
            and item.get("reason") == expected_reason
            and type(item.get("expected")) is int and item["expected"] == want
            and type(item.get("found")) is int and item["found"] in allowed
        ):
            matched_entry = item
            break

    if matched_entry is None:
        reasons.append(f"missing matching cache.files[] rejection entry for reason={expected_reason}")

    try:
        log_text = lpath.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        reasons.append(f"unreadable log: {exc}")
        log_text = ""

    if matched_entry is not None:
        expected_line = (f"Nx2Cache reject kind=transferable reason={expected_reason} "
                         f"expected={want} found={matched_entry['found']} action={action}")
        if not any(line.rstrip().endswith(expected_line) for line in log_text.splitlines()):
            reasons.append(f"{lpath}: missing matching log line: {expected_line}")

    return {
        "verdict": "PASS" if not reasons else "FAIL",
        "reasons": reasons,
        "matched_entry": matched_entry,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_pair = sub.add_parser("score-pair", help="Score one cold/second schema-2 report pair")
    p_pair.add_argument("--cold", required=True)
    p_pair.add_argument("--second", required=True)
    p_pair.add_argument("--label", default="pair")
    p_pair.add_argument("--no-baseline", action="store_true")

    p_rej = sub.add_parser("check-reject", help="Verify incompatible-cache rejection report + log")
    p_rej.add_argument("--report", required=True)
    p_rej.add_argument("--log", required=True)
    p_rej.add_argument("--reason", choices=("format_version", "legacy_format"), default="format_version")
    p_rej.add_argument("--expected-version", type=int, default=1)
    p_rej.add_argument("--found-version", type=int, default=999)

    p_disc = sub.add_parser("discover-caches", help="List title cache dirs including semantics_r*")
    p_disc.add_argument("--user-dir", required=True)
    p_disc.add_argument("--title-id", required=True)

    p_hdr = sub.add_parser("make-reject-header", help="Write a synthetic 160-byte EGL05CCH v999 file")
    p_hdr.add_argument("--out", required=True)
    p_hdr.add_argument("--version", type=int, default=999)

    args = parser.parse_args(argv)
    if args.cmd == "score-pair":
        res = score_pair(args.cold, args.second, label=args.label, require_baseline=not args.no_baseline)
        print(json.dumps(res, indent=2))
        return 0 if res["verdict"] == "PASS" else (2 if res["verdict"] == "INCOMPLETE" else 1)

    if args.cmd == "check-reject":
        res = verify_cache_rejection(
            args.report,
            args.log,
            expected_reason=args.reason,
            expected_version=args.expected_version,
            found_version=args.found_version,
        )
        print(json.dumps(res, indent=2))
        return 0 if res["verdict"] == "PASS" else 1

    if args.cmd == "discover-caches":
        try:
            dirs = [str(p) for p in discover_title_cache_dirs(args.user_dir, args.title_id)]
            files = [str(p) for p in find_title_cache_files(args.user_dir, args.title_id)]
        except ValueError as exc:
            p_disc.error(str(exc))
        print(json.dumps({"dirs": dirs, "files": files}, indent=2))
        return 0

    if args.cmd == "make-reject-header":
        if not 0 <= args.version <= 0xFFFFFFFF or args.version == 1:
            p_hdr.error("--version must be an incompatible unsigned 32-bit version (not 1)")
        try:
            with Path(args.out).open("xb") as dest:
                dest.write(build_synthetic_envelope(args.version))
        except OSError as exc:
            p_hdr.error(str(exc))
        return 0

    return 2


if __name__ == "__main__":
    raise SystemExit(main())
