#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded local incident capture for Eglantine (docs/nx2/incident-capture.md).

Watches a running emulator's native log (and, when EDEN_NX2_DEBUG_SERVER is set, its debug
server), indexes native signals as they happen, and on a marker keeps the lead-up and the
aftermath with the clocks tied together. Metadata only by default.

  incident.py watch --out DIR [--log FILE] [--server HOST:PORT] [--pid N]   attach to a running game
  incident.py watch --out DIR --launch eglantine.exe [--preset diagnostic]    start one and watch it
  incident.py mark --session DIR [--label TEXT]                               mark from another shell
  incident.py check DIR                                                       verify a finished session

Standard library only. Never reads guest memory, never writes to the emulator, never uploads.
"""

import argparse
import collections
import hashlib
import json
import os
import queue
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
# -I leaves the script's directory off sys.path.
sys.path.insert(0, HERE)

import debug_mcp  # noqa: E402

SCHEMA = 1
LOG_NAMES = ("eden_log.txt", "eden_log_2.txt")
# The only debug server tools this recorder will call: no guest or GPU memory, no input.
ALLOWED_TOOLS = ("status", "mark", "log_tail", "system_events", "capture_frame", "sample")
# System event rows kept per mark: this many before the mark's own row, then everything after it
# up to the page bound (rows a page, pages a mark).
SYS_ROWS_BEFORE = 1000
SYS_PAGE_ROWS = 1000
SYS_MAX_PAGES = 8
# What the log already shows for a window is kept as counts and signal rows, never as text,
# unless --raw-log asks for redacted text under private/.
DISTINCT = {"report", "frames_summary", "ipc_summary", "mark_line", "log_file_limit", "host_crash",
            "hid_summary", "timeline_summary", "audio_summary"}
FLUSH_SLACK_S = 1.0  # the log file is flushed once a second
# Never enabled for a game this tool starts: raw IPC words, and guest memory writes.
SCRUBBED_ENV = ("EDEN_NX2_IPC_TRACE", "EDEN_NX2_DEBUG_WRITE")

COVERAGE = {
    "indexed_from_native_log": {
        "level": "every Warning, Error and Critical line: class, level, source site, fingerprint",
        "frame_report": "the 10 s report: fps, longest frame, frames over 100 ms, limiter, audio "
                        "underruns, long file reads, shader build holds, low host memory",
        "frames_summary": "late frames, longest, median and 99th percentile frame time",
        "ipc_summary": "EDEN_NX2_IPC_SUMMARY=1 only: per service command call counts and results",
        "sched_summary": "EDEN_NX2_SCHED_SUMMARY=1 only: per guest core, the virtual tick and the "
                         "switch and switch-to-idle counts of each batch of 4096 actual thread "
                         "switches; no thread or process identity",
        "fs_short_read": "IStorage short read warnings: requested, returned, occurrence",
        "hid_summary": "EDEN_NX2_HID_SUMMARY=1 only: NPad updates, published samples and samples "
                       "with buttons held; totals, never which controls",
        "timeline_summary": "EDEN_NX2_TIMELINE=1 only: a sampled per-frame average of GPU command "
                            "lists, front/back completion lag, fence wake and waits; not an "
                            "exact GPU chronology",
        "unmapped_access": "guest accesses to unmapped memory (count only)",
        "device_lost": "host graphics device loss text",
        "host_crash": "the Critical Host crash line",
        "log_file_limit": "the native log file reaching its size limit",
    },
    "not_covered": [
        "guest memory: never read, searched or written by this tool",
        "pixels, video: only with --detail frame, which writes local images; no audio is recorded",
        "native detail before the mark: frame capture, sample and draw list start at the mark",
        "lines the emulator's log filter drops (status log_filter shows what it is)",
        "lines after the native file limit unless the debug server's ring is reachable",
        "input, scheduling and GPU submissions: no log signal; --detail sample and frame only",
        "exit status of a game this tool did not launch and has no --pid for",
    ],
}

NATIVE = re.compile(rb"^\[\s*(\d+)\.(\d{6})\]\s+(\S+) <(\w+)> (.*)$", re.S)
SITE = re.compile(rb"^(.*?):(\d+):([^:\s]*): (.*)$", re.S)
REPORT = re.compile(rb"^Last (\d+) s: ([\d.]+) fps(?: \(frame ([\d.]+) ms, longest (\d+) ms, "
                    rb"(\d+) over 100 ms\))?\. Limited by: ([^;]*)")
UNDERRUNS = re.compile(rb"\baudio underruns: (\d+)")
FILE_READ = re.compile(rb"; a file read took (\d+) ms")
SHADER_HOLD = re.compile(rb"; shader builds held rendering for (\d+) ms")
FRAMES = re.compile(rb"^Frames: (\d+) shown \| late \(> 1\.5x the median interval\) (\d+) \| "
                    rb"longest ([\d.]+) ms \| median ([\d.]+) ms \| 99th ([\d.]+) ms")
IPC = re.compile(rb"^ipc summary \((\d+) kinds\)(.*)$", re.S)
IPC_ROW = re.compile(rb"^(\d+)x ([A-Za-z0-9_.:\-]{1,32}?)::(\d+)( \(tipc\))? -> (0x[0-9a-f]+)$")
SHORT_READ = re.compile(rb"^IStorage read length mismatch: requested=(\d+) returned=(\d+) "
                        rb"\(occurrence (\d+) since process start\)$")
UNMAPPED = re.compile(rb"Unmapped (?:Read|Write)\d+ @")
DEVICE_LOST = re.compile(rb"DEVICE_LOST|device (?:was )?lost", re.I)
LOG_LIMIT = re.compile(rb"Log file size limit of (\d+) bytes reached")
MARK_LINE = re.compile(rb"^Incident mark #(\d+) session=(\d+) presents=(\d+) label_bytes=(\d+)")
HID = re.compile(rb"^NPad delivery: (\d+) updates, (\d+) published samples, "
                 rb"(\d+) samples with buttons held")
SCHED = re.compile(rb"^Guest scheduler: core=(\d+) tick=(\d+) switches=(\d+) "
                   rb"switches_to_idle=(\d+)")
AUDIO_SUMMARY = re.compile(rb"^Audio summary: underruns=(\d+) frames=(\d+)")
NUM = rb"([-+]?\d+(?:\.\d+)?)"
TIMELINE = re.compile(
    rb"^nx2 timeline \((\d+) frames\): frame " + NUM + rb" ms; " + NUM + rb" lists, " + NUM +
    rb" entries; first submit " + NUM + rb" ms after the previous present, last " + NUM +
    rb" ms before this one; front done " + NUM + rb" ms and back done " + NUM +
    rb" ms vs the present; fence wake " + NUM + rb" ms after the back; waited " + NUM +
    rb" ms; front CPU " + NUM + rb" ms, back CPU " + NUM + rb" ms per frame")
TIMELINE_FIELDS = ("frame_ms", "lists", "entries", "first_submit_ms", "last_submit_ms",
                   "front_lag_ms", "back_lag_ms", "fence_wake_ms", "waited_ms",
                   "front_cpu_ms", "back_cpu_ms")
METRIC_TAGS = ("report", "frames_summary", "hid_summary", "timeline_summary", "sched_summary",
               "audio_summary")
SAFE_WORDS = re.compile(r"[^A-Za-z0-9 _/.\-]")

PATHS = re.compile(r"(?:[A-Za-z]:[\\/]|\\\\[^\s\\/]+[\\/]|(?<![\w.])/(?:home|Users|tmp|var|mnt|media|"
                   r"opt|root|run|data|storage)/)[^\s\"'<>|,;)\]]*")
IPV4 = re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}\b")
HEXBLOB = re.compile(r"\b[0-9A-Fa-f]{32,}\b")
EMAIL = re.compile(r"[\w.+-]+@[\w-]+\.[\w.-]+")


def private_names():
    names = set()
    for key in ("USERNAME", "USER", "LOGNAME"):
        value = os.environ.get(key, "")
        if len(value) >= 3:
            names.add(value)
    home = os.path.basename(os.path.expanduser("~"))
    if len(home) >= 3:
        names.add(home)
    return names


def redact(text, names=None):
    """Best effort: paths, user names, addresses, e-mail and long hex blobs become placeholders."""
    for name in names if names is not None else private_names():
        text = re.sub(re.escape(name), "<user>", text, flags=re.I)
    text = PATHS.sub("<path>", text)
    text = EMAIL.sub("<email>", text)
    text = IPV4.sub("<ip>", text)
    return HEXBLOB.sub("<hex>", text)


def clean_site(file_part, line, func):
    parts = re.split(r"[\\/]", file_part.decode("utf-8", "replace"))
    absolute = bool(re.match(r"^[A-Za-z]:|^[\\/]", file_part.decode("utf-8", "replace")))
    keep = parts[-1:] if absolute else parts[-2:]
    return f"{'/'.join(keep)}:{line.decode()}:{func.decode('utf-8', 'replace')}"


def fingerprint(site, message):
    text = re.sub(rb"0x[0-9a-fA-F]+|\d+(?:\.\d+)?", b"#", message)
    text = re.sub(rb"[A-Za-z]:[\\/][^\s]*|/[^\s]*/[^\s]*", b"<p>", text)
    return hashlib.sha1((site or "").encode() + b"\0" + text).hexdigest()[:10]


def to_int(value):
    return int(value.decode())


def classify(level, message):
    """The signal tags of one line and the numbers taken from it. Free text is never kept."""
    tags = []
    fields = {}
    if level in ("Warning", "Error", "Critical"):
        tags.append(level.lower())
    if message.startswith(b"Last "):
        match = REPORT.match(message)
        if match:
            tags.append("report")
            fields["window_s"] = to_int(match.group(1))
            fields["fps"] = float(match.group(2))
            if match.group(3):
                fields["frame_ms"] = float(match.group(3))
                fields["longest_ms"] = to_int(match.group(4))
                fields["over_100ms"] = to_int(match.group(5))
                if fields["over_100ms"]:
                    tags.append("frame_over_100ms")
            fields["limited_by"] = SAFE_WORDS.sub("", match.group(6).decode("utf-8", "replace")).strip()[:48]
            under = UNDERRUNS.search(message)
            if under:
                fields["audio_underruns"] = to_int(under.group(1))
                if fields["audio_underruns"]:
                    tags.append("audio_underrun")
            read = FILE_READ.search(message)
            if read:
                fields["file_read_ms"] = to_int(read.group(1))
                tags.append("slow_file_read")
            hold = SHADER_HOLD.search(message)
            if hold:
                fields["shader_hold_ms"] = to_int(hold.group(1))
                tags.append("shader_build_hold")
            if b"; host memory is nearly full" in message:
                tags.append("host_memory_low")
    elif message.startswith(b"Frames: "):
        match = FRAMES.match(message)
        if match:
            tags.append("frames_summary")
            fields.update(shown=to_int(match.group(1)), late=to_int(match.group(2)),
                          longest_ms=float(match.group(3)), median_ms=float(match.group(4)),
                          p99_ms=float(match.group(5)))
    elif message.startswith(b"ipc summary ("):
        match = IPC.match(message)
        if match:
            tags.append("ipc_summary")
            rows = []
            unparsed = 0
            for text in match.group(2).split(b" | "):
                if not text.strip():
                    continue
                row = IPC_ROW.match(text.strip())
                if row:
                    rows.append([row.group(2).decode(), to_int(row.group(3)), bool(row.group(4)),
                                 row.group(5).decode(), to_int(row.group(1))])
                else:
                    unparsed += 1
            kinds = to_int(match.group(1))
            errors = [row for row in rows if int(row[3], 16) != 0]
            fields.update(kinds=kinds, listed=len(rows), unlisted_kinds=max(0, kinds - len(rows)),
                          unparsed_rows=unparsed, calls=sum(row[4] for row in rows),
                          error_calls=sum(row[4] for row in errors), errors=errors[:24],
                          rows=rows)
            if errors:
                tags.append("ipc_error_result")
    elif message.startswith(b"IStorage read length mismatch"):
        match = SHORT_READ.match(message)
        if match:
            tags.append("fs_short_read")
            fields.update(requested=to_int(match.group(1)), returned=to_int(match.group(2)),
                          occurrence=to_int(match.group(3)))
    elif message.startswith(b"NPad delivery:"):
        match = HID.match(message)
        if match:
            tags.append("hid_summary")
            fields.update(updates=to_int(match.group(1)), published=to_int(match.group(2)),
                          with_buttons_held=to_int(match.group(3)))
    elif message.startswith(b"Guest scheduler:"):
        match = SCHED.match(message)
        if match:
            tags.append("sched_summary")
            fields.update(core=to_int(match.group(1)), tick=to_int(match.group(2)),
                          switches=to_int(match.group(3)), to_idle=to_int(match.group(4)))
    elif message.startswith(b"Audio summary:"):
        match = AUDIO_SUMMARY.match(message)
        if match:
            tags.append("audio_summary")
            fields.update(underruns=to_int(match.group(1)), frames=to_int(match.group(2)))
    elif message.startswith(b"nx2 timeline ("):
        match = TIMELINE.match(message)
        if match:
            tags.append("timeline_summary")
            fields["frames"] = to_int(match.group(1))
            for name, value in zip(TIMELINE_FIELDS, match.groups()[1:]):
                fields[name] = float(value)
    elif message.startswith(b"Incident mark #"):
        match = MARK_LINE.match(message)
        if match:
            tags.append("mark_line")
            fields.update(mark=to_int(match.group(1)), session=to_int(match.group(2)),
                          presents=to_int(match.group(3)))
    elif message.startswith(b"Host crash"):
        tags.append("host_crash")
    elif LOG_LIMIT.search(message):
        tags.append("log_file_limit")
        fields["limit_bytes"] = to_int(LOG_LIMIT.search(message).group(1))
    if UNMAPPED.search(message):
        tags.append("unmapped_access")
    if DEVICE_LOST.search(message):
        tags.append("device_lost")
    return tags, fields


Rec = collections.namedtuple("Rec", "sec byte cls level site fp tags fields text")


def parse_line(line, byte, keep_text):
    """One native log line as a Rec, or None when it does not have the native shape."""
    match = NATIVE.match(line)
    if not match:
        return None
    sec = int(match.group(1)) + int(match.group(2)) / 1e6
    cls = match.group(3).decode("ascii", "replace")
    level = match.group(4).decode("ascii", "replace")
    rest = match.group(5)
    site_match = SITE.match(rest)
    if site_match:
        site = clean_site(site_match.group(1), site_match.group(2), site_match.group(3))
        message = site_match.group(4)
    else:
        site, message = None, rest
    tags, fields = classify(level, message)
    fp = fingerprint(site, message) if tags else None
    text = line.decode("utf-8", "replace")[:400] if keep_text else None
    return Rec(sec, byte, cls, level, site, fp, tuple(tags), fields, text)


def iso(wall):
    return datetime.fromtimestamp(wall, timezone.utc).isoformat(timespec="milliseconds")


class Refused(Exception):
    pass


class Busy(Exception):
    pass


class ServerLink:
    """The debug server through an allowlist; one request at a time, as the server serves."""

    def __init__(self, emulator):
        self.emulator = emulator
        self.lock = threading.Lock()
        self.calls = collections.Counter()

    def call(self, method, params=None, timeout=5, wait=False):
        if method not in ALLOWED_TOOLS:
            raise Refused(f"{method} is not a tool this recorder may call")
        if not self.lock.acquire(blocking=wait):
            raise Busy(method)
        try:
            self.calls[method] += 1
            return self.emulator.call(method, params or {}, timeout)
        finally:
            self.lock.release()


class Store:
    """The session's metadata files with a hard byte bound; refused writes are counted.

    Native captures (--detail) are written by the emulator outside this count and are held back
    by their own disk-free and size guards, not by this bound."""

    RESERVE = 256 * 1024  # kept for gaps, losses, marks and the closing files

    def __init__(self, root, max_bytes):
        self.root = root
        self.max_bytes = max_bytes
        self.used = 0
        self.dropped = collections.Counter()
        self.files = {}
        self.lock = threading.RLock()

    def path(self, rel):
        full = os.path.join(self.root, *rel.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        return full

    def room(self, size, essential):
        return self.used + size <= self.max_bytes - (0 if essential else self.RESERVE)

    def append(self, rel, row, essential=False):
        data = (json.dumps(row, separators=(",", ":")) + "\n").encode()
        with self.lock:
            if not self.room(len(data), essential):
                self.dropped[rel] += 1
                return False
            handle = self.files.get(rel)
            if handle is None:
                handle = self.files[rel] = open(self.path(rel), "ab")
            handle.write(data)
            self.used += len(data)
            return True

    def write(self, rel, data, essential=True):
        if isinstance(data, str):
            data = data.encode()
        with self.lock:
            if not self.room(len(data), essential):
                self.dropped[rel] += 1
                return False
            full = self.path(rel)
            with open(full + ".tmp", "wb") as handle:
                handle.write(data)
            os.replace(full + ".tmp", full)
            self.used += len(data)
            return True

    def replace_json(self, rel, document):
        """session.json is rewritten in place and sits in the reserve, not in the count."""
        full = self.path(rel)
        with open(full + ".tmp", "w", encoding="utf-8") as handle:
            json.dump(document, handle, indent=1)
        os.replace(full + ".tmp", full)

    def flush(self):
        for handle in self.files.values():
            handle.flush()

    def close(self):
        for handle in self.files.values():
            handle.close()
        self.files.clear()

    @property
    def capped(self):
        return bool(self.dropped)


class Coalescer:
    """Warnings that repeat are one row per native second with a count."""

    def __init__(self):
        self.buckets = {}

    def add(self, rec):
        key = (int(rec.sec), rec.cls, rec.level, rec.site, rec.fp, rec.tags)
        bucket = self.buckets.get(key)
        if bucket is None:
            bucket = self.buckets[key] = {"count": 0, "first": rec.sec, "last": rec.sec,
                                          "byte": rec.byte, "samples": []}
        bucket["count"] += 1
        bucket["last"] = rec.sec
        if rec.fields and len(bucket["samples"]) < 3:
            bucket["samples"].append(rec.fields)

    def drain(self, before_second=None):
        rows = []
        for key in sorted(k for k in self.buckets if before_second is None or k[0] < before_second):
            bucket = self.buckets.pop(key)
            row = signal_row(key[1], key[2], key[3], key[4], key[5], bucket["first"], bucket["byte"])
            row["count"] = bucket["count"]
            if bucket["last"] != bucket["first"]:
                row["last_seconds"] = bucket["last"]
            if bucket["samples"]:
                row["samples"] = bucket["samples"]
            rows.append(row)
        return rows


def signal_row(cls, level, site, fp, tags, seconds, byte):
    row = {"seconds": seconds, "class": cls, "level": level, "signals": list(tags)}
    if site:
        row["site"] = site
    if fp:
        row["fp"] = fp
    if byte is not None:
        row["byte"] = byte
    return row


class WindowAgg:
    """What happened in one side of a mark's window, as counts and signal rows."""

    MAX_ROWS = 2000
    MAX_METRICS = 200

    def __init__(self, reference, raw_cap):
        self.reference = reference
        self.raw_cap = raw_cap
        self.lines = 0
        self.by_level = collections.Counter()
        self.by_class = collections.Counter()
        self.rows = {}
        self.rows_dropped = 0
        self.metrics = []
        self.raw = []
        self.raw_dropped = 0

    def add(self, rec):
        rel = round(rec.sec - self.reference, 3)
        self.lines += 1
        self.by_level[rec.level] += 1
        self.by_class[rec.cls] += 1
        if rec.text is not None:
            if len(self.raw) < self.raw_cap:
                self.raw.append((rel, rec.text))
            else:
                self.raw_dropped += 1
        if not rec.tags:
            return
        key = (rec.cls, rec.level, rec.site, rec.fp, rec.tags)
        row = self.rows.get(key)
        if row is None:
            if len(self.rows) >= self.MAX_ROWS:
                self.rows_dropped += 1
            else:
                row = self.rows[key] = {"count": 0, "first_rel_s": rel, "samples": []}
        if row is not None:
            row["count"] += 1
            row["last_rel_s"] = rel
            if rec.fields and len(row["samples"]) < 3 and not (set(rec.tags) & {"ipc_summary"}):
                row["samples"].append(rec.fields)
        kind = next((t for t in rec.tags if t in METRIC_TAGS), None)
        if kind is not None and len(self.metrics) < self.MAX_METRICS:
            self.metrics.append({"rel_s": rel, "kind": kind, **rec.fields})

    def to_json(self):
        signals = []
        for (cls, level, site, fp, tags), row in sorted(self.rows.items(),
                                                        key=lambda item: item[1]["first_rel_s"]):
            entry = {"class": cls, "level": level, "signals": list(tags), **row}
            if site:
                entry["site"] = site
            if fp:
                entry["fp"] = fp
            if not entry["samples"]:
                del entry["samples"]
            signals.append(entry)
        return {"lines": self.lines, "by_level": dict(self.by_level),
                "by_class": dict(self.by_class.most_common(30)), "signals": signals,
                "signal_rows_dropped": self.rows_dropped, "metrics": self.metrics}


