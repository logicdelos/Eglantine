#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Behaviour of the incident recorder against a synthetic native log and a fake debug server.

The log lines are built from the emulator's own format strings (nx2_bottleneck.cpp,
service.cpp, nx2_gsv.h, fs_i_storage.cpp, logging.cpp, nx2_debug_tools.cpp). No emulator runs, so
nothing here shows that a real game produces these lines: it shows that the recorder keeps what
those lines say, and says what it did not keep.
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
# -I leaves the script's directory off sys.path.
sys.path.insert(0, HERE)

import incident  # noqa: E402

SITE = "core/x.cpp:1:F"
REPORT = ("Last 10 s: 59.8 fps (frame 16.7 ms, longest 340 ms, 2 over 100 ms). Limited by: the "
          "guest; a file read took 340 ms; audio underruns: 3")
FRAMES = ("Frames: 598 shown | late (> 1.5x the median interval) 4 | longest 340.1 ms | "
          "median 16.6 ms | 99th 33.4 ms")
SHORT_READ = ("IStorage read length mismatch: requested=4096 returned=100 "
              "(occurrence 7 since process start)")
IPC = "ipc summary (30 kinds) | 12x nvdrv::3 -> 0x0 | 5x fsp-srv::8 (tipc) -> 0x2f8002"
HID = "NPad delivery: 300 updates, 290 published samples, 12 samples with buttons held"
SCHED = "Guest scheduler: core=2 tick=123456789 switches=4096 switches_to_idle=1500"
TIMELINE = ("nx2 timeline (60 frames): frame 16.70 ms; 3.0 lists, 120 entries; first submit 1.20 ms "
            "after the previous present, last 0.80 ms before this one; front done -0.50 ms and "
            "back done +1.25 ms vs the present; fence wake 0.30 ms after the back; waited 2.10 "
            "ms; front CPU 4.00 ms, back CPU 5.50 ms per frame")
LIMIT = ("Log file size limit of 104857600 bytes reached; later lines of this session are not "
         "written to this file")


def line(sec, message, cls="Core", level="Info", site=SITE):
    whole = int(sec)
    micro = int(round((sec - whole) * 1e6))
    return f"[{whole:4d}.{micro:06d}] {cls} <{level}> {site}: {message}\n"


def parse(message, cls="Core", level="Info", site=SITE):
    return incident.parse_line(line(1.0, message, cls, level, site).rstrip("\n").encode(), 0, False)


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class FakeEmulator:
    """What incident.ServerLink needs of debug_mcp.Emulator, with scripted answers."""

    def __init__(self):
        self.calls = []
        self.down = False
        self.session = 1
        self.limit_reached = False
        self.mark_native = 40.25
        self.mark_logged = True
        self.mark_count = 0
        self.ring = {"lines": [], "next": 0, "more": False, "lost": 0, "omitted": 0}

    def call(self, method, params, timeout):
        self.calls.append(method)
        if self.down:
            raise ConnectionRefusedError("down")
        if method == "status":
            return {"session": self.session, "presents": 500, "fps": 60.0, "uptime_s": 50.0,
                    "log_filter": "*:Info",
                    "log": {"ring": {"enabled": True, "capacity": 20000},
                            "file": {"open": True, "bytes_written": 1000, "limit": 100,
                                     "limit_reached": self.limit_reached}}}
        if method == "mark":
            self.mark_count += 1
            result = {"mark": self.mark_count, "session": self.session, "presents": 777,
                      "uptime_s": 50.0, "logged": self.mark_logged}
            if self.mark_logged:
                result["native_seconds"] = self.mark_native
            return result
        if method == "log_tail":
            page = dict(self.ring)  # a page is delivered once, as a cursor would
            self.ring = {"lines": [], "next": page["next"], "more": False, "lost": 0,
                         "omitted": 0}
            return page
        if method == "system_events":
            return {"events": [], "next": 0, "more": False, "lost": 0,
                    "ring": {"capacity": 8192, "newest": 0, "oldest": 1, "evicted": 0,
                             "dropped": 0, "diagnostic_mode": False},
                    "sources": []}
        raise AssertionError(f"unexpected call {method}")


