#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 logicdelos
# SPDX-License-Identifier: GPL-3.0-or-later

"""Tests for tools/check_accuracy_policy.py.

Only synthetic source strings and temporary directories are used. No game, dump or key is read.

    python tools/integration/test_accuracy_policy.py
"""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE)))
sys.dont_write_bytecode = True

import check_accuracy_policy as policy  # noqa: E402

INVENTORY = {"X1": {"summary": "synthetic exception", "removal": "delete with the test"}}
HAVE_GIT = shutil.which("git") is not None


def hits(path, text):
    return sorted((f.category, f.match) for f in policy.scan_text(path, text))


def categories(path, text):
    return {f.category for f in policy.scan_text(path, text)}


def entries_for(path, text, only=None, **extra):
    """Allow-list entries recording every finding of the text (or the categories in `only`)."""
    out = {}
    for f in policy.scan_text(path, text):
        if only and f.category not in only:
            continue
        key = f.key()
        if key in out:
            out[key]["count"] = out[key].get("count", 1) + 1
            continue
        entry = {
            "id": "X1",
            "path": f.path,
            "category": f.category,
            "match": f.match,
            "fingerprint": f.fingerprint,
            "origin": "synthetic",
        }
        entry.update(extra)
        out[key] = entry
    return list(out.values())


def write_json(path, data):
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(data, handle)


def write_file(root, rel, text):
    full = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


class TempDirCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = os.path.join(self.tmp.name, "repo")
        os.makedirs(os.path.join(self.root, "tools"))
        self.allowlist = os.path.join(self.root, policy.ALLOWLIST_PATH)
        self.set_allowlist([])

    def set_allowlist(self, entries):
        write_json(self.allowlist, {"schema": 1, "inventory": INVENTORY, "entries": entries})

    def run_all(self):
        return policy.run(self.root, scan_all=True)

    def cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = policy.main(list(args))
        return code, out.getvalue(), err.getvalue()