class Mark:
    def __init__(self, ident, label, pre, post, native, correlation, uncertainty, mono, wall):
        self.id = ident
        self.label = label
        self.pre = pre
        self.post = post
        self.native = native
        self.correlation = correlation
        self.uncertainty = uncertainty
        self.mono = mono
        self.wall = wall
        self.present = None
        self.server_mark = None
        self.system_seq = None
        self.log_byte = None
        self.notes = []
        self.pre_agg = None
        self.post_agg = None
        self.detail = []
        self.detail_thread = None


class Config:
    """Every knob with its default; tests and the command line both build one."""

    def __init__(self, **values):
        self.out = None
        self.log = None
        self.profile = None
        self.exe = None
        self.server = None
        self.pid = None
        self.pre = 30.0
        self.post = 30.0
        self.grace = 2.0
        self.poll = 0.25
        self.status_every = 10.0
        self.buffer_lines = 100000
        self.max_mb = 64
        self.max_bytes = None
        self.max_marks = 25
        self.catchup_mb = 32
        self.raw_log = False
        self.raw_max_lines = 4000
        self.detail = ()
        self.max_detail = 3
        self.detail_cooldown = 60.0
        self.detail_timeout = 120
        self.sample_seconds = 5
        self.diag_dir = None
        self.min_free_gb = 5.0
        self.max_detail_mb = 2048
        self.auto_mark = ("host_crash", "device_lost")
        self.max_auto_marks = 5
        self.auto_cooldown = 60.0
        self.duration = None
        self.silence_exit = 60.0
        self.inline_detail = False
        self.scrubbed_env = ()
        self.preset = None
        self.hotkey = False
        self.stdin = False
        for key, value in values.items():
            if not hasattr(self, key):
                raise TypeError(f"unknown option {key}")
            setattr(self, key, value)
        if self.max_bytes is None:
            self.max_bytes = int(self.max_mb * 1024 * 1024)