class Env:
    """A temporary folder with a native log and a recorder on a clock the test drives."""

    def __init__(self, test, server=None, **options):
        self.dir = tempfile.TemporaryDirectory()
        test.addCleanup(self.dir.cleanup)
        self.out = os.path.join(self.dir.name, "session")
        self.log = os.path.join(self.dir.name, "eden_log.txt")
        open(self.log, "wb").close()
        self.clock = Clock()
        self.server = server
        settings = dict(out=self.out, log=self.log, pre=10.0, post=5.0, grace=2.0, poll=0.25,
                        status_every=1.0, stdin=False, hotkey=False)
        settings.update(options)
        self.cfg = incident.Config(**settings)
        link = incident.ServerLink(server) if server is not None else None
        self.rec = incident.Recorder(self.cfg, link, clock=self.clock, wall=lambda: 1.7e9 + self.clock.t)
        test.addCleanup(self.rec.store.close)  # runs before the folder goes (Windows locks)

    def append(self, *lines):
        with open(self.log, "ab") as handle:
            handle.write("".join(lines).encode())

    def run(self, seconds, step=0.5):
        """Poll for `seconds` of wall time in steps small enough to be no stall."""
        for _ in range(int(round(seconds / step))):
            self.clock.t += step
            self.rec.step()

    def mark(self, label="m", pre=None, post=None):
        self.rec.requests.put((label, pre, post))
        self.rec.step()

    def finish(self):
        self.rec.finish()

    def read(self, rel):
        with open(os.path.join(self.out, *rel.split("/")), encoding="utf-8") as handle:
            return handle.read()

    def window(self, number=1):
        return json.loads(self.read(f"marks/{number:03d}/window.json"))

    def rows(self, rel):
        return [json.loads(text) for text in self.read(rel).splitlines()]

    def session(self):
        return json.loads(self.read("session.json"))


class ParseTest(unittest.TestCase):
    def test_report_numbers(self):
        rec = parse(REPORT)
        self.assertEqual(set(rec.tags), {"report", "frame_over_100ms", "audio_underrun",
                                         "slow_file_read"})
        self.assertEqual(rec.fields["fps"], 59.8)
        self.assertEqual(rec.fields["longest_ms"], 340)
        self.assertEqual(rec.fields["over_100ms"], 2)
        self.assertEqual(rec.fields["audio_underruns"], 3)
        self.assertEqual(rec.fields["file_read_ms"], 340)
        self.assertEqual(rec.fields["limited_by"], "the guest")

    def test_frames_summary(self):
        rec = parse(FRAMES)
        self.assertEqual(rec.tags, ("frames_summary",))
        self.assertEqual((rec.fields["shown"], rec.fields["late"], rec.fields["p99_ms"]),
                         (598, 4, 33.4))

    def test_short_read_occurrence(self):
        rec = parse(SHORT_READ, "Service_FS", "Warning")
        self.assertEqual(rec.tags, ("warning", "fs_short_read"))
        self.assertEqual((rec.fields["requested"], rec.fields["returned"],
                          rec.fields["occurrence"]), (4096, 100, 7))

    def test_ipc_summary_rows_and_errors(self):
        rec = parse(IPC, "Service")
        self.assertEqual(rec.tags, ("ipc_summary", "ipc_error_result"))
        self.assertEqual(rec.fields["calls"], 17)
        self.assertEqual(rec.fields["error_calls"], 5)
        self.assertEqual(rec.fields["unlisted_kinds"], 28)
        self.assertEqual(rec.fields["rows"][1], ["fsp-srv", 8, True, "0x2f8002", 5])
        self.assertEqual(rec.fields["rows"][0], ["nvdrv", 3, False, "0x0", 12])

    def test_hid_totals_only(self):
        rec = parse(HID, "Service_HID")
        self.assertEqual(rec.tags, ("hid_summary",))
        self.assertEqual(rec.fields, {"updates": 300, "published": 290, "with_buttons_held": 12})

    def test_scheduler_batch_numbers_only(self):
        rec = parse(SCHED, "Core_Timing")
        self.assertEqual(rec.tags, ("sched_summary",))
        self.assertEqual(rec.fields, {"core": 2, "tick": 123456789, "switches": 4096,
                                      "to_idle": 1500})

    def test_timeline_fields(self):
        rec = parse(TIMELINE, "Service")
        self.assertEqual(rec.tags, ("timeline_summary",))
        self.assertEqual(rec.fields["frames"], 60)
        self.assertEqual(rec.fields["front_lag_ms"], -0.5)
        self.assertEqual(rec.fields["back_lag_ms"], 1.25)
        self.assertEqual(rec.fields["lists"], 3.0)
        self.assertEqual(rec.fields["back_cpu_ms"], 5.5)

    def test_native_markers(self):
        self.assertIn("log_file_limit", parse(LIMIT, "Log", "Warning").tags)
        self.assertIn("unmapped_access", parse("Unmapped Write32 @ 0x00001000 = 0x1",
                                               "HW_Memory", "Error").tags)
        self.assertIn("host_crash", parse("Host crash: exception", "Core", "Critical").tags)
        mark = parse("Incident mark #3 session=2 presents=900 label_bytes=0", "Debug")
        self.assertEqual(mark.fields, {"mark": 3, "session": 2, "presents": 900})

    def test_plain_info_has_no_signal_and_no_text(self):
        rec = parse("loaded something private")
        self.assertEqual(rec.tags, ())
        self.assertIsNone(rec.text)
        self.assertIsNone(rec.fp)

    def test_site_keeps_no_machine_path(self):
        rec = parse("cannot open it", level="Warning",
                    site="C:\\Users\\bob\\src\\core\\x.cpp:5:F")
        self.assertEqual(rec.site, "x.cpp:5:F")
        self.assertNotIn("bob", json.dumps(rec.site))

    def test_fingerprint_ignores_numbers_not_words(self):
        first = parse("read of 4096 bytes failed at 0x1000", level="Warning")
        second = parse("read of 12 bytes failed at 0x2fff", level="Warning")
        other = parse("write of 12 bytes failed at 0x2fff", level="Warning")
        self.assertEqual(first.fp, second.fp)
        self.assertNotEqual(first.fp, other.fp)

    def test_unparsed_lines_are_skipped(self):
        self.assertIsNone(incident.parse_line(b"garbage without the native shape", 0, False))


