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
    python tools/replay_qualify/replay_qualify.py --verify-images <replay-images-dir>
    python tools/replay_qualify/replay_qualify.py --reconcile-census <census.json>
    python tools/replay_qualify/replay_qualify.py --selftest

Exit codes:
    0  PASS (all checks for the requested scope passed)
    1  FAIL (pixel envelope violation, sync/effect mismatch, or unreconciled call/effect)
    2  INVALID (malformed/missing/extra inputs or dimension/format mismatch)
    3  INCOMPLETE (census completeness gate failed)
"""

from __future__ import annotations

import argparse
import binascii
import json
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

IMAGE_LIMITS = {
    "max_rgb_mae": 1.0,
    "max_channel_delta": 4,
    "max_outlier_fraction": 0.005,
    "outlier_delta_threshold": 2,
    "max_alpha_delta": 0,
}
FIXTURE_EFFECTS = [
    "submit", "consume", "draw", "compute_dispatch", "syncpoint_increment",
    "fence_signal", "readback_visibility", "draw", "present_compose",
]
LOSS_COUNTERS = (
    "role_conflicts", "probe_table_full", "target_table_full", "pending_overflow",
    "returns_not_observed", "rendezvous_timeouts", "name_read_failures", "late_modules",
    "stream_overflow", "guest_thread_overflow", "unknown_overflow",
    "epoch_reservation_timeouts", "code_changes", "unaudited_entries", "ambiguous_pending",
)

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


def field(obj: Any, key: str, cls: type, where: str) -> Any:
    value = obj.get(key) if isinstance(obj, dict) else None
    if type(value) is not cls or (cls is int and value < 0):
        raise InvalidInputError(f"{where}.{key}: expected {cls.__name__}, got {value!r}")
    return value


def read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (ValueError, OSError) as exc:
        raise InvalidInputError(f"{path}: {exc}") from exc


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
                not 1 <= width <= 8192
                or not 1 <= height <= 8192
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

    expected_raw = height * (1 + width * 4)
    inflater = zlib.decompressobj()
    try:
        raw = inflater.decompress(b"".join(idat_parts), expected_raw + 1)
    except zlib.error as exc:
        raise InvalidInputError(f"{path.name}: corrupt zlib stream: {exc}") from exc

    bpp = 4
    stride = width * bpp
    if len(raw) != expected_raw or not inflater.eof or inflater.unused_data or offset != len(blob):
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


def picture_checksum(pixels: bytes) -> str:
    """ReplayFrameLine hashes tightly packed BGRA before the runner swizzles it to PNG."""
    bgra = bytearray(pixels)
    bgra[0::4], bgra[2::4] = pixels[2::4], pixels[0::4]
    h = 0xCBF29CE484222325
    for byte in bgra:
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


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
    outputs = {}
    frame_ids = [1, 2]
    frame_lines: list[str] = []
    effect_lines: list[str] = []

    for frame_id in frame_ids:
        pixels = make_synthetic_rgba(width, height, frame_id)
        checksum = picture_checksum(pixels)
        live_name = f"live_{frame_id:08d}.png"
        replay_name = f"replay_{frame_id:08d}.png"
        png_bytes = encode_rgba_png(width, height, pixels)
        outputs[live_name] = outputs[replay_name] = png_bytes

        syncpoint_val = frame_id * 10
        fence_id = frame_id * 100
        frame_lines.append(
            json.dumps(
                {
                    "schema": 1,
                    "frame_id": frame_id,
                    "width": width,
                    "height": height,
                    "live_png": live_name,
                    "replay_png": replay_name,
                    "live_checksum": checksum,
                    "replay_checksum": checksum,
                    "identical": True,
                    "semantic_mismatches": 0,
                    "replay_ns": 0,
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

    outputs["frames.jsonl"] = ("\n".join(frame_lines) + "\n").encode()
    outputs["effects.jsonl"] = ("\n".join(effect_lines) + "\n").encode()

    manifest = {
        "schema": 2,
        "kind": "standalone_replay_qualification_fixture",
        "origin": "synthetic_procedural_original",
        "envelope": IMAGE_LIMITS,
        "expected_effects": {
            str(fid): {"kinds": FIXTURE_EFFECTS, "syncpoint_value": fid * 10, "fence_id": fid * 100}
            for fid in frame_ids
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
    outputs["manifest.json"] = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()

    census = {
        "schema": 2,
        "kind": "nvn_call_census",
        "final": True,
        "run_complete": True,
        "stop": {"reason": "frame_limit", "clean": True},
        "replay_capture": {"failed": False, "detail": ""},
        "bootstrap": {"sites": 1, "bootstrap_calls": 4},
        "provenance": {"min_frames": 2},
        "audit": {
            "enabled": True, "region_known": True, "known_entries": 12,
            "unknown_targets": 0, "unknown_calls": 0, "unknown_overflow": 0,
            "mismatched_calls": 0,
        },
        "errors": {name: 0 for name in (*LOSS_COUNTERS, "id_gaps", "id_duplicates", "late_calls", "unmatched_returns")},
        "functions": [
            {"provenance": [source], "acked": True, "calls": 6, "audited_entries": 6,
             "startup_calls": 1, "frame_calls": 5, "late_calls": 0, "open_tail_calls": 0}
            for source in ("resolver", "audit")
        ],
        "guest_threads": [{"id": 1, "calls": 12}],
        "coverage_complete": True,
        "coverage": {"failed": [], "resolved_target_share": 1.0, "instrumented_target_share": 1.0},
        "accounting": {
            "exact": True, "executed_total": 12, "startup_calls": 2,
            "closed_frame_calls": 10, "late_calls": 0, "open_tail_calls": 0, "mismatched_functions": 0,
        },
        "frames": {
            "presents": 3, "closed": 2, "presents_by_path": {"nx2_vi2": 3, "buffer_queue": 0},
            "streams": [{"path": "nx2_vi2", "stream": "0x1", "token": "0x1", "tokens": 1,
                         "ambiguous": False, "presents": 3}],
        },
    }
    outputs["census.json"] = (json.dumps(census, indent=2, sort_keys=True) + "\n").encode()
    out_dir.mkdir(parents=True, exist_ok=True)
    existing = list(out_dir.iterdir())
    if existing:
        if ({p.name for p in existing} == outputs.keys()
                and all(p.is_file() and p.read_bytes() == outputs[p.name] for p in existing)):
            return out_dir
        raise InvalidInputError(f"{out_dir}: refusing to overwrite existing evidence; choose an empty directory")
    for name, data in outputs.items():
        with (out_dir / name).open("xb") as out:
            out.write(data)
    return out_dir


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
        "live_checksum": picture_checksum(live_px),
        "replay_checksum": picture_checksum(replay_px),
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

    def section(key: str) -> dict:
        return field(census, key, dict, "census")

    def number(obj: dict, key: str) -> int:
        return field(obj, key, int, "census")

    audit, cov, acct = section("audit"), section("coverage"), section("accounting")
    pres, errors, bootstrap = section("frames"), section("errors"), section("bootstrap")
    final_report = field(census, "final", bool, "census")
    run_complete = field(census, "run_complete", bool, "census")
    functions = field(census, "functions", list, "census")
    known = len(functions)
    discovered = known + number(audit, "unknown_targets")
    instrumented = audit_targets = audit_counted = audited = executed = shortfall = 0
    rows_exact = True
    for f in functions:
        acked = field(f, "acked", bool, "census.functions")
        sources = field(f, "provenance", list, "census.functions")
        calls, entries = number(f, "calls"), number(f, "audited_entries")
        instrumented += acked
        if "audit" in sources:
            audit_targets += 1
            audit_counted += acked
        executed += calls
        audited += entries
        shortfall += max(0, entries - calls)
        rows_exact &= calls == sum(number(f, k) for k in
                                   ("startup_calls", "frame_calls", "late_calls", "open_tail_calls"))

    resolved_share = known / discovered if discovered else 0.0
    instrumented_share = instrumented / discovered if discovered else 0.0
    parts = sum(number(acct, k) for k in
                ("startup_calls", "closed_frame_calls", "late_calls", "open_tail_calls"))
    accounting_exact = (
        field(acct, "exact", bool, "census.accounting") and rows_exact
        and parts == executed == number(acct, "executed_total")
        and not number(acct, "mismatched_functions")
    )
    threads = field(census, "guest_threads", list, "census")
    tolerance = len(threads)
    # Early schema-2 writers omitted mismatched_calls (the per-function figures are sufficient).
    reported_shortfall = number(audit, "mismatched_calls") if "mismatched_calls" in audit else shortfall
    enabled = field(audit, "enabled", bool, "census.audit")
    paths = field(pres, "presents_by_path", dict, "census.frames")
    used = [p for p in paths if number(paths, p)]
    streams = field(pres, "streams", list, "census.frames")
    identity_ok = False
    if len(streams) == 1:
        stream = streams[0]
        path = field(stream, "path", str, "census.frames.streams")
        identity_ok = (
            used == [path] and int(field(stream, "stream", str, "census.frames.streams"), 16) != 0
            and number(stream, "tokens") == 1 and not field(stream, "ambiguous", bool, "stream")
            and (path != "nx2_vi2" or int(field(stream, "token", str, "stream"), 16) != 0)
        )
    presents, closed = number(pres, "presents"), number(pres, "closed")
    min_frames = number(section("provenance"), "min_frames")
    lost = {k: n for k in LOSS_COUNTERS if (n := number(errors, k))}
    conditions = [
        ("bootstrap_found_before_first_request",
         number(bootstrap, "sites") and number(bootstrap, "bootstrap_calls")
         and not number(errors, "late_modules") and not number(errors, "code_changes"),
         f"sites={bootstrap['sites']}"),
        ("every_target_resolved", enabled and discovered and known == discovered,
         f"resolved={known}/{discovered}"),
        ("every_target_instrumented", discovered and instrumented == discovered,
         f"instrumented={instrumented}/{discovered}"),
        ("audit_found_entries_counted_at_entry", audit_targets == audit_counted,
         f"counted={audit_counted}/{audit_targets}"),
        ("no_unknown_dispatch", enabled and not number(audit, "unknown_calls")
         and not number(audit, "unknown_overflow") and not number(errors, "unaudited_entries"),
         f"unknown_calls={audit.get('unknown_calls')} unaudited={errors['unaudited_entries']}"),
        ("audit_matches_counts", enabled and field(audit, "region_known", bool, "audit")
         and audited == number(audit, "known_entries") and audited > 0
         and shortfall == reported_shortfall and shortfall <= tolerance,
         f"audited={audited} shortfall={shortfall} tolerance={tolerance}"),
        ("frame_accounting_exact", accounting_exact and not number(acct, "late_calls")
         and not number(errors, "late_calls"), f"parts={parts} executed={executed}"),
        ("one_present_stream", identity_ok, f"paths={used} streams={len(streams)}"),
        ("presents_consistent", presents == closed + 1 and not number(errors, "id_gaps")
         and not number(errors, "id_duplicates"), f"presents={presents} closed={closed}"),
        ("enough_frames", closed >= min_frames, f"closed={closed} required={min_frames}"),
        ("no_dropped_or_lost_events", not lost, f"lost={lost}"),
    ]

    failed = [name for name, ok, _ in conditions if not ok]
    reasons = [f"{name}: {detail}" for name, ok, detail in conditions if not ok]

    stop = section("stop")
    stop_reason = field(stop, "reason", str, "census.stop")
    clean = field(stop, "clean", bool, "census.stop")
    if not final_report:
        failed.append("not_a_final_report")
        reasons.append("not_a_final_report: report is not final")
    elif not run_complete or not clean or stop_reason not in {
        "guest_exit", "window_closed", "requested", "frame_limit", "timeout"
    }:
        failed.append("run_not_complete")
        reasons.append(f"run_not_complete: stop.reason={stop_reason!r}")
    for name in field(cov, "failed", list, "census.coverage"):
        if not isinstance(name, str):
            raise InvalidInputError("census.coverage.failed: expected condition names")
        if name not in failed:
            failed.append(name)
            reasons.append(f"producer reports failed condition: {name}")
    if not field(census, "coverage_complete", bool, "census") and not failed:
        failed.append("coverage_not_complete")
        reasons.append("producer did not certify coverage_complete")

    complete = final_report and run_complete and len(failed) == 0
    return {
        "verdict": "COMPLETE" if complete else "INCOMPLETE",
        "complete": complete,
        "resolved_share": resolved_share,
        "instrumented_share": instrumented_share,
        "failed": failed,
        "reasons": reasons,
    }


def read_records(path: Path):
    with path.open(encoding="utf-8") as rows:
        for n, line in enumerate(rows, 1):
            where = f"{path.name}:{n}"
            try:
                record = json.loads(line)
            except ValueError as exc:
                raise InvalidInputError(f"{where}: {exc}") from exc
            if not isinstance(record, dict):
                raise InvalidInputError(f"{where}: expected object")
            yield where, record


def verify_frames(root: Path, envelope: dict) -> tuple[dict, list[str]]:
    frames = {}
    failures = []
    listed = set()
    previous = -1
    for where, rec in read_records(root / "frames.jsonl"):
        if field(rec, "schema", int, where) != 1:
            raise InvalidInputError(f"{where}: expected replay frame schema 1")
        fid = field(rec, "frame_id", int, where)
        if fid in frames:
            raise InvalidInputError(f"{where}: duplicate frame_id {fid}")
        before = len(failures)
        if fid <= previous:
            failures.append(f"{where}: frame_id {fid} after {previous}")
        previous = fid
        for side in ("live", "replay"):
            name = f"{side}_{fid:08d}.png"
            if field(rec, side + "_png", str, where) != name:
                raise InvalidInputError(f"{where}: expected {side}_png {name}")
            listed.add(name)
        ok, metrics, detail = compare_image_pair(root / rec["live_png"], root / rec["replay_png"], envelope)
        if not ok:
            failures.append(f"{where}: image envelope violation: {detail}")
        for key in ("width", "height"):
            if metrics[key] != field(rec, key, int, where):
                raise InvalidInputError(f"{where}: {key} {rec[key]} != PNG {metrics[key]}")
        for key in ("live_checksum", "replay_checksum"):
            if metrics[key] != field(rec, key, str, where):
                failures.append(f"{where}: {key} {rec[key]} != PNG checksum {metrics[key]}")
        if field(rec, "identical", bool, where) != metrics["exact_match"]:
            failures.append(f"{where}: identical disagrees with PNG pair")
        if field(rec, "semantic_mismatches", int, where):
            failures.append(f"{where}: semantic_mismatches={rec['semantic_mismatches']}")
        frames[fid] = {"frame_id": fid, "image": metrics, "passed": len(failures) == before}
    if not frames:
        raise InvalidInputError("frames.jsonl is empty")
    for p in root.glob("*.png"):
        if p.name not in listed:
            raise InvalidInputError(f"unlisted PNG: {p.name}")
    return frames, failures


def verify_effects(root: Path, expected: dict, frames: dict) -> tuple[dict, list[str]]:
    if set(expected) != {str(fid) for fid in frames}:
        raise InvalidInputError("manifest.json: expected_effects frame IDs differ from frames.jsonl")
    counts = dict.fromkeys(sorted(VALID_CLASSIFICATIONS), 0)
    rows = {fid: [] for fid in frames}
    failures = []
    prev = -1
    for where, row in read_records(root / "effects.jsonl"):
        fid = field(row, "frame_id", int, where)
        if fid not in rows:
            raise InvalidInputError(f"{where}: unknown frame_id {fid}")
        if fid < prev:
            failures.append(f"{where}: frame {fid} after {prev}")
            frames[fid]["passed"] = False
        prev = fid
        cls = field(row, "classification", str, where)
        if cls not in counts or field(row, "kind", str, where) not in VALID_EFFECT_KINDS:
            raise InvalidInputError(f"{where}: invalid classification or effect kind")
        counts[cls] += 1
        rows[fid].append((where, row))
    last_sync = last_fence = -1
    total_expected = 0
    for fid, records in rows.items():
        where = f"manifest.json:expected_effects.{fid}"
        spec = expected[str(fid)]
        kinds = field(spec, "kinds", list, where)
        order = ("submit", "consume", "syncpoint_increment", "fence_signal", "present_compose")
        if (not all(isinstance(k, str) and k in VALID_EFFECT_KINDS for k in kinds)
                or not all(k in kinds for k in order)
                or [kinds.index(k) for k in order] != sorted(kinds.index(k) for k in order)):
            raise InvalidInputError(f"{where}: invalid ordered effect denominator")
        total_expected += len(kinds)
        sync = field(spec, "syncpoint_value", int, where)
        fence = field(spec, "fence_id", int, where)
        before = len(failures)
        if sync <= last_sync or fence <= last_fence:
            failures.append(f"{where}: non-monotonic syncpoint or fence")
        last_sync, last_fence = sync, fence
        if [row["kind"] for _, row in records] != kinds:
            failures.append(f"effects.jsonl: frame {fid}: effects do not match expected order/count")
        for seq, (where, row) in enumerate(records, 1):
            if (field(row, "seq", int, where) != seq or not field(row, "reconciled", bool, where)
                    or field(row, "syncpoint_value", int, where) != sync
                    or field(row, "fence_id", int, where) != fence):
                failures.append(f"{where}: expected reconciled seq={seq} syncpoint={sync} fence={fence}")
        if len(failures) != before:
            frames[fid]["passed"] = False
    return {"total": sum(counts.values()), "expected": total_expected,
            "by_classification": counts, "reconciled": not failures}, failures


def verify_fixture_dir(fixture_dir: Path, *, images_only: bool = False) -> tuple[int, dict[str, Any]]:
    """Raw replay image evidence alone is not an effect ledger or census certificate."""
    try:
        envelope = IMAGE_LIMITS
        if not images_only:
            manifest = read_json(fixture_dir / "manifest.json")
            if (field(manifest, "schema", int, "manifest.json") != 2 or
                    manifest.get("kind") != "standalone_replay_qualification_fixture"):
                raise InvalidInputError("manifest.json: expected qualification manifest schema 2; regenerate old fixtures")
            envelope = field(manifest, "envelope", dict, "manifest.json")
            for key, limit in IMAGE_LIMITS.items():
                value = envelope.get(key)
                if (type(value) not in (int, float) or not 0 <= value <= limit
                        or (type(limit) is int and type(value) is not int)):
                    raise InvalidInputError(f"manifest.json: {key}={value!r} outside allowed 0..{limit}")
        frames, failures = verify_frames(fixture_dir, envelope)
        result = {"scope": "images" if images_only else "fixture", "frames": list(frames.values()),
                  "failures": failures}
        code = EXIT_FAIL if failures else EXIT_PASS
        if not images_only:
            n = field(manifest, "expected_frames", int, "manifest.json")
            if len(frames) != n:
                raise InvalidInputError(f"frames.jsonl: {len(frames)} frames, manifest expects {n}")
            expected = field(manifest, "expected_effects", dict, "manifest.json")
            effects, bad_effects = verify_effects(fixture_dir, expected, frames)
            result["effect_reconciliation"] = effects
            failures.extend(bad_effects)
            census = evaluate_census_completeness(read_json(fixture_dir / "census.json"))
            result["census"] = census
            code = EXIT_FAIL if failures else EXIT_PASS if census["complete"] else EXIT_INCOMPLETE
            failures.extend(census["reasons"])
        result.update(exit_code=code, status={0: "PASS", 1: "FAIL", 3: "INCOMPLETE"}[code])
        return code, result
    except (InvalidInputError, OSError, ValueError) as exc:
        return EXIT_INVALID, {"status": "INVALID", "exit_code": EXIT_INVALID, "failures": [str(exc)]}


def run_selftest() -> int:
    """Run positive and intentional mismatch-rejection controls in a temporary directory."""
    checks: list[tuple[str, int, int]] = []

    with tempfile.TemporaryDirectory(prefix="replay_qualify_selftest_") as tmp:
        root = Path(tmp)

        # 1. Exact synthetic fixture -> PASS (0)
        exact_dir = write_synthetic_fixture(root / "case_exact")
        write_synthetic_fixture(exact_dir)  # an unchanged second generation is allowed
        code, _ = verify_fixture_dir(exact_dir)
        checks.append(("exact_fixture_pass", EXIT_PASS, code))

        # 2. Within-envelope 1-LSB perturbation on replay frame 1 -> PASS (0)
        env_dir = write_synthetic_fixture(root / "case_within_envelope")
        w, h, px = decode_rgba_png(env_dir / "replay_00000001.png")
        mutated = bytearray(px)
        mutated[0] = mutated[0] + 1 if mutated[0] < 255 else 254
        (env_dir / "replay_00000001.png").write_bytes(encode_rgba_png(w, h, bytes(mutated)))
        code, _ = verify_fixture_dir(env_dir)
        checks.append(("stale_checksum_reject", EXIT_FAIL, code))
        rows = [r for _, r in read_records(env_dir / "frames.jsonl")]
        rows[0].update(replay_checksum=picture_checksum(mutated), identical=False)
        (env_dir / "frames.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")
        code, _ = verify_fixture_dir(env_dir)
        checks.append(("within_envelope_1lsb_pass", EXIT_PASS, code))

        # 3. Pixel mismatch exceeding envelope -> FAIL (1)
        bad_px_dir = write_synthetic_fixture(root / "case_bad_pixels")
        w, h, px = decode_rgba_png(bad_px_dir / "replay_00000002.png")
        mutated = bytearray(px)
        for i in range(0, len(mutated), 4):
            mutated[i] = (mutated[i] + 40) & 0xFF
        (bad_px_dir / "replay_00000002.png").write_bytes(encode_rgba_png(w, h, bytes(mutated)))
        rows = [r for _, r in read_records(bad_px_dir / "frames.jsonl")]
        rows[1].update(replay_checksum=picture_checksum(mutated), identical=False)
        (bad_px_dir / "frames.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")
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

        for label, change in (
            ("omitted_draw", lambda r: r.pop(2)),
            ("duplicate_seq", lambda r: r[2].update(seq=2)),
            ("wrong_sync", lambda r: r[4].update(syncpoint_value=11)),
            ("wrong_fence", lambda r: r[5].update(fence_id=99)),
        ):
            scene = write_synthetic_fixture(root / label)
            rows = [r for _, r in read_records(scene / "effects.jsonl")]
            change(rows)
            (scene / "effects.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")
            code, result = verify_fixture_dir(scene)
            checks.append((label, EXIT_FAIL, code))
            checks.append((label + "_unreconciled", False, result["effect_reconciliation"]["reconciled"]))

        for label, change, expected in (
            ("lost_probe", lambda c: c["errors"].update(probe_table_full=1), EXIT_INCOMPLETE),
            ("unclean_stop", lambda c: c["stop"].update(clean=False), EXIT_INCOMPLETE),
            ("mixed_stream", lambda c: c["frames"]["streams"][0].update(tokens=2), EXIT_INCOMPLETE),
            ("wrong_counter_type", lambda c: c["audit"].update(known_entries="12"), EXIT_INVALID),
            ("capture_independent", lambda c: c["replay_capture"].update(failed=True), EXIT_PASS),
        ):
            scene = write_synthetic_fixture(root / label)
            c = read_json(scene / "census.json")
            change(c)
            (scene / "census.json").write_text(json.dumps(c), encoding="utf-8")
            code, _ = verify_fixture_dir(scene)
            checks.append((label, expected, code))

        # The standalone runner emits no synthetic manifest, effect ledger or census.
        for name in ("manifest.json", "effects.jsonl", "census.json"):
            (exact_dir / name).unlink()
        code, _ = verify_fixture_dir(exact_dir, images_only=True)
        checks.append(("runner_images_without_sidecars", EXIT_PASS, code))
        (exact_dir / "frames.jsonl").write_text("[]\n", encoding="utf-8")
        code, _ = verify_fixture_dir(exact_dir, images_only=True)
        checks.append(("malformed_runner_row", EXIT_INVALID, code))
        try:
            write_synthetic_fixture(exact_dir)
        except InvalidInputError:
            pass
        checks.append(("real_evidence_not_overwritten", True,
                       (exact_dir / "frames.jsonl").read_text(encoding="utf-8") == "[]\n"))

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
    group.add_argument("--verify-images", type=Path, help="Verify actual --replay-images output (images and semantics only)")
    group.add_argument("--reconcile-census", type=Path, help="Evaluate schema-2 census JSON")
    group.add_argument("--selftest", action="store_true", help="Run positive & negative controls")
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()
    if args.write_fixture is not None:
        try:
            out = write_synthetic_fixture(args.write_fixture)
        except (OSError, InvalidInputError) as exc:
            print(exc, file=sys.stderr)
            return EXIT_INVALID
        print(f"wrote original synthetic fixture to {out}")
        return EXIT_PASS
    if args.reconcile_census is not None:
        try:
            data = read_json(args.reconcile_census)
            verdict = evaluate_census_completeness(data)
        except (InvalidInputError, ValueError) as exc:
            print(json.dumps({"status": "INVALID", "error": f"{args.reconcile_census}: {exc}"}, indent=2))
            return EXIT_INVALID
        print(json.dumps(verdict, indent=2, sort_keys=True))
        return EXIT_PASS if verdict["complete"] else EXIT_INCOMPLETE

    code, summary = verify_fixture_dir(args.verify_images or args.verify, images_only=args.verify_images is not None)
    print(json.dumps(summary, indent=2, sort_keys=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