class Recorder:
    def __init__(self, cfg, link=None, clock=time.monotonic, wall=time.time, pid_alive=None):
        self.cfg = cfg
        self.clock = clock
        self.wall = wall
        self.t0 = clock()
        self.wall0 = wall()
        self.link = link
        self.pid_alive = pid_alive
        self.store = Store(cfg.out, cfg.max_bytes)
        os.makedirs(os.path.join(cfg.out, "control"), exist_ok=True)
        self.requests = queue.Queue()
        self.names = private_names()
        self.stats = collections.Counter()
        self.tag_counts = collections.Counter()
        self.coal = Coalescer()
        self.buffer = collections.deque()
        self.history_start = None
        self.native_high = None
        self.anchor = None
        self.gaps = []
        self.open_gaps = {}
        self.unresolved = []
        self.gap_causes = collections.Counter()
        self.marks = []
        self.open_marks = []
        self.log_path = cfg.log
        self.offset = None
        self.ident = None
        self.tail = b""
        self.tail_start = 0
        self.file_last_sec = None
        self.ring_active = False
        self.ring_next = 0
        self.server_seen = False
        self.server_ok = False
        self.server_info = {}
        self.server_session = None
        self.next_status = 0.0
        self.last_activity = clock()
        self.last_sync = clock()
        self.ipc = {}
        self.ipc_dropped_keys = 0
        self.ipc_unlisted_max = 0
        self.short_read_last = None
        self.short_read_unlogged = 0
        self.sched = {}
        self.auto_marks = 0
        self.last_auto = -1e9
        self.detail_count = 0
        self.last_detail = -1e9
        self.stopping = False
        self.stop_reason = None
        self.exit_code = None
        self.process = None
        self.finished = False
        self.event("session_start", schema=SCHEMA, tool="incident.py",
                   debug_server=link is not None)
        self.write_local()
        self.write_session()

    # ------------------------------------------------------------------ output
    def event(self, kind, **data):
        row = {"t": iso(self.wall()), "m": round(self.clock() - self.t0, 3), "kind": kind, **data}
        self.store.append("timeline.jsonl", row, essential=kind in ESSENTIAL)

    def write_local(self):
        local = {"note": "private: absolute paths of this machine; do not share",
                 "log": self.log_path, "exe": self.cfg.exe, "pid": self.cfg.pid,
                 "server": self.cfg.server}
        self.store.write("private/local.json", json.dumps(local, indent=1))

    def native_est(self, now):
        if self.anchor is None:
            return None
        return self.anchor[1] + (now - self.anchor[0])

    # ------------------------------------------------------------------ gaps
    def gap(self, cause, source, now=None, detail="", lost=None, unit=None, native_from=None,
            native_to=None, interval=False):
        now = self.clock() if now is None else now
        record = {"id": len(self.gaps) + 1, "cause": cause, "source": source,
                  "native_from": native_from if native_from is not None else self.native_est(now),
                  "native_to": native_to}
        if detail:
            record["detail"] = detail
        if lost is not None:
            record["lost"] = lost
            record["unit"] = unit
        if interval:
            record["open"] = True
        if len(self.gaps) < 5000:
            self.gaps.append(record)
        else:
            self.stats["gaps_dropped"] += 1
        self.gap_causes[cause] += 1
        if native_from is not None and native_to is None and not interval and \
                cause in LOSS_UNTIL_NEXT_LINE:
            self.unresolved.append(record)
        self.event("gap", **{k: v for k, v in record.items() if k != "id"}, gap=record["id"])
        return record

    def open_gap(self, key, cause, source, now, detail=""):
        if key in self.open_gaps:
            return
        self.open_gaps[key] = self.gap(cause, source, now, detail, interval=True)

    def close_gap(self, key, now):
        record = self.open_gaps.pop(key, None)
        if record is None:
            return
        record["open"] = False
        record["native_to"] = self.native_est(now)
        self.event("gap_end", gap=record["id"], cause=record["cause"], source=record["source"])

    # ------------------------------------------------------------------ the log file
    def candidates(self):
        found = []
        if self.cfg.log:
            found.append(self.cfg.log)
        else:
            folders = []
            if self.cfg.profile:
                folders.append(os.path.join(self.cfg.profile, "log"))
            if self.cfg.exe:
                folders.append(os.path.join(os.path.dirname(os.path.abspath(self.cfg.exe)),
                                            "user", "log"))
            if os.name == "nt" and os.environ.get("APPDATA"):
                folders.append(os.path.join(os.environ["APPDATA"], "eglantine", "log"))
            data = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"),
                                                                   ".local", "share")
            folders.append(os.path.join(data, "eglantine", "log"))
            found = [os.path.join(folder, name) for folder in folders for name in LOG_NAMES]
        return found

    def discover(self):
        best = None
        for path in self.candidates():
            try:
                stat = os.stat(path)
            except OSError:
                continue
            if self.cfg.exe and self.launched_at and stat.st_mtime < self.launched_at - 2:
                continue
            if best is None or stat.st_mtime > best[0]:
                best = (stat.st_mtime, path)
        return best[1] if best else None

    launched_at = None

    def poll_file(self, now):
        if self.log_path is None or (self.cfg.exe and self.offset is None and not self.cfg.log):
            self.log_path = self.log_path or self.discover()
        if self.log_path is None:
            self.open_gap("file", "log_file_unavailable", "file", now, "no native log file found")
            return
        try:
            stat = os.stat(self.log_path)
        except OSError:
            self.open_gap("file", "log_file_unavailable", "file", now, "the log file is missing")
            return
        ident = (stat.st_ino, stat.st_dev) if stat.st_ino else None
        if self.offset is not None and ((self.ident and ident and ident != self.ident) or
                                        stat.st_size < self.offset):
            self.gap("log_file_replaced_or_truncated", "file", now,
                     detail="a new log file or a shorter one; the bytes after the last read of "
                            "the old one are not known",
                     native_from=self.native_high)
            self.offset = 0
            self.tail = b""
            self.tail_start = 0
            self.file_last_sec = None
        self.ident = ident
        if self.offset is None:
            self.attach_offset(stat.st_size, now)
        self.close_gap("file", now)
        budget = 8 * 1024 * 1024
        while stat.st_size > self.offset and budget > 0:
            try:
                with open(self.log_path, "rb") as handle:
                    handle.seek(self.offset)
                    data = handle.read(min(stat.st_size - self.offset, 4 * 1024 * 1024))
            except OSError as error:
                self.open_gap("file", "log_file_unreadable", "file", now,
                              redact(str(error), self.names)[:120])
                return
            if not data:
                break
            self.offset += len(data)
            budget -= len(data)
            self.stats["bytes_read"] += len(data)
            self.ingest_chunk(data, now)
        # The native clock is anchored on the last line read, not on this poll: a quiet log
        # must not look as if no time passed in it (see ingest).

    def attach_offset(self, size, now):
        cap = int(self.cfg.catchup_mb * 1024 * 1024)
        if size > cap:
            self.offset = size - cap
            self.tail_start = self.offset
            skipped = self.offset
            self.skip_partial = True
            self.gap("attach_skipped_history", "file", now, lost=skipped, unit="bytes",
                     detail=f"the log was larger than the {self.cfg.catchup_mb} MiB catch-up bound")
            self.history_start = -1.0  # set from the first line read
        else:
            self.offset = 0
            self.history_start = 0.0
            self.skip_partial = False
        self.event("attached", log_bytes_at_attach=size, from_start=self.offset == 0)

    skip_partial = False

    def ingest_chunk(self, data, now):
        buffer = self.tail + data
        lines = buffer.split(b"\n")
        self.tail = lines.pop()
        position = self.tail_start
        if self.skip_partial and lines:
            position += len(lines.pop(0)) + 1
            self.skip_partial = False
        for line in lines:
            line_length = len(line) + 1
            self.ingest(line.rstrip(b"\r"), position, now)
            position += line_length
        self.tail_start = position

    # ------------------------------------------------------------------ one line
    def ingest(self, line, byte, now, source="file"):
        if not line:
            return
        self.stats["lines"] += 1
        rec = parse_line(line, byte, self.cfg.raw_log)
        if rec is None:
            self.stats["unparsed_lines"] += 1
            return
        if source == "file":
            self.file_last_sec = rec.sec
        elif self.file_last_sec is not None and rec.sec <= self.file_last_sec:
            self.stats["ring_duplicates_skipped"] += 1
            return
        if self.native_high is not None and rec.sec < self.native_high - 5.0:
            self.gap("native_clock_went_back", "file", now,
                     detail="the log's elapsed time restarted: a new process wrote to this file",
                     native_from=self.native_high, native_to=rec.sec)
            self.native_high = None
            self.anchor = None
            self.buffer.clear()
            self.history_start = 0.0
        if self.history_start == -1.0:
            self.history_start = rec.sec
        self.native_high = rec.sec if self.native_high is None else max(self.native_high, rec.sec)
        if self.anchor is None or rec.sec >= self.anchor[1]:
            self.anchor = (now, rec.sec)
        self.last_activity = now
        self.buffer.append(rec)
        if len(self.buffer) > self.cfg.buffer_lines:
            old = self.buffer.popleft()
            self.stats["buffer_evicted"] += 1
            self.history_start = max(self.history_start or 0.0, old.sec)
        for record in self.unresolved:
            record["native_to"] = rec.sec
        self.unresolved = []
        for mark in self.open_marks:
            if rec.sec > mark.native:
                mark.post_agg.add(rec)
        if not rec.tags:
            return
        for tag in rec.tags:
            self.tag_counts[tag] += 1
        if set(rec.tags) & DISTINCT:
            row = signal_row(rec.cls, rec.level, rec.site, rec.fp, rec.tags, rec.sec, rec.byte)
            if rec.fields:
                row["fields"] = {k: v for k, v in rec.fields.items() if k != "rows"}
            if not self.store.append("signals.jsonl", row):
                self.stats["signal_rows_dropped"] += 1
        else:
            self.coal.add(rec)
        self.special(rec, now)

    def special(self, rec, now):
        tags = rec.tags
        if "ipc_summary" in tags:
            for service, command, tipc, result, count in rec.fields["rows"]:
                key = f"{service}::{command}{' (tipc)' if tipc else ''} -> {result}"
                if key in self.ipc or len(self.ipc) < 512:
                    self.ipc[key] = self.ipc.get(key, 0) + count
                else:
                    self.ipc_dropped_keys += 1
            self.ipc_unlisted_max = max(self.ipc_unlisted_max, rec.fields["unlisted_kinds"])
        if "sched_summary" in tags:
            core = self.sched.setdefault(rec.fields["core"], {"batches": 0, "switches": 0,
                                                              "to_idle": 0, "last_tick": 0})
            core["batches"] += 1
            core["switches"] += rec.fields["switches"]
            core["to_idle"] += rec.fields["to_idle"]
            core["last_tick"] = rec.fields["tick"]
        if "fs_short_read" in tags:
            occurrence = rec.fields["occurrence"]
            if self.short_read_last is not None and occurrence > self.short_read_last + 1:
                missed = occurrence - self.short_read_last - 1
                self.short_read_unlogged += missed
                self.gap("native_occurrences_not_logged", "native", now, lost=missed,
                         unit="short reads", detail="the occurrence counter skipped numbers",
                         native_from=rec.sec, native_to=rec.sec)
            self.short_read_last = max(self.short_read_last or 0, occurrence)
        if "mark_line" in tags:
            for mark in self.open_marks + self.marks:
                if mark.server_mark == (rec.fields["mark"], rec.fields["session"]):
                    mark.log_byte = rec.byte
        if "log_file_limit" in tags:
            self.gap("native_log_file_full", "native", now,
                     detail="the file stopped growing; lines after this are only in the ring",
                     native_from=rec.sec, lost=None)
            self.ring_active = True
        for tag in self.cfg.auto_mark:
            if tag in tags:
                self.auto_mark(tag, rec, now)
                break

    def auto_mark(self, tag, rec, now):
        if self.auto_marks >= self.cfg.max_auto_marks or now - self.last_auto < self.cfg.auto_cooldown:
            self.stats["auto_marks_suppressed"] += 1
            return
        self.auto_marks += 1
        self.last_auto = now
        self.open_mark(f"auto:{tag}", None, None, now, native=rec.sec)

    # ------------------------------------------------------------------ the debug server
    def poll_server(self, now):
        if self.link is None or now < self.next_status:
            return
        self.next_status = now + self.cfg.status_every
        try:
            status = self.link.call("status", {"fps_window_ms": 0}, timeout=5)
        except Busy:
            self.stats["status_skipped_busy"] += 1
            return
        except Exception as error:  # noqa: BLE001 - any failure is a gap, not a crash
            if self.server_seen:
                self.open_gap("server", "debug_server_unreachable", "server", now,
                              redact(str(error), self.names)[:120])
            elif not self.stats["server_waiting"]:
                self.stats["server_waiting"] += 1
                self.event("server_waiting", detail="not listening yet: it starts when a game "
                                                    "has loaded")
            self.server_ok = False
            return
        self.server_seen = True
        self.server_ok = True
        self.close_gap("server", now)
        log = status.get("log", {})
        ring = log.get("ring", {})
        file_state = log.get("file", {})
        if self.server_session is not None and status.get("session") != self.server_session:
            self.gap("debug_server_session_changed", "server", now,
                     detail="the game was booted again in this process or the emulator "
                            "restarted; the ring and present counts restart")
        self.server_session = status.get("session")
        if status.get("dump_dir"):
            self.dump_dir = status["dump_dir"]  # private: kept in memory for the disk guard
        self.server_info = {k: status.get(k) for k in ("program_id", "build", "scm_rev",
                                                       "renderer", "switch2", "log_filter")}
        row = {"presents": status.get("presents"), "fps": status.get("fps"),
               "paused": status.get("paused"), "uptime_s": status.get("uptime_s"),
               "native_est_s": self.native_est(now), "ring": ring,
               "file": {k: file_state.get(k) for k in ("bytes_written", "limit", "limit_reached")}}
        self.event("status", **row)
        if self.cfg.log is None and file_state.get("path") and \
                os.path.exists(file_state["path"]) and file_state["path"] != self.log_path:
            self.log_path = file_state["path"]
            self.offset = None
            self.event("source_switch", detail="the native log file the emulator reports")
            self.write_local()
        if file_state.get("limit_reached") and not self.ring_active:
            self.ring_active = True
            self.gap("native_log_file_full", "native", now, detail="the debug server reports "
                     "the log file at its size limit; reading the in-memory ring instead")
        filter_text = status.get("log_filter", "")
        if filter_text and not self.stats["filter_noted"]:
            self.stats["filter_noted"] += 1
            self.event("log_filter", filter=filter_text,
                       detail="lines the filter drops are never logged and cannot be recovered")
        if self.ring_active:
            self.poll_ring(now)

    ring_started = False

    def poll_ring(self, now):
        for _ in range(10):
            try:
                result = self.link.call("log_tail", {"after": self.ring_next, "limit": 2000},
                                        timeout=10)
            except Exception:  # noqa: BLE001
                self.stats["ring_poll_failed"] += 1
                return
            first = not self.ring_started
            self.ring_started = True
            # A first read from 0 reports everything evicted since boot; most of that is in the
            # file. Only lines after the file's last one count as missing, judged below.
            lost = 0 if first else result.get("lost", 0) + result.get("omitted", 0)
            if lost:
                self.gap("ring_lines_lost", "ring", now, lost=lost, unit="lines",
                         detail="the in-memory ring dropped lines before they were read")
            texts = result.get("lines", [])
            if first and texts:
                head = parse_line(texts[0].encode("utf-8", "replace"), None, False)
                if head is not None and (self.file_last_sec is None or
                                         head.sec > self.file_last_sec + 0.001):
                    self.gap("ring_started_after_file_end", "ring", now,
                             detail="lines between the last one in the file and the oldest in "
                                    "the ring may have been evicted; the count is not known",
                             native_from=self.file_last_sec, native_to=head.sec)
            for text in texts:
                self.ingest(text.encode("utf-8", "replace"), None, now, source="ring")
            self.ring_next = result.get("next", self.ring_next)
            if not result.get("more"):
                break
        # (the anchor follows lines, see ingest)

    # ------------------------------------------------------------------ marks
    def open_mark(self, label, pre, post, now, native=None):
        if len(self.marks) >= self.cfg.max_marks:
            self.event("loss", what="mark_refused", reason="max_marks", bound=self.cfg.max_marks)
            return None
        pre = self.cfg.pre if pre is None else min(float(pre), 3600.0)
        post = self.cfg.post if post is None else min(float(post), 3600.0)
        label = redact("".join(c if " " <= c <= "~" else "?" for c in str(label or ""))[:120],
                       self.names)
        notes = []
        correlation, uncertainty = "approximate", FLUSH_SLACK_S + self.cfg.poll
        present = None
        server_mark = None
        system_seq = None
        if native is not None:
            correlation, uncertainty = "log_line", 0.0
        elif self.link is not None and self.server_ok:
            try:
                result = self.link.call("mark", {}, timeout=5)
                present = result.get("presents")
                system_seq = result.get("system_event_seq") or None
                if result.get("logged") and "native_seconds" in result:
                    native = result["native_seconds"]
                    correlation, uncertainty = "exact", 0.0
                    server_mark = (result["mark"], result["session"])
                else:
                    notes.append("server mark not in the log (log filter): approximate")
            except Busy:
                notes.append("server busy with a detail capture: approximate")
            except Exception as error:  # noqa: BLE001
                notes.append("server mark failed: " + redact(str(error), self.names)[:100])
        elif self.link is not None:
            notes.append("debug server not reachable: approximate")
        if native is None:
            native = self.native_est(now)
        if native is None:
            native = 0.0
            correlation = "none"
            notes.append("no native log line seen yet")
        mark = Mark(len(self.marks) + 1, label, pre, post, native, correlation, uncertainty, now,
                    self.wall())
        mark.present = present
        mark.server_mark = server_mark
        mark.system_seq = system_seq
        mark.notes = notes
        mark.pre_agg = WindowAgg(native, self.cfg.raw_max_lines)
        mark.post_agg = WindowAgg(native, self.cfg.raw_max_lines)
        for rec in reversed(self.buffer):
            if rec.sec < native - pre:
                break
            (mark.pre_agg if rec.sec <= native else mark.post_agg).add(rec)
        self.marks.append(mark)
        self.open_marks.append(mark)
        self.event("mark", mark=mark.id, label=label, native_s=native, correlation=correlation,
                   uncertainty_s=uncertainty, present=present, pre_s=pre, post_s=post, notes=notes)
        if self.cfg.detail:
            self.start_detail(mark, now)
        return mark

    def advance_marks(self, now):
        for mark in list(self.open_marks):
            reached = self.native_high is not None and self.native_high >= mark.native + mark.post
            if reached or now - mark.mono >= mark.post + self.cfg.grace or self.stopping:
                self.close_mark(mark, now)

    def fetch_system_events(self, mark):
        """The system event rows around a mark, paged: (rows, info), or None without a reply.

        The window starts SYS_ROWS_BEFORE rows before the mark's own row (its sequence number,
        not the oldest row the ring still has) and runs to the newest, SYS_PAGE_ROWS rows a page
        and at most SYS_MAX_PAGES pages. `lost` counts rows before the window start that the ring
        had already evicted; `more` says rows were left unread when the page bound was reached."""
        seq = mark.system_seq
        after = max(0, seq - SYS_ROWS_BEFORE) if seq else 0
        start = after
        rows, pages, lost, reply, more = [], 0, 0, None, False
        while pages < SYS_MAX_PAGES:
            reply = self.link.call("system_events", {"after": after, "limit": SYS_PAGE_ROWS},
                                   timeout=2.0)
            if not isinstance(reply, dict) or "events" not in reply:
                break
            pages += 1
            if pages == 1:
                lost = reply.get("lost", 0)
            rows += reply["events"]
            more = bool(reply.get("more"))
            following = reply.get("next", after)
            if not more or following <= after:
                break
            after = following
        if pages == 0:
            return None
        return rows, {"reply": reply, "mark_seq": seq, "window_after_seq": start, "pages": pages,
                      "lost": lost, "more": more}

    def close_mark(self, mark, now):
        self.open_marks.remove(mark)
        if mark.detail_thread is not None:
            mark.detail_thread.join(timeout=1.0)
        high_line = self.native_high if self.native_high is not None else mark.native
        quiet = 0.0
        ended = self.stop_reason in EXIT_REASONS
        high = high_line
        if not ended:
            # The process was alive and the file is flushed about once a second, so a polled
            # silence is evidence of a quiet log for all but the last second.
            est = self.native_est(now)
            if est is not None:
                high = max(high_line, est - FLUSH_SLACK_S)
                quiet = max(0.0, min(high, mark.native + mark.post) - max(high_line, mark.native))
        post_covered = max(0.0, min(mark.post, high - mark.native))
        history = self.history_start if self.history_start is not None else 0.0
        earliest = max(history, mark.native - mark.pre)
        pre_covered = max(0.0, min(mark.pre, mark.native - earliest))
        window_from, window_to = mark.native - mark.pre, mark.native + mark.post
        overlapping = [g for g in self.gaps
                       if (g["native_from"] is None or g["native_from"] <= window_to) and
                       (g.get("native_to") is None or g["native_to"] >= window_from)]
        reasons = []
        if pre_covered < mark.pre - FLUSH_SLACK_S:
            # The log starts with the process: time before that never existed to record.
            reasons.append("pre_before_process_start" if history <= FLUSH_SLACK_S
                           else "pre_window_short")
        if post_covered < mark.post - FLUSH_SLACK_S:
            reasons.append("post_window_short:" + (self.stop_reason or "closed_early"))
        reasons += sorted({"gap:" + g["cause"] for g in overlapping})
        if mark.pre_agg.rows_dropped or mark.post_agg.rows_dropped:
            reasons.append("window_rows_dropped")
        if mark.pre_agg.raw_dropped or mark.post_agg.raw_dropped:
            reasons.append("raw_lines_dropped")
        if self.store.capped:
            reasons.append("recorder_size_cap")
        if mark.correlation == "none":
            reasons.append("no_native_time")
        if quiet > FLUSH_SLACK_S:
            mark.notes.append(f"no log line in the last {quiet:.1f} s of the post window: a quiet "
                              "log, or a log filter that hides the 10 s report")
        window = {
            "schema": SCHEMA, "mark": mark.id, "label": mark.label, "wall": iso(mark.wall),
            "native_seconds": mark.native, "correlation": mark.correlation,
            "uncertainty_s": mark.uncertainty, "present": mark.present,
            "log_byte": mark.log_byte, "notes": mark.notes,
            "pre": {"requested_s": mark.pre, "covered_s": round(pre_covered, 3),
                    "shortfall_s": round(mark.pre - pre_covered, 3),
                    **mark.pre_agg.to_json()},
            "post": {"requested_s": mark.post, "covered_s": round(post_covered, 3),
                     "shortfall_s": round(mark.post - post_covered, 3),
                     "quiet_s": round(quiet, 3), **mark.post_agg.to_json()},
            "gaps": overlapping, "detail": mark.detail,
            # complete: the whole requested window was observed. evidence_lost: something that
            # existed was missed; a mark early in boot is incomplete but loses nothing.
            "complete": not reasons,
            "evidence_lost": any(r != "pre_before_process_start" for r in reasons),
            "incomplete_reasons": reasons,
        }
        sys_events_list = []
        if self.link is not None:
            fetched = self.fetch_system_events(mark)
            if fetched is not None:
                sys_events_list, info = fetched
                sys_lines = [json.dumps(ev, separators=(",", ":")) for ev in sys_events_list]
                self.store.write(f"marks/{mark.id:03d}/system_events.jsonl",
                                 ("\n".join(sys_lines) + "\n") if sys_lines else "")
                sources = info["reply"].get("sources") or []
                ring = info["reply"].get("ring") or {}
                window["system_log"] = {
                    "count": len(sys_lines),
                    "dropped_events": ring.get("dropped", 0),
                    "evicted_events": ring.get("evicted", 0),
                    "missing_sources": [
                        {"name": s.get("name"), "reason": s.get("missing_reason")}
                        for s in sources if not s.get("enabled")
                    ],
                    # Where the rows were taken from: the mark's own sequence number (None when
                    # the server gave none, then from the oldest row), the pages read, rows
                    # already evicted before the window, and whether the page bound cut it short.
                    "mark_seq": info["mark_seq"],
                    "window_after_seq": info["window_after_seq"],
                    "pages": info["pages"],
                    "lost_rows": info["lost"],
                    "more_rows_unread": info["more"],
                }
        def agg_signal_counts(agg):
            counts = collections.Counter()
            for (_cls, _lvl, _site, _fp, tags), row in agg.rows.items():
                for t in tags:
                    counts[t] += row.get("count", 1)
            for m in agg.metrics:
                t = m.get("kind")
                if t:
                    counts[t] += 1
                if t == "report":
                    if m.get("over_100ms", 0) > 0:
                        counts["frame_over_100ms"] += m["over_100ms"]
                    if m.get("underruns", 0) > 0:
                        counts["audio_underrun"] += m["underruns"]
            return dict(counts)

        pre_sig = agg_signal_counts(mark.pre_agg)
        post_sig = agg_signal_counts(mark.post_agg)
        suspected = []
        combined = collections.Counter(pre_sig) + collections.Counter(post_sig)
        if combined.get("host_crash"):
            suspected.append({"cause": "host_process_crash", "basis": "host_crash_log_line",
                              "confidence": "suspected", "evidence_count": combined["host_crash"]})
        if combined.get("device_lost"):
            suspected.append({"cause": "gpu_device_lost", "basis": "vulkan_device_lost_signal",
                              "confidence": "suspected", "evidence_count": combined["device_lost"]})
        if combined.get("frame_over_100ms"):
            suspected.append({"cause": "long_frame_hitch_over_100ms", "basis": "10s_report_frame_over_100ms",
                              "confidence": "suspected", "evidence_count": combined["frame_over_100ms"]})
        if combined.get("audio_underrun"):
            suspected.append({"cause": "audio_sink_starvation", "basis": "10s_report_audio_underrun",
                              "confidence": "suspected", "evidence_count": combined["audio_underrun"]})
        if combined.get("fs_short_read"):
            suspected.append({"cause": "storage_short_read", "basis": "istorage_short_read_warning",
                              "confidence": "suspected", "evidence_count": combined["fs_short_read"]})
        hitch_by_detail = collections.Counter()
        for ev in sys_events_list:
            if ev.get("kind") == "FrameHitch":
                hitch_by_detail[ev.get("detail") or "Cpu/Unknown"] += 1
        for detail_name, cnt in hitch_by_detail.items():
            suspected.append({"cause": detail_name, "basis": "system_log_frame_hitch",
                              "confidence": "suspected", "evidence_count": cnt})
        if not suspected:
            suspected.append({"cause": "undetermined", "basis": "no_anomalous_signal_in_window",
                              "confidence": "unknown", "evidence_count": 0})

        sys_info = window.get("system_log", {})
        window["assessment"] = {
            "observed": {
                "mark": mark.id,
                "label": mark.label,
                "native_seconds": mark.native,
                "present": mark.present,
                "log_byte": mark.log_byte,
                "correlation": mark.correlation,
                "uncertainty_s": mark.uncertainty,
                "pre_covered_s": round(pre_covered, 3),
                "post_covered_s": round(post_covered, 3),
                "pre_signals": pre_sig,
                "post_signals": post_sig,
                "gaps": [g["cause"] for g in overlapping],
                "system_log_events": sys_info.get("count", 0),
                "dropped_events": sys_info.get("dropped_events", 0),
                "missing_sources": sys_info.get("missing_sources", []),
            },
            "suspected_causes": suspected,
        }
        key = f"marks/{mark.id:03d}/window.json"
        self.store.write(key, json.dumps(window, indent=1))
        if self.cfg.raw_log:
            lines = [f"[{rel:+.3f}] pre  {redact(text, self.names)}" for rel, text in mark.pre_agg.raw]
            lines += [f"[{rel:+.3f}] post {redact(text, self.names)}" for rel, text in mark.post_agg.raw]
            self.store.write(f"private/marks/{mark.id:03d}/log-window.txt", "\n".join(lines) + "\n",
                             essential=False)
        self.event("mark_closed", mark=mark.id, complete=not reasons, reasons=reasons)

    # ------------------------------------------------------------------ native detail
    def start_detail(self, mark, now):
        if self.cfg.inline_detail:
            self.run_detail(mark)
        else:
            mark.detail_thread = threading.Thread(target=self.run_detail, args=(mark,), daemon=True)
            mark.detail_thread.start()

    def run_detail(self, mark):
        for kind in self.cfg.detail:
            now = self.clock()
            skip = None
            if self.detail_count >= self.cfg.max_detail:
                skip = "max_detail"
            elif now - self.last_detail < self.cfg.detail_cooldown:
                skip = "cooldown"
            elif self.link is None and kind != "draws":
                skip = "no_debug_server"
            if skip is None:
                target = self.dump_dir if self.dump_dir and os.path.isdir(self.dump_dir) \
                    else self.cfg.out
                try:
                    free = shutil.disk_usage(target).free
                    held = tree_bytes(target if target != self.cfg.out
                                      else os.path.join(self.cfg.out, "private"))
                except OSError:
                    free, held = 0, 0  # cannot tell: do not write
                if free < self.cfg.min_free_gb * 1024 ** 3:
                    skip = "low_disk"
                elif held > self.cfg.max_detail_mb * 1024 ** 2:
                    skip = "detail_size_cap"
            if skip:
                mark.detail.append({"kind": kind, "ok": False, "skipped": skip})
                self.event("detail_skipped", mark=mark.id, what=kind, reason=skip)
                continue
            self.detail_count += 1
            self.last_detail = now
            began = self.clock()
            try:
                if kind == "frame":
                    result = self.link.call("capture_frame", {
                        "sampled": False, "buffers": False, "manifest_lines": 0,
                        "timeout_s": self.cfg.detail_timeout}, timeout=self.cfg.detail_timeout + 30,
                        wait=False)
                    entry = {"kind": kind, "ok": True,
                             "directory": os.path.basename(str(result.get("directory", "")).rstrip("/\\")),
                             "kinds": result.get("kinds"), "entries_total": result.get("entries_total")}
                elif kind == "sample":
                    result = self.link.call("sample", {
                        "seconds": self.cfg.sample_seconds, "threads": "GPU,GPUBack",
                        "max_chars": 0}, timeout=self.cfg.sample_seconds + 90, wait=False)
                    entry = {"kind": kind, "ok": True,
                             "file": os.path.basename(str(result.get("file", "")))}
                elif kind == "draws":
                    entry = self.draw_list()
                else:
                    entry = {"kind": kind, "ok": False, "error": "unknown detail kind"}
            except Busy:
                entry = {"kind": kind, "ok": False, "skipped": "server_busy"}
            except Exception as error:  # noqa: BLE001
                entry = {"kind": kind, "ok": False,
                         "error": redact(str(error), self.names)[:200]}
            entry["seconds"] = round(self.clock() - began, 2)
            mark.detail.append(entry)
            self.event("detail", mark=mark.id, what=kind,
                       **{k: v for k, v in entry.items() if k != "kind"})

    def draw_list(self):
        folder = self.cfg.diag_dir
        if not folder:
            return {"kind": "draws", "ok": False, "skipped": "no_diag_dir"}
        before = set(f for f in os.listdir(folder) if f.startswith("draw-list-"))
        trigger = os.path.join(folder, "draw-list")
        with open(trigger, "x"):
            pass
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            new = sorted(f for f in os.listdir(folder) if f.startswith("draw-list-")
                         and f not in before)
            if new:
                return {"kind": "draws", "ok": True, "files": new}
            time.sleep(0.05)
        try:
            os.remove(trigger)
        except OSError:
            pass
        return {"kind": "draws", "ok": False, "error": "no draw list within 3 s"}

    # ------------------------------------------------------------------ requests and loop
    def poll_controls(self, now):
        folder = os.path.join(self.cfg.out, "control")
        while self.hotkey_notes is not None:
            try:
                note, code = self.hotkey_notes.get_nowait()
            except queue.Empty:
                break
            if note == "hotkey_ignored_not_foreground":
                self.stats[note] += 1
            else:
                self.event(note, win32_error=code or None)
        for name in sorted(os.listdir(folder)):
            if name == "stop.json":
                try:
                    os.remove(os.path.join(folder, name))
                except OSError:
                    pass
                self.stop("stop_requested")
                continue
            if not (name.startswith("mark-") and name.endswith(".json")):
                continue
            path = os.path.join(folder, name)
            try:
                with open(path, encoding="utf-8") as handle:
                    request = json.load(handle)
                os.remove(path)
            except (OSError, ValueError):
                self.event("control_unreadable", name=name[:40])
                try:
                    os.remove(path)
                except OSError:
                    pass
                continue
            self.requests.put((request.get("label", ""), request.get("pre"), request.get("post")))
        while True:
            try:
                label, pre, post = self.requests.get_nowait()
            except queue.Empty:
                break
            self.open_mark(label, pre, post, now)

    def check_exit(self, now):
        if self.cfg.duration is not None and now - self.t0 >= self.cfg.duration:
            self.stop("duration")
        if self.process is not None and self.process.poll() is not None:
            self.exit_code = self.process.returncode
            self.stop("emulator_exited")
        elif self.cfg.pid and self.pid_alive and not self.pid_alive(self.cfg.pid):
            self.stop("emulator_exited")
        elif not self.cfg.pid and self.process is None and self.cfg.silence_exit and \
                now - self.last_activity >= self.cfg.silence_exit and not self.server_ok and \
                self.native_high is not None:
            self.stop("exit_inferred_from_silence")

    def stop(self, reason):
        if not self.stopping:
            self.stopping = True
            self.stop_reason = reason

    def flush_signals(self, now, final=False):
        high = None if final or self.native_high is None else int(self.native_high)
        for row in self.coal.drain(None if final else high):
            if not self.store.append("signals.jsonl", row):
                self.stats["signal_rows_dropped"] += 1

    last_step = None
    dump_dir = None
    hotkey_notes = None

    def step(self):
        now = self.clock()
        if self.last_step is not None and now - self.last_step > max(5.0, self.cfg.poll * 20):
            self.gap("recorder_stall", "recorder", now, detail=f"no poll for {now - self.last_step:.1f} s "
                     "(machine asleep or recorder starved); the log was still read from the offset")
        self.last_step = now
        self.poll_controls(now)
        self.poll_file(now)
        self.poll_server(now)
        if now - self.last_sync >= 30.0 and self.anchor is not None:
            self.last_sync = now
            self.event("sync", native_s=self.native_est(now), log_byte=self.offset)
        self.advance_marks(now)
        self.flush_signals(now)
        self.check_exit(now)
        self.store.flush()
        if self.store.capped and not self.stats["size_cap_noted"]:
            self.stats["size_cap_noted"] += 1
            self.gap("recorder_size_cap", "recorder", now, detail="the session size bound was "
                     "reached; rows after this are counted in store_dropped, not written")

    def run(self):
        try:
            while not self.stopping:
                self.step()
                time.sleep(self.cfg.poll)
        except KeyboardInterrupt:
            self.stop("interrupted")
        finally:
            self.finish()

    def finish(self):
        if self.finished:
            return
        self.finished = True
        now = self.clock()
        for key in list(self.open_gaps):
            self.close_gap(key, now)
        self.stopping = True
        self.stop_reason = self.stop_reason or "stopped"
        try:
            self.poll_file(now)
        except OSError:
            pass
        for mark in list(self.open_marks):
            self.close_mark(mark, now)
        self.flush_signals(now, final=True)
        self.event("session_end", reason=self.stop_reason, exit_code=self.exit_code,
                   exit_known=self.exit_code is not None)
        self.write_session(final=True)
        self.store.close()

    # ------------------------------------------------------------------ session.json
    def write_session(self, final=False):
        cfg = self.cfg
        incomplete = [m.id for m in self.marks if m not in self.open_marks and
                      self.window_incomplete(m)]
        document = {
            "schema": SCHEMA, "tool": "incident.py", "state": "finished" if final else "running",
            "started": iso(self.wall0), "ended": iso(self.wall()) if final else None,
            "stop_reason": self.stop_reason, "exit_code": self.exit_code,
            "privacy": {
                "raw_log": cfg.raw_log, "detail": list(cfg.detail),
                "shareable": ["session.json", "timeline.jsonl", "signals.jsonl",
                              "marks/*/window.json"],
                "private": "private/ holds absolute paths, redacted log text (--raw-log) and "
                           "native captures: review before sharing; nothing is uploaded",
                "guest_memory": "never read",
                "size_bound": "bounds.max_bytes covers the files listed as shareable and the "
                              "private log windows only; native captures are outside it and "
                              "guarded by max_detail, max_detail_mb and the free-disk floor",
            },
            "bounds": {"pre_s": cfg.pre, "post_s": cfg.post, "buffer_lines": cfg.buffer_lines,
                       "max_bytes": cfg.max_bytes, "max_marks": cfg.max_marks,
                       "max_detail": cfg.max_detail, "catchup_mb": cfg.catchup_mb,
                       "status_every_s": cfg.status_every},
            "counts": {**dict(self.stats), "marks": len(self.marks),
                       "store_bytes": self.store.used, "signals": dict(self.tag_counts)},
            "store_dropped": dict(self.store.dropped),
            "gaps": dict(self.gap_causes), "gaps_first_200": self.gaps[:200],
            "marks_incomplete": incomplete,
            "history_start_native_s": self.history_start,
            "ipc": {"calls": self.ipc, "dropped_keys": self.ipc_dropped_keys,
                    "max_unlisted_kinds": self.ipc_unlisted_max,
                    "note": "the native summary lists only its top 24 kinds per 5 s"},
            "short_reads": {"native_occurrences_not_logged": self.short_read_unlogged},
            "guest_scheduler": {"per_core": {str(k): v for k, v in sorted(self.sched.items())},
                                "note": "sums of the batches the log showed; one batch is 4096 "
                                        "actual switches; not a thread-level trace"},
            "server": {"calls": dict(self.link.calls) if self.link else {},
                       "reachable_ever": self.server_seen, **self.server_info},
            "scrubbed_env": list(cfg.scrubbed_env), "preset": cfg.preset,
            "coverage": COVERAGE,
            "complete": final and not self.gaps and not incomplete and not self.store.capped,
        }
        self.store.replace_json("session.json", document)

    def window_incomplete(self, mark):
        path = os.path.join(self.cfg.out, "marks", f"{mark.id:03d}", "window.json")
        try:
            with open(path, encoding="utf-8") as handle:
                return not json.load(handle)["complete"]
        except (OSError, ValueError, KeyError):
            return True