class ServerLinkTest(unittest.TestCase):
    def test_memory_and_input_tools_are_refused(self):
        fake = FakeEmulator()
        link = incident.ServerLink(fake)
        for method in ("read_memory", "search_memory", "write_memory", "read_gpu_memory",
                       "input", "input_sequence", "reject_shaders", "log_filter", "screenshot"):
            with self.assertRaises(incident.Refused, msg=method):
                link.call(method)
        self.assertEqual(fake.calls, [])
        self.assertFalse(set(incident.ALLOWED_TOOLS) &
                         {"read_memory", "search_memory", "write_memory", "read_gpu_memory"})

    def test_allowed_tool_passes_and_busy_is_reported(self):
        link = incident.ServerLink(FakeEmulator())
        self.assertEqual(link.call("status")["session"], 1)
        self.assertTrue(link.lock.acquire())
        try:
            with self.assertRaises(incident.Busy):
                link.call("status")
        finally:
            link.lock.release()


class WindowTest(unittest.TestCase):
    def test_pre_and_post_are_kept_with_signals_and_check_passes(self):
        env = Env(self)
        env.append(*[line(s, f"tick {s}") for s in range(0, 35)])
        env.append(line(35.0, REPORT), line(38.0, SHORT_READ, "Service_FS", "Warning"),
                   line(40.0, "tick"))
        env.run(0.5)
        env.mark("glitch")
        env.append(*[line(40 + s, f"tick {s}") for s in range(1, 7)])
        env.run(1.0)
        env.finish()
        window = env.window()
        self.assertTrue(window["complete"], window["incomplete_reasons"])
        self.assertEqual(window["pre"]["covered_s"], 10)
        self.assertEqual(window["post"]["covered_s"], 5)
        self.assertEqual(window["native_seconds"], 40.0)
        kinds = {tuple(s["signals"]) for s in window["pre"]["signals"]}
        self.assertIn(("warning", "fs_short_read"), kinds)
        self.assertTrue(any(m["kind"] == "report" and m["fps"] == 59.8
                            for m in window["pre"]["metrics"]))
        self.assertGreaterEqual(window["post"]["lines"], 5)
        self.assertEqual(env.session()["state"], "finished")
        self.assertEqual(incident.check_session(env.out), [])

    def test_mark_early_in_boot_is_not_complete_but_loses_nothing(self):
        env = Env(self, pre=35.0, post=1.0)
        env.append(*[line(s, "tick") for s in range(0, 28)])
        env.run(0.5)
        env.mark()
        env.append(line(28.5, "tick"))
        env.run(1.0)
        env.finish()
        window = env.window()
        self.assertEqual(window["pre"]["covered_s"], 27.0)
        self.assertEqual(window["pre"]["shortfall_s"], 8.0)
        self.assertEqual(window["incomplete_reasons"], ["pre_before_process_start"])
        self.assertFalse(window["complete"])
        self.assertFalse(window["evidence_lost"])
        self.assertEqual(incident.check_session(env.out), [])

    def test_missing_history_is_pre_window_short_and_evidence_lost(self):
        env = Env(self, pre=30.0, post=1.0, catchup_mb=0.001)
        env.append(*[line(s, "tick with padding to make the line long enough") for s in range(0, 300)])
        env.run(0.5)
        env.mark()
        env.append(line(300.5, "tick"))
        env.run(1.0)
        env.finish()
        window = env.window()
        self.assertIn("pre_window_short", window["incomplete_reasons"])
        self.assertNotIn("pre_before_process_start", window["incomplete_reasons"])
        self.assertTrue(window["evidence_lost"])
        self.assertIn("attach_skipped_history", env.session()["gaps"])
        self.assertLess(window["pre"]["covered_s"], 29)

    def test_quiet_post_window_is_observed_while_the_game_lives(self):
        env = Env(self)
        env.append(*[line(s, "tick") for s in range(0, 41)])
        env.run(0.5)
        env.mark()
        env.run(8.0)
        env.finish()
        window = env.window()
        self.assertTrue(window["complete"], window["incomplete_reasons"])
        self.assertEqual(window["post"]["covered_s"], 5)
        self.assertGreaterEqual(window["post"]["quiet_s"], 4.0)
        self.assertTrue(any("no log line" in note for note in window["notes"]))

    def test_quiet_after_exit_is_not_claimed(self):
        env = Env(self)
        env.append(*[line(s, "tick") for s in range(0, 41)])
        env.run(0.5)
        env.mark()
        env.run(1.0)
        env.rec.stop("emulator_exited")
        env.finish()
        window = env.window()
        self.assertFalse(window["complete"])
        self.assertIn("post_window_short:emulator_exited", window["incomplete_reasons"])
        self.assertLess(window["post"]["covered_s"], 2.0)
        self.assertEqual(window["post"]["quiet_s"], 0)
        self.assertTrue(window["evidence_lost"])

    def test_check_rejects_complete_with_shortfall(self):
        env = Env(self, pre=35.0, post=1.0)
        env.append(*[line(s, "tick") for s in range(0, 28)])
        env.run(0.5)
        env.mark()
        env.append(line(28.5, "tick"))
        env.run(1.0)
        env.finish()
        path = os.path.join(env.out, "marks", "001", "window.json")
        window = env.window()
        window["complete"] = True
        window["incomplete_reasons"] = []
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(window, handle)
        problems = incident.check_session(env.out)
        self.assertTrue(any("covers less" in p for p in problems), problems)