class DetectionTests(unittest.TestCase):
    def test_hex_title_id_with_suffix(self):
        got = hits("src/a.cpp", "if (program_id == 0x0100ABCDEF012001ULL) {")
        self.assertIn(("title_id_literal", "0100ABCDEF012001"), got)
        self.assertIn(("program_id_flow", "program_id == 0x0100ABCDEF012001ULL"), got)

    def test_title_id_forms(self):
        for text in (
            'const char* kId = "0400ABCDEF012000";',
            "constexpr u64 k = 0x0400abcdef012000;",
            "u64 id = 0100ABCDEF012000;",
        ):
            self.assertIn("title_id_literal", categories("src/a.cpp", text), text)

    def test_system_ids_are_not_title_ids(self):
        text = "constexpr u64 kAppletId = 0x0100000000001000ULL;\nauto x = 0x0100000000000816;"
        self.assertNotIn("title_id_literal", categories("src/a.cpp", text))

    def test_binary_patterns_are_not_title_ids(self):
        self.assertEqual(hits("src/a.inc", 'INST(AESE, "AESE", "0100111000101000010010nnnnnddddd")'), [])

    def test_decimal_title_id(self):
        text = "constexpr u64 kId = %d;" % 0x0100ABCDEF012001
        self.assertEqual(hits("src/a.cpp", text), [("title_id_encoded", str(0x0100ABCDEF012001))])
        self.assertEqual(hits("src/a.cpp", "constexpr u64 k = 123456789012345678;"), [])

    def test_decimal_system_id_is_not_flagged(self):
        self.assertEqual(hits("src/a.cpp", "constexpr u64 k = %d;" % 0x0100000000001000), [])

    def test_byte_array_title_id(self):
        arr = "const u8 title_id[8] = {0x34, 0x12, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01};"
        self.assertIn("title_id_encoded", categories("src/a.cpp", arr))
        # Without an id in the context the same bytes are data, not an identity.
        self.assertEqual(hits("src/a.cpp", "const u8 t[8] = {0x34, 0x12, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01};"), [])

    def test_program_id_flow_forms(self):
        flagged = (
            "switch (system.GetApplicationProcessProgramID()) {",
            "if (title_id == kSomething) {",
            "if (kSomething != process->GetProgramId()) {",
            "const auto it = tweaks.find(program_id);",
            "return table[title_id];",
            "if (build_id == kBuildA) return;",
        )
        for text in flagged:
            self.assertIn("program_id_flow", categories("src/a.cpp", text), text)

    def test_program_id_flow_ignores_non_conditions(self):
        quiet = (
            "if (program_id == 0) return;",
            "if (title_id != 0x0ULL) return;",
            "if (a.program_id == b.program_id) {",
            "if (loader->ReadProgramId(program_id) != Loader::ResultStatus::Success) {",
            "if (titleId != null) {",
            "const u64 program_id = process.GetProgramId();",
            "LOG_INFO(Core, \"program id {}\", program_id);",
        )
        for text in quiet:
            self.assertNotIn("program_id_flow", categories("src/a.cpp", text), text)

    def test_simple_aliases_are_followed(self):
        text = "const auto id = process.GetProgramId();\nif (id == kNeutralName) {"
        got = hits("src/a.cpp", text)
        self.assertEqual([c for c, _ in got], ["program_id_flow"])

    def test_literal_alias_is_followed(self):
        text = "constexpr u64 kAlias = 0x0100ABCDEF012000ULL;\nif (current == kAlias) {}"
        self.assertIn("program_id_flow", categories("src/a.cpp", text))
        text = "constexpr u64 kAlias = 0x0100ABCDEF012000ULL;\nauto id = kAlias;\nif (id == other) {}"
        self.assertIn("program_id_flow", categories("src/a.cpp", text))
        # A system-range id is not an alias of a game.
        text = "constexpr u64 kApplet = 0x0100000000001000ULL;\nif (current == kApplet) {}"
        self.assertEqual(hits("src/a.cpp", text), [])

    def test_hash_switch_and_build_id(self):
        text = "switch (shader_hash) {\ncase 0x72fd62555e7196cdULL:\n  break;"
        self.assertIn(("identity_hash", "72fd62555e7196cd"), hits("src/a.cpp", text))
        build = 'constexpr std::string_view kBuild = "A7730D0F74D168D90422354C8317F76FF78AD938";'
        self.assertIn(("identity_hash", "a7730d0f74d168d90422354c8317f76ff78ad938"), hits("src/a.cpp", build))
        native = 'constexpr const char* kNativeKey = "6e0ae200ee9aa220";'
        self.assertIn("identity_hash", categories("src/a.cpp", native))

    def test_known_hash_constants_and_bit_patterns_are_ignored(self):
        self.assertEqual(hits("src/a.cpp", "u64 hash = 0xcbf29ce484222325ULL;"), [])
        self.assertEqual(hits("src/a.cpp", 'static constexpr char hex[] = "0123456789abcdef";'), [])
        self.assertEqual(hits("src/a.inc", 'INST(SEV, "SEV", "1011111101000000")'), [])

    def test_game_names_and_aliases(self):
        for text in (
            "// Xenoblade needs the larger window.",
            "constexpr bool kIsXenoblade = true;",
            "if (is_xc2) {",
            "// Fire Emblem: Fortune's Weave reads this",
            "// FE reads this offset",
            "const bool is_title4 = Detect();",
            "// title 1 waits here",
        ):
            self.assertIn("game_name", categories("src/a.cpp", text), text)

    def test_name_matching_has_no_false_hits(self):
        for text in (
            "constexpr u8 kBytes[] = {0xC2, 0xFE};",
            "// the feature is enabled",
            "const int arms = 3;",  # only the upper-case spelling is catalogued
            "title_id = std::stoull(text);",
            "// the xenon blade server",
        ):
            self.assertNotIn("game_name", categories("src/a.cpp", text), text)

    def test_one_name_finding_per_line(self):
        found = [f for f in policy.scan_text("src/a.cpp", "// Xenoblade and Fortune's Weave") if f.category == "game_name"]
        self.assertEqual(len(found), 1)
        self.assertNotIn("enoblade", found[0].match)  # names are never echoed into the finding key

    def test_workaround_knobs(self):
        self.assertIn("workaround_knob", categories("src/a.cpp", 'if (getenv("EDEN_NX2_FOO_WORKAROUND")) {'))
        self.assertIn("workaround_knob", categories("dist/switches.txt", "EDEN_NX2_RESETFIX=1  turns the fix on"))
        self.assertIn(
            "workaround_knob",
            categories("src/common/settings.h", "SETTING(bool, fix_bloom_effects, false, 0, \"x\")"),
        )
        self.assertNotIn("workaround_knob", categories("src/a.cpp", 'getenv("EDEN_NX2_SKIP_EMPTY_FLUSH")'))

    def test_override_paths(self):
        self.assertIn("override_path", categories("src/a.cpp", 'auto p = dir / fmt::format("{:016x}.override.bin", key);'))

    def test_layout_constants_only_in_gpu_and_loader_paths(self):
        text = "if (width >= 1920 && height == 1080) {"
        self.assertEqual({m for c, m in hits("src/video_core/a.cpp", text) if c == "layout_constant"},
                         {"width >= 1920", "height == 1080"})
        self.assertNotIn("layout_constant", categories("src/common/a.cpp", text))
        self.assertIn("layout_constant", categories("src/video_core/a.cpp", "if (cbuf_offset == 0x1B0) {"))
        self.assertNotIn("layout_constant", categories("src/video_core/a.cpp", "if (width >= 8) {"))

    def test_patch_constants_only_in_loader_paths(self):
        text = "patch.offset = 0xB444ADC;"
        self.assertIn("patch_constant", categories("src/core/loader/nso.cpp", text))
        self.assertNotIn("patch_constant", categories("src/core/arm/a.cpp", text))

    def test_data_file_with_policy_keys(self):
        text = '{"program_id": "0100ABCDEF012000", "presets": {"gpu": "low"}}'
        got = hits("src/video_core/compat/games.json", text)
        self.assertIn(("data_policy", "key:program_id"), got)
        self.assertIn(("data_policy", "key:presets"), got)
        self.assertIn(("data_policy", "data-file"), got)
        self.assertIn(("title_id_literal", "0100ABCDEF012000"), got)

    def test_neutral_alias_and_moved_branches(self):
        # A game renamed to a neutral alias still reads the program id ...
        self.assertIn("program_id_flow", categories("src/a.cpp", "if (GetProgramId() == kTitleAlpha) {"))
        # ... and a branch moved into a data file is caught as an id, a key and an unknown data file.
        moved = hits("src/core/profiles.json", '{"workarounds": [{"title_id": "0400ABCDEF012000"}]}')
        self.assertEqual(
            {c for c, _ in moved}, {"title_id_literal", "data_policy"}
        )

    def test_neutral_storage_ids_stay_allowed(self):
        text = (
            "constexpr u64 kSaveNamespace = 0x0100000000000000ULL;\n"
            "constexpr u64 kSystemApplet = 0x0100000000001000ULL;\n"
            'const auto dir = fmt::format("{:016X}", title_id);\n'
            "if (a.title_id == b.title_id) {}\n"
            "if (title_id == 0) {}\n"
        )
        self.assertEqual(hits("src/core/file_sys/a.cpp", text), [])

    def test_name_digest_matches_catalogue(self):
        self.assertIn(policy.name_digest("Xenoblade"), policy.NAME_DIGESTS)
        self.assertNotIn(policy.name_digest("Unrelated Word"), policy.NAME_DIGESTS)
        self.assertEqual(policy.name_digest("xenoblade"), policy.name_digest(" XENOBLADE "))

    def test_scanning_is_deterministic(self):
        text = "// Xenoblade\nif (program_id == 0x0100ABCDEF012000ULL) {}\n"
        first = [f.as_dict() for f in policy.scan_text("src/a.cpp", text)]
        second = [f.as_dict() for f in policy.scan_text("src/a.cpp", text)]
        self.assertEqual(first, second)

    def test_included_paths(self):
        self.assertTrue(policy.included("src/core/a.cpp"))
        self.assertTrue(policy.included("dist/nx2_switches.txt"))
        for path in (
            "docs/notes.md",
            "README.md",
            "fork-notes/a.json",
            "externals/dep/a.cpp",
            "dist/languages/de.ts",
            "tools/check_accuracy_policy.py",
            "tools/accuracy_policy_allowlist.json",
            "tools/integration/test_accuracy_policy.py",
            "src/art.png",
        ):
            self.assertFalse(policy.included(path), path)


