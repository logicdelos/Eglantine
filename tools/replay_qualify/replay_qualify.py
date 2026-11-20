#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Standalone replay envelope, effect reconciliation, and census verifier (Issue #5).

Standard-library only (no Pillow/numpy required). Uses an embedded pure-Python RGBA8 PNG
codec so the original synthetic replay fixture and all positive/negative controls run
portably on Windows and Linux without any game binary, save, key, firmware, or GPU.

Commands:
    python tools/replay_qualify/replay_qualify.py --write-fixture <dir>
    python tools/replay_qualify/replay_qualify.py --verify <dir> \\
        [--expected-effects <expected_effects.jsonl>] [--require-equivalent]
    python tools/replay_qualify/replay_qualify.py --verify-replay <game-free-witness-dir>
    python tools/replay_qualify/replay_qualify.py --compare-runs <dir-a> <dir-b> \\
        [--witness-only] [--expected-effects <expected_effects.jsonl>] [--require-equivalent]
    python tools/replay_qualify/replay_qualify.py --verify-images <replay-images-dir>
    python tools/replay_qualify/replay_qualify.py --reconcile-census <census.json>
    python tools/replay_qualify/replay_qualify.py --selftest

Event identity in a replay witness is (frame_id, ordinal, kind). Ordinals are zero-based in
recorded order and must be contiguous, unique and reproduced in that order. effects.jsonl is
the replay/observed ledger; expected_effects.jsonl is the independent live/recorder ledger.
Per frame, the stream FRAME_END effect_count (frames.jsonl) must equal both ledgers' row
counts, so deleting an effect from both sidecars is still rejected. Supported and fallback
effects require equal payload and outcome; a live-supported effect may be fallback in replay
with a reason, but not unsupported or unobservable. Unobservable and unsupported effects must
carry an explicit reason, never a fabricated payload/outcome; they are reported and the replay
is not called equivalent while any remain. --verify-replay requires manifest kind
standalone_replay_witness with zero guest calls per frame and no census.

Exit codes:
    0  PASS (inputs valid, images within envelope, every effect accounted and reconciled)
    1  FAIL (missing/extra/duplicate/reordered/mismatched effect, count/frame gap, or
             image envelope/checksum violation)
    2  INVALID (malformed/missing/extra inputs, schema or dimension mismatch)
    3  INCOMPLETE (census completeness gate failed, or --require-equivalent and opaque
                   effects mean replay equivalence is not established)
"""

from __future__ import annotations

import argparse
import binascii
import json
import shutil
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
FIXTURE_SCHEMA = 3
FRAME_SCHEMA = 2
EFFECT_SCHEMA = 1
OPAQUE_CLASSIFICATIONS = frozenset({"unsupported", "unobservable"})
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


def canonical(value: Any) -> str:
    """Strict JSON form so 1 and true never compare equal in a payload."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def optional(row: dict, key: str, cls: type, where: str) -> Any:
    value = row.get(key)
    if value is None:
        return None
    if type(value) is not cls:
        raise InvalidInputError(f"{where}.{key}: expected {cls.__name__}, got {value!r}")
    return value


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


def effects_for_frame(
    frame_id: int, width: int, height: int
) -> list[tuple[str, str, dict | None, str | None, str | None]]:
    """Declared ordered effects for one synthetic frame, independent of either writer."""
    syncpoint = frame_id * 10
    fence = frame_id * 100
    return [
        ("submit", "supported",
         {"channel": 1, "pushbuffer_words": 128}, "queued on channel 1", None),
        ("consume", "supported",
         {"channel": 1, "consumed_words": 128}, "pushbuffer consumed", None),
        ("draw", "supported",
         {"primitive": "triangles", "vertices": 64, "indexed": True}, "draw executed", None),
        ("compute_dispatch", "fallback",
         {"groups": [4, 2, 1], "path": "legacy launch register"}, "dispatch executed",
         "native compute routed through the legacy launch register"),
        ("syncpoint_increment", "supported",
         {"syncpoint": syncpoint, "value": syncpoint}, "syncpoint incremented", None),
        ("fence_signal", "supported",
         {"fence": fence}, "fence signaled", None),
        ("readback_visibility", "unobservable", None, None,
         "transient scratch is unmapped before any readback"),
        ("draw", "unsupported", None, None,
         "debug marker method is retired as a no-op in the replay renderer"),
        ("present_compose", "supported",
         {"format": "RGBA8", "width": width, "height": height}, "frame composed", None),
    ]


def effect_row(
    frame_id: int,
    ordinal: int,
    kind: str,
    classification: str,
    payload: dict | None,
    outcome: str | None,
    reason: str | None,
) -> dict[str, Any]:
    row = {
        "schema": EFFECT_SCHEMA,
        "frame_id": frame_id,
        "ordinal": ordinal,
        "kind": kind,
        "classification": classification,
    }
    if payload is not None:
        row["payload"] = payload
    if outcome is not None:
        row["outcome"] = outcome
    if reason is not None:
        row["reason"] = reason
    return row