class GapTest(unittest.TestCase):
    def test_truncated_log_is_a_gap(self):
        env = Env(self)
        env.append(*[line(s, "tick") for s in range(0, 30)])
        env.run(0.5)
        with open(env.log, "wb") as handle:
            handle.write(line(1.0, "new process").encode())
        env.run(1.0)
        self.assertIn("log_file_replaced_or_truncated", env.rec.gap_causes)
        self.assertIn("native_clock_went_back", env.rec.gap_causes)

    def test_native_write_limit_marker_is_a_gap_and_a_signal(self):
        env = Env(self)
        env.append(line(5.0, "tick"), line(9.0, LIMIT, "Log", "Warning", "common/logging.cpp:1:Write"))
        env.run(1.0)
        env.finish()
        self.assertIn("native_log_file_full", env.rec.gap_causes)
        rows = env.rows("signals.jsonl")
        self.assertTrue(any("log_file_limit" in row["signals"] for row in rows))
        self.assertTrue(env.rec.ring_active)

    def test_missing_log_is_a_gap_then_closes(self):
        env = Env(self)
        os.remove(env.log)
        env.run(1.0)
        self.assertIn("log_file_unavailable", env.rec.gap_causes)
        env.append(line(1.0, "tick"))
        env.run(1.0)
        self.assertFalse(env.rec.open_gaps)

    def test_skipped_short_read_occurrences_are_reported(self):
        env = Env(self)
        first = SHORT_READ.replace("occurrence 7", "occurrence 1")
        third = SHORT_READ.replace("occurrence 7", "occurrence 4")
        env.append(line(1.0, first, "Service_FS", "Warning"),
                   line(2.0, third, "Service_FS", "Warning"))
        env.run(1.0)
        self.assertEqual(env.rec.short_read_unlogged, 2)
        self.assertIn("native_occurrences_not_logged", env.rec.gap_causes)

    def test_size_bound_drops_rows_and_says_so(self):
        env = Env(self, max_bytes=300 * 1024)
        env.append(*[line(s, REPORT) for s in range(0, 600)])
        env.run(1.0)
        env.finish()
        self.assertGreater(env.rec.store.dropped["signals.jsonl"], 0)
        self.assertEqual(env.rec.gap_causes["recorder_size_cap"], 1)
        self.assertLessEqual(env.rec.store.used, env.cfg.max_bytes)
        session = env.session()
        self.assertFalse(session["complete"])
        self.assertGreater(session["store_dropped"]["signals.jsonl"], 0)
        self.assertEqual(incident.check_session(env.out), [])

    def test_stall_is_a_gap(self):
        env = Env(self)
        env.append(line(1.0, "tick"))
        env.run(0.5)
        env.clock.t += 60
        env.rec.step()
        self.assertIn("recorder_stall", env.rec.gap_causes)

    def test_max_marks_refuses_with_a_loss_row(self):
        env = Env(self, max_marks=1)
        env.append(line(1.0, "tick"))
        env.run(0.5)
        env.mark("one")
        env.mark("two")
        env.finish()
        losses = [r for r in env.rows("timeline.jsonl") if r["kind"] == "loss"]
        self.assertEqual([r["reason"] for r in losses], ["max_marks"])
        self.assertEqual(len(env.rec.marks), 1)