class AllowlistTests(TempDirCase):
    SOURCE = "// Xenoblade quirk\nif (program_id == 0x0100ABCDEF012000ULL) {\n    Apply();\n}\n"

    def put_source(self, text=None, path="src/core/a.cpp"):
        write_file(self.root, path, self.SOURCE if text is None else text)

    def test_clean_tree_passes(self):
        self.put_source("int main() { return 0; }\n")
        reported, allowed, total, scanned = self.run_all()
        self.assertEqual((reported, total, scanned), ([], 0, 1))

    def test_known_exception_passes_at_its_recorded_scope(self):
        self.put_source()
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE))
        reported, allowed, total, _ = self.run_all()
        self.assertEqual(reported, [])
        self.assertEqual(sum(allowed.values()), total)
        self.assertEqual(total, 3)  # a name, a literal and a comparison

    def test_same_text_in_another_file_is_new(self):
        self.put_source()
        self.put_source(path="src/core/b.cpp")
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE))
        reported, _, _, _ = self.run_all()
        self.assertTrue(reported)
        self.assertEqual({f.path for f in reported}, {"src/core/b.cpp"})

    def test_new_branch_fails(self):
        self.put_source()
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE))
        self.put_source(self.SOURCE + "if (program_id == 0x0100ABCDEF012001ULL) {}\n")
        reported, _, _, _ = self.run_all()
        self.assertEqual({f.category for f in reported}, {"title_id_literal", "program_id_flow"})
        self.assertEqual({f.line for f in reported}, {5})

    def test_changed_line_invalidates_the_fingerprint(self):
        self.put_source()
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE))
        self.put_source(self.SOURCE.replace("== 0x0100ABCDEF012000ULL", "== 0x0100ABCDEF012000ULL || extra"))
        reported, _, _, _ = self.run_all()
        self.assertIn("stale_exception", {f.category for f in reported})
        self.assertIn("program_id_flow", {f.category for f in reported})

    def test_reformatting_does_not_matter_but_text_does(self):
        self.put_source()
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE))
        self.put_source(self.SOURCE.replace("    Apply();", "\tApply();").replace("if (program_id", "  if (program_id"))
        self.assertEqual(self.run_all()[0], [])

    def test_expansion_of_an_allowed_exception_fails(self):
        self.put_source()
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE))
        self.put_source(self.SOURCE + self.SOURCE.splitlines()[1] + "\n")
        reported, _, _, _ = self.run_all()
        self.assertTrue(reported)
        self.assertTrue(all("exceeds recorded count 1" in f.note for f in reported))

    def test_recorded_count_allows_exactly_that_many(self):
        text = "// Xenoblade\n// Xenoblade\n"
        self.put_source(text)
        entries = entries_for("src/core/a.cpp", text)
        self.assertEqual(entries[0]["count"], 2)
        self.set_allowlist(entries)
        self.assertEqual(self.run_all()[0], [])
        self.put_source("// Xenoblade\n")
        reported, _, _, _ = self.run_all()
        self.assertEqual([f.category for f in reported], ["stale_exception"])

    def test_stale_entry_fails(self):
        self.put_source()
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE))
        self.put_source("int main() { return 0; }\n")
        reported, _, _, _ = self.run_all()
        self.assertEqual({f.category for f in reported}, {"stale_exception"})
        self.assertTrue(all(f.note == "text no longer present" for f in reported))

    def test_entry_for_a_deleted_file_is_stale(self):
        self.put_source()
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE))
        os.remove(os.path.join(self.root, "src", "core", "a.cpp"))
        reported, _, _, _ = self.run_all()
        self.assertEqual({f.category for f in reported}, {"stale_exception"})

    def test_expired_removal_condition_fails(self):
        self.put_source()
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE, expires="2000-01-01"))
        reported, _, _, _ = self.run_all()
        self.assertEqual({f.category for f in reported}, {"expired_exception"})
        self.set_allowlist(entries_for("src/core/a.cpp", self.SOURCE, expires="2999-01-01"))
        self.assertEqual(self.run_all()[0], [])

    def test_invalid_allowlists_are_usage_errors(self):
        good = entries_for("src/core/a.cpp", self.SOURCE)
        cases = {
            "schema": {"schema": 2, "inventory": INVENTORY, "entries": []},
            "unknown id": {"schema": 1, "inventory": INVENTORY, "entries": [dict(good[0], id="Q9")]},
            "no origin": {"schema": 1, "inventory": INVENTORY, "entries": [{k: v for k, v in good[0].items() if k != "origin"}]},
            "bad category": {"schema": 1, "inventory": INVENTORY, "entries": [dict(good[0], category="nope")]},
            "no removal": {"schema": 1, "inventory": {"X1": {"summary": "s"}}, "entries": []},
            "duplicate": {"schema": 1, "inventory": INVENTORY, "entries": [good[0], dict(good[0])]},
            "bad date": {"schema": 1, "inventory": INVENTORY, "entries": [dict(good[0], expires="soon")]},
        }
        for name, doc in cases.items():
            write_json(self.allowlist, doc)
            with self.assertRaises(policy.UsageError, msg=name):
                policy.load_allowlist(self.root)

    def test_system_ids_need_no_entry(self):
        self.put_source("constexpr u64 kApplet = 0x0100000000001000ULL;\n")
        self.assertEqual(self.run_all()[0], [])

    def test_links_and_binary_files_are_not_scanned(self):
        write_file(self.root, "src/core/blob.cpp", "// Xenoblade\0 binary")
        reported, _, _, scanned = self.run_all()
        self.assertEqual((reported, scanned), ([], 0))


