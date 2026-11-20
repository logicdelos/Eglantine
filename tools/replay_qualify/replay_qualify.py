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
        [--expected-effects <expected_effects.jsonl>] [--require-equivalent] \\
        [--capture <finalized.egcap> [--run-report <replay report.json>]]
    python tools/replay_qualify/replay_qualify.py --verify-replay <game-free-witness-dir> \\
        --capture <finalized.egcap> [--run-report <replay report.json>]
    python tools/replay_qualify/replay_qualify.py --compare-runs <dir-a> <dir-b> \\
        [--witness-only] [--expected-effects <expected_effects.jsonl>] [--require-equivalent] \\
        [--capture <finalized.egcap>]
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
TITLE_CENSUS_SCHEMA = 3
TITLE_CENSUS_FRAME_ID_BASIS = "closing_accepted_present"
CAPTURE_MAGIC = b"EGLCAP\x00\x00"
CAPTURE_FORMAT_MAJOR = 2
CAPTURE_HEADER_BYTES = 256
CAPTURE_CHUNK_HEADER_BYTES = 64
CAPTURE_INDEX_ENTRY_BYTES = 32
CAPTURE_FRAME_ENTRY_BYTES = 40
CAPTURE_MAX_BYTES = 16 << 30
CAPTURE_HEADER_COMPLETE = 1 << 0
CAPTURE_CHUNK_FRAME_END = 12
CAPTURE_CHUNK_INDEX = 13
CAPTURE_CHUNK_COMPLETE = 14
CAPTURE_KEEP_PAYLOAD = frozenset({CAPTURE_CHUNK_FRAME_END, CAPTURE_CHUNK_INDEX,
                                  CAPTURE_CHUNK_COMPLETE})
CAPTURE_FRAME_PREROLL = 1 << 1
CAPTURE_KNOWN_FRAME_FLAGS = 0x7
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
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
                    "call_count": 5,
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
        "schema": TITLE_CENSUS_SCHEMA,
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
        "frame_rows": [[frame_id, [[7, 5]]] for frame_id in frame_ids],
        "frame_id_basis": TITLE_CENSUS_FRAME_ID_BASIS,
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


def _capture_chunk(ctype: int, seq: int, frame_id: int, payload: bytes,
                   flags: int = 0) -> bytes:
    checksum = fnv1a64(payload)
    header = struct.pack(
        "<IHHQQQQQQQ", ctype, 1, flags, seq, frame_id, len(payload), len(payload), checksum, 0, 0)
    header = header[:56] + struct.pack("<Q", fnv1a64(header[:56]))
    return header + payload


def _capture_header(*, index_offset: int, chunk_count: int, file_length: int,
                    first_frame: int, requested: int, finalized: int,
                    preroll: int) -> bytes:
    header = bytearray(CAPTURE_HEADER_BYTES)
    header[:8] = CAPTURE_MAGIC
    struct.pack_into("<HHIQ", header, 8, CAPTURE_FORMAT_MAJOR, 0, CAPTURE_HEADER_BYTES, 1)
    struct.pack_into("<II", header, 32, CAPTURE_HEADER_COMPLETE, 1)
    struct.pack_into("<QQQ", header, 40, index_offset, chunk_count, file_length)
    struct.pack_into("<QIII", header, 64, first_frame, requested, finalized, preroll)
    struct.pack_into("<Q", header, 240, fnv1a64(bytes(header[:240])))
    return bytes(header)