def write_synthetic_fixture(out_dir: Path, width: int = 32, height: int = 32) -> Path:
    """Write a complete, redistributable original replay + census fixture into out_dir."""
    outputs = {}
    frame_ids = [1, 2]
    frame_lines: list[str] = []
    expected_lines: list[str] = []
    observed_lines: list[str] = []

    for frame_id in frame_ids:
        pixels = make_synthetic_rgba(width, height, frame_id)
        checksum = picture_checksum(pixels)
        live_name = f"live_{frame_id:08d}.png"
        replay_name = f"replay_{frame_id:08d}.png"
        png_bytes = encode_rgba_png(width, height, pixels)
        outputs[live_name] = outputs[replay_name] = png_bytes

        effects = effects_for_frame(frame_id, width, height)
        for ordinal, (kind, cls_name, payload, outcome, reason) in enumerate(effects):
            row = effect_row(frame_id, ordinal, kind, cls_name, payload, outcome, reason)
            line = json.dumps(row, sort_keys=True)
            expected_lines.append(line)
            # The replay witness reproduces the same identity, classification and evidence.
            observed_lines.append(line)

        frame_lines.append(
            json.dumps(
                {
                    "schema": FRAME_SCHEMA,
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
                    "call_count": 12 + frame_id,
                    "effect_count": len(effects),
                },
                sort_keys=True,
            )
        )

    outputs["frames.jsonl"] = ("\n".join(frame_lines) + "\n").encode()
    outputs["expected_effects.jsonl"] = ("\n".join(expected_lines) + "\n").encode()
    outputs["effects.jsonl"] = ("\n".join(observed_lines) + "\n").encode()

    manifest = {
        "schema": FIXTURE_SCHEMA,
        "kind": "standalone_replay_qualification_fixture",
        "origin": "synthetic_procedural_original",
        "envelope": IMAGE_LIMITS,
        "effects_schema": EFFECT_SCHEMA,
        "event_identity": "frame_id/ordinal/kind",
        "expected_frame_ids": frame_ids,
        "expected_frames": len(frame_ids),
        "runner_invocation": (
            "eden-cli --replay <capture.egcap> --replay-images <out_dir> --report <report.json>"
        ),
        "census_invocation": (
            "eden-cli --baseline -g <owned_title_path> --nvn-census <census.json> "
            "--nvn-census-audit --run-seconds 300 --report <report.json>"
        ),
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


def read_ledger(path: Path) -> list[tuple[str, dict]]:
    """Read one effect ledger; reject unknown schema, kind or classification."""
    rows: list[tuple[str, dict]] = []
    for where, row in read_records(path):
        if field(row, "schema", int, where) != EFFECT_SCHEMA:
            raise InvalidInputError(
                f"{where}: expected effect ledger schema {EFFECT_SCHEMA}; regenerate old sidecars"
            )
        field(row, "frame_id", int, where)
        field(row, "ordinal", int, where)
        kind = field(row, "kind", str, where)
        cls_name = field(row, "classification", str, where)
        if kind not in VALID_EFFECT_KINDS:
            raise InvalidInputError(f"{where}: unknown effect kind {kind!r}")
        if cls_name not in VALID_CLASSIFICATIONS:
            raise InvalidInputError(f"{where}: unknown classification {cls_name!r}")
        optional(row, "payload", dict, where)
        optional(row, "outcome", str, where)
        optional(row, "reason", str, where)
        rows.append((where, row))
    return rows


def verify_frames(
    root: Path,
    envelope: dict,
    *,
    counts_required: bool = True,
    expected_ids: list[int] | None = None,
) -> tuple[dict[int, dict], list[str]]:
    frames: dict[int, dict] = {}
    failures: list[str] = []
    listed = set()
    previous = -1
    for where, rec in read_records(root / "frames.jsonl"):
        schema = field(rec, "schema", int, where)
        if counts_required and schema != FRAME_SCHEMA:
            raise InvalidInputError(
                f"{where}: expected replay frame schema {FRAME_SCHEMA} with stream call/effect counts"
            )
        if schema not in (1, FRAME_SCHEMA):
            raise InvalidInputError(f"{where}: unsupported replay frame schema {schema}")
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
        call_count = effect_count = None
        if counts_required:
            call_count = field(rec, "call_count", int, where)
            effect_count = field(rec, "effect_count", int, where)
        frames[fid] = {
            "frame_id": fid,
            "image": metrics,
            "call_count": call_count,
            "effect_count": effect_count,
            "passed": len(failures) == before,
        }
    if not frames:
        raise InvalidInputError("frames.jsonl is empty")
    if expected_ids is not None:
        missing = [fid for fid in expected_ids if fid not in frames]
        extra = [fid for fid in sorted(frames) if fid not in expected_ids]
        if missing:
            failures.append(f"frames.jsonl: missing declared frame(s) {missing}")
        if extra:
            failures.append(f"frames.jsonl: undeclared frame(s) {extra}")
        for fid in missing + extra:
            if fid in frames:
                frames[fid]["passed"] = False
    for p in root.glob("*.png"):
        if p.name not in listed:
            raise InvalidInputError(f"unlisted PNG: {p.name}")
    return frames, failures


def compare_effect_pair(fid: int, expected: dict, observed: dict) -> list[str]:
    ident = f"frame {fid} effect #{expected['ordinal']} {expected['kind']}"
    failures: list[str] = []
    expected_cls = expected["classification"]
    observed_cls = observed["classification"]
    live_supported = expected_cls == "supported"
    replay_fallback = observed_cls == "fallback"
    if expected_cls != observed_cls and not (live_supported and replay_fallback):
        return [f"{ident}: classification {expected_cls} != observed {observed_cls}"]

    expected_payload = expected.get("payload")
    observed_payload = observed.get("payload")
    expected_outcome = expected.get("outcome")
    observed_outcome = observed.get("outcome")
    if observed_cls in OPAQUE_CLASSIFICATIONS:
        if (expected_payload is not None or observed_payload is not None
                or expected_outcome is not None or observed_outcome is not None):
            failures.append(f"{ident}: {observed_cls} effect must not fabricate a payload/outcome")
        if not (expected.get("reason") or "").strip():
            failures.append(f"{ident}: expected {observed_cls} effect has no reason")
        if not (observed.get("reason") or "").strip():
            failures.append(f"{ident}: observed {observed_cls} effect has no reason")
        return failures

    if expected_payload is None or expected_outcome is None:
        failures.append(f"{ident}: expected {expected_cls} effect declares no payload/outcome")
    if observed_payload is None or observed_outcome is None:
        failures.append(f"{ident}: observed {observed_cls} effect declares no payload/outcome")
    if (expected_payload is not None and observed_payload is not None
            and canonical(expected_payload) != canonical(observed_payload)):
        failures.append(
            f"{ident}: payload mismatch expected={canonical(expected_payload)} "
            f"observed={canonical(observed_payload)}"
        )
    if (expected_outcome is not None and observed_outcome is not None
            and expected_outcome != observed_outcome):
        failures.append(
            f"{ident}: outcome mismatch expected={expected_outcome!r} observed={observed_outcome!r}"
        )
    if expected_cls == "fallback" and not (expected.get("reason") or "").strip():
        failures.append(f"{ident}: expected fallback effect requires a reason")
    if observed_cls == "fallback" and not (observed.get("reason") or "").strip():
        failures.append(f"{ident}: observed fallback effect requires a reason")
    return failures


def verify_effect_ledgers(
    expected_path: Path, observed_path: Path, frames: dict[int, dict]
) -> tuple[dict[str, Any], list[str], bool, list[str]]:
    """Reconcile ordered live/expected against replay/observed effects per frame."""
    expected_rows = read_ledger(expected_path)
    observed_rows = read_ledger(observed_path)

    counts = {cls: {"expected": 0, "observed": 0} for cls in sorted(VALID_CLASSIFICATIONS)}
    frame_failures: dict[int, list[str]] = {fid: [] for fid in frames}
    failures: list[str] = []

    def index(
        path: Path, rows: list[tuple[str, dict]]
    ) -> tuple[dict[int, list[tuple[str, dict]]], list[str]]:
        indexed: dict[int, list[tuple[str, dict]]] = {fid: [] for fid in frames}
        seen_pairs: set[tuple[int, int]] = set()
        seen_triples: set[tuple[int, int, str]] = set()
        problems: list[str] = []
        previous: tuple[int, int] | None = None
        for where, row in rows:
            fid = row["frame_id"]
            ordinal = row["ordinal"]
            kind = row["kind"]
            if fid not in indexed:
                raise InvalidInputError(f"{where}: frame_id {fid} is not in frames.jsonl")
            pair = (fid, ordinal)
            triple = (fid, ordinal, kind)
            if pair in seen_pairs:
                problems.append(
                    f"{where}: duplicate effect identity frame={fid} ordinal={ordinal}"
                )
            elif triple in seen_triples:
                problems.append(
                    f"{where}: duplicate effect kind at frame={fid} ordinal={ordinal} kind={kind}"
                )
            if previous is not None and pair < previous:
                problems.append(
                    f"{where}: ledger row frame={fid} ordinal={ordinal} after "
                    f"frame={previous[0]} ordinal={previous[1]}"
                )
            seen_pairs.add(pair)
            seen_triples.add(triple)
            previous = pair
            indexed[fid].append((where, row))
        return indexed, problems

    expected, expected_problems = index(expected_path, expected_rows)
    observed, observed_problems = index(observed_path, observed_rows)
    all_ids = set(frames)
    for name, path, indexed, problems in (
        ("expected", expected_path, expected, expected_problems),
        ("observed", observed_path, observed, observed_problems),
    ):
        missing_frames = [fid for fid in sorted(all_ids) if not indexed[fid]]
        for fid in missing_frames:
            message = f"{path.name}: frame {fid}: no {name} effect rows"
            frame_failures[fid].append(message)
            failures.append(message)
        for problem in problems:
            failures.append(problem)

    equivalence_reasons: list[str] = []
    for fid in sorted(frames):
        expected_seq = expected[fid]
        observed_seq = observed[fid]
        declared = frames[fid]["effect_count"]
        if declared is not None:
            if declared != len(expected_seq):
                message = (
                    f"frame {fid}: stream effect_count={declared} but expected ledger has "
                    f"{len(expected_seq)} (silent omission)"
                )
                frame_failures[fid].append(message)
                failures.append(message)
            if declared != len(observed_seq):
                message = (
                    f"frame {fid}: stream effect_count={declared} but observed ledger has "
                    f"{len(observed_seq)} (missing or extra effect)"
                )
                frame_failures[fid].append(message)
                failures.append(message)

        for side, seq in (("expected", expected_seq), ("observed", observed_seq)):
            ordinals = [row["ordinal"] for _, row in seq]
            if ordinals != list(range(len(ordinals))):
                message = f"{side} ledger frame {fid}: ordinals {ordinals} are not contiguous from 0"
                frame_failures[fid].append(message)
                failures.append(message)
            for _, row in seq:
                counts[row["classification"]][side] += 1

        expected_ids = [(row["ordinal"], row["kind"]) for _, row in expected_seq]
        observed_ids = [(row["ordinal"], row["kind"]) for _, row in observed_seq]
        if expected_ids != observed_ids:
            missing = [item for item in expected_ids if item not in observed_ids]
            extra = [item for item in observed_ids if item not in expected_ids]
            if missing:
                message = f"frame {fid}: missing effects {missing}"
                frame_failures[fid].append(message)
                failures.append(message)
            if extra:
                message = f"frame {fid}: extra effects {extra}"
                frame_failures[fid].append(message)
                failures.append(message)
            if not missing and not extra:
                message = f"frame {fid}: effects reordered"
                frame_failures[fid].append(message)
                failures.append(message)
        else:
            for (_, expected_row), (_, observed_row) in zip(expected_seq, observed_seq):
                pair_failures = compare_effect_pair(fid, expected_row, observed_row)
                if pair_failures:
                    frame_failures[fid].extend(pair_failures)
                    failures.extend(pair_failures)

        for _, row in expected_seq:
            if row["classification"] in OPAQUE_CLASSIFICATIONS:
                reason = (row.get("reason") or "").strip()
                equivalence_reasons.append(
                    f"frame {fid} effect #{row['ordinal']} {row['kind']}: "
                    f"{row['classification']} ({reason or 'no reason'})"
                )

    for fid, problems in frame_failures.items():
        if problems:
            frames[fid]["passed"] = False

    equivalent = (
        not failures
        and counts["unsupported"]["expected"] == 0
        and counts["unobservable"]["expected"] == 0
    )
    report = {
        "total_expected": len(expected_rows),
        "total_observed": len(observed_rows),
        "by_classification": counts,
        "replay_equivalent": equivalent,
        "reconciled": not failures,
    }
    return report, failures, equivalent, equivalence_reasons


def verify_fixture_dir(
    fixture_dir: Path,
    *,
    images_only: bool = False,
    expected_effects: Path | None = None,
    require_equivalent: bool = False,
    witness_only: bool = False,
) -> tuple[int, dict[str, Any]]:
    """Raw replay image evidence alone is not an effect ledger or census certificate."""
    try:
        envelope = IMAGE_LIMITS
        expected_ids = None
        expected_kind = (
            "standalone_replay_witness" if witness_only
            else "standalone_replay_qualification_fixture"
        )
        if not images_only:
            manifest = read_json(fixture_dir / "manifest.json")
            if (field(manifest, "schema", int, "manifest.json") != FIXTURE_SCHEMA or
                    manifest.get("kind") != expected_kind):
                raise InvalidInputError(
                    f"manifest.json: expected qualification manifest schema {FIXTURE_SCHEMA} "
                    f"kind {expected_kind!r}; regenerate old fixtures"
                )
            envelope = field(manifest, "envelope", dict, "manifest.json")
            for key, limit in IMAGE_LIMITS.items():
                value = envelope.get(key)
                if (type(value) not in (int, float) or not 0 <= value <= limit
                        or (type(limit) is int and type(value) is not int)):
                    raise InvalidInputError(f"manifest.json: {key}={value!r} outside allowed 0..{limit}")
            expected_ids = field(manifest, "expected_frame_ids", list, "manifest.json")
            if (not expected_ids or not all(type(fid) is int and fid >= 0 for fid in expected_ids)
                    or expected_ids != sorted(set(expected_ids))):
                raise InvalidInputError(
                    "manifest.json: expected_frame_ids must be unique ascending integers"
                )
            if field(manifest, "expected_frames", int, "manifest.json") != len(expected_ids):
                raise InvalidInputError("manifest.json: expected_frames disagrees with expected_frame_ids")
        frames, failures = verify_frames(
            fixture_dir,
            envelope,
            counts_required=not images_only,
            expected_ids=expected_ids,
        )
        if witness_only:
            for fid in sorted(frames):
                if frames[fid]["call_count"] != 0:
                    message = (
                        f"frame {fid}: game-free replay declares call_count="
                        f"{frames[fid]['call_count']}, expected 0"
                    )
                    failures.append(message)
                    frames[fid]["passed"] = False
        result: dict[str, Any] = {
            "scope": "images" if images_only else ("replay_witness" if witness_only else "fixture"),
            "frames": list(frames.values()),
            "failures": failures,
        }
        code = EXIT_FAIL if failures else EXIT_PASS
        if not images_only:
            observed_path = fixture_dir / "effects.jsonl"
            if not observed_path.is_file():
                alternative = fixture_dir / "observed_effects.jsonl"
                if alternative.is_file():
                    observed_path = alternative
            if not observed_path.is_file():
                raise InvalidInputError(
                    f"{fixture_dir}/effects.jsonl: missing observed (replay) effect ledger"
                )
            expected_path = expected_effects or (fixture_dir / "expected_effects.jsonl")
            if not expected_path.is_file():
                raise InvalidInputError(
                    f"{expected_path}: missing independent expected (live) effect ledger; "
                    "pass --expected-effects"
                )
            effects, bad_effects, equivalent, equivalence_reasons = verify_effect_ledgers(
                Path(expected_path), observed_path, frames
            )
            failures.extend(bad_effects)
            result["effect_reconciliation"] = effects
            result["replay_equivalent"] = equivalent
            result["equivalence_reasons"] = equivalence_reasons[:64]
            census_complete = True
            if witness_only:
                result["census"] = {
                    "scope": "game_free_replay_no_guest_census",
                    "required": False,
                }
            else:
                census = evaluate_census_completeness(read_json(fixture_dir / "census.json"))
                result["census"] = census
                census_complete = census["complete"]
            if failures:
                code = EXIT_FAIL
            elif not census_complete or (require_equivalent and not equivalent):
                code = EXIT_INCOMPLETE
            else:
                code = EXIT_PASS
        result.update(
            exit_code=code,
            status={0: "PASS", 1: "FAIL", 2: "INVALID", 3: "INCOMPLETE"}[code],
        )
        return code, result
    except (InvalidInputError, OSError, ValueError) as exc:
        return EXIT_INVALID, {"status": "INVALID", "exit_code": EXIT_INVALID, "failures": [str(exc)]}


def observed_digest(run_dir: Path) -> list[str]:
    path = run_dir / "effects.jsonl"
    if not path.is_file():
        path = run_dir / "observed_effects.jsonl"
    rows = read_ledger(path)
    keys = ("frame_id", "ordinal", "kind", "classification", "payload", "outcome", "reason")
    return [canonical({key: row[key] for key in keys if key in row}) for _, row in rows]


def compare_runs(
    run_a: Path,
    run_b: Path,
    *,
    expected_effects: Path | None = None,
    require_equivalent: bool = False,
    witness_only: bool = False,
) -> tuple[int, dict[str, Any]]:
    """Two standalone replays are identical only if every witness and image agrees."""
    code_a, result_a = verify_fixture_dir(
        run_a,
        expected_effects=expected_effects,
        require_equivalent=require_equivalent,
        witness_only=witness_only,
    )
    code_b, result_b = verify_fixture_dir(
        run_b,
        expected_effects=expected_effects,
        require_equivalent=require_equivalent,
        witness_only=witness_only,
    )
    if code_a == EXIT_INVALID or code_b == EXIT_INVALID:
        return EXIT_INVALID, {
            "status": "INVALID",
            "exit_code": EXIT_INVALID,
            "runs": {"a": result_a, "b": result_b},
            "failures": ["one or both replays are invalid; nothing to compare"],
        }

    divergences: list[str] = []
    frames_a = {frame["frame_id"]: frame for frame in result_a["frames"]}
    frames_b = {frame["frame_id"]: frame for frame in result_b["frames"]}
    if set(frames_a) != set(frames_b):
        divergences.append(
            f"frame set differs: a={sorted(frames_a)} b={sorted(frames_b)}"
        )
    for fid in sorted(set(frames_a) & set(frames_b)):
        for key in ("live_checksum", "replay_checksum"):
            if frames_a[fid]["image"][key] != frames_b[fid]["image"][key]:
                divergences.append(
                    f"frame {fid}: {key} {frames_a[fid]['image'][key]} != {frames_b[fid]['image'][key]}"
                )
    try:
        digest_a = observed_digest(run_a)
        digest_b = observed_digest(run_b)
    except (InvalidInputError, OSError, ValueError) as exc:
        return EXIT_INVALID, {
            "status": "INVALID",
            "exit_code": EXIT_INVALID,
            "runs": {"a": result_a, "b": result_b},
            "failures": [str(exc)],
        }
    if digest_a != digest_b:
        first = next(
            (i for i, (left, right) in enumerate(zip(digest_a, digest_b)) if left != right),
            min(len(digest_a), len(digest_b)),
        )
        divergences.append(
            f"observed effects differ at row {first} "
            f"(a rows={len(digest_a)}, b rows={len(digest_b)})"
        )

    failed = bool(divergences) or code_a == EXIT_FAIL or code_b == EXIT_FAIL
    if failed:
        code = EXIT_FAIL
    elif code_a == EXIT_INCOMPLETE or code_b == EXIT_INCOMPLETE:
        code = EXIT_INCOMPLETE
    else:
        code = EXIT_PASS
    return code, {
        "status": {0: "PASS", 1: "FAIL", 3: "INCOMPLETE"}[code],
        "exit_code": code,
        "runs": {"a": result_a, "b": result_b},
        "divergences": divergences,
        "identical": code == EXIT_PASS,
    }


def run_selftest() -> int:
    """Run positive and intentional mismatch-rejection controls in a temporary directory."""
    checks: list[tuple[str, Any, Any]] = []

    def write_rows(path: Path, rows: list[dict]) -> None:
        path.write_text(
            "".join(json.dumps(row, sort_keys=True) + "\n" for row in rows), encoding="utf-8"
        )

    def ledger(scene: Path, name: str = "effects.jsonl") -> list[dict]:
        return [row for _, row in read_records(scene / name)]

    def frame_rows(scene: Path) -> list[dict]:
        return [row for _, row in read_records(scene / "frames.jsonl")]

    with tempfile.TemporaryDirectory(prefix="replay_qualify_selftest_") as tmp:
        root = Path(tmp)

        # 1. Exact synthetic fixture -> PASS (0), and an unchanged regeneration is allowed.
        exact_dir = write_synthetic_fixture(root / "case_exact")
        write_synthetic_fixture(exact_dir)
        code, result = verify_fixture_dir(exact_dir)
        checks.append(("exact_fixture_pass", EXIT_PASS, code))

        # 2. Every classification is counted; opaque effects are not called equivalence.
        counts = result["effect_reconciliation"]["by_classification"]
        checks.append(("classification_counts", {
            "supported": 12, "fallback": 2, "unsupported": 2, "unobservable": 2,
        }, {name: counts[name]["expected"] for name in counts}))
        checks.append(("opaque_not_equivalent", False, result["replay_equivalent"]))
        checks.append(("opaque_reasons_reported", True, len(result["equivalence_reasons"]) == 4))
        code, _ = verify_fixture_dir(exact_dir, require_equivalent=True)
        checks.append(("require_equivalent_incomplete", EXIT_INCOMPLETE, code))

        # 3. Within-envelope 1-LSB perturbation passes only with updated checksums.
        env_dir = write_synthetic_fixture(root / "case_within_envelope")
        w, h, px = decode_rgba_png(env_dir / "replay_00000001.png")
        mutated = bytearray(px)
        mutated[0] = mutated[0] + 1 if mutated[0] < 255 else 254
        (env_dir / "replay_00000001.png").write_bytes(encode_rgba_png(w, h, bytes(mutated)))
        code, _ = verify_fixture_dir(env_dir)
        checks.append(("stale_checksum_reject", EXIT_FAIL, code))
        rows = frame_rows(env_dir)
        rows[0].update(replay_checksum=picture_checksum(mutated), identical=False)
        write_rows(env_dir / "frames.jsonl", rows)
        code, _ = verify_fixture_dir(env_dir)
        checks.append(("within_envelope_1lsb_pass", EXIT_PASS, code))

        # 4. Pixel mismatch exceeding envelope -> FAIL (1)
        bad_px_dir = write_synthetic_fixture(root / "case_bad_pixels")
        w, h, px = decode_rgba_png(bad_px_dir / "replay_00000002.png")
        mutated = bytearray(px)
        for i in range(0, len(mutated), 4):
            mutated[i] = (mutated[i] + 40) & 0xFF
        (bad_px_dir / "replay_00000002.png").write_bytes(encode_rgba_png(w, h, bytes(mutated)))
        rows = frame_rows(bad_px_dir)
        rows[1].update(replay_checksum=picture_checksum(mutated), identical=False)
        write_rows(bad_px_dir / "frames.jsonl", rows)
        code, _ = verify_fixture_dir(bad_px_dir)
        checks.append(("pixel_envelope_reject", EXIT_FAIL, code))

        # 5. Dimension mismatch -> INVALID (2)
        bad_dim_dir = write_synthetic_fixture(root / "case_bad_dim")
        (bad_dim_dir / "replay_00000001.png").write_bytes(
            encode_rgba_png(16, 16, make_synthetic_rgba(16, 16, 1))
        )
        code, _ = verify_fixture_dir(bad_dim_dir)
        checks.append(("dimension_mismatch_reject", EXIT_INVALID, code))

        # 6. Non-zero semantic output mismatch -> FAIL (1)
        bad_sem_dir = write_synthetic_fixture(root / "case_bad_semantic")
        rows = frame_rows(bad_sem_dir)
        rows[0]["semantic_mismatches"] = 2
        write_rows(bad_sem_dir / "frames.jsonl", rows)
        code, _ = verify_fixture_dir(bad_sem_dir)
        checks.append(("semantic_mismatch_reject", EXIT_FAIL, code))

        # 7. Missing frame declared by the manifest -> FAIL (1)
        missing_frame = write_synthetic_fixture(root / "case_missing_frame")
        write_rows(missing_frame / "frames.jsonl",
                   [r for r in frame_rows(missing_frame) if r["frame_id"] != 2])
        for side in ("live", "replay"):
            (missing_frame / f"{side}_00000002.png").unlink()
        for name in ("expected_effects.jsonl", "effects.jsonl"):
            write_rows(missing_frame / name,
                       [r for r in ledger(missing_frame, name) if r["frame_id"] != 2])
        code, result = verify_fixture_dir(missing_frame)
        checks.append(("missing_frame_reject", EXIT_FAIL, code))

        # 8. Missing observed effect -> FAIL (1)
        missing_eff = write_synthetic_fixture(root / "case_missing_effect")
        rows = ledger(missing_eff)
        rows.pop(2)
        write_rows(missing_eff / "effects.jsonl", rows)
        code, result = verify_fixture_dir(missing_eff)
        checks.append(("missing_effect_reject", EXIT_FAIL, code))
        checks.append(("missing_effect_unreconciled", False,
                       result["effect_reconciliation"]["reconciled"]))

        # 9. Extra observed effect -> FAIL (1)
        extra_eff = write_synthetic_fixture(root / "case_extra_effect")
        rows = ledger(extra_eff)
        rows.append(effect_row(1, 9, "draw", "supported", {"extra": True}, "executed", None))
        write_rows(extra_eff / "effects.jsonl", rows)
        code, _ = verify_fixture_dir(extra_eff)
        checks.append(("extra_effect_reject", EXIT_FAIL, code))

        # 10. Duplicate ordinal / identity -> FAIL (1)
        dup_eff = write_synthetic_fixture(root / "case_duplicate")
        rows = ledger(dup_eff)
        rows[2]["ordinal"] = 1
        write_rows(dup_eff / "effects.jsonl", rows)
        code, _ = verify_fixture_dir(dup_eff)
        checks.append(("duplicate_ordinal_reject", EXIT_FAIL, code))

        # 11. Reordered observed effects -> FAIL (1)
        reorder = write_synthetic_fixture(root / "case_reorder")
        rows = ledger(reorder)
        rows[2], rows[3] = rows[3], rows[2]
        write_rows(reorder / "effects.jsonl", rows)
        code, _ = verify_fixture_dir(reorder)
        checks.append(("reordered_effect_reject", EXIT_FAIL, code))

        # 12. Mismatched payload, outcome and classification -> FAIL (1)
        for label, change in (
            ("payload", lambda r: r[0]["payload"].update(pushbuffer_words=129)),
            ("outcome", lambda r: r[1].update(outcome="dropped")),
            ("classification", lambda r: r[3].update(classification="supported")),
        ):
            scene = write_synthetic_fixture(root / f"case_mismatch_{label}")
            rows = ledger(scene)
            change(rows)
            write_rows(scene / "effects.jsonl", rows)
            code, _ = verify_fixture_dir(scene)
            checks.append((f"mismatched_{label}_reject", EXIT_FAIL, code))

        # 13. Deleted from BOTH sidecars, caught by the stream's declared effect_count.
        both_deleted = write_synthetic_fixture(root / "case_both_deleted")
        for name in ("expected_effects.jsonl", "effects.jsonl"):
            rows = ledger(both_deleted, name)
            rows.pop(2)
            write_rows(both_deleted / name, rows)
        code, result = verify_fixture_dir(both_deleted)
        checks.append(("both_sidecars_deleted_reject", EXIT_FAIL, code))
        checks.append(("stream_count_caught_deletion", True,
                       any("silent omission" in failure or "effect_count" in failure
                           for failure in result["failures"])))

        # 14. Stream count that disagrees with intact ledgers -> FAIL (1)
        bad_count = write_synthetic_fixture(root / "case_bad_count")
        rows = frame_rows(bad_count)
        rows[0]["effect_count"] += 1
        write_rows(bad_count / "frames.jsonl", rows)
        code, _ = verify_fixture_dir(bad_count)
        checks.append(("stream_count_mismatch_reject", EXIT_FAIL, code))

        # 15. Expected ledger missing a whole frame -> FAIL (1)
        bad_expected = write_synthetic_fixture(root / "case_expected_frame")
        write_rows(bad_expected / "expected_effects.jsonl",
                   [r for r in ledger(bad_expected, "expected_effects.jsonl") if r["frame_id"] != 2])
        code, _ = verify_fixture_dir(bad_expected)
        checks.append(("expected_ledger_missing_frame_reject", EXIT_FAIL, code))

        # 16. Opaque effect without a reason or with a fabricated payload -> FAIL (1)
        for label, change in (
            ("no_reason", lambda r: (r[6].pop("reason"), r[6])),
            ("fabricated", lambda r: (r[6].update(payload={"guessed": True}), r[6])),
        ):
            scene = write_synthetic_fixture(root / f"case_opaque_{label}")
            rows = ledger(scene)
            change(rows)
            write_rows(scene / "effects.jsonl", rows)
            code, _ = verify_fixture_dir(scene)
            checks.append((f"opaque_{label}_reject", EXIT_FAIL, code))

        # 16b. Live-supported effect may take a documented replay fallback path.
        fallback_path = write_synthetic_fixture(root / "case_fallback_path")
        rows = ledger(fallback_path, "expected_effects.jsonl")
        rows[3]["classification"] = "supported"
        write_rows(fallback_path / "expected_effects.jsonl", rows)
        code, _ = verify_fixture_dir(fallback_path)
        checks.append(("live_supported_replay_fallback_pass", EXIT_PASS, code))
        rows = ledger(fallback_path)
        rows[3].pop("reason", None)
        write_rows(fallback_path / "effects.jsonl", rows)
        code, _ = verify_fixture_dir(fallback_path)
        checks.append(("fallback_observed_no_reason_reject", EXIT_FAIL, code))

        # 16c. A live-observable effect may not become unobservable in replay.
        became_opaque = write_synthetic_fixture(root / "case_became_opaque")
        rows = ledger(became_opaque)
        rows[0]["classification"] = "unobservable"
        rows[0].pop("payload", None)
        rows[0].pop("outcome", None)
        rows[0]["reason"] = "replay could not observe the submission"
        write_rows(became_opaque / "effects.jsonl", rows)
        code, _ = verify_fixture_dir(became_opaque)
        checks.append(("observable_to_unobservable_reject", EXIT_FAIL, code))

        # 17. Orphan effect row referencing an undeclared frame -> INVALID (2)
        orphan_dir = write_synthetic_fixture(root / "case_orphan_effect")
        with (orphan_dir / "effects.jsonl").open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(
                effect_row(999, 0, "draw", "supported", {"vertices": 3}, "drawn", None),
                sort_keys=True,
            ) + "\n")
        code, _ = verify_fixture_dir(orphan_dir)
        checks.append(("orphan_effect_frame_reject", EXIT_INVALID, code))

        # 18. Missing stream call_count or stale manifest schema -> INVALID (2)
        no_calls = write_synthetic_fixture(root / "case_no_call_count")
        rows = frame_rows(no_calls)
        rows[0].pop("call_count")
        write_rows(no_calls / "frames.jsonl", rows)
        code, _ = verify_fixture_dir(no_calls)
        checks.append(("missing_stream_count_invalid", EXIT_INVALID, code))
        old_manifest = write_synthetic_fixture(root / "case_old_manifest")
        manifest = read_json(old_manifest / "manifest.json")
        manifest["schema"] = 2
        (old_manifest / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        code, _ = verify_fixture_dir(old_manifest)
        checks.append(("old_manifest_schema_reject", EXIT_INVALID, code))

        # 19. Incomplete census controls.
        for label, change, expected in (
            ("lost_probe", lambda c: c["errors"].update(probe_table_full=1), EXIT_INCOMPLETE),
            ("unclean_stop", lambda c: c["stop"].update(clean=False), EXIT_INCOMPLETE),
            ("mixed_stream", lambda c: c["frames"]["streams"][0].update(tokens=2), EXIT_INCOMPLETE),
            ("wrong_counter_type", lambda c: c["audit"].update(known_entries="12"), EXIT_INVALID),
            ("capture_independent", lambda c: c["replay_capture"].update(failed=True), EXIT_PASS),
        ):
            scene = write_synthetic_fixture(root / label)
            census = read_json(scene / "census.json")
            change(census)
            (scene / "census.json").write_text(json.dumps(census), encoding="utf-8")
            code, _ = verify_fixture_dir(scene)
            checks.append((label, expected, code))

        # 20. Game-free replay witness: zero guest calls, no census, distinct kind.
        witness = write_synthetic_fixture(root / "case_witness")
        manifest = read_json(witness / "manifest.json")
        manifest["kind"] = "standalone_replay_witness"
        (witness / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        rows = frame_rows(witness)
        for row in rows:
            row["call_count"] = 0
        write_rows(witness / "frames.jsonl", rows)
        (witness / "census.json").unlink()
        code, result = verify_fixture_dir(witness, witness_only=True)
        checks.append(("game_free_witness_pass", EXIT_PASS, code))
        checks.append(("game_free_witness_census_scope", False, result["census"]["required"]))
        rows[0]["call_count"] = 1
        write_rows(witness / "frames.jsonl", rows)
        code, _ = verify_fixture_dir(witness, witness_only=True)
        checks.append(("game_free_witness_nonzero_calls_reject", EXIT_FAIL, code))
        code, _ = verify_fixture_dir(witness)
        checks.append(("witness_cannot_use_census_mode", EXIT_INVALID, code))

        # 20b. --require-equivalent passes only a witness with every effect observable.
        equivalent = write_synthetic_fixture(root / "case_witness_equivalent")
        manifest = read_json(equivalent / "manifest.json")
        manifest["kind"] = "standalone_replay_witness"
        (equivalent / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        for name in ("expected_effects.jsonl", "effects.jsonl"):
            kept = [row for row in ledger(equivalent, name) if row["ordinal"] not in (6, 7)]
            rows = []
            for fid in sorted({row["frame_id"] for row in kept}):
                for ordinal, row in enumerate(row for row in kept if row["frame_id"] == fid):
                    row["ordinal"] = ordinal
                    rows.append(row)
            write_rows(equivalent / name, rows)
        rows = frame_rows(equivalent)
        for row in rows:
            row["call_count"] = 0
            row["effect_count"] = 7
        write_rows(equivalent / "frames.jsonl", rows)
        (equivalent / "census.json").unlink()
        code, _ = verify_fixture_dir(equivalent, witness_only=True, require_equivalent=True)
        checks.append(("game_free_equivalent_pass", EXIT_PASS, code))

        opaque_witness = write_synthetic_fixture(root / "case_witness_opaque")
        manifest = read_json(opaque_witness / "manifest.json")
        manifest["kind"] = "standalone_replay_witness"
        (opaque_witness / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        rows = frame_rows(opaque_witness)
        for row in rows:
            row["call_count"] = 0
        write_rows(opaque_witness / "frames.jsonl", rows)
        (opaque_witness / "census.json").unlink()
        code, _ = verify_fixture_dir(opaque_witness, witness_only=True, require_equivalent=True)
        checks.append(("game_free_opaque_require_incomplete", EXIT_INCOMPLETE, code))

        # 21. Independent expected ledger supplied from the capture directory.
        external = write_synthetic_fixture(root / "case_external")
        external_expected = root / "capture_expected_effects.jsonl"
        shutil.copyfile(external / "expected_effects.jsonl", external_expected)
        (external / "expected_effects.jsonl").unlink()
        code, _ = verify_fixture_dir(external, expected_effects=external_expected)
        checks.append(("external_expected_ledger_pass", EXIT_PASS, code))
        code, _ = verify_fixture_dir(external)
        checks.append(("missing_expected_ledger_invalid", EXIT_INVALID, code))

        # 22. Two replays must agree on images and observed effects.
        run_a = write_synthetic_fixture(root / "case_run_a")
        run_b = root / "case_run_b"
        shutil.copytree(run_a, run_b)
        code, _ = compare_runs(run_a, run_b)
        checks.append(("dual_run_identical_pass", EXIT_PASS, code))
        rows = ledger(run_b)
        rows[3]["reason"] = "conflicting witness note"
        write_rows(run_b / "effects.jsonl", rows)
        code, _ = compare_runs(run_a, run_b)
        checks.append(("dual_run_ledger_divergence_reject", EXIT_FAIL, code))
        rows[3]["reason"] = ledger(run_a)[3].get("reason")
        write_rows(run_b / "effects.jsonl", rows)
        w, h, px = decode_rgba_png(run_b / "replay_00000001.png")
        mutated = bytearray(px)
        mutated[0] = mutated[0] + 1 if mutated[0] < 255 else 254
        (run_b / "replay_00000001.png").write_bytes(encode_rgba_png(w, h, bytes(mutated)))
        rows = frame_rows(run_b)
        rows[0].update(replay_checksum=picture_checksum(mutated), identical=False)
        write_rows(run_b / "frames.jsonl", rows)
        code, _ = compare_runs(run_a, run_b)
        checks.append(("dual_run_image_divergence_reject", EXIT_FAIL, code))

        # 23. The standalone runner emits images and semantics only.
        runner_dir = write_synthetic_fixture(root / "case_runner")
        for name in ("manifest.json", "effects.jsonl", "expected_effects.jsonl", "census.json"):
            (runner_dir / name).unlink()
        code, _ = verify_fixture_dir(runner_dir, images_only=True)
        checks.append(("runner_images_without_sidecars", EXIT_PASS, code))
        (runner_dir / "frames.jsonl").write_text("[]\n", encoding="utf-8")
        code, _ = verify_fixture_dir(runner_dir, images_only=True)
        checks.append(("malformed_runner_row", EXIT_INVALID, code))
        try:
            write_synthetic_fixture(runner_dir)
        except InvalidInputError:
            pass
        checks.append(("real_evidence_not_overwritten", True,
                       (runner_dir / "frames.jsonl").read_text(encoding="utf-8") == "[]\n"))

    failed = [(name, exp, got) for name, exp, got in checks if exp != got]
    for name, exp, got in checks:
        print(f"[selftest] {name}: expected={exp} actual={got} {'OK' if exp == got else 'FAIL'}")
    if failed:
        return 1
    print(f"selftest ok ({len(checks)}/{len(checks)} checks passed)")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write-fixture", type=Path, help="Write original synthetic fixture to DIR")
    group.add_argument("--verify", type=Path,
                       help="Verify qualification fixture in DIR (images + ledgers + census)")
    group.add_argument("--verify-replay", type=Path,
                       help="Verify a game-free replay witness in DIR (zero guest calls, no census)")
    group.add_argument("--compare-runs", type=Path, nargs=2, metavar=("RUN_A", "RUN_B"),
                       help="Verify two replay runs independently, then require identical witnesses")
    group.add_argument("--verify-images", type=Path,
                       help="Verify actual --replay-images output (images and semantics only)")
    group.add_argument("--reconcile-census", type=Path, help="Evaluate schema-2 census JSON")
    group.add_argument("--selftest", action="store_true", help="Run positive & negative controls")
    parser.add_argument("--expected-effects", type=Path,
                        help="Capture-side expected_effects.jsonl when it is not inside the run directory")
    parser.add_argument("--witness-only", action="store_true",
                        help="With --compare-runs, treat both runs as game-free replay witnesses")
    parser.add_argument("--require-equivalent", action="store_true",
                        help="Exit 3 unless every effect is observable and matched")
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
    if args.compare_runs is not None:
        code, summary = compare_runs(
            args.compare_runs[0],
            args.compare_runs[1],
            expected_effects=args.expected_effects,
            require_equivalent=args.require_equivalent,
            witness_only=args.witness_only,
        )
    elif args.verify_replay is not None:
        code, summary = verify_fixture_dir(
            args.verify_replay,
            expected_effects=args.expected_effects,
            require_equivalent=args.require_equivalent,
            witness_only=True,
        )
    else:
        code, summary = verify_fixture_dir(
            args.verify_images or args.verify,
            images_only=args.verify_images is not None,
            expected_effects=args.expected_effects,
            require_equivalent=args.require_equivalent,
        )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