class CommandLineTests(TempDirCase):
    def test_exit_codes(self):
        write_file(self.root, "src/a.cpp", "int x;\n")
        self.assertEqual(self.cli(self.root, "--all")[0], 0)
        write_file(self.root, "src/a.cpp", "if (program_id == 0x0100ABCDEF012001ULL) {}\n")
        code, out, _ = self.cli(self.root, "--all")
        self.assertEqual(code, 1)
        self.assertIn("src/a.cpp:1: [title_id_literal] 0100ABCDEF012001", out)
        self.assertIn("src/a.cpp:1: [program_id_flow]", out)
        self.assertIn("reported by category:", out)

    def test_usage_errors_exit_two(self):
        self.assertEqual(self.cli(self.root)[0], 2)
        self.assertEqual(self.cli(self.root, "--all", "--base", "HEAD")[0], 2)
        self.assertEqual(self.cli(os.path.join(self.root, "missing"), "--all")[0], 2)
        self.assertEqual(self.cli(self.root, "--bogus")[0], 2)
        write_file(self.root, policy.ALLOWLIST_PATH, "{not json")
        code, _, err = self.cli(self.root, "--all")
        self.assertEqual(code, 2)
        self.assertIn("cannot read allow-list", err)

    def test_base_option_rejects_option_like_values(self):
        code, _, err = self.cli(self.root, "--base=--output=x")
        self.assertEqual(code, 2)
        self.assertIn("invalid base revision", err)

    def test_json_output_is_deterministic(self):
        write_file(self.root, "src/b.cpp", "// Xenoblade\nif (program_id == 0x0100ABCDEF012001ULL) {}\n")
        write_file(self.root, "src/a.cpp", "constexpr u64 k = %d;\n" % 0x0400ABCDEF012000)
        code1, out1, _ = self.cli(self.root, "--all", "--json")
        code2, out2, _ = self.cli(self.root, "--all", "--json")
        self.assertEqual((code1, code2), (1, 1))
        self.assertEqual(out1, out2)
        doc = json.loads(out1)
        self.assertEqual(doc["mode"], "all")
        self.assertEqual(doc["files_scanned"], 2)
        paths = [(f["path"], f["line"]) for f in doc["reported"]]
        self.assertEqual(paths, sorted(paths))
        self.assertEqual(doc["reported"][0]["category"], "title_id_encoded")

    def test_name_digest_option(self):
        code, out, _ = self.cli("--name-digest", "Xenoblade")
        self.assertEqual(code, 0)
        self.assertEqual(out.strip(), policy.name_digest("Xenoblade"))
        code, out, _ = self.cli("--name-digest", "FE", "--exact-case")
        self.assertEqual((code, out.strip()), (0, policy.name_digest("FE", True)))
        self.assertIn(policy.name_digest("FE", True), policy.NAME_DIGESTS_CS)