class ServerTest(unittest.TestCase):
    def test_exact_mark_correlates_log_time_and_present(self):
        fake = FakeEmulator()
        env = Env(self, fake)
        env.append(*[line(s, "tick") for s in range(0, 41)])
        env.run(1.0)
        env.mark("exact")
        env.append(line(40.25, "Incident mark #1 session=1 presents=777 label_bytes=5", "Debug"),
                   *[line(41 + s, "tick") for s in range(0, 6)])
        env.run(1.0)
        env.finish()
        window = env.window()
        self.assertEqual(window["correlation"], "exact")
        self.assertEqual(window["native_seconds"], 40.25)
        self.assertEqual(window["present"], 777)
        self.assertIsNotNone(window["log_byte"])
        self.assertEqual(set(fake.calls) - {"status", "mark", "system_events"}, set())

    def test_filtered_mark_falls_back_to_approximate(self):
        fake = FakeEmulator()
        fake.mark_logged = False
        env = Env(self, fake)
        env.append(*[line(s, "tick") for s in range(0, 41)])
        env.run(1.0)
        env.mark("filtered")
        env.run(8.0)
        env.finish()
        window = env.window()
        self.assertEqual(window["correlation"], "approximate")
        self.assertEqual(window["present"], 777)
        self.assertTrue(any("log filter" in note for note in window["notes"]))
        self.assertGreater(window["uncertainty_s"], 0)

    def test_server_loss_and_session_change_are_gaps(self):
        fake = FakeEmulator()
        env = Env(self, fake)
        env.append(line(1.0, "tick"))
        env.run(2.0)
        self.assertTrue(env.rec.server_ok)
        fake.session = 2
        env.run(2.0)
        self.assertIn("debug_server_session_changed", env.rec.gap_causes)
        fake.down = True
        env.run(2.0)
        self.assertIn("debug_server_unreachable", env.rec.gap_causes)
        self.assertIn("server", env.rec.open_gaps)
        fake.down = False
        env.run(2.0)
        self.assertNotIn("server", env.rec.open_gaps)
        self.assertEqual(env.rec.gaps[-1]["open"], False)

    def test_server_not_yet_listening_is_not_a_gap(self):
        fake = FakeEmulator()
        fake.down = True
        env = Env(self, fake)
        env.append(line(1.0, "tick"))
        env.run(3.0)
        self.assertNotIn("debug_server_unreachable", env.rec.gap_causes)
        self.assertTrue(any(r["kind"] == "server_waiting" for r in env.rows("timeline.jsonl")))

    def test_full_file_switches_to_the_ring_and_reports_what_it_lost(self):
        fake = FakeEmulator()
        fake.limit_reached = True
        fake.ring = {"lines": [line(50.0, "late line", level="Warning").rstrip("\n")],
                     "next": 12, "more": False, "lost": 999, "omitted": 0}
        env = Env(self, fake)
        env.append(line(10.0, "tick"))
        env.run(2.0)
        self.assertIn("native_log_file_full", env.rec.gap_causes)
        # The first read from 0 counts everything ever evicted, mostly in the file: not a loss.
        self.assertNotIn("ring_lines_lost", env.rec.gap_causes)
        # But the ring's oldest line is after the file's last one: a hole of unknown size.
        self.assertIn("ring_started_after_file_end", env.rec.gap_causes)
        self.assertEqual(env.rec.native_high, 50.0)
        self.assertEqual(env.rec.ring_next, 12)
        fake.ring = {"lines": [], "next": 30, "more": False, "lost": 7, "omitted": 0}
        env.run(1.0)
        lost = [g for g in env.rec.gaps if g["cause"] == "ring_lines_lost"]
        self.assertEqual(lost[0]["lost"], 7)
        self.assertEqual(lost[0]["unit"], "lines")