ESSENTIAL = {"gap", "gap_end", "mark", "mark_closed", "loss", "session_end", "detail",
             "detail_skipped", "session_start"}
LOSS_UNTIL_NEXT_LINE = {"log_file_replaced_or_truncated"}
# The game is gone: silence after its last line is not evidence of a quiet game.
EXIT_REASONS = {"emulator_exited", "exit_inferred_from_silence"}


# ---------------------------------------------------------------------- launching
def launch_env(base, address, dump_dir, preset, diag_dir=None):
    """The environment of a game this tool starts: the debug server, no memory writes, no raw
    IPC trace. The diagnostic preset adds aggregate summaries only (IPC, NPad totals, GPU
    timeline); the basic preset adds none of them."""
    env = dict(base)
    scrubbed = [name for name in SCRUBBED_ENV if name in env]
    for name in scrubbed:
        del env[name]
    env["EDEN_NX2_DEBUG_SERVER"] = address
    env["EDEN_NX2_DEBUG_DIR"] = dump_dir
    if preset == "diagnostic":
        env["EDEN_NX2_DIAGNOSTIC"] = "1"
        env["EDEN_NX2_GPU_TRACE"] = "1"
        env["EDEN_NX2_AUDIO_SUMMARY"] = "1"
        env["EDEN_NX2_FS_SUMMARY"] = "1"
        env["EDEN_NX2_IPC_SUMMARY"] = "1"
        env["EDEN_NX2_HID_SUMMARY"] = "1"
        env["EDEN_NX2_TIMELINE"] = "1"
        env["EDEN_NX2_SCHED_SUMMARY"] = "1"
    if diag_dir:
        env["EDEN_NX2_DIAG_DIR"] = diag_dir
    return env, scrubbed