@unittest.skipUnless(HAVE_GIT, "git is not available")
class DiffModeTests(TempDirCase):
    def git(self, *args):
        env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
        return subprocess.run(
            ["git", "-C", self.root, "-c", "user.name=t", "-c", "user.email=t@example.invalid",
             "-c", "commit.gpgsign=false", *args],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
        ).stdout.decode().strip()

    def commit_all(self, message="c"):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")

    def setUp(self):
        super().setUp()
        self.git("init", "-q")
        self.old = "// Xenoblade\nif (program_id == 0x0100ABCDEF012000ULL) {}\n"
        write_file(self.root, "src/old.cpp", self.old)
        write_file(self.root, "src/quiet.cpp", "int x;\n")
        self.set_allowlist(entries_for("src/old.cpp", self.old))
        self.base = self.commit_all("base")

    def diff(self):
        return policy.run(self.root, base=self.base)

    def test_unchanged_tree_is_clean(self):
        reported, _, total, scanned = self.diff()
        self.assertEqual((reported, total, scanned), ([], 0, 0))

    def test_new_literal_in_a_changed_file_fails(self):
        write_file(self.root, "src/quiet.cpp", "int x;\nif (program_id == 0x0100ABCDEF012001ULL) {}\n")
        reported, _, _, scanned = self.diff()
        self.assertEqual(scanned, 1)
        self.assertEqual({f.category for f in reported}, {"title_id_literal", "program_id_flow"})
        self.assertEqual({f.path for f in reported}, {"src/quiet.cpp"})

    def test_committed_changes_and_untracked_files_both_count(self):
        write_file(self.root, "src/new.cpp", "// Fortune's Weave only\n")
        reported, _, _, _ = self.diff()
        self.assertEqual({f.category for f in reported}, {"game_name"})
        self.commit_all("add")
        self.assertEqual({f.category for f in self.diff()[0]}, {"game_name"})

    def test_changed_allowed_file_keeps_passing_at_recorded_scope(self):
        write_file(self.root, "src/old.cpp", self.old + "int more;\n")
        self.assertEqual(self.diff()[0], [])

    def test_expansion_in_a_changed_allowed_file_fails(self):
        write_file(self.root, "src/old.cpp", self.old + self.old.splitlines()[1] + "\n")
        self.assertTrue(self.diff()[0])

    def test_rename_moves_the_exception_and_must_be_reviewed(self):
        self.git("mv", "src/old.cpp", "src/renamed.cpp")
        reported, _, _, _ = self.diff()
        cats = {(f.path, f.category) for f in reported}
        self.assertIn(("src/old.cpp", "stale_exception"), cats)
        self.assertIn(("src/renamed.cpp", "game_name"), cats)

    def test_deleting_an_excepted_file_requires_removing_its_entries(self):
        self.git("rm", "-q", "src/old.cpp")
        reported, _, _, _ = self.diff()
        self.assertEqual({f.category for f in reported}, {"stale_exception"})

    def test_untouched_allowlist_entries_are_not_rechecked(self):
        write_file(self.root, "src/quiet.cpp", "int y;\n")
        self.assertEqual(self.diff()[0], [])

    def test_added_data_file_is_flagged(self):
        write_file(self.root, "src/core/tweaks.json", '{"presets": {"gpu": "low"}}\n')
        reported, _, _, _ = self.diff()
        self.assertEqual({(f.category, f.match) for f in reported}, {("data_policy", "key:presets"), ("data_policy", "data-file")})

    def test_data_file_referenced_by_a_changed_source_is_scanned(self):
        # The table was committed earlier; only the loader changes now.
        write_file(self.root, "src/data/table.inc", "// Xenoblade\n")
        write_file(self.root, "src/loader.cpp", "int load();\n")
        self.base = self.commit_all("table")
        write_file(self.root, "src/loader.cpp", 'int load() { return Read("data/table.inc"); }\n')
        reported, _, _, scanned = self.diff()
        self.assertEqual(scanned, 2)
        self.assertEqual({(f.path, f.category) for f in reported}, {("src/data/table.inc", "game_name")})

    def test_references_never_leave_the_repository(self):
        write_file(self.root, "src/loader.cpp", 'int load() { return Read("../../outside/table.inc"); }\n')
        reported, _, _, scanned = self.diff()
        self.assertEqual((reported, scanned), ([], 1))

    def test_unknown_base_is_a_usage_error(self):
        with self.assertRaises(policy.UsageError):
            policy.run(self.root, base="no-such-revision")
        self.assertEqual(self.cli(self.root, "--base", "no-such-revision")[0], 2)

    def test_cli_base_mode(self):
        write_file(self.root, "src/quiet.cpp", "// Xenoblade\n")
        code, out, _ = self.cli(self.root, "--base", self.base)
        self.assertEqual(code, 1)
        self.assertIn("src/quiet.cpp:1: [game_name]", out)


if __name__ == "__main__":
    unittest.main()