class PrivacyTest(unittest.TestCase):
    SECRET = "C:\\Users\\bob\\Games\\secret game.nca"

    def build(self, raw):
        env = Env(self, raw_log=raw)
        env.append(line(1.0, f"cannot open {self.SECRET} token=abcdefabcdefabcdefabcdefabcdefabcdef",
                        level="Warning", site="C:\\Users\\bob\\src\\core\\x.cpp:5:F"),
                   line(2.0, "private-text-42"), line(3.0, REPORT))
        env.run(0.5)
        env.mark("a user label")
        env.append(line(4.0, "private-text-43"), line(5.0, "tick"))
        env.run(8.0)
        env.finish()
        return env

    def shareable_text(self, env):
        text = ""
        for folder, _, names in os.walk(env.out):
            if os.path.relpath(folder, env.out).replace("\\", "/").startswith("private"):
                continue
            for name in names:
                with open(os.path.join(folder, name), encoding="utf-8") as handle:
                    text += handle.read()
        return text

    def test_default_keeps_no_text_and_no_path(self):
        env = self.build(False)
        text = self.shareable_text(env)
        for forbidden in ("secret game", "private-text", "bob", "abcdefabcdef", "Users"):
            self.assertNotIn(forbidden, text)
        self.assertFalse(os.path.exists(os.path.join(env.out, "private", "marks")))
        self.assertEqual(incident.check_session(env.out), [])
        self.assertEqual(env.session()["privacy"]["guest_memory"], "never read")
        self.assertIn("warning", {s for row in env.rows("signals.jsonl") for s in row["signals"]})

    def test_raw_log_is_redacted_and_private(self):
        env = self.build(True)
        path = os.path.join(env.out, "private", "marks", "001", "log-window.txt")
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("<path>", text)
        self.assertIn("private-text-42", text)
        self.assertNotIn("bob", text)
        self.assertNotIn("abcdefabcdefabcdef", text)
        self.assertNotIn("private-text", self.shareable_text(env))
        self.assertEqual(incident.check_session(env.out), [])

    def test_check_flags_a_leaked_path(self):
        env = self.build(False)
        with open(os.path.join(env.out, "signals.jsonl"), "a", encoding="utf-8") as handle:
            handle.write(json.dumps({"seconds": 1, "site": "C:\\Users\\bob\\x.cpp"}) + "\n")
        self.assertTrue(any("absolute path" in p for p in incident.check_session(env.out)))

    def test_label_is_cleaned_and_bounded(self):
        env = Env(self)
        env.append(line(1.0, "tick"))
        env.run(0.5)
        env.mark("bad\x00label " + "x" * 300 + " C:\\Users\\bob\\x")
        mark = env.rec.marks[0]
        self.assertLessEqual(len(mark.label), 120)
        self.assertNotIn("\x00", mark.label)

    def test_absolute_log_path_only_in_private_file(self):
        env = self.build(False)
        with open(os.path.join(env.out, "private", "local.json"), encoding="utf-8") as handle:
            self.assertIn("eden_log.txt", handle.read())
        self.assertNotIn("eden_log.txt", self.shareable_text(env))


class AggregateTest(unittest.TestCase):
    def test_ipc_hid_and_timeline_land_in_session_and_signals(self):
        env = Env(self)
        env.append(line(1.0, IPC, "Service"), line(2.0, IPC, "Service"),
                   line(3.0, HID, "Service_HID"), line(4.0, TIMELINE, "Service"),
                   line(5.0, SCHED, "Core"), line(6.0, SCHED.replace("tick=1", "tick=2"), "Core"))
        env.run(1.0)
        env.finish()
        ipc = env.session()["ipc"]
        self.assertEqual(ipc["calls"]["nvdrv::3 -> 0x0"], 24)
        self.assertEqual(ipc["calls"]["fsp-srv::8 (tipc) -> 0x2f8002"], 10)
        self.assertEqual(ipc["max_unlisted_kinds"], 28)
        rows = env.rows("signals.jsonl")
        hid = [r for r in rows if "hid_summary" in r["signals"]]
        self.assertEqual(hid[0]["fields"]["with_buttons_held"], 12)
        timeline = [r for r in rows if "timeline_summary" in r["signals"]]
        self.assertEqual(timeline[0]["fields"]["back_lag_ms"], 1.25)
        self.assertEqual(env.session()["counts"]["signals"]["ipc_summary"], 2)
        core = env.session()["guest_scheduler"]["per_core"]["2"]
        self.assertEqual((core["batches"], core["switches"], core["to_idle"]), (2, 8192, 3000))
        self.assertEqual(core["last_tick"], 223456789)

    def test_repeated_warnings_coalesce_per_second(self):
        env = Env(self)
        env.append(*[line(5.0 + i / 1000, f"unmapped {i}", level="Warning") for i in range(500)],
                   line(8.0, "tick"))
        env.run(1.0)
        env.finish()
        rows = [r for r in env.rows("signals.jsonl") if r["signals"] == ["warning"]]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["count"], 500)