def emulator_running(name="eglantine"):
    try:
        if os.name == "nt":
            out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}.exe", "/NH", "/FO",
                                  "CSV"], capture_output=True, text=True, timeout=10).stdout
            return f"{name}.exe".lower() in out.lower()
        return subprocess.run(["pgrep", "-x", name], capture_output=True,
                              timeout=10).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def process_alive(pid):
    if os.name == "nt":
        import ctypes
        handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)
        if not handle:
            return False
        try:
            code = ctypes.c_ulong()
            ok = ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
            return bool(ok) and code.value == 259
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def free_address():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{probe.getsockname()[1]}"


def tree_bytes(path):
    total = 0
    for folder, _, names in os.walk(path):
        for name in names:
            try:
                total += os.path.getsize(os.path.join(folder, name))
            except OSError:
                pass
    return total


def start_hotkey(recorder):
    """Ctrl+Alt+F8 anywhere, Windows only. The thread only queues: marks and notes are
    handled on the recorder's own thread, which owns the files."""
    if os.name != "nt":
        raise SystemExit("--hotkey is Windows only")
    import ctypes
    from ctypes import wintypes
    user = ctypes.WinDLL("user32", use_last_error=True)  # own copy: argtypes stay ours
    user.RegisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT]
    user.RegisterHotKey.restype = wintypes.BOOL
    user.UnregisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int]
    user.UnregisterHotKey.restype = wintypes.BOOL
    user.PeekMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT,
                                  wintypes.UINT, wintypes.UINT]
    user.PeekMessageW.restype = wintypes.BOOL
    user.GetForegroundWindow.argtypes = []
    user.GetForegroundWindow.restype = wintypes.HWND
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowThreadProcessId.restype = wintypes.DWORD
    notes = recorder.hotkey_notes = queue.Queue()

    def loop():
        # Registered and read on this one thread: a thread-level hotkey belongs to its thread.
        if not user.RegisterHotKey(None, 1, 0x4003, 0x77):  # CTRL|ALT|NOREPEAT, F8
            notes.put(("hotkey_unavailable", ctypes.get_last_error()))
            return
        notes.put(("hotkey_registered", 0))
        message = wintypes.MSG()
        try:
            while not recorder.stopping:
                if not user.PeekMessageW(ctypes.byref(message), None, 0x312, 0x312, 1):
                    time.sleep(0.05)
                    continue
                if recorder.cfg.pid:
                    owner = wintypes.DWORD()
                    user.GetWindowThreadProcessId(user.GetForegroundWindow(), ctypes.byref(owner))
                    if owner.value != recorder.cfg.pid:
                        notes.put(("hotkey_ignored_not_foreground", 0))
                        continue
                recorder.requests.put(("hotkey", None, None))
        finally:
            user.UnregisterHotKey(None, 1)

    threading.Thread(target=loop, daemon=True).start()


