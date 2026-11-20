#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Standalone replay envelope, effect reconciliation, and census verifier (Issue #5).

Standard-library only (no Pillow/numpy required). Uses an embedded pure-Python RGBA8 PNG
codec so the original synthetic replay fixture and all positive/negative controls run
portably on Windows and Linux without any game binary, save, key, firmware, or GPU.

Commands:
    python tools/replay_qualify/replay_qualify.py --write-fixture <dir>
    python tools/replay_qualify/replay_qualify.py --verify <dir>
    python tools/replay_qualify/replay_qualify.py --reconcile-census <census.json>
    python tools/replay_qualify/replay_qualify.py --selftest

Exit codes:
    0  PASS (all image envelope, sync/effect, and census completeness checks passed)
    1  FAIL (pixel envelope violation, sync/effect mismatch, or unreconciled call/effect)
    2  INVALID (malformed/missing/extra inputs or dimension/format mismatch)
    3  INCOMPLETE (census completeness gate failed)
"""

from __future__ import annotations

import argparse
import binascii
import json
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path
from typing import Any


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
EXIT_PASS = 0
EXIT_FAIL = 1
EXIT_INVALID = 2
EXIT_INCOMPLETE = 3

VALID_EFFECT_KINDS = frozenset(
    {
        "submit",
        "consume",
        "draw",
        "compute_dispatch",
        "syncpoint_increment",
        "fence_signal",
        "present_compose",
        "readback_visibility",
    }
)
VALID_CLASSIFICATIONS = frozenset(
    {"supported", "fallback", "unsupported", "unobservable"}
)


class InvalidInputError(Exception):
    """Raised when fixture files are missing, malformed, or structurally invalid."""


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    crc = binascii.crc32(tag + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)


def encode_rgba_png(width: int, height: int, pixels: bytes) -> bytes:
    """Encode tight RGBA8 bytes (width * height * 4) into a valid PNG."""
    if width <= 0 or height <= 0:
        raise ValueError("image dimensions must be positive")
    expected_len = width * height * 4
    if len(pixels) != expected_len:
        raise ValueError(f"expected {expected_len} RGBA bytes, got {len(pixels)}")
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    stride = width * 4
    raw_rows = bytearray()
    for y in range(height):
        raw_rows.append(0)  # filter type 0 (None)
        start = y * stride
        raw_rows.extend(pixels[start : start + stride])
    idat = zlib.compress(bytes(raw_rows), level=6)
    return (
        PNG_SIGNATURE
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", idat)
        + _png_chunk(b"IEND", b"")
    )


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_rgba_png(path: Path) -> tuple[int, int, bytes]:
    """Decode an 8-bit RGBA PNG file strictly, validating CRCs and scanline filters."""
    try:
        blob = path.read_bytes()
    except OSError as exc:
        raise InvalidInputError(f"cannot read {path.name}: {exc}") from exc

    if len(blob) < len(PNG_SIGNATURE) + 12 or blob[:8] != PNG_SIGNATURE:
        raise InvalidInputError(f"{path.name}: invalid PNG signature")

    offset = 8
    width = 0
    height = 0
    ihdr_seen = False
    iend_seen = False
    idat_parts: list[bytes] = []

    while offset < len(blob):
        if offset + 12 > len(blob):
            raise InvalidInputError(f"{path.name}: truncated PNG chunk header")
        length = struct.unpack(">I", blob[offset : offset + 4])[0]
        tag = blob[offset + 4 : offset + 8]
        data_start = offset + 8
        data_end = data_start + length
        if data_end + 4 > len(blob):
            raise InvalidInputError(f"{path.name}: truncated PNG chunk {tag!r}")
        data = blob[data_start:data_end]
        stored_crc = struct.unpack(">I", blob[data_end : data_end + 4])[0]
        computed_crc = binascii.crc32(tag + data) & 0xFFFFFFFF
        if stored_crc != computed_crc:
            raise InvalidInputError(f"{path.name}: CRC mismatch in chunk {tag!r}")
        offset = data_end + 4

        if tag == b"IHDR":
            if ihdr_seen or length != 13:
                raise InvalidInputError(f"{path.name}: invalid IHDR")
            width, height, bit_depth, color_type, comp, filt, interlace = struct.unpack(
                ">IIBBBBB", data
            )
            if (
                width == 0
                or height == 0
                or bit_depth != 8
                or color_type != 6
                or comp != 0
                or filt != 0
                or interlace != 0
            ):
                raise InvalidInputError(
                    f"{path.name}: unsupported PNG format (expected 8-bit non-interlaced RGBA)"
                )
            ihdr_seen = True
        elif tag == b"IDAT":
            if not ihdr_seen:
                raise InvalidInputError(f"{path.name}: IDAT before IHDR")
            idat_parts.append(data)
        elif tag == b"IEND":
            if length != 0:
                raise InvalidInputError(f"{path.name}: non-empty IEND")
            iend_seen = True
            break

    if not ihdr_seen or not iend_seen or not idat_parts:
        raise InvalidInputError(f"{path.name}: missing IHDR, IDAT, or IEND")

    try:
        raw = zlib.decompress(b"".join(idat_parts))
    except zlib.error as exc:
        raise InvalidInputError(f"{path.name}: corrupt zlib stream: {exc}") from exc

    bpp = 4
    stride = width * bpp
    expected_raw = height * (1 + stride)
    if len(raw) != expected_raw:
        raise InvalidInputError(
            f"{path.name}: unexpected decompressed size {len(raw)} != {expected_raw}"
        )

    out = bytearray(height * stride)
    prev_row = bytes(stride)
    pos = 0
    for y in range(height):
        filt = raw[pos]
        pos += 1
        scan = raw[pos : pos + stride]
        pos += stride
        row = bytearray(stride)
        if filt == 0:
            row[:] = scan
        elif filt == 1:
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (scan[i] + left) & 0xFF
        elif filt == 2:
            for i in range(stride):
                row[i] = (scan[i] + prev_row[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (scan[i] + ((left + prev_row[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                up = prev_row[i]
                up_left = prev_row[i - bpp] if i >= bpp else 0
                row[i] = (scan[i] + _paeth(left, up, up_left)) & 0xFF
        else:
            raise InvalidInputError(f"{path.name}: invalid PNG filter {filt}")
        out[y * stride : (y + 1) * stride] = row
        prev_row = bytes(row)

    return width, height, bytes(out)


def fnv1a64_rgba(pixels: bytes) -> int:
    """Compute FNV-1a 64-bit checksum over RGBA pixel bytes (matches PictureChecksum style)."""
    h = 0xCBF29CE484222325
    for byte in pixels:
        h ^= byte
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def make_synthetic_rgba(width: int, height: int, frame_id: int) -> bytes:
    """Generate a deterministic original RGBA8 test pattern with gradient, grid, and alpha."""
    out = bytearray(width * height * 4)
    phase = (frame_id * 19) & 0xFF
    idx = 0
    for y in range(height):
        for x in range(width):
            checker = 24 if ((x // 4 + y // 4 + frame_id) & 1) == 0 else 0
            r = (32 + x * 5 + y * 2 + phase + checker) % 240
            g = (24 + x * 3 + y * 6 + phase) % 240
            b = (48 + (width - 1 - x) * 4 + y * 3 + checker) % 240
            a = 255 if (x + y) % 7 != 0 else 224
            out[idx] = r
            out[idx + 1] = g
            out[idx + 2] = b
            out[idx + 3] = a
            idx += 4
    return bytes(out)


def write_synthetic_fixture(out_dir: Path, width: int = 32, height: int = 32) -> Path:
    """Write a complete, redistributable original replay + census fixture into out_dir."""
    out_dir.mkdir(parents=True, exist_ok=True)
    frame_ids = [1, 2]
    frame_lines: list[str] = []
    effect_lines: list[str] = []

    for frame_id in frame_ids:
        pixels = make_synthetic_rgba(width, height, frame_id)
        checksum = fnv1a64_rgba(pixels)
        live_name = f"live_{frame_id:08d}.png"
        replay_name = f"replay_{frame_id:08d}.png"
        png_bytes = encode_rgba_png(width, height, pixels)
        (out_dir / live_name).write_bytes(png_bytes)
        (out_dir / replay_name).write_bytes(png_bytes)

        syncpoint_val = frame_id * 10
        fence_id = frame_id * 100
        frame_lines.append(
            json.dumps(
                {
                    "schema": 1,
                    "frame_id": frame_id,
                    "width": width,
                    "height": height,
                    "live_picture": live_name,
                    "replay_picture": replay_name,
                    "live_checksum": checksum,
                    "replay_checksum": checksum,
                    "identical": True,
                    "semantic_mismatches": 0,
                    "syncpoint_value": syncpoint_val,
                    "fence_id": fence_id,
                },
                sort_keys=True,
            )
        )

        # Complete GPU/host call and effect reconciliation for this frame.
        frame_effects = [
            (1, "submit", "supported", "pushbuffer list queued on channel 1"),
            (2, "consume", "supported", "DmaPusher consumed 128 pushbuffer words"),
            (3, "draw", "supported", "Maxwell3D indexed draw 64 vertices"),
            (4, "compute_dispatch", "fallback", "native compute dispatch routed via fallback"),
            (5, "syncpoint_increment", "supported", f"syncpoint incremented to {syncpoint_val}"),
            (6, "fence_signal", "supported", f"host fence {fence_id} signaled"),
            (7, "readback_visibility", "unobservable", "transient scratch unmapped before read"),
            (8, "draw", "unsupported", "debug marker method retired as no-op"),
            (9, "present_compose", "supported", "offscreen composite emitted RGBA8 frame"),
        ]
        for seq, kind, cls_name, detail in frame_effects:
            effect_lines.append(
                json.dumps(
                    {
                        "frame_id": frame_id,
                        "seq": seq,
                        "kind": kind,
                        "classification": cls_name,
                        "reconciled": True,
                        "syncpoint_value": syncpoint_val,
                        "fence_id": fence_id,
                        "detail": detail,
                    },
                    sort_keys=True,
                )
            )

    (out_dir / "frames.jsonl").write_text("\n".join(frame_lines) + "\n", encoding="utf-8")
    (out_dir / "effects.jsonl").write_text("\n".join(effect_lines) + "\n", encoding="utf-8")

    manifest = {
        "schema": 1,
        "kind": "standalone_replay_qualification_fixture",
        "origin": "synthetic_procedural_original",
        "envelope": {
            "max_rgb_mae": 1.0,
            "max_channel_delta": 4,
            "max_outlier_fraction": 0.005,
            "outlier_delta_threshold": 2,
            "max_alpha_delta": 0,
        },
        "runner_invocation": (
            "eden-cli --replay <capture.egcap> --replay-images <out_dir> --report <report.json>"
        ),
        "census_invocation": (
            "eden-cli --baseline -g <owned_title_path> --nvn-census <census.json> "
            "--nvn-census-audit --run-seconds 300 --report <report.json>"
        ),
        "expected_frames": len(frame_ids),
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    census = {
        "schema": 2,
        "kind": "nvn_call_census",
        "final": True,
        "run_complete": True,
        "stop": {"reason": "frame_limit", "clean": True},
        "replay_capture": {"failed": False, "detail": ""},
        "audit": {
            "enabled": True,
            "region_known": True,
            "bootstrap_sites": 2,
            "bootstrap_calls": 4,
            "late_modules": 0,
            "code_changes": 0,
            "audited_entries": 1200,
            "shortfall_functions": 0,
            "shortfall_calls": 0,
            "tolerance": 4,
            "unknown_targets": 0,
            "unknown_calls": 0,
            "unknown_overflow": 0,
            "unaudited_entries": 0,
            "return_sites": 18,
        },
        "coverage": {
            "named_targets": 42,
            "audit_found_targets": 3,
            "audit_found_counted_at_entry": 3,
            "instrumented_targets": 45,
            "lost_events": 0,
            "lost_detail": "",
        },
        "accounting": {
            "exact": True,
            "executed": 1200,
            "startup": 120,
            "frames": 1060,
            "late": 0,
            "tail": 20,
            "mismatched": 0,
        },
        "presents": {
            "paths_used": 1,
            "streams": 1,
            "stream_ambiguous": False,
            "stream_tokens": 1,
            "identity_ok": True,
            "presents": 3,
            "frames_closed": 2,
            "min_frames": 2,
            "id_gaps": 0,
            "id_duplicates": 0,
        },
    }
    (out_dir / "census.json").write_text(
        json.dumps(census, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return out_dir


def _plain_relative_name(base: Path, name: Any) -> Path | None:
    if not isinstance(name, str) or not name or os.path.isabs(name):
        return None
    parts = name.replace("\\", "/").split("/")
    if ".." in parts or "" in parts or ":" in parts[0]:
        return None
    return base / Path(*parts)


def compare_image_pair(
    live_path: Path, replay_path: Path, envelope: dict[str, Any]
) -> tuple[bool, dict[str, Any], str]:
    """Compare one live/replay RGBA8 pair against declared numeric envelope bounds."""
    lw, lh, live_px = decode_rgba_png(live_path)
    rw, rh, replay_px = decode_rgba_png(replay_path)
    if (lw, lh) != (rw, rh):
        raise InvalidInputError(
            f"dimension mismatch {live_path.name}={lw}x{lh} vs {replay_path.name}={rw}x{rh}"
        )

    pixel_count = lw * lh
    rgb_abs_sum = 0
    max_rgb_delta = 0
    max_alpha_delta = 0
    outliers = 0
    outlier_thresh = int(envelope.get("outlier_delta_threshold", 2))
    signed_sum = [0, 0, 0]

    for i in range(0, len(live_px), 4):
        dr = replay_px[i] - live_px[i]
        dg = replay_px[i + 1] - live_px[i + 1]
        db = replay_px[i + 2] - live_px[i + 2]
        da = abs(replay_px[i + 3] - live_px[i + 3])

        adr, adg, adb = abs(dr), abs(dg), abs(db)
        rgb_abs_sum += adr + adg + adb
        signed_sum[0] += dr
        signed_sum[1] += dg
        signed_sum[2] += db

        px_max = max(adr, adg, adb)
        if px_max > max_rgb_delta:
            max_rgb_delta = px_max
        if da > max_alpha_delta:
            max_alpha_delta = da
        if px_max > outlier_thresh:
            outliers += 1

    rgb_mae = rgb_abs_sum / float(pixel_count * 3)
    outlier_fraction = outliers / float(pixel_count)
    metrics = {
        "width": lw,
        "height": lh,
        "exact_match": live_px == replay_px,
        "rgb_mae": round(rgb_mae, 6),
        "max_channel_delta": max_rgb_delta,
        "outlier_fraction": round(outlier_fraction, 6),
        "max_alpha_delta": max_alpha_delta,
        "signed_bias_rgb": [round(s / float(pixel_count), 6) for s in signed_sum],
    }

    failures: list[str] = []
    if rgb_mae > float(envelope.get("max_rgb_mae", 1.0)):
        failures.append(f"rgb_mae {rgb_mae:.4f} > {envelope.get('max_rgb_mae', 1.0)}")
    if max_rgb_delta > int(envelope.get("max_channel_delta", 4)):
        failures.append(
            f"max_channel_delta {max_rgb_delta} > {envelope.get('max_channel_delta', 4)}"
        )
    if outlier_fraction > float(envelope.get("max_outlier_fraction", 0.005)):
        failures.append(
            f"outlier_fraction {outlier_fraction:.4f} > {envelope.get('max_outlier_fraction', 0.005)}"
        )
    if max_alpha_delta > int(envelope.get("max_alpha_delta", 0)):
        failures.append(
            f"max_alpha_delta {max_alpha_delta} > {envelope.get('max_alpha_delta', 0)}"
        )

    return (len(failures) == 0), metrics, "; ".join(failures)


def evaluate_census_completeness(census: dict[str, Any]) -> dict[str, Any]:
    """Re-derive all 11 completeness conditions matching nvn_census_verdict.h."""
    if not isinstance(census, dict):
        raise InvalidInputError("census JSON must be an object")
    if census.get("schema") != 2 or census.get("kind") != "nvn_call_census":
        raise InvalidInputError("expected schema=2 nvn_call_census document")

    audit = census.get("audit") or {}
    cov = census.get("coverage") or {}
    acct = census.get("accounting") or {}
    pres = census.get("presents") or {}

    final_report = bool(census.get("final", False))
    run_complete = bool(census.get("run_complete", False))

    named_targets = int(cov.get("named_targets", 0))
    audit_targets = int(cov.get("audit_found_targets", 0))
    audit_entry_counted = int(cov.get("audit_found_counted_at_entry", 0))
    unknown_targets = int(audit.get("unknown_targets", 0))
    instrumented = int(cov.get("instrumented_targets", 0))

    known = named_targets + audit_targets
    discovered = known + unknown_targets
    resolved_share = (known / discovered) if discovered != 0 else 0.0
    instrumented_share = (instrumented / discovered) if discovered != 0 else 0.0

    executed = int(acct.get("executed", 0))
    startup = int(acct.get("startup", 0))
    frames = int(acct.get("frames", 0))
    late = int(acct.get("late", 0))
    tail = int(acct.get("tail", 0))
    mismatched = int(acct.get("mismatched", 0))
    accounting_exact = bool(acct.get("exact", False)) and (
        executed == startup + frames + late + tail
    ) and mismatched == 0

    presents = int(pres.get("presents", 0))
    frames_closed = int(pres.get("frames_closed", 0))
    min_frames = int(pres.get("min_frames", 0))

    audit_enabled = bool(audit.get("enabled", False))
    conditions: list[tuple[str, bool, str]] = [
        (
            "bootstrap_found_before_first_request",
            int(audit.get("bootstrap_sites", 0)) != 0
            and int(audit.get("bootstrap_calls", 0)) != 0
            and int(audit.get("late_modules", 0)) == 0
            and int(audit.get("code_changes", 0)) == 0,
            f"sites={audit.get('bootstrap_sites', 0)} late_modules={audit.get('late_modules', 0)}",
        ),
        (
            "every_target_resolved",
            audit_enabled and resolved_share == 1.0,
            f"audit={audit_enabled} resolved_share={resolved_share:.6f} unknown={unknown_targets}",
        ),
        (
            "every_target_instrumented",
            instrumented_share == 1.0,
            f"instrumented_share={instrumented_share:.6f} ({instrumented}/{discovered})",
        ),
        (
            "audit_found_entries_counted_at_entry",
            audit_entry_counted == audit_targets,
            f"audit_found={audit_targets} counted_at_entry={audit_entry_counted}",
        ),
        (
            "no_unknown_dispatch",
            audit_enabled
            and int(audit.get("unknown_calls", 0)) == 0
            and int(audit.get("unknown_overflow", 0)) == 0
            and int(audit.get("unaudited_entries", 0)) == 0,
            f"unknown_calls={audit.get('unknown_calls', 0)} unaudited={audit.get('unaudited_entries', 0)}",
        ),
        (
            "audit_matches_counts",
            audit_enabled
            and bool(audit.get("region_known", False))
            and int(audit.get("audited_entries", 0)) != 0
            and int(audit.get("shortfall_calls", 0)) <= int(audit.get("tolerance", 0)),
            f"audited_entries={audit.get('audited_entries', 0)} shortfall={audit.get('shortfall_calls', 0)}",
        ),
        (
            "frame_accounting_exact",
            accounting_exact and late == 0,
            f"executed={executed} sum={startup + frames + late + tail} mismatched={mismatched}",
        ),
        (
            "one_present_stream",
            int(pres.get("paths_used", 0)) == 1 and bool(pres.get("identity_ok", False)),
            f"paths={pres.get('paths_used', 0)} identity_ok={pres.get('identity_ok', False)}",
        ),
        (
            "presents_consistent",
            presents != 0
            and int(pres.get("id_gaps", 0)) == 0
            and int(pres.get("id_duplicates", 0)) == 0
            and frames_closed + 1 == presents,
            f"presents={presents} closed={frames_closed}",
        ),
        (
            "enough_frames",
            frames_closed >= min_frames,
            f"closed={frames_closed} required={min_frames}",
        ),
        (
            "no_dropped_or_lost_events",
            int(cov.get("lost_events", 0)) == 0,
            f"lost_events={cov.get('lost_events', 0)}",
        ),
    ]

    failed = [name for name, ok, _ in conditions if not ok]
    reasons = [f"{name}: {detail}" for name, ok, detail in conditions if not ok]

    stop_reason = str((census.get("stop") or {}).get("reason", ""))
    if not final_report:
        failed.append("not_a_final_report")
        reasons.append("not_a_final_report: report is not final")
    elif not run_complete or stop_reason == "capture_failed":
        failed.append("run_not_complete")
        reasons.append(f"run_not_complete: stop.reason={stop_reason!r}")

    complete = final_report and run_complete and len(failed) == 0
    return {
        "verdict": "COMPLETE" if complete else "INCOMPLETE",
        "complete": complete,
        "resolved_share": resolved_share,
        "instrumented_share": instrumented_share,
        "failed": failed,
        "reasons": reasons,
    }


def verify_fixture_dir(fixture_dir: Path) -> tuple[int, dict[str, Any]]:
    """Verify images, sync/effect ledger, and census completeness for fixture_dir."""
    try:
        manifest_path = fixture_dir / "manifest.json"
        frames_path = fixture_dir / "frames.jsonl"
        effects_path = fixture_dir / "effects.jsonl"
        census_path = fixture_dir / "census.json"
        for required in (manifest_path, frames_path, effects_path, census_path):
            if not required.is_file():
                raise InvalidInputError(f"missing {required.name}")

        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        envelope = manifest.get("envelope")
        if not isinstance(envelope, dict):
            raise InvalidInputError("manifest.json missing envelope object")

        raw_frames = frames_path.read_text(encoding="utf-8").splitlines()
        if not raw_frames:
            raise InvalidInputError("frames.jsonl is empty")

        frames: list[dict[str, Any]] = []
        seen_ids: set[int] = set()
        listed_pictures: set[str] = set()
        for line_no, raw in enumerate(raw_frames, 1):
            try:
                rec = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise InvalidInputError(f"frames.jsonl:{line_no} invalid JSON: {exc}") from exc
            if not isinstance(rec, dict):
                raise InvalidInputError(f"frames.jsonl:{line_no} must be a JSON object")
            fid = rec.get("frame_id")
            if not isinstance(fid, int) or fid in seen_ids:
                raise InvalidInputError(f"frames.jsonl:{line_no} invalid or duplicate frame_id")
            seen_ids.add(fid)
            live_rel = _plain_relative_name(fixture_dir, rec.get("live_picture"))
            replay_rel = _plain_relative_name(fixture_dir, rec.get("replay_picture"))
            if live_rel is None or replay_rel is None or live_rel == replay_rel:
                raise InvalidInputError(f"frames.jsonl:{line_no} invalid picture path")
            if not live_rel.is_file() or not replay_rel.is_file():
                raise InvalidInputError(f"frames.jsonl:{line_no} missing picture file")
            listed_pictures.add(live_rel.name)
            listed_pictures.add(replay_rel.name)
            frames.append(rec)

        expected_frames = manifest.get("expected_frames")
        if expected_frames is not None and len(frames) != int(expected_frames):
            raise InvalidInputError(
                f"frame count {len(frames)} != expected_frames {expected_frames}"
            )

        for png_file in fixture_dir.glob("*.png"):
            if png_file.name not in listed_pictures:
                raise InvalidInputError(f"unlisted PNG file in fixture dir: {png_file.name}")

        # Verify effects ledger & sync order.
        raw_effects = effects_path.read_text(encoding="utf-8").splitlines()
        if not raw_effects:
            raise InvalidInputError("effects.jsonl is empty")
        counts_by_class = {cls_name: 0 for cls_name in VALID_CLASSIFICATIONS}
        effects_by_frame: dict[int, list[dict[str, Any]]] = {}
        for line_no, raw in enumerate(raw_effects, 1):
            try:
                eff = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise InvalidInputError(f"effects.jsonl:{line_no} invalid JSON: {exc}") from exc
            if eff.get("kind") not in VALID_EFFECT_KINDS:
                raise InvalidInputError(f"effects.jsonl:{line_no} invalid effect kind")
            cls_name = eff.get("classification")
            if cls_name not in VALID_CLASSIFICATIONS:
                raise InvalidInputError(f"effects.jsonl:{line_no} invalid classification")
            eff_fid = eff.get("frame_id")
            if not isinstance(eff_fid, int) or eff_fid not in seen_ids:
                raise InvalidInputError(
                    f"effects.jsonl:{line_no} references unknown frame_id {eff_fid!r}"
                )
            counts_by_class[cls_name] += 1
            effects_by_frame.setdefault(eff_fid, []).append(eff)

        fail_reasons: list[str] = []
        frame_results: list[dict[str, Any]] = []
        prev_fid = -1
        prev_syncpt = -1
        prev_fence = -1

        for rec in frames:
            fid = int(rec["frame_id"])
            syncpt = int(rec["syncpoint_value"])
            fence = int(rec["fence_id"])
            if fid <= prev_fid:
                fail_reasons.append(f"frame {fid}: non-monotonic frame_id after {prev_fid}")
            if syncpt <= prev_syncpt:
                fail_reasons.append(f"frame {fid}: non-monotonic syncpoint {syncpt} <= {prev_syncpt}")
            if fence <= prev_fence:
                fail_reasons.append(f"frame {fid}: non-monotonic fence {fence} <= {prev_fence}")
            prev_fid, prev_syncpt, prev_fence = fid, syncpt, fence

            if int(rec.get("semantic_mismatches", 0)) != 0:
                fail_reasons.append(
                    f"frame {fid}: semantic_mismatches={rec.get('semantic_mismatches')}"
                )

            frame_effs = sorted(effects_by_frame.get(fid, []), key=lambda e: int(e["seq"]))
            if not frame_effs:
                fail_reasons.append(f"frame {fid}: missing effects ledger entries")
            else:
                seq_by_kind: dict[str, int] = {}
                for eff in frame_effs:
                    if not bool(eff.get("reconciled", False)):
                        fail_reasons.append(
                            f"frame {fid} seq {eff['seq']}: unreconciled effect"
                        )
                    seq_by_kind[eff["kind"]] = int(eff["seq"])
                for needed in ("submit", "consume", "syncpoint_increment", "fence_signal", "present_compose"):
                    if needed not in seq_by_kind:
                        fail_reasons.append(f"frame {fid}: missing required {needed} effect")
                if "present_compose" in seq_by_kind:
                    present_seq = seq_by_kind["present_compose"]
                    for before in ("submit", "consume", "syncpoint_increment", "fence_signal"):
                        if before in seq_by_kind and seq_by_kind[before] >= present_seq:
                            fail_reasons.append(
                                f"frame {fid}: {before} seq {seq_by_kind[before]} not before present_compose seq {present_seq}"
                            )

            ok_img, img_metrics, img_err = compare_image_pair(
                fixture_dir / rec["live_picture"],
                fixture_dir / rec["replay_picture"],
                envelope,
            )
            if int(rec["width"]) != img_metrics["width"] or int(rec["height"]) != img_metrics["height"]:
                raise InvalidInputError(f"frame {fid}: PNG dimensions differ from frames.jsonl")
            if not ok_img:
                fail_reasons.append(f"frame {fid} image envelope violation: {img_err}")
            frame_results.append({"frame_id": fid, "image": img_metrics, "passed": ok_img})

        census = json.loads(census_path.read_text(encoding="utf-8"))
        census_verdict = evaluate_census_completeness(census)

        total_effects = sum(counts_by_class.values())
        reconciled_ok = total_effects == len(raw_effects)
        if not reconciled_ok:
            fail_reasons.append("effect denominator mismatch")

        if fail_reasons:
            status = "FAIL"
            exit_code = EXIT_FAIL
        elif not census_verdict["complete"]:
            status = "INCOMPLETE"
            exit_code = EXIT_INCOMPLETE
        else:
            status = "PASS"
            exit_code = EXIT_PASS

        return exit_code, {
            "status": status,
            "exit_code": exit_code,
            "frames": frame_results,
            "effect_reconciliation": {
                "total": total_effects,
                "by_classification": counts_by_class,
                "reconciled": reconciled_ok,
            },
            "census": census_verdict,
            "failures": fail_reasons + ([] if census_verdict["complete"] else census_verdict["reasons"]),
        }
    except InvalidInputError as exc:
        return EXIT_INVALID, {"status": "INVALID", "exit_code": EXIT_INVALID, "failures": [str(exc)]}


def run_selftest() -> int:
    """Run positive and intentional mismatch-rejection controls in a temporary directory."""
    checks: list[tuple[str, int, int]] = []

    with tempfile.TemporaryDirectory(prefix="replay_qualify_selftest_") as tmp:
        root = Path(tmp)

        # 1. Exact synthetic fixture -> PASS (0)
        exact_dir = write_synthetic_fixture(root / "case_exact")
        code, res = verify_fixture_dir(exact_dir)
        checks.append(("exact_fixture_pass", EXIT_PASS, code))

        # 2. Within-envelope 1-LSB perturbation on replay frame 1 -> PASS (0)
        env_dir = write_synthetic_fixture(root / "case_within_envelope")
        w, h, px = decode_rgba_png(env_dir / "replay_00000001.png")
        mutated = bytearray(px)
        mutated[0] = mutated[0] + 1 if mutated[0] < 255 else 254
        (env_dir / "replay_00000001.png").write_bytes(encode_rgba_png(w, h, bytes(mutated)))
        code, _ = verify_fixture_dir(env_dir)
        checks.append(("within_envelope_1lsb_pass", EXIT_PASS, code))

        # 3. Pixel mismatch exceeding envelope -> FAIL (1)
        bad_px_dir = write_synthetic_fixture(root / "case_bad_pixels")
        w, h, px = decode_rgba_png(bad_px_dir / "replay_00000002.png")
        mutated = bytearray(px)
        for i in range(0, len(mutated), 4):
            mutated[i] = (mutated[i] + 40) & 0xFF
        (bad_px_dir / "replay_00000002.png").write_bytes(encode_rgba_png(w, h, bytes(mutated)))
        code, _ = verify_fixture_dir(bad_px_dir)
        checks.append(("pixel_envelope_reject", EXIT_FAIL, code))

        # 4. Dimension mismatch -> INVALID (2)
        bad_dim_dir = write_synthetic_fixture(root / "case_bad_dim")
        (bad_dim_dir / "replay_00000001.png").write_bytes(
            encode_rgba_png(16, 16, make_synthetic_rgba(16, 16, 1))
        )
        code, _ = verify_fixture_dir(bad_dim_dir)
        checks.append(("dimension_mismatch_reject", EXIT_INVALID, code))

        # 5. Out-of-order syncpoint / fence after present -> FAIL (1)
        bad_sync_dir = write_synthetic_fixture(root / "case_bad_sync")
        eff_lines = [
            json.loads(line)
            for line in (bad_sync_dir / "effects.jsonl").read_text(encoding="utf-8").splitlines()
        ]
        for eff in eff_lines:
            if eff["frame_id"] == 1 and eff["kind"] == "fence_signal":
                eff["seq"] = 99  # signaled after present_compose (seq 9)
        (bad_sync_dir / "effects.jsonl").write_text(
            "\n".join(json.dumps(e, sort_keys=True) for e in eff_lines) + "\n",
            encoding="utf-8",
        )
        code, _ = verify_fixture_dir(bad_sync_dir)
        checks.append(("sync_order_reject", EXIT_FAIL, code))

        # 6. Non-zero semantic output mismatch -> FAIL (1)
        bad_sem_dir = write_synthetic_fixture(root / "case_bad_semantic")
        frm_lines = [
            json.loads(line)
            for line in (bad_sem_dir / "frames.jsonl").read_text(encoding="utf-8").splitlines()
        ]
        frm_lines[0]["semantic_mismatches"] = 2
        (bad_sem_dir / "frames.jsonl").write_text(
            "\n".join(json.dumps(f, sort_keys=True) for f in frm_lines) + "\n",
            encoding="utf-8",
        )
        code, _ = verify_fixture_dir(bad_sem_dir)
        checks.append(("semantic_mismatch_reject", EXIT_FAIL, code))

        # 7. Unreconciled effect entry (silent omission) -> FAIL (1)
        unrec_dir = write_synthetic_fixture(root / "case_unreconciled")
        eff_lines = [
            json.loads(line)
            for line in (unrec_dir / "effects.jsonl").read_text(encoding="utf-8").splitlines()
        ]
        eff_lines[3]["reconciled"] = False
        (unrec_dir / "effects.jsonl").write_text(
            "\n".join(json.dumps(e, sort_keys=True) for e in eff_lines) + "\n",
            encoding="utf-8",
        )
        code, _ = verify_fixture_dir(unrec_dir)
        checks.append(("unreconciled_effect_reject", EXIT_FAIL, code))

        # 8. Incomplete census (unknown dispatch + capture_failed pollution) -> INCOMPLETE (3)
        inc_dir = write_synthetic_fixture(root / "case_incomplete_census")
        census = json.loads((inc_dir / "census.json").read_text(encoding="utf-8"))
        census["audit"]["unknown_targets"] = 2
        census["audit"]["unknown_calls"] = 14
        census["run_complete"] = False
        census["stop"] = {"reason": "capture_failed", "clean": False}
        (inc_dir / "census.json").write_text(json.dumps(census, indent=2) + "\n", encoding="utf-8")
        code, _ = verify_fixture_dir(inc_dir)
        checks.append(("incomplete_census_reject", EXIT_INCOMPLETE, code))

        # 9. Orphan effect row referencing an unlisted frame_id -> INVALID (2)
        orphan_dir = write_synthetic_fixture(root / "case_orphan_effect")
        orphan_eff = {
            "frame_id": 999,
            "seq": 1,
            "kind": "draw",
            "classification": "supported",
            "reconciled": True,
            "syncpoint_value": 9990,
            "fence_id": 99900,
            "detail": "orphan frame entry",
        }
        with (orphan_dir / "effects.jsonl").open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(orphan_eff, sort_keys=True) + "\n")
        code, _ = verify_fixture_dir(orphan_dir)
        checks.append(("orphan_effect_frame_reject", EXIT_INVALID, code))

    failed = [(name, exp, got) for name, exp, got in checks if exp != got]
    for name, exp, got in checks:
        print(f"[selftest] {name}: expected={exp} actual={got} {'OK' if exp == got else 'FAIL'}")
    if failed:
        return 1
    print(f"selftest ok ({len(checks)}/{len(checks)} checks passed)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write-fixture", type=Path, help="Write original synthetic fixture to DIR")
    group.add_argument("--verify", type=Path, help="Verify replay + effects + census in DIR")
    group.add_argument("--reconcile-census", type=Path, help="Evaluate schema-2 census JSON")
    group.add_argument("--selftest", action="store_true", help="Run positive & negative controls")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()
    if args.write_fixture is not None:
        out = write_synthetic_fixture(args.write_fixture)
        print(f"wrote original synthetic fixture to {out}")
        return EXIT_PASS
    if args.reconcile_census is not None:
        try:
            data = json.loads(args.reconcile_census.read_text(encoding="utf-8"))
            verdict = evaluate_census_completeness(data)
        except (OSError, json.JSONDecodeError, InvalidInputError) as exc:
            print(json.dumps({"status": "INVALID", "error": str(exc)}, indent=2))
            return EXIT_INVALID
        print(json.dumps(verdict, indent=2, sort_keys=True))
        return EXIT_PASS if verdict["complete"] else EXIT_INCOMPLETE

    code, summary = verify_fixture_dir(args.verify)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