class ControlTest(unittest.TestCase):
    def run_cli(self, *argv):
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = incident.main(list(argv))
        return code, out.getvalue() + err.getvalue()

    def test_mark_file_and_orderly_stop(self):
        env = Env(self)
        env.append(line(1.0, "tick"), line(2.0, "tick"))
        env.run(0.5)
        code, _ = self.run_cli("mark", "--session", env.out, "--label", "from shell")
        self.assertEqual(code, 0)
        env.run(0.5)
        self.assertEqual(env.rec.marks[0].label, "from shell")
        self.assertEqual(os.listdir(os.path.join(env.out, "control")), [])
        code, _ = self.run_cli("stop", "--session", env.out)
        self.assertEqual(code, 0)
        env.run(0.5)
        self.assertTrue(env.rec.stopping)
        self.assertEqual(env.rec.stop_reason, "stop_requested")
        env.finish()
        self.assertEqual(env.session()["state"], "finished")
        self.assertEqual(incident.check_session(env.out), [])

    def test_mark_and_stop_refuse_without_a_running_session(self):
        env = Env(self)
        env.finish()
        self.assertEqual(self.run_cli("mark", "--session", env.out)[0], 1)
        self.assertEqual(self.run_cli("stop", "--session", env.out)[0], 1)
        self.assertEqual(self.run_cli("mark", "--session", os.path.join(env.dir.name, "none"))[0], 1)

    def test_process_exit_ends_the_session_after_the_last_bytes(self):
        alive = {"value": True}
        env = Env(self, pid=4242)
        env.rec.pid_alive = lambda pid: alive["value"]
        env.append(line(1.0, "tick"))
        env.run(0.5)
        alive["value"] = False
        env.append(line(2.0, "last words"))
        env.run(0.5)
        self.assertEqual(env.rec.stop_reason, "emulator_exited")
        env.finish()
        self.assertEqual(env.rec.native_high, 2.0)

    def test_second_session_in_a_used_folder_is_refused(self):
        env = Env(self)
        code, text = self.run_cli("watch", "--out", env.out, "--log", env.log, "--duration", "1")
        self.assertEqual(code, 2)
        self.assertIn("already holds a session", text)


class LaunchTest(unittest.TestCase):
    def test_basic_preset_adds_no_summary_and_scrubs_raw_traces(self):
        base = {"PATH": "x", "EDEN_NX2_IPC_TRACE": "1", "EDEN_NX2_DEBUG_WRITE": "1"}
        env, scrubbed = incident.launch_env(base, "127.0.0.1:1", "dump", "basic")
        self.assertNotIn("EDEN_NX2_IPC_TRACE", env)
        self.assertNotIn("EDEN_NX2_DEBUG_WRITE", env)
        self.assertEqual(sorted(scrubbed), ["EDEN_NX2_DEBUG_WRITE", "EDEN_NX2_IPC_TRACE"])
        for name in ("EDEN_NX2_IPC_SUMMARY", "EDEN_NX2_HID_SUMMARY", "EDEN_NX2_TIMELINE",
                     "EDEN_NX2_SCHED_SUMMARY"):
            self.assertNotIn(name, env)
        self.assertEqual(env["EDEN_NX2_DEBUG_SERVER"], "127.0.0.1:1")
        self.assertIn("EDEN_NX2_IPC_TRACE", base)  # the caller's environment is not touched

    def test_diagnostic_preset_adds_aggregates_only(self):
        env, _ = incident.launch_env({}, "127.0.0.1:1", "dump", "diagnostic")
        for name in ("EDEN_NX2_IPC_SUMMARY", "EDEN_NX2_HID_SUMMARY", "EDEN_NX2_TIMELINE",
                     "EDEN_NX2_SCHED_SUMMARY"):
            self.assertEqual(env[name], "1")
        self.assertNotIn("EDEN_NX2_IPC_TRACE", env)

    def test_preset_default_is_basic(self):
        args = incident.build_parser().parse_args(["watch"])
        self.assertEqual(args.preset, "basic")