def start_stdin(recorder):
    def loop():
        for line in sys.stdin:
            recorder.requests.put((line.strip()[:120] or "stdin", None, None))

    threading.Thread(target=loop, daemon=True).start()


# ---------------------------------------------------------------------- commands
def cmd_watch(args):
    out = args.out or time.strftime("incident-%Y%m%d-%H%M%S")
    if os.path.exists(os.path.join(out, "session.json")):
        print(f"{out} already holds a session", file=sys.stderr)
        return 2
    os.makedirs(out, exist_ok=True)
    cfg = Config(out=out, log=args.log, profile=args.profile, exe=args.launch, pid=args.pid,
                 pre=args.pre, post=args.post, max_mb=args.max_mb, max_marks=args.max_marks,
                 buffer_lines=args.buffer_lines, raw_log=args.raw_log,
                 detail=tuple(args.detail), diag_dir=args.diag_dir, duration=args.duration,
                 status_every=args.status_every, preset=args.preset if args.launch else None,
                 auto_mark=tuple(x for x in args.auto_mark.split(",") if x),
                 catchup_mb=args.catchup_mb, hotkey=args.hotkey,
                 stdin=args.stdin or sys.stdin.isatty())
    process = None
    server = args.server
    launched_at = None
    if args.launch:
        if emulator_running() and not args.allow_running:
            print("an Eglantine process is already running; close it normally first (or pass "
                  "--allow-running). Nothing was started.", file=sys.stderr)
            return 2
        server = server or free_address()
        native_dir = os.path.join(out, "private", "native")
        os.makedirs(native_dir, exist_ok=True)
        diag = None
        if "draws" in cfg.detail:
            diag = cfg.diag_dir = os.path.join(native_dir, "draws")
            os.makedirs(diag, exist_ok=True)
        env, cfg.scrubbed_env = launch_env(os.environ, server, native_dir, cfg.preset, diag)
        launched_at = time.time()
        process = subprocess.Popen([args.launch] + list(args.game_args), env=env,
                                   cwd=os.path.dirname(os.path.abspath(args.launch)))
        cfg.pid = process.pid
    elif "draws" in cfg.detail and not cfg.diag_dir:
        print("--detail draws needs --diag-dir (the emulator's EDEN_NX2_DIAG_DIR)", file=sys.stderr)
        return 2
    link = ServerLink(debug_mcp.Emulator(server)) if server else None
    recorder = Recorder(cfg, link, pid_alive=process_alive)
    recorder.process = process
    recorder.launched_at = launched_at
    if cfg.hotkey:
        start_hotkey(recorder)
    if cfg.stdin:
        start_stdin(recorder)
    print(f"recording to {out}; mark with: incident.py mark --session {out}"
          + ("; Ctrl+Alt+F8" if cfg.hotkey else "") + "; Ctrl+C to finish", flush=True)
    recorder.run()
    print(f"finished: {recorder.stop_reason}; {len(recorder.marks)} marks, "
          f"{sum(recorder.gap_causes.values())} gaps; see {os.path.join(out, 'session.json')}")
    return 0