def write_test_capture(path: Path, *, first_frame: int, preroll: int,
                       selected_ids: list[int], effect_counts: dict[int, int],
                       call_counts: dict[int, int] | None = None,
                       omit_last_frame: bool = False) -> dict:
    """Emit a minimal valid EGLCAP flow so verifier controls run without a GPU."""
    call_counts = call_counts or {}
    stored_ids = list(range(first_frame - preroll, first_frame + len(selected_ids)))
    emitted = stored_ids[:-1] if omit_last_frame else stored_ids
    body: list[bytes] = []
    frame_records = []
    sequence = 0
    body.append(_capture_chunk(1, sequence, 0, b""))
    sequence += 1
    for fid in emitted:
        flags = CAPTURE_FRAME_PREROLL if fid < first_frame else 0
        payload = struct.pack("<QIIQQQQQI", fid, flags, 0, 0, sequence, sequence, 0,
                              call_counts.get(fid, 0), effect_counts[fid])
        body.append(_capture_chunk(CAPTURE_CHUNK_FRAME_END, sequence, fid, payload))
        frame_records.append(struct.pack("<QQQIIQ", fid, sequence, sequence, flags, 0, 0))
        sequence += 1

    offsets = []
    offset = CAPTURE_HEADER_BYTES
    for chunk in body:
        offsets.append(offset)
        offset += len(chunk)
    index_payload = bytearray(struct.pack("<QQ", len(body), len(frame_records)))
    for chunk, chunk_offset in zip(body, offsets):
        ctype, _version, cflags = struct.unpack_from("<IHH", chunk, 0)
        seq, frame_id, stored = struct.unpack_from("<QQQ", chunk, 8)
        index_payload += struct.pack("<IHHQQQ", ctype, 1, cflags, chunk_offset, frame_id, stored)
    index_payload += b"".join(frame_records)
    index_offset = offset
    body.append(_capture_chunk(CAPTURE_CHUNK_INDEX, sequence, 0, bytes(index_payload)))
    sequence += 1
    chain = FNV_OFFSET
    for chunk in body[:-1]:
        checksum = struct.unpack_from("<Q", chunk, 56)[0]
        for shift in range(0, 64, 8):
            chain = ((chain ^ ((checksum >> shift) & 0xFF)) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    complete_payload = struct.pack(
        "<IIQQQQQQQQQIIIIII", 1, 0, chain, len(frame_records), len(body) + 1,
        CAPTURE_HEADER_BYTES + sum(len(chunk) for chunk in body) + 64 + 136,
        0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0)
    complete_payload += bytes(32)
    body.append(_capture_chunk(CAPTURE_CHUNK_COMPLETE, sequence, 0, complete_payload))
    total_size = CAPTURE_HEADER_BYTES + sum(len(chunk) for chunk in body)
    header = _capture_header(
        index_offset=index_offset, chunk_count=len(body), file_length=total_size,
        first_frame=first_frame, requested=len(selected_ids), finalized=len(selected_ids),
        preroll=preroll)
    path.write_bytes(header + b"".join(body))
    return read_capture(path)


def write_test_report(path: Path, capture: dict, *, frames_replayed: int | None = None,
                      guest_calls: int = 0, chain_digest: str | None = None) -> None:
    replay = {
        "status": "complete",
        "run_complete": True,
        "frames_expected": capture["finalized"],
        "frames_replayed": capture["finalized"] if frames_replayed is None else frames_replayed,
        "pictures_written": capture["finalized"],
        "preroll_frames_executed": capture["preroll_frames"],
        "replay_guest_call_count": guest_calls,
        "capture_chain_digest": chain_digest or f"{capture['chain_digest']:016x}",
        "error": "",
    }
    path.write_text(json.dumps({"replay": replay}, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


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
    if census.get("schema") != TITLE_CENSUS_SCHEMA or census.get("kind") != "nvn_call_census":
        raise InvalidInputError(
            f"expected schema={TITLE_CENSUS_SCHEMA} nvn_call_census document"
        )

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
         and shortfall == 0 and audited <= executed
         and shortfall == reported_shortfall,
         f"audited={audited} executed={executed} shortfall={shortfall} "
         f"reported={reported_shortfall}"),
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


def fnv1a64(data: bytes) -> int:
    digest = FNV_OFFSET
    for byte in data:
        digest = ((digest ^ byte) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return digest


def read_capture(path: Path) -> dict:
    """Validate a finalized EGLCAP v2 stream and return its trusted frame window.

    The verifier never trusts an adapter-side frames.jsonl window or a manifest count on its
    own: the selected and pre-roll frame ids and each frame's live call/effect counts come from
    this stream.
    """
    where = str(path)
    if path.name.endswith(".partial") or path.suffix != ".egcap":
        raise InvalidInputError(f"{where}: not a finalized .egcap capture")

    def fail(message: str) -> None:
        raise InvalidInputError(f"{where}: {message}")

    try:
        size = path.stat().st_size
    except OSError as exc:
        raise InvalidInputError(f"{where}: {exc}") from exc
    if size < CAPTURE_HEADER_BYTES + CAPTURE_CHUNK_HEADER_BYTES:
        fail("shorter than one header and one chunk")
    if size > CAPTURE_MAX_BYTES:
        fail(f"file length {size} exceeds the 16 GiB capture limit")

    with path.open("rb") as stream:
        def read_at(offset: int, length: int) -> bytes:
            stream.seek(offset)
            data = stream.read(length)
            if len(data) != length:
                fail(f"truncated at offset {offset} (wanted {length} bytes)")
            return data

        header = read_at(0, CAPTURE_HEADER_BYTES)
        if header[:8] != CAPTURE_MAGIC:
            fail("bad magic")
        major, _minor, header_length = struct.unpack_from("<HHI", header, 8)
        if major != CAPTURE_FORMAT_MAJOR:
            fail(f"format major {major} is not {CAPTURE_FORMAT_MAJOR}")
        if header_length != CAPTURE_HEADER_BYTES:
            fail(f"header length {header_length}")
        required_features = struct.unpack_from("<Q", header, 16)[0]
        if required_features & ~0x1:
            fail(f"unsupported required feature bits 0x{required_features & ~0x1:x}")
        flags, checksum_algorithm = struct.unpack_from("<II", header, 32)
        if checksum_algorithm != 1:
            fail(f"checksum algorithm {checksum_algorithm}")
        if flags & ~0x1F:
            fail(f"undefined header flag bits 0x{flags & ~0x1F:x}")
        if not flags & CAPTURE_HEADER_COMPLETE:
            fail("capture is not complete")
        index_offset, chunk_count, file_length = struct.unpack_from("<QQQ", header, 40)
        first_frame, requested, finalized, preroll = struct.unpack_from("<QIII", header, 64)
        if struct.unpack_from("<I", header, 84)[0]:
            fail("header reserved field is nonzero")
        if fnv1a64(header[:240]) != struct.unpack_from("<Q", header, 240)[0]:
            fail("header checksum mismatch")
        if struct.unpack_from("<Q", header, 248)[0]:
            fail("trailing reserved header field is nonzero")
        if file_length != size:
            fail(f"header file length {file_length} != on-disk {size}")
        if first_frame < 1:
            fail(f"first_frame {first_frame}")
        if not 1 <= requested <= 120:
            fail(f"requested_frames {requested}")
        if finalized > requested:
            fail(f"finalized_frames {finalized} > requested {requested}")
        if preroll > 2048 or preroll + finalized > 2048:
            fail(f"pre-roll {preroll} + selected {finalized} exceeds the frame bound")
        if first_frame - preroll < 1:
            fail(f"pre-roll starts below frame 1 ({first_frame} - {preroll})")
        if not CAPTURE_HEADER_BYTES <= index_offset < file_length:
            fail(f"index_offset {index_offset}")
        if not 2 <= chunk_count <= 1 << 20:
            fail(f"chunk_count {chunk_count}")

        chunks: list[dict] = []
        frame_ends: list[dict] = []
        frame_ranges: dict[int, list[int]] = {}
        digest = FNV_OFFSET
        offset = CAPTURE_HEADER_BYTES
        sequence = 0
        previous_frame = 0
        while offset < index_offset:
            head = read_at(offset, CAPTURE_CHUNK_HEADER_BYTES)
            ctype, version, cflags = struct.unpack_from("<IHH", head, 0)
            seq, frame_id, stored, uncompressed, payload_sum = struct.unpack_from(
                "<QQQQQ", head, 8)
            reserved, header_sum = struct.unpack_from("<QQ", head, 48)
            if header_sum != fnv1a64(head[:56]):
                fail(f"chunk at {offset}: header checksum mismatch")
            if cflags & ~0x3:
                fail(f"chunk at {offset}: undefined flag bits")
            if cflags & 0x2:
                fail(f"chunk at {offset}: compressed payloads are not supported")
            if reserved:
                fail(f"chunk at {offset}: reserved field is nonzero")
            if version != 1:
                fail(f"chunk at {offset}: payload version {version} is not 1")
            if seq != sequence:
                fail(f"chunk at {offset}: sequence {seq} != {sequence}")
            if frame_id < previous_frame:
                fail(f"chunk at {offset}: frame id {frame_id} after {previous_frame}")
            if stored > 64 << 20:
                fail(f"chunk at {offset}: stored length {stored} exceeds the chunk limit")
            if uncompressed != stored:
                fail(f"chunk at {offset}: compressed length {uncompressed} != {stored}")
            end = offset + CAPTURE_CHUNK_HEADER_BYTES + stored
            if end > index_offset:
                fail(f"chunk at {offset}: payload runs into the INDEX chunk")
            if ctype in CAPTURE_KEEP_PAYLOAD:
                payload = read_at(offset + CAPTURE_CHUNK_HEADER_BYTES, stored)
                if fnv1a64(payload) != payload_sum:
                    fail(f"chunk at {offset}: payload checksum mismatch")
                if ctype == CAPTURE_CHUNK_FRAME_END:
                    if stored < 60:
                        fail(f"FRAME_END at {offset}: payload {stored} bytes")
                    (end_frame, end_flags, output_count, gpu_words, first_seq, last_seq,
                     output_id, call_count, effect_count) = struct.unpack_from(
                        "<QIIQQQQQI", payload, 0)
                    if end_frame != frame_id:
                        fail(f"FRAME_END at {offset}: names frame {end_frame} in a chunk of "
                             f"{frame_id}")
                    if end_flags & ~CAPTURE_KNOWN_FRAME_FLAGS:
                        fail(f"FRAME_END {frame_id}: undefined flags 0x{end_flags:x}")
                    if bool(end_flags & CAPTURE_FRAME_PREROLL) != (frame_id < first_frame):
                        fail(f"FRAME_END {frame_id}: pre-roll flag disagrees with the window")
                    if last_seq < first_seq or last_seq > seq:
                        fail(f"FRAME_END {frame_id}: bad sequence range {first_seq}..{last_seq}")
                    if not first_frame - preroll <= frame_id < first_frame + finalized:
                        fail(f"FRAME_END {frame_id}: outside the declared window")
                    if frame_ends and frame_id != frame_ends[-1]["frame_id"] + 1:
                        fail(f"FRAME_END {frame_id}: frames are not gapless")
                    frame_ends.append({"frame_id": frame_id, "flags": end_flags,
                                       "first_seq": first_seq, "last_seq": last_seq,
                                       "output_id": output_id, "call_count": call_count,
                                       "effect_count": effect_count})
            chunks.append({"type": ctype, "version": version, "flags": cflags,
                           "offset": offset, "stored": stored, "frame_id": frame_id})
            if frame_id:
                previous_frame = frame_id
                if frame_id not in frame_ranges:
                    frame_ranges[frame_id] = [seq, seq]
                frame_ranges[frame_id][1] = seq
            for shift in range(0, 64, 8):
                digest = ((digest ^ ((header_sum >> shift) & 0xFF)) * FNV_PRIME) & \
                    0xFFFFFFFFFFFFFFFF
            offset = end
            sequence += 1

        if offset != index_offset:
            fail(f"chunk walk ended at {offset}, not the INDEX offset {index_offset}")

        index_head = read_at(index_offset, CAPTURE_CHUNK_HEADER_BYTES)
        ctype, version, cflags = struct.unpack_from("<IHH", index_head, 0)
        seq, frame_id, stored, uncompressed, payload_sum = struct.unpack_from(
            "<QQQQQ", index_head, 8)
        reserved, header_sum = struct.unpack_from("<QQ", index_head, 48)
        if ctype != CAPTURE_CHUNK_INDEX or frame_id != 0 or version != 1 or cflags != 0:
            fail("INDEX chunk header is wrong")
        if reserved or header_sum != fnv1a64(index_head[:56]):
            fail("INDEX chunk header checksum/reserved field is wrong")
        if seq != sequence:
            fail(f"INDEX sequence {seq} != {sequence}")
        if uncompressed != stored or stored > 64 << 20:
            fail("INDEX length fields are wrong")
        index_payload = read_at(index_offset + CAPTURE_CHUNK_HEADER_BYTES, stored)
        if fnv1a64(index_payload) != payload_sum:
            fail("INDEX payload checksum mismatch")
        if len(index_payload) < 16:
            fail("INDEX payload is truncated")
        entry_count, frame_count = struct.unpack_from("<QQ", index_payload, 0)
        if entry_count != len(chunks):
            fail(f"INDEX lists {entry_count} chunks, stream has {len(chunks)}")
        if frame_count != preroll + finalized:
            fail(f"INDEX lists {frame_count} frames, window declares {preroll + finalized}")
        expected_index_bytes = (16 + entry_count * CAPTURE_INDEX_ENTRY_BYTES +
                                frame_count * CAPTURE_FRAME_ENTRY_BYTES)
        if len(index_payload) != expected_index_bytes:
            fail(f"INDEX payload {len(index_payload)} != {expected_index_bytes}")
        cursor = 16
        for chunk in chunks:
            entry = struct.unpack_from("<IHHQQQ", index_payload, cursor)
            cursor += CAPTURE_INDEX_ENTRY_BYTES
            if entry != (chunk["type"], chunk["version"], chunk["flags"], chunk["offset"],
                         chunk["frame_id"], chunk["stored"]):
                fail(f"INDEX entry at offset {chunk['offset']} disagrees with the chunk")
        for (frame_record, packed) in zip(frame_ends, struct.iter_unpack(
                "<QQQIIQ", index_payload[cursor:])):
            first_chunk_seq, last_chunk_seq = frame_ranges[frame_record["frame_id"]]
            if packed[4] != 0 or packed != (
                    frame_record["frame_id"], first_chunk_seq, last_chunk_seq,
                    frame_record["flags"], 0, frame_record["output_id"]):
                fail(f"INDEX frame record for {frame_record['frame_id']} disagrees with the "
                     "chunk sequence it indexes")

        complete_offset = index_offset + CAPTURE_CHUNK_HEADER_BYTES + stored
        complete_head = read_at(complete_offset, CAPTURE_CHUNK_HEADER_BYTES)
        ctype, version, cflags = struct.unpack_from("<IHH", complete_head, 0)
        seq, frame_id, complete_stored, uncompressed, payload_sum = struct.unpack_from(
            "<QQQQQ", complete_head, 8)
        reserved, header_sum = struct.unpack_from("<QQ", complete_head, 48)
        if ctype != CAPTURE_CHUNK_COMPLETE or frame_id != 0 or version != 1 or cflags != 0:
            fail("COMPLETE chunk header is wrong")
        if reserved or header_sum != fnv1a64(complete_head[:56]):
            fail("COMPLETE chunk header checksum/reserved field is wrong")
        if seq != sequence + 1:
            fail(f"COMPLETE sequence {seq} != {sequence + 1}")
        if uncompressed != complete_stored or complete_stored < 136:
            fail("COMPLETE length fields are wrong")
        if complete_offset + CAPTURE_CHUNK_HEADER_BYTES + complete_stored != size:
            fail("COMPLETE is not the last chunk in the file")
        complete_payload = read_at(complete_offset + CAPTURE_CHUNK_HEADER_BYTES,
                                   complete_stored)
        if fnv1a64(complete_payload) != payload_sum:
            fail("COMPLETE payload checksum mismatch")
        (status, reasons, chain_digest, total_frames, total_chunks, total_bytes, _gpu_words,
         _library_calls, _initial_bytes, _delta_bytes, _dedup_bytes, dependency_complete,
         library_ready, unsupported_schemas, unresolved_resources, overflow_events,
         dropped_events) = struct.unpack_from("<IIQQQQQQQQQIIIIII", complete_payload, 0)
        if any(complete_payload[104:136]):
            fail("COMPLETE reserved bytes are nonzero")
        if status != 1 or reasons:
            fail(f"COMPLETE status {status} reasons 0x{reasons:x} is not a clean finish")
        if dependency_complete > 1 or library_ready > 1:
            fail("COMPLETE boolean fields are out of range")
        if unsupported_schemas or unresolved_resources or overflow_events or dropped_events:
            fail("COMPLETE reports unsupported schemas or unresolved resources")
        if chain_digest != digest:
            fail("COMPLETE chain digest does not match the chunk headers")
        if total_frames != preroll + finalized:
            fail(f"COMPLETE total_frames {total_frames} != {preroll + finalized}")
        if total_chunks != chunk_count:
            fail(f"COMPLETE total_chunks {total_chunks} != header {chunk_count}")
        if total_bytes != size:
            fail(f"COMPLETE total_bytes {total_bytes} != file length {size}")
        if len(chunks) + 2 != chunk_count:
            fail(f"header chunk_count {chunk_count} != {len(chunks) + 2} walked chunks")
        if len(frame_ends) != preroll + finalized:
            fail(f"stream has {len(frame_ends)} FRAME_END chunks, window declares "
                 f"{preroll + finalized}")

    return {
        "path": path,
        "requested": requested,
        "finalized": finalized,
        "preroll_frames": preroll,
        "selected_ids": list(range(first_frame, first_frame + finalized)),
        "preroll_ids": list(range(first_frame - preroll, first_frame)),
        "frames": {
            end["frame_id"]: {
                "call_count": end["call_count"],
                "effect_count": end["effect_count"],
                "preroll": bool(end["flags"] & CAPTURE_FRAME_PREROLL),
            }
            for end in frame_ends
        },
        "chain_digest": digest,
        "chunks": chunk_count,
    }


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
    expected_path: Path,
    observed_path: Path,
    frames: dict[int, dict],
    *,
    preroll: dict[int, int] | None = None,
    observed_preroll: Path | None = None,
) -> tuple[dict[str, Any], list[str], bool, list[str]]:
    """Reconcile ordered live/expected against replay/observed effects per frame.

    The window is the selected frames plus the pre-roll frames the capture declares. Pre-roll
    rows are not pictures; their effect counts still come from the stream/sidecar, so an effect
    cannot be dropped there either.
    """
    preroll = preroll or {}
    expected_rows = read_ledger(expected_path)
    observed_rows = read_ledger(observed_path)
    if observed_preroll is not None:
        if not observed_preroll.is_file():
            raise InvalidInputError(f"{observed_preroll}: missing pre-roll observed ledger")
        observed_rows = read_ledger(observed_preroll) + observed_rows

    counts = {cls: {"expected": 0, "observed": 0} for cls in sorted(VALID_CLASSIFICATIONS)}
    window = sorted(set(frames) | set(preroll))
    frame_failures: dict[int, list[str]] = {fid: [] for fid in window}
    failures: list[str] = []

    def index(
        path: Path, rows: list[tuple[str, dict]]
    ) -> tuple[dict[int, list[tuple[str, dict]]], list[str]]:
        indexed: dict[int, list[tuple[str, dict]]] = {fid: [] for fid in window}
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


def read_title_census(path: Path) -> dict[int, int]:
    """Per-frame live call totals from a #5 census report's `frame_rows`."""
    document = read_json(path)
    if not isinstance(document, dict):
        raise InvalidInputError(f"{path}: census must be a JSON object")
    convention = document.get("frame_id_basis")
    schema = document.get("schema")
    if schema != TITLE_CENSUS_SCHEMA or convention != TITLE_CENSUS_FRAME_ID_BASIS:
        raise InvalidInputError(
            f"{path}: frame_rows requires schema {TITLE_CENSUS_SCHEMA} and "
            f"frame_id_basis={TITLE_CENSUS_FRAME_ID_BASIS!r}; "
            f"got schema={schema!r} frame_id_basis={convention!r}"
        )
    rows = document.get("frame_rows")
    if not isinstance(rows, list) or not rows:
        raise InvalidInputError(f"{path}: no frame_rows; not a live census report")
    totals: dict[int, int] = {}
    for row in rows:
        if not isinstance(row, list) or len(row) != 2:
            raise InvalidInputError(f"{path}: frame_rows entry {row!r} is not [frame_id, counts]")
        frame_id, entries = row
        if type(frame_id) is not int or frame_id < 0:
            raise InvalidInputError(
                f"{path}: frame id {frame_id!r} is not a non-negative integer"
            )
        if not isinstance(entries, list):
            raise InvalidInputError(f"{path}: frame {frame_id} counts are not a list")
        total = 0
        for entry in entries:
            if not (isinstance(entry, list) and len(entry) == 2):
                raise InvalidInputError(
                    f"{path}: frame {frame_id} count {entry!r} is not [target, calls]"
                )
            target, calls = entry
            if type(target) is not int or type(calls) is not int or calls < 0:
                raise InvalidInputError(
                    f"{path}: frame {frame_id} count {entry!r} is not non-negative integers"
                )
            total += calls
        if frame_id in totals:
            raise InvalidInputError(f"{path}: duplicate frame id {frame_id} in frame_rows")
        totals[frame_id] = total
    return totals


def resolve_title_census(fixture_dir: Path, explicit: Path | None) -> Path | None:
    """The explicit report wins; otherwise an in-directory census with frame_rows."""
    if explicit is not None:
        return explicit
    candidate = fixture_dir / "census.json"
    if not candidate.is_file():
        return None
    try:
        document = read_json(candidate)
    except (InvalidInputError, OSError, ValueError):
        return None
    if isinstance(document, dict) and isinstance(document.get("frame_rows"), list):
        return candidate
    return None


def verify_run_report(path: Path, capture: dict) -> tuple[dict[str, Any], list[str]]:
    """A capture is only qualified together with the replay run that consumed it."""
    document = read_json(path)
    if not isinstance(document, dict) or not isinstance(document.get("replay"), dict):
        raise InvalidInputError(f"{path}: missing replay section")
    replay = document["replay"]
    failures: list[str] = []
    if replay.get("status") != "complete" or replay.get("run_complete") is not True:
        failures.append(f"{path}: replay did not complete (status={replay.get('status')!r}, "
                        f"run_complete={replay.get('run_complete')!r}, "
                        f"error={replay.get('error')!r})")
    for key, expected in (("frames_expected", capture["finalized"]),
                          ("frames_replayed", capture["finalized"]),
                          ("preroll_frames_executed", capture["preroll_frames"])):
        value = replay.get(key)
        if type(value) is not int or value != expected:
            failures.append(f"{path}: replay.{key}={value!r} != capture {expected}")
    written = replay.get("pictures_written")
    if type(written) is not int or written < 0 or written > capture["finalized"]:
        failures.append(f"{path}: replay.pictures_written={written!r} is outside 0.."
                        f"{capture['finalized']}")
    guest_calls = replay.get("replay_guest_call_count")
    if type(guest_calls) is not int or guest_calls != 0:
        failures.append(f"{path}: replay.replay_guest_call_count={guest_calls!r} is not zero")
    digest = replay.get("capture_chain_digest")
    if isinstance(digest, str):
        try:
            digest = int(digest, 16)
        except ValueError:
            failures.append(f"{path}: capture_chain_digest {digest!r} is not hexadecimal")
            digest = None
    if digest != capture["chain_digest"]:
        failures.append(f"{path}: capture_chain_digest={digest!r} != stream "
                        f"{capture['chain_digest']:016x}")
    return {
        "scope": "replay_report",
        "path": str(path),
        "required": True,
        "frames_replayed": replay.get("frames_replayed"),
        "preroll_frames_executed": replay.get("preroll_frames_executed"),
        "pictures_written": written,
        "guest_call_count": guest_calls,
        "reconciled": not failures,
    }, failures


def read_preroll_frames(path: Path | None, capture: dict) -> dict[int, int]:
    """Declared per-frame effect counts for the pre-roll frames replay must execute."""
    if not capture["preroll_ids"]:
        if path is not None and path.is_file() and path.stat().st_size:
            raise InvalidInputError(f"{path}: rows for a capture with no pre-roll frames")
        return {}
    if path is None or not path.is_file():
        raise InvalidInputError(
            f"pre-roll window {capture['preroll_ids']} declared by the capture but "
            "preroll_frames.jsonl is missing"
        )
    declared: dict[int, int] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise InvalidInputError(f"{path}: {exc}") from exc
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        where = f"{path}:{number}"
        try:
            row = json.loads(line)
        except ValueError as exc:
            raise InvalidInputError(f"{where}: {exc}") from exc
        if not isinstance(row, dict):
            raise InvalidInputError(f"{where}: expected a JSON object")
        fid = field(row, "frame_id", int, where)
        if fid not in capture["preroll_ids"]:
            raise InvalidInputError(f"{where}: frame_id {fid} is not a declared pre-roll frame "
                                    f"{capture['preroll_ids']}")
        if fid in declared:
            raise InvalidInputError(f"{where}: duplicate pre-roll frame_id {fid}")
        declared[fid] = field(row, "effect_count", int, where)
    missing = [fid for fid in capture["preroll_ids"] if fid not in declared]
    if missing:
        raise InvalidInputError(f"{path}: missing pre-roll frame(s) {missing}")
    return declared


def verify_fixture_dir(
    fixture_dir: Path,
    *,
    images_only: bool = False,
    expected_effects: Path | None = None,
    require_equivalent: bool = False,
    witness_only: bool = False,
    title_census: Path | None = None,
    capture: Path | None = None,
    run_report: Path | None = None,
) -> tuple[int, dict[str, Any]]:
    """Raw replay image evidence alone is not an effect ledger or census certificate."""
    try:
        envelope = IMAGE_LIMITS
        expected_ids = None
        expected_kind = (
            "standalone_replay_witness" if witness_only
            else "standalone_replay_qualification_fixture"
        )
        capture_info: dict | None = None
        preroll_counts: dict[int, int] = {}
        report_info: dict[str, Any] | None = None
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
            if capture is not None or witness_only or \
                    manifest.get("kind") == "standalone_replay_witness":
                if capture is None:
                    raise InvalidInputError(
                        f"{fixture_dir}: a replay witness needs --capture <finalized.egcap>; "
                        "manifest counts and adapter sidecars are not trusted evidence"
                    )
                capture_info = read_capture(capture)
                if capture_info["selected_ids"] != expected_ids:
                    raise InvalidInputError(
                        f"{capture}: selected frames {capture_info['selected_ids']} != "
                        f"manifest expected_frame_ids {expected_ids}"
                    )
        frames, failures = verify_frames(
            fixture_dir,
            envelope,
            counts_required=not images_only,
            expected_ids=expected_ids,
        )
        if capture_info is not None:
            for fid in capture_info["selected_ids"]:
                declared = capture_info["frames"][fid]
                if declared["preroll"]:
                    failures.append(f"{capture}: selected frame {fid} is marked pre-roll")
                for key in ("call_count", "effect_count"):
                    if frames[fid][key] != declared[key]:
                        failures.append(
                            f"frame {fid}: frames.jsonl {key}={frames[fid][key]} != capture "
                            f"FRAME_END {key}={declared[key]}"
                        )
                        frames[fid]["passed"] = False
            preroll_counts = read_preroll_frames(fixture_dir / "preroll_frames.jsonl",
                                                 capture_info)
            report_path = run_report or (fixture_dir / "report.json")
            if not report_path.is_file():
                raise InvalidInputError(
                    f"{report_path}: missing replay report for {capture}; pass --run-report"
                )
            report_info, report_failures = verify_run_report(report_path, capture_info)
            failures.extend(report_failures)
            result_capture = {
                "path": str(capture),
                "selected_frames": len(capture_info["selected_ids"]),
                "preroll_frames": capture_info["preroll_frames"],
                "chunks": capture_info["chunks"],
                "chain_digest": f"{capture_info['chain_digest']:016x}",
                "report": report_info,
            }
        else:
            result_capture = {"scope": "not_present", "required": False}
        title_path = None if images_only else resolve_title_census(fixture_dir, title_census)
        call_report: dict[str, Any] = {"scope": "not_present", "required": False}
        if title_path is not None:
            totals = read_title_census(title_path)
            call_failures: list[str] = []
            for fid in sorted(frames):
                declared = frames[fid]["call_count"]
                if fid not in totals:
                    call_failures.append(
                        f"{title_path}: frame {fid} is missing; replay declares call_count={declared}"
                    )
                elif totals[fid] != declared:
                    call_failures.append(
                        f"{title_path}: frame {fid} live calls {totals[fid]} != "
                        f"replay call_count {declared}"
                    )
            failures.extend(call_failures)
            if call_failures:
                for fid in frames:
                    if fid not in totals or totals[fid] != frames[fid]["call_count"]:
                        frames[fid]["passed"] = False
            call_report = {
                "scope": "title_census",
                "required": True,
                "path": str(title_path),
                "frames": len(totals),
                "reconciled": not call_failures,
            }
        elif any(frames[fid]["call_count"] for fid in frames):
            for fid in sorted(frames):
                if frames[fid]["call_count"]:
                    failures.append(
                        f"frame {fid}: call_count={frames[fid]['call_count']} is unreconciled; "
                        "pass --title-census <live census.json>"
                    )
                    frames[fid]["passed"] = False
            call_report = {"scope": "unreconciled", "required": True}
        result: dict[str, Any] = {
            "scope": "images" if images_only else ("replay_witness" if witness_only else "fixture"),
            "frames": list(frames.values()),
            "title_census": call_report,
            "capture": result_capture,
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
                Path(expected_path), observed_path, frames,
                preroll=preroll_counts,
                observed_preroll=(fixture_dir / "preroll_effects.jsonl"
                                  if preroll_counts else None),
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
    title_census: Path | None = None,
    capture: Path | None = None,
) -> tuple[int, dict[str, Any]]:
    """Two standalone replays are identical only if every witness and image agrees."""
    code_a, result_a = verify_fixture_dir(
        run_a,
        expected_effects=expected_effects,
        require_equivalent=require_equivalent,
        witness_only=witness_only,
        title_census=title_census,
        capture=capture,
    )
    code_b, result_b = verify_fixture_dir(
        run_b,
        expected_effects=expected_effects,
        require_equivalent=require_equivalent,
        witness_only=witness_only,
        title_census=title_census,
        capture=capture,
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

    def attach_capture(scene: Path) -> Path:
        """Emit the minimal stream + terminal report that a witness run must carry."""
        rows = frame_rows(scene)
        ids = [row["frame_id"] for row in rows]
        path = scene / "capture.egcap"
        capture = write_test_capture(
            path, first_frame=ids[0], preroll=0, selected_ids=ids,
            effect_counts={row["frame_id"]: row["effect_count"] for row in rows},
            call_counts={row["frame_id"]: row["call_count"] for row in rows},
        )
        write_test_report(scene / "report.json", capture)
        return path

    def make_witness(name: str) -> Path:
        scene = write_synthetic_fixture(root / name)
        manifest = read_json(scene / "manifest.json")
        manifest["kind"] = "standalone_replay_witness"
        (scene / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        rows = frame_rows(scene)
        for row in rows:
            row["call_count"] = 0
        write_rows(scene / "frames.jsonl", rows)
        (scene / "census.json").unlink()
        return scene

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
        capture = attach_capture(witness)
        code, result = verify_fixture_dir(witness, witness_only=True, capture=capture)
        checks.append(("game_free_witness_pass", EXIT_PASS, code))
        checks.append(("game_free_witness_census_scope", False, result["census"]["required"]))
        rows[0]["call_count"] = 1
        write_rows(witness / "frames.jsonl", rows)
        attach_capture(witness)
        code, _ = verify_fixture_dir(witness, witness_only=True, capture=capture)
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
        equivalent_capture = attach_capture(equivalent)
        code, _ = verify_fixture_dir(
            equivalent, witness_only=True, require_equivalent=True, capture=equivalent_capture)
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
        opaque_capture = attach_capture(opaque_witness)
        code, _ = verify_fixture_dir(
            opaque_witness, witness_only=True, require_equivalent=True, capture=opaque_capture)
        checks.append(("game_free_opaque_require_incomplete", EXIT_INCOMPLETE, code))

        # 20c. Nonzero guest calls reconcile against the live census and are never rewritten.
        counted = write_synthetic_fixture(root / "case_counted_witness")
        manifest = read_json(counted / "manifest.json")
        manifest["kind"] = "standalone_replay_witness"
        (counted / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        (counted / "census.json").unlink()
        counted_capture = attach_capture(counted)
        code, _ = verify_fixture_dir(counted, witness_only=True, capture=counted_capture)
        checks.append(("unreconciled_calls_fail", EXIT_FAIL, code))

        title_census = write_synthetic_fixture(root / "case_title_census") / "census.json"
        code, _ = verify_fixture_dir(
            counted, witness_only=True, title_census=title_census, capture=counted_capture)
        checks.append(("title_census_reconciled_pass", EXIT_PASS, code))

        in_dir = root / "case_counted_in_dir"
        shutil.copytree(counted, in_dir)
        shutil.copyfile(title_census, in_dir / "census.json")
        code, _ = verify_fixture_dir(in_dir, witness_only=True, capture=in_dir / "capture.egcap")
        checks.append(("in_dir_title_census_pass", EXIT_PASS, code))

        undeclared = root / "case_title_census_undeclared.json"
        census_doc = read_json(title_census)
        del census_doc["frame_id_basis"]
        undeclared.write_text(json.dumps(census_doc), encoding="utf-8")
        code, _ = verify_fixture_dir(
            counted, witness_only=True, title_census=undeclared, capture=counted_capture)
        checks.append(("title_census_undeclared_invalid", EXIT_INVALID, code))

        wrong_convention = root / "case_title_census_wrong_basis.json"
        census_doc["frame_id_basis"] = "frame_index"
        wrong_convention.write_text(json.dumps(census_doc), encoding="utf-8")
        code, _ = verify_fixture_dir(
            counted, witness_only=True, title_census=wrong_convention, capture=counted_capture)
        checks.append(("title_census_wrong_basis_invalid", EXIT_INVALID, code))

        old_schema = root / "case_title_census_schema2.json"
        census_doc["frame_id_basis"] = TITLE_CENSUS_FRAME_ID_BASIS
        census_doc["schema"] = 2
        old_schema.write_text(json.dumps(census_doc), encoding="utf-8")
        code, _ = verify_fixture_dir(
            counted, witness_only=True, title_census=old_schema, capture=counted_capture)
        checks.append(("title_census_schema2_invalid", EXIT_INVALID, code))

        off_by_one = root / "case_title_census_off_by_one.json"
        census_doc = read_json(title_census)
        census_doc["frame_rows"][0][1] = [[7, 4]]
        off_by_one.write_text(json.dumps(census_doc), encoding="utf-8")
        code, _ = verify_fixture_dir(
            counted, witness_only=True, title_census=off_by_one, capture=counted_capture)
        checks.append(("title_census_off_by_one_fail", EXIT_FAIL, code))

        missing_frame = root / "case_title_census_missing_frame.json"
        census_doc = read_json(title_census)
        census_doc["frame_rows"] = census_doc["frame_rows"][:1]
        missing_frame.write_text(json.dumps(census_doc), encoding="utf-8")
        code, _ = verify_fixture_dir(
            counted, witness_only=True, title_census=missing_frame, capture=counted_capture)
        checks.append(("title_census_missing_frame_fail", EXIT_FAIL, code))

        code, _ = compare_runs(
            counted, counted, witness_only=True, title_census=title_census,
            capture=counted_capture)
        checks.append(("title_census_compare_pass", EXIT_PASS, code))
        code, _ = compare_runs(counted, counted, witness_only=True, capture=counted_capture)
        checks.append(("title_census_compare_absent_fail", EXIT_FAIL, code))

        # 20d. Witness qualification is anchored to a finalized capture and its report.
        uncaptured = make_witness("case_witness_no_capture")
        code, _ = verify_fixture_dir(uncaptured, witness_only=True)
        checks.append(("witness_without_capture_invalid", EXIT_INVALID, code))

        windowed = make_witness("case_witness_wrong_window")
        wrong_window = write_test_capture(
            root / "case_wrong_window.egcap", first_frame=2, preroll=0, selected_ids=[2],
            effect_counts={2: 4})
        write_test_report(root / "case_wrong_window_report.json", wrong_window)
        code, _ = verify_fixture_dir(
            windowed, witness_only=True, capture=root / "case_wrong_window.egcap",
            run_report=root / "case_wrong_window_report.json")
        checks.append(("capture_window_mismatch_invalid", EXIT_INVALID, code))

        counted_frames = make_witness("case_witness_count_mismatch")
        good_capture = attach_capture(counted_frames)
        rows = frame_rows(counted_frames)
        rows[0]["effect_count"] += 1
        write_rows(counted_frames / "frames.jsonl", rows)
        code, _ = verify_fixture_dir(counted_frames, witness_only=True, capture=good_capture)
        checks.append(("frame_effect_count_mismatch_fail", EXIT_FAIL, code))

        truncated = make_witness("case_witness_truncated")
        truncated_capture = attach_capture(truncated)
        truncated_capture.write_bytes(truncated_capture.read_bytes()[:-10])
        code, _ = verify_fixture_dir(truncated, witness_only=True, capture=truncated_capture)
        checks.append(("truncated_capture_invalid", EXIT_INVALID, code))

        report_case = make_witness("case_witness_report")
        report_capture = attach_capture(report_case)
        (report_case / "report.json").unlink()
        code, _ = verify_fixture_dir(report_case, witness_only=True, capture=report_capture)
        checks.append(("missing_run_report_invalid", EXIT_INVALID, code))

        bad_report = write_test_capture(
            root / "case_report_ok.egcap", first_frame=1, preroll=0, selected_ids=[1, 2],
            effect_counts={1: 4, 2: 4})
        write_test_report(root / "case_report_short.json", bad_report, frames_replayed=1)
        write_test_report(root / "case_report_calls.json", bad_report, guest_calls=1)
        write_test_report(root / "case_report_digest.json", bad_report, chain_digest="0" * 16)
        for label, path in (("short", root / "case_report_short.json"),
                            ("calls", root / "case_report_calls.json"),
                            ("digest", root / "case_report_digest.json")):
            code, _ = verify_fixture_dir(
                report_case, witness_only=True, capture=report_capture, run_report=path)
            checks.append((f"run_report_{label}_fail", EXIT_FAIL, code))

        # 20e. Preroll frames reconcile through their own declared and observed ledgers.
        preroll = make_witness("case_witness_preroll")
        rows = frame_rows(preroll)
        selected_rows = [row for row in rows if row["frame_id"] == 2]
        write_rows(preroll / "frames.jsonl", selected_rows)
        manifest = read_json(preroll / "manifest.json")
        manifest["expected_frame_ids"] = [2]
        manifest["expected_frames"] = 1
        (preroll / "manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        all_effects = ledger(preroll)
        preroll_effects = [row for row in all_effects if row["frame_id"] == 1]
        write_rows(preroll / "effects.jsonl", [row for row in all_effects if row["frame_id"] == 2])
        write_rows(preroll / "preroll_effects.jsonl", preroll_effects)
        (preroll / "preroll_frames.jsonl").write_text(
            json.dumps({"frame_id": 1, "effect_count": len(preroll_effects)}, sort_keys=True) + "\n",
            encoding="utf-8")
        for name in ("live_00000001.png", "replay_00000001.png"):
            (preroll / name).unlink()
        preroll_capture = write_test_capture(
            preroll / "capture.egcap", first_frame=2, preroll=1, selected_ids=[2],
            effect_counts={1: len(preroll_effects), 2: selected_rows[0]["effect_count"]})
        write_test_report(preroll / "report.json", preroll_capture)
        code, _ = verify_fixture_dir(preroll, witness_only=True, capture=preroll / "capture.egcap")
        checks.append(("preroll_reconciled_pass", EXIT_PASS, code))
        kept = ledger(preroll, "preroll_effects.jsonl")
        write_rows(preroll / "preroll_effects.jsonl", kept[:-1])
        code, _ = verify_fixture_dir(preroll, witness_only=True, capture=preroll / "capture.egcap")
        checks.append(("preroll_observed_count_fail", EXIT_FAIL, code))
        write_rows(preroll / "preroll_effects.jsonl", kept)
        expected = [row for _, row in read_records(preroll / "expected_effects.jsonl")
                    if row["frame_id"] != 1]
        write_rows(preroll / "expected_effects.jsonl", expected)
        code, _ = verify_fixture_dir(preroll, witness_only=True, capture=preroll / "capture.egcap")
        checks.append(("preroll_expected_count_fail", EXIT_FAIL, code))

        # 20f. A producer-reported audit mismatch cannot hide a one-entry shortfall.
        shortfall = write_synthetic_fixture(root / "case_census_shortfall")
        census_doc = read_json(shortfall / "census.json")
        census_doc["functions"][0].update(calls=7, startup_calls=2, frame_calls=5)
        census_doc["functions"][1].update(calls=5, startup_calls=0, frame_calls=5)
        census_doc["accounting"].update(executed_total=12, closed_frame_calls=10)
        census_doc["guest_threads"][0]["calls"] = 12
        census_doc["audit"]["mismatched_calls"] = 1
        (shortfall / "census.json").write_text(json.dumps(census_doc), encoding="utf-8")
        code, result = verify_fixture_dir(shortfall)
        checks.append(("census_audit_shortfall_incomplete", EXIT_INCOMPLETE, code))
        checks.append(("census_audit_shortfall_named", True,
                       "audit_matches_counts" in result["census"]["failed"]))

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
                       help="Verify a replay witness in DIR (effects + images; guest calls reconciled)")
    group.add_argument("--compare-runs", type=Path, nargs=2, metavar=("RUN_A", "RUN_B"),
                       help="Verify two replay runs independently, then require identical witnesses")
    group.add_argument("--verify-images", type=Path,
                       help="Verify actual --replay-images output (images and semantics only)")
    group.add_argument("--reconcile-census", type=Path, help="Evaluate schema-2 census JSON")
    group.add_argument("--selftest", action="store_true", help="Run positive & negative controls")
    parser.add_argument("--expected-effects", type=Path,
                        help="Capture-side expected_effects.jsonl when it is not inside the run directory")
    parser.add_argument("--title-census", type=Path,
                        help="Schema-3 live census JSON; every captured frame's call_count must match its frame_rows")
    parser.add_argument("--capture", type=Path,
                        help="Finalized .egcap whose INDEX/FRAME_END define the trusted frame window")
    parser.add_argument("--run-report", type=Path,
                        help="Replay report.json for --capture when it is not inside the run directory")
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
            title_census=args.title_census,
            capture=args.capture,
        )
    elif args.verify_replay is not None:
        code, summary = verify_fixture_dir(
            args.verify_replay,
            expected_effects=args.expected_effects,
            require_equivalent=args.require_equivalent,
            witness_only=True,
            title_census=args.title_census,
            capture=args.capture,
            run_report=args.run_report,
        )
    else:
        code, summary = verify_fixture_dir(
            args.verify_images or args.verify,
            images_only=args.verify_images is not None,
            expected_effects=args.expected_effects,
            require_equivalent=args.require_equivalent,
            title_census=args.title_census,
            capture=args.capture,
            run_report=args.run_report,
        )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return code


if __name__ == "__main__":
    sys.exit(main())