class DetailTest(unittest.TestCase):
    def setUp(self):
        self.fake = FakeEmulator()
        self.fake.capture = []
        original = self.fake.call

        def call(method, params, timeout):
            if method in ("capture_frame", "sample"):
                self.fake.calls.append(method)
                self.fake.capture.append(params)
                return {"directory": "C:/x/dump/frame-1", "kinds": ["present"], "file": "s.txt"}
            return original(method, params, timeout)

        self.fake.call = call

    def test_detail_is_off_unless_asked(self):
        env = Env(self, self.fake)
        env.append(line(1.0, "tick"))
        env.run(2.0)
        env.mark()
        env.run(8.0)
        self.assertNotIn("capture_frame", self.fake.calls)
        self.assertNotIn("sample", self.fake.calls)

    def test_frame_detail_runs_once_per_cooldown_and_keeps_no_path(self):
        env = Env(self, self.fake, detail=("frame",), inline_detail=True, min_free_gb=0,
                  detail_cooldown=1000.0)
        env.append(line(1.0, "tick"))
        env.run(2.0)
        env.mark("a")
        env.mark("b")
        env.run(8.0)
        env.finish()
        self.assertEqual(self.fake.calls.count("capture_frame"), 1)
        first = env.window(1)["detail"][0]
        self.assertTrue(first["ok"])
        self.assertEqual(first["directory"], "frame-1")
        self.assertEqual(env.window(2)["detail"][0]["skipped"], "cooldown")
        self.assertFalse(self.fake.capture[0]["buffers"])
        self.assertNotIn("C:/", env.read("marks/001/window.json"))

    def test_low_disk_blocks_the_capture(self):
        env = Env(self, self.fake, detail=("frame",), inline_detail=True, min_free_gb=10 ** 9)
        env.append(line(1.0, "tick"))
        env.run(2.0)
        env.mark()
        env.run(8.0)
        env.finish()
        self.assertNotIn("capture_frame", self.fake.calls)
        self.assertEqual(env.window()["detail"][0]["skipped"], "low_disk")

    def test_detail_size_cap_blocks_the_capture(self):
        env = Env(self, self.fake, detail=("frame",), inline_detail=True, min_free_gb=0,
                  max_detail_mb=0)
        env.append(line(1.0, "tick"))
        env.run(2.0)
        os.makedirs(os.path.join(env.out, "private"), exist_ok=True)
        with open(os.path.join(env.out, "private", "old.bin"), "wb") as handle:
            handle.write(b"x" * 10)
        env.mark()
        env.run(8.0)
        env.finish()
        self.assertEqual(env.window()["detail"][0]["skipped"], "detail_size_cap")

    def test_audio_summary_and_diagnostic_preset(self):
        rec = incident.parse_line(
            b"[   5.000000] Audio.Sink <Info> audio_core/sink/sink_stream.cpp:250:"
            b"ProcessAudioOutAndRender: Audio summary: underruns=12 frames=240000",
            0, False)
        self.assertIsNotNone(rec)
        self.assertIn("audio_summary", rec.tags)
        self.assertEqual(rec.fields["underruns"], 12)
        self.assertEqual(rec.fields["frames"], 240000)

        env, _ = incident.launch_env({}, "127.0.0.1:0", "dumps", "diagnostic")
        for key in ("EDEN_NX2_DIAGNOSTIC", "EDEN_NX2_GPU_TRACE", "EDEN_NX2_AUDIO_SUMMARY",
                    "EDEN_NX2_FS_SUMMARY", "EDEN_NX2_IPC_SUMMARY", "EDEN_NX2_HID_SUMMARY",
                    "EDEN_NX2_TIMELINE", "EDEN_NX2_SCHED_SUMMARY"):
            self.assertEqual(env.get(key), "1")

    def test_window_assessment_separates_observed_from_suspected(self):
        env = Env(self)
        env.append(line(1.0, "IStorage read length mismatch: requested=4096 returned=0 "
                             "(occurrence 1 since process start)",
                        cls="Service.FS", level="Warning"))
        env.run(2.0)
        env.mark(label="hitch_check")
        env.run(8.0)
        env.finish()
        w = env.window()
        self.assertIn("assessment", w)
        obs = w["assessment"]["observed"]
        sus = w["assessment"]["suspected_causes"]
        self.assertEqual(obs["mark"], 1)
        self.assertEqual(obs["label"], "hitch_check")
        self.assertIn("pre_signals", obs)
        self.assertTrue(any(c["confidence"] == "suspected" for c in sus))


class RealLogTest(unittest.TestCase):
    """Reads one existing native log read-only, when a path is given, and reports counts only."""

    def test_native_log_from_environment(self):
        path = os.environ.get("INCIDENT_TEST_LOG")
        if not path or not os.path.isfile(path):
            self.skipTest("set INCIDENT_TEST_LOG to a native eden_log.txt to run this")
        parsed = unparsed = 0
        tags = {}
        with open(path, "rb") as handle:
            for number, raw in enumerate(handle):
                if number > 200000:
                    break
                rec = incident.parse_line(raw.rstrip(b"\r\n"), 0, False)
                if rec is None:
                    unparsed += 1
                    continue
                parsed += 1
                for tag in rec.tags:
                    tags[tag] = tags.get(tag, 0) + 1
        print(f"\n{parsed} lines parsed, {unparsed} not native-shaped; signals {tags}")
        self.assertGreater(parsed, 0)


if __name__ == "__main__":
    unittest.main()