def cmd_mark(args):
    path = os.path.join(args.session, "session.json")
    try:
        with open(path, encoding="utf-8") as handle:
            state = json.load(handle).get("state")
    except (OSError, ValueError):
        state = None
    if state != "running":
        print(f"{args.session} has no running session", file=sys.stderr)
        return 1
    request = {"label": args.label[:120]}
    if args.pre is not None:
        request["pre"] = args.pre
    if args.post is not None:
        request["post"] = args.post
    name = f"mark-{time.time_ns()}-{os.getpid()}"
    base = os.path.join(args.session, "control", name)
    with open(base + ".tmp", "w", encoding="utf-8") as handle:
        json.dump(request, handle)
    os.replace(base + ".tmp", base + ".json")
    print("mark queued")
    return 0


def cmd_stop(args):
    """Orderly finish: open marks are closed with what was seen, session.json is completed."""
    path = os.path.join(args.session, "session.json")
    try:
        with open(path, encoding="utf-8") as handle:
            state = json.load(handle).get("state")
    except (OSError, ValueError):
        state = None
    if state != "running":
        print(f"{args.session} has no running session", file=sys.stderr)
        return 1
    base = os.path.join(args.session, "control", "stop")
    with open(base + ".tmp", "w", encoding="utf-8") as handle:
        handle.write("{}")
    os.replace(base + ".tmp", base + ".json")
    print("stop requested")
    return 0


def check_session(root):
    """Problems found in a session folder; an empty list means it keeps its promises."""
    problems = []
    try:
        with open(os.path.join(root, "session.json"), encoding="utf-8") as handle:
            session = json.load(handle)
    except (OSError, ValueError) as error:
        return [f"session.json: {error}"]
    if session.get("schema") != SCHEMA:
        problems.append("session.json: unknown schema")
    if session.get("state") != "finished":
        problems.append("session.json: the session is not finished")
    shareable = []
    private_size = 0
    total = 0
    for folder, _, names in os.walk(root):
        relative = os.path.relpath(folder, root).replace("\\", "/")
        for name in names:
            full = os.path.join(folder, name)
            size = os.path.getsize(full)
            if relative == "private" or relative.startswith("private/"):
                private_size += size
                continue
            if relative == "control" and name.endswith(".tmp"):
                continue
            total += size
            shareable.append(full)
    if total > session["bounds"]["max_bytes"]:
        problems.append(f"size: {total} bytes exceed the bound {session['bounds']['max_bytes']}")
    names = private_names()
    last = -1.0
    for full in shareable:
        with open(full, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        shown = os.path.relpath(full, root)
        if PATHS.search(text):
            problems.append(f"{shown}: contains an absolute path")
        for name in names:
            if re.search(re.escape(name), text, flags=re.I):
                problems.append(f"{shown}: contains the user name")
        if full.endswith(".jsonl"):
            for number, line in enumerate(text.splitlines(), 1):
                try:
                    row = json.loads(line)
                except ValueError:
                    problems.append(f"{shown}:{number}: not JSON")
                    continue
                for forbidden in ("text", "message", "raw"):
                    if forbidden in row:
                        problems.append(f"{shown}:{number}: free text field {forbidden}")
                if shown == "timeline.jsonl":
                    if row["m"] < last:
                        problems.append(f"{shown}:{number}: time went back")
                    last = row["m"]
    for mark_id in range(1, session["counts"].get("marks", 0) + 1):
        path = os.path.join(root, "marks", f"{mark_id:03d}", "window.json")
        try:
            with open(path, encoding="utf-8") as handle:
                window = json.load(handle)
        except (OSError, ValueError):
            problems.append(f"marks/{mark_id:03d}/window.json: missing or unreadable")
            continue
        for side in ("pre", "post"):
            if window[side]["covered_s"] > window[side]["requested_s"] + 1e-6:
                problems.append(f"mark {mark_id}: {side} covers more than was asked")
        if window["complete"] and window["incomplete_reasons"]:
            problems.append(f"mark {mark_id}: complete with reasons")
        if not window["complete"] and not window["incomplete_reasons"]:
            problems.append(f"mark {mark_id}: incomplete without a reason")
        if window["complete"] and any(window[side]["shortfall_s"] > FLUSH_SLACK_S
                                      for side in ("pre", "post")):
            problems.append(f"mark {mark_id}: complete but a side covers less than was asked")
        if window["evidence_lost"] != any(r != "pre_before_process_start"
                                          for r in window["incomplete_reasons"]):
            problems.append(f"mark {mark_id}: evidence_lost disagrees with the reasons")
    return problems


def cmd_check(args):
    problems = check_session(args.session)
    for problem in problems:
        print(problem)
    print("ok" if not problems else f"{len(problems)} problems")
    return 1 if problems else 0


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    watch = sub.add_parser("watch", help="record a running or launched emulator")
    watch.add_argument("--out", help="session folder (default incident-<time>)")
    watch.add_argument("--log", help="native log file (default: found from the profile)")
    watch.add_argument("--profile", help="emulator user folder holding log/")
    watch.add_argument("--server", help="host:port of EDEN_NX2_DEBUG_SERVER of the running game")
    watch.add_argument("--pid", type=int, help="emulator process id, to see it exit")
    watch.add_argument("--launch", metavar="EXE", help="start this emulator instead of attaching")
    watch.add_argument("--preset", choices=("basic", "diagnostic"), default="basic",
                       help="with --launch: diagnostic also sets EDEN_NX2_IPC_SUMMARY=1")
    watch.add_argument("--allow-running", action="store_true")
    watch.add_argument("--pre", type=float, default=30.0, help="seconds kept before a mark")
    watch.add_argument("--post", type=float, default=30.0, help="seconds kept after a mark")
    watch.add_argument("--max-mb", type=float, default=64, help="bound on the session folder")
    watch.add_argument("--max-marks", type=int, default=25)
    watch.add_argument("--buffer-lines", type=int, default=100000)
    watch.add_argument("--catchup-mb", type=float, default=32)
    watch.add_argument("--status-every", type=float, default=10.0)
    watch.add_argument("--duration", type=float, help="stop after this many seconds")
    watch.add_argument("--raw-log", action="store_true",
                       help="also keep redacted log text of each window under private/")
    watch.add_argument("--detail", action="append", default=[], choices=("frame", "sample", "draws"),
                       help="native detailed capture at each mark (local files, off by default)")
    watch.add_argument("--diag-dir", help="the emulator's EDEN_NX2_DIAG_DIR, for --detail draws")
    watch.add_argument("--auto-mark", default="host_crash,device_lost",
                       help="signal tags that mark by themselves (comma list, empty for none)")
    watch.add_argument("--hotkey", action="store_true", help="Ctrl+Alt+F8 marks (Windows)")
    watch.add_argument("--stdin", action="store_true", help="a line on stdin marks")
    watch.add_argument("game_args", nargs="*", help="with --launch: arguments for the emulator")
    mark = sub.add_parser("mark", help="ask a running recorder for a mark")
    mark.add_argument("--session", required=True)
    mark.add_argument("--label", default="")
    mark.add_argument("--pre", type=float)
    mark.add_argument("--post", type=float)
    stop = sub.add_parser("stop", help="finish a running recorder in an orderly way")
    stop.add_argument("--session", required=True)
    check = sub.add_parser("check", help="verify a finished session folder")
    check.add_argument("session")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    return {"watch": cmd_watch, "mark": cmd_mark, "stop": cmd_stop,
            "check": cmd_check}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
