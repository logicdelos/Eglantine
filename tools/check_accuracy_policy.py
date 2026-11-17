#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 logicdelos
# SPDX-License-Identifier: GPL-3.0-or-later

"""Offline check for per-game code in the emulator sources.

The emulator is meant to behave like the console, so a game must not need code of its own. This
check finds the places where code or data selects behaviour by game identity, records the ones that
already exist in tools/accuracy_policy_allowlist.json, and fails on any other. See
fork-notes/accuracy-policy.md for the rule.

    python tools/check_accuracy_policy.py --all                  # whole tree
    python tools/check_accuracy_policy.py --base <commit>       # files changed since <commit>
    python tools/check_accuracy_policy.py --all --json

Exit codes: 0 no findings, 1 findings, 2 usage or configuration error.

A text scan cannot prove that no branch classifies a game by its content. It finds identity
literals, program-id comparisons, hash and build-id tables, game names, workaround knobs and a few
layout constants. New heuristic branches still need an architectural review.

The scan reads only files inside the repository root and uses git only to list files and diffs.
"""

import argparse
import functools
import collections
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(HERE)
ALLOWLIST_PATH = "tools/accuracy_policy_allowlist.json"
SCHEMA = 1

CATEGORIES = (
    "title_id_literal",  # a title/program id as a hex literal
    "title_id_encoded",  # the same id as a decimal literal or an 8-byte array
    "program_id_flow",  # a program/title/build id compared, switched on or looked up
    "identity_hash",  # build-id or shader/native-key hash literal
    "game_name",  # a game name, abbreviation or title<N> alias
    "workaround_knob",  # a setting or environment name for a fix/workaround
    "layout_constant",  # image-shape or constant-buffer-layout comparison
    "patch_constant",  # loader/patch-manager address or offset constant
    "override_path",  # shader replacement or override file path
    "data_policy",  # data file with policy-like keys, or an unknown data file
)
META_CATEGORIES = ("stale_exception", "expired_exception", "excess_exception")

# Only files that can be built into or loaded by the emulator are scanned. Prose documentation,
# translations and third-party trees are not. The three files of this check are excluded by exact
# path because they must quote synthetic identifiers.
TEXT_EXTS = frozenset(
    ".c .cc .cpp .cxx .h .hh .hpp .inc .ipp .mm .m .kt .java .py .sh .ps1 .bat .cmake .txt .json "
    ".toml .ini .yaml .yml .csv .cfg .xml .ui .qrc .glsl .vert .frag .comp .geom .fx .js .gradle "
    ".kts .in .cmake".split()
)
EXCLUDED_PREFIXES = (
    "externals/",
    "fork-notes/",
    "docs/",
    "LICENSES/",
    ".git/",
    ".reuse/",
    "dist/languages/",
)
SELF_PATHS = frozenset(
    (
        "tools/check_accuracy_policy.py",
        "tools/accuracy_policy_allowlist.json",
        "tools/integration/test_accuracy_policy.py",
    )
)
EXCLUDED_FILES = frozenset(("cpmfile.json",))  # dependency pins, not emulation
DATA_EXTS = frozenset(".json .toml .ini .yaml .yml .csv .cfg".split())
DATA_POLICY_ROOTS = ("src/", "dist/")
MAX_LINE = 4000
MAX_FILE = 4 * 1024 * 1024

# Game names are kept as digests so that this file does not itself list games. Each digest is the
# first 12 hex digits of sha1 over the lowercase tokens joined by one space (case-insensitive set)
# or over the exact token (case-sensitive set). Add one with: --name-digest "some name".
NAME_DIGESTS = frozenset(
    """00ffc3893b18 046e41154f18 0776d0fa18cb 0843b5aa38e0 0ee654c34133 1763ba0d5f02 1da44d8fe52e 22842dbe61c5 2883c85e5882 2a362cb6bb6c 2c10fbe5e0b4 3193cbadb85f 31ef78645f4f 3a906815d077 406868a951a3 4829baec0d83 4a28e7ba5406 4ae46a47a165 558839ee59cf 5a2fa4da9967 5a966c7a1bf0 5de7ac50708b 624c22a8c8f8 627b7a6aaf5f 641cdc3ba713 64dbc14ea0a9 6b8903281917 6f602f12f6db 71d3cb6ad48a 74dfba49ea41 814e69be6046 851261ea7256 85abbbdeb487 85b1d4392a59 8c82e33482fb 992efda4ce64 99580bb1cb2a 9c881bdb6bc9 9f88a5f9be58 a56d3276ca2d a84f4306d85e b158f46c71ef b42db9d879b7 b633850b8bca be2fc33a153e c542737f1794 cee4c099beed d6d767d999c0 d99610dbfb97 df70f9b975b4 e96cadcffe5f f4143ed3d285 fe046a408689 fe79c0d2c65b feb051e448bb""".split()
)
NAME_DIGESTS_CS = frozenset(
    """08b1b61fba10 3b7c2814bfcd""".split()
)
NAME_FIRST_DIGESTS = frozenset(
    """01ba7992f85d 04a4fce796c2 0c62cbdb682c 113eecab17a0 1da840244989 235d8c310661 31fe827aea4c 4c742a231475 4e3e01b9af84 4f0aa52d656a 8451ba8a14d7 addb47291ee1 b802f384302c d87c448044de ddb5b361edc4 e628da552b75 ee3e4af9c48a""".split()
)
NAME_WINDOW = 4

TITLE_ID_RE = re.compile(r"(?<![0-9A-Za-z_])(?:0[xX])?((?:0100|0400)[0-9A-Fa-f]{12})(?![0-9A-Fa-f]|[G-KM-TV-Zg-km-tv-z_])")
DECIMAL_RE = re.compile(r"(?<![0-9A-Za-z_.])(\d{15,20})(?![0-9A-Za-z_.])")
BYTES8_RE = re.compile(r"(?<![0-9A-Za-z_])(?:0[xX][0-9A-Fa-f]{2}\s*,\s*){7}0[xX][0-9A-Fa-f]{2}")
HASH_HEX_RE = re.compile(r"(?<![0-9A-Za-z_])0[xX]([0-9A-Fa-f]{16})(?![0-9A-Fa-f])")
HASH_STR_RE = re.compile(r"[\"']((?:[0-9A-Fa-f]{16}|[0-9A-Fa-f]{40}))[\"']")
WORD_RE = re.compile(r"[A-Za-z0-9]+")
# Constants of well-known algorithms and tables. They are not identities of anything.
ALGORITHM_CONSTANTS = frozenset(
    "9e3779b97f4a7c15 cbf29ce484222325 ff51afd7ed558ccd c4ceb9fe1a85ec53 9ddfea08eb382d69 "
    "0123456789abcdef fedcba9876543210 0102040810204080 8040201008040201 "
    "b492b66fbe98f273 9ae16a3b2f90404f c3a5c85c97cb3127".split()
)
TITLE_ALIAS_RE = re.compile(r"(?i)(?<![a-z])title[ _]?[0-9]+(?![0-9a-z])")
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
TOKEN_RE = re.compile(r"[A-Z]+(?![a-z])[0-9]*|[A-Z]?[a-z]+[0-9]*|[0-9]+")
WORKAROUND_WORDS = frozenset(
    "fix fixes workaround workarounds hack hacks quirk quirks tweak tweaks wa".split()
)
ENV_NAME_RE = re.compile(r"\b((?:EDEN|EGLANTINE|YUZU)_[A-Z0-9_]+)\b")
ENV_WORKAROUND_RE = re.compile(r"(?:FIX|HACK|WORKAROUND|QUIRK|TWEAK|OVERRIDE)")
OVERRIDE_PATH_RE = re.compile(
    r"\.override\.bin|\boverride\.bin\b|NATIVE_SHADER_OVERRIDE|shader[_-]?(?:override|replace)\w*[/\\]|"
    r"[\"'][^\"']*(?:override|replacement)s?[/\\][^\"']*[\"']"
)
LAYOUT_RE = re.compile(
    r"\b[A-Za-z_]*(?:width|height|pitch|extent|pixels)\b\s*(?:==|!=|>=|<=|<|>)\s*(0[xX][0-9A-Fa-f]+|\d+)\b"
    r"|\b(0[xX][0-9A-Fa-f]+|\d+)\s*(?:==|!=|>=|<=|<|>)\s*[A-Za-z_]*(?:width|height|pitch|extent|pixels)\b"
)
LAYOUT_PREFIXES = (
    "src/video_core/",
    "src/shader_recompiler/",
    "src/core/hle/service/nx2",
    "src/core/loader/",
)
CBUF_RE = re.compile(
    r"\b(?:cbuf|cb|const_buffer|constbuf)\w*\s*(?:==|!=)\s*(?:0[xX][0-9A-Fa-f]{2,}|[1-9][0-9]{2,})\b"
)
PATCH_PREFIXES = ("src/core/loader/", "src/core/file_sys/patch_manager")
PATCH_RE = re.compile(r"(?i)patch|offset")
PATCH_NUM_RE = re.compile(r"(?<![0-9A-Za-z_])0[xX][0-9A-Fa-f]{4,8}(?![0-9A-Fa-f])")
DATA_KEY_RE = re.compile(
    r"[\"'](program_?id|title_?id|build_?id|application_?id|shader_?hash|native_?key|"
    r"presets?|profiles?|patches|workarounds?|quirks|overrides|compat\w*|game_?(?:settings|db|list))[\"']\s*[:=]",
    re.IGNORECASE,
)
PATH_REF_RE = re.compile(r"[\"<]([A-Za-z0-9_./\\-]+\.(?:json|toml|ini|ya?ml|csv|cfg|inc|txt))[\">]")

COMPARE_RE = re.compile(
    r"([A-Za-z_][\w.>:-]*(?:\([^()]*\))?)\s*([!=]=)\s*([A-Za-z_0-9][\w.>:-]*(?:\([^()]*\))?)"
)
ZERO_RE = re.compile(r"^(?:(?:0|0[xX]0+)[uUlL]*|\{\}|nullptr|false|true)$")
SAFE_OTHER_RE = re.compile(
    r"(?i)result|status|success|error|\bend\(|\bcend\(|\bnpos\b|nullopt|\bnull\b|\bnone\b|invalid"
)
LOOKUP_RE = re.compile(
    r"(?:\.|::|\b)(?:contains|count|find|at|lower_bound|upper_bound|binary_search|any_of|find_if)\s*\(|"
    r"\bswitch\s*\("
)
INDEX_RE = re.compile(r"\w\[([^\]]+)\]")
ASSIGN_RE = re.compile(r"(?<![=!<>])\b([A-Za-z_]\w*)\s*=\s*([^;=][^;]{0,160});")
SIMPLE_RHS_RE = re.compile(r"^(?:(?:static_cast|reinterpret_cast)<[\w:]+>\()?[\w.>:\-]*(?:\(\s*\))?\)?$")
HEX_LITERAL_RE = re.compile(r"0[xX][0-9A-Fa-f]+[uUlL]*")
PID_CONTEXT_RE = re.compile(r"(?i)title|program|\btid\b|application_?id")
HASH_CONTEXT_RE = re.compile(r"(?i)hash|key|crc|fingerprint|digest|checksum|build_?id")


def sha(text, n=16):
    return hashlib.sha1(text.encode("utf-8", "replace")).hexdigest()[:n]


def normalize_line(line):
    return " ".join(line.split())


def name_digest(text, case_sensitive=False):
    if case_sensitive:
        return sha("cs:" + text.strip(), 12)
    return sha(" ".join(text.lower().split()), 12)


class Finding:
    __slots__ = ("path", "line", "category", "match", "context", "fingerprint", "note")

    def __init__(self, path, line, category, match, context, fingerprint=None, note=""):
        self.path = path
        self.line = line
        self.category = category
        self.match = match
        self.context = context
        self.fingerprint = fingerprint or sha(context)
        self.note = note

    def key(self):
        return (self.path, self.category, self.fingerprint, self.match)

    def sort_key(self):
        return (self.path, self.line, self.category, self.match, self.note)

    def as_dict(self):
        out = {
            "path": self.path,
            "line": self.line,
            "category": self.category,
            "match": self.match,
            "fingerprint": self.fingerprint,
            "context": self.context[:200],
        }
        if self.note:
            out["note"] = self.note
        return out


def is_system_id(digits16):
    """System modules, applets and archives: the ids after the prefix are all zero but the last."""
    return digits16[4:12] == "0" * 8


def code_part(line):
    """The line without a trailing // comment (a naive cut that ignores strings)."""
    idx = line.find("//")
    while idx != -1:
        if line[:idx].count('"') % 2 == 0:
            return line[:idx]
        idx = line.find("//", idx + 2)
    return line


PID_QUICK_RE = re.compile(r"(?i)(?:program|title|build|application)_?id")


@functools.lru_cache(maxsize=1 << 16)
def pid_identifier(name):
    flat = name.replace("_", "").lower()
    return any(k in flat for k in ("programid", "titleid", "buildid", "applicationid"))


def tracked_names(text, aliases):
    """Identifiers in text that name a program/title/build id, or alias one."""
    if not aliases and not PID_QUICK_RE.search(text):
        return set()
    return {i for i in IDENT_RE.findall(text) if pid_identifier(i) or i in aliases}


def balanced_args(code, start):
    """Text from start up to the parenthesis that closes the call opened just before it."""
    depth = 1
    for i in range(start, len(code)):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                return code[start:i]
    return code[start:]


@functools.lru_cache(maxsize=1 << 16)
def _token_digest(token):
    return sha(token, 12)


def name_hits(line):
    """Catalogue keys of game names found in the line (empty when none)."""
    hits = []
    tokens = TOKEN_RE.findall(HEX_LITERAL_RE.sub(" ", line))
    lowered = [t.lower() for t in tokens]
    for word in WORD_RE.findall(HEX_LITERAL_RE.sub(" ", line)):
        digest = _token_digest(word.lower())
        if digest in NAME_DIGESTS:
            hits.append("name:" + digest)
    for i, tok in enumerate(tokens):
        if len(tok) <= 5 and _token_digest("cs:" + tok) in NAME_DIGESTS_CS:
            hits.append("name:" + _token_digest("cs:" + tok))
        digest = _token_digest(lowered[i])
        if digest in NAME_DIGESTS:
            hits.append("name:" + digest)
        if digest in NAME_FIRST_DIGESTS:
            for n in range(2, NAME_WINDOW + 1):
                if i + n <= len(lowered):
                    d = sha(" ".join(lowered[i : i + n]), 12)
                    if d in NAME_DIGESTS:
                        hits.append("name:" + d)
    for m in TITLE_ALIAS_RE.finditer(line):
        hits.append("alias:" + re.sub(r"[ _]", "", m.group(0).lower()))
    return sorted(set(hits))


def scan_text(path, text):
    """All findings for one file's text. Pure: no file access."""
    findings = []
    lines = text.splitlines()
    ext = os.path.splitext(path)[1].lower()
    aliases = set()
    previous = ""
    for number, raw in enumerate(lines, 1):
        if len(raw) > MAX_LINE:
            raw = raw[:MAX_LINE]
        if not raw.strip():
            continue
        context = normalize_line(raw)
        fp = sha(context)

        def add(category, match, note=""):
            findings.append(Finding(path, number, category, match, context, fp, note))

        for m in TITLE_ID_RE.finditer(raw):
            if not is_system_id(m.group(1)):
                add("title_id_literal", m.group(1).upper())
        for m in DECIMAL_RE.finditer(raw):
            value = int(m.group(1))
            if value >> 48 in (0x0100, 0x0400) and (value >> 16) & 0xFFFFFFFF:
                add("title_id_encoded", m.group(1), "decimal")
        for m in BYTES8_RE.finditer(raw):
            b = [int(x, 16) for x in re.findall(r"0[xX]([0-9A-Fa-f]{2})", m.group(0))]
            if (
                PID_CONTEXT_RE.search(previous + raw)
                and any(b[2:6])
                and ((b[7] in (1, 4) and b[6] == 0) or (b[0] in (1, 4) and b[1] == 0))
            ):
                add("title_id_encoded", normalize_line(m.group(0)), "bytes")

        for m in HASH_HEX_RE.finditer(raw):
            digits = m.group(1)
            if (
                digits[:4] not in ("0100", "0400")
                and digits.lower() not in ALGORITHM_CONSTANTS
                and digits.count("0") <= 10
                and HASH_CONTEXT_RE.search(previous + raw)
            ):
                add("identity_hash", digits.lower())
        for m in HASH_STR_RE.finditer(raw):
            digits = m.group(1)
            if (
                digits[:4] not in ("0100", "0400")
                and digits.lower() not in ALGORITHM_CONSTANTS
                and set(digits) - {"0", "1"}
            ):
                add("identity_hash", digits.lower())

        code = code_part(raw)
        # ProgramId/title id/build id flowing into a condition, switch or table lookup.
        tracked = tracked_names(code, aliases)
        if tracked:
            for m in COMPARE_RE.finditer(code):
                left, right = m.group(1), m.group(3)
                left_hit, right_hit = tracked_names(left, aliases), tracked_names(right, aliases)
                if bool(left_hit) == bool(right_hit):
                    continue  # no id, or an id compared with another id (identity)
                other = right if left_hit else left
                if ZERO_RE.match(other) or SAFE_OTHER_RE.search(other):
                    continue
                add("program_id_flow", normalize_line(m.group(0)))
            for m in LOOKUP_RE.finditer(code):
                args = balanced_args(code, m.end())
                if tracked_names(args, aliases):
                    add("program_id_flow", normalize_line(code[m.start() : m.end() + len(args) + 1])[:100], "lookup")
            for m in INDEX_RE.finditer(code):
                if tracked_names(m.group(1), aliases):
                    add("program_id_flow", normalize_line(m.group(0))[:100], "index")
        for m in ASSIGN_RE.finditer(code):
            rhs = m.group(2).strip()
            if not SIMPLE_RHS_RE.match(rhs):
                continue
            literal = TITLE_ID_RE.fullmatch(rhs.rstrip("uUlL"))
            if tracked_names(rhs, aliases) or (literal and not is_system_id(literal.group(1))):
                aliases.add(m.group(1))
        previous = raw + "\n"

        hits = name_hits(raw)
        if hits:
            add("game_name", ",".join(hits))

        for name in sorted(set(ENV_NAME_RE.findall(raw))):
            if ENV_WORKAROUND_RE.search(name):
                add("workaround_knob", name)
        if "SETTING" in raw or "Setting<" in raw or "BasicSetting" in raw:
            bad = sorted({t.lower() for t in TOKEN_RE.findall(raw)} & WORKAROUND_WORDS)
            if bad:
                add("workaround_knob", "setting:" + bad[0])

        if OVERRIDE_PATH_RE.search(raw):
            add("override_path", OVERRIDE_PATH_RE.search(raw).group(0)[:80])

        if path.startswith(LAYOUT_PREFIXES) and ext in (".cpp", ".h", ".inc", ".hpp"):
            for m in LAYOUT_RE.finditer(code):
                lit = m.group(1) or m.group(2)
                if int(lit, 0) >= 256:
                    add("layout_constant", normalize_line(m.group(0)))
            for m in CBUF_RE.finditer(code):
                add("layout_constant", normalize_line(m.group(0)))
        if path.startswith(PATCH_PREFIXES) and PATCH_RE.search(code):
            for m in PATCH_NUM_RE.finditer(code):
                add("patch_constant", m.group(0).lower())

    if ext in DATA_EXTS:
        for number, raw in enumerate(lines, 1):
            for m in DATA_KEY_RE.finditer(raw):
                context = normalize_line(raw)
                findings.append(Finding(path, number, "data_policy", "key:" + m.group(1).lower(), context))
        if path.startswith(DATA_POLICY_ROOTS):
            findings.append(
                Finding(path, 1, "data_policy", "data-file", os.path.basename(path), sha(path))
            )
    return findings


def included(path):
    if path in SELF_PATHS or path in EXCLUDED_FILES or path.startswith(EXCLUDED_PREFIXES):
        return False
    base = os.path.basename(path)
    return os.path.splitext(base)[1].lower() in TEXT_EXTS


def read_text(root, path):
    """File text, or None when it is missing, a link, outside the root, binary or too large."""
    full = os.path.join(root, *path.split("/"))
    real_root = os.path.realpath(root)
    try:
        if os.path.islink(full) or not os.path.isfile(full):
            return None
        real = os.path.realpath(full)
        if os.path.commonpath([real_root, real]) != real_root:
            return None
        if os.path.getsize(full) > MAX_FILE:
            return None
        with open(full, "rb") as handle:
            data = handle.read()
    except OSError:
        return None
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8", "replace")


class UsageError(Exception):
    pass


def git(root, *args):
    try:
        proc = subprocess.run(
            ["git", "-C", root, *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise UsageError("git is not available: %s" % exc)
    if proc.returncode != 0:
        raise UsageError("git %s failed: %s" % (args[0], proc.stderr.decode("utf-8", "replace").strip()))
    return proc.stdout


def list_all(root):
    """Tracked and untracked files. Falls back to a walk when the root is not a git checkout."""
    try:
        out = git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
        return sorted(set(p for p in out.decode("utf-8", "replace").split("\0") if p))
    except UsageError:
        found = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames if d != ".git")
            for name in filenames:
                rel = os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/")
                found.append(rel)
        return sorted(found)


def changed_files(root, base):
    """(paths to scan, paths whose allow-list entries are in scope) for the diff against base."""
    if base.startswith("-"):
        raise UsageError("invalid base revision: %s" % base)
    git(root, "rev-parse", "--verify", "--quiet", base + "^{commit}")
    out = git(root, "diff", "--name-status", "-M", "-z", base).decode("utf-8", "replace")
    fields = [f for f in out.split("\0")]
    scan, scope = set(), set()
    i = 0
    while i < len(fields) and fields[i]:
        status = fields[i]
        if status[0] in "RC":
            old, new = fields[i + 1], fields[i + 2]
            scope.update((old, new))
            scan.add(new)
            i += 3
        else:
            path = fields[i + 1]
            scope.add(path)
            if status[0] != "D":
                scan.add(path)
            i += 2
    untracked = git(root, "ls-files", "-z", "--others", "--exclude-standard").decode("utf-8", "replace")
    for p in untracked.split("\0"):
        if p:
            scan.add(p)
            scope.add(p)
    return scan, scope


def referenced_files(root, path, text, tracked):
    """Repository data files that a scanned file names in a quoted path or include."""
    by_base = collections.defaultdict(list)
    for t in tracked:
        by_base[os.path.basename(t)].append(t)
    found = set()
    for m in PATH_REF_RE.finditer(text):
        ref = m.group(1).replace("\\", "/")
        if ".." in ref.split("/"):
            continue
        candidates = [ref, os.path.dirname(path) + "/" + ref if os.path.dirname(path) else ref]
        for c in candidates:
            if c in tracked:
                found.add(c)
        if not any(c in tracked for c in candidates):
            matches = by_base.get(os.path.basename(ref), [])
            if len(matches) == 1:
                found.add(matches[0])
    return found


def load_allowlist(root, path=None):
    full = os.path.join(root, *(path or ALLOWLIST_PATH).split("/")) if not os.path.isabs(path or "") else path
    try:
        with open(full, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError) as exc:
        raise UsageError("cannot read allow-list %s: %s" % (full, exc))
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise UsageError("allow-list schema must be %d" % SCHEMA)
    inventory = data.get("inventory")
    entries = data.get("entries")
    if not isinstance(inventory, dict) or not isinstance(entries, list):
        raise UsageError("allow-list needs an 'inventory' object and an 'entries' list")
    for inv_id, rec in inventory.items():
        if not isinstance(rec, dict) or not rec.get("removal") or not rec.get("summary"):
            raise UsageError("inventory %s needs 'summary' and 'removal'" % inv_id)
    index = {}
    for n, entry in enumerate(entries):
        for field in ("id", "path", "category", "match", "fingerprint", "origin"):
            if not isinstance(entry.get(field), str) or not entry[field]:
                raise UsageError("entry %d: missing '%s'" % (n, field))
        if entry["category"] not in CATEGORIES:
            raise UsageError("entry %d: unknown category %s" % (n, entry["category"]))
        if entry["id"] not in inventory:
            raise UsageError("entry %d: unknown inventory id %s" % (n, entry["id"]))
        count = entry.get("count", 1)
        if not isinstance(count, int) or count < 1:
            raise UsageError("entry %d: bad count" % n)
        expires = entry.get("expires")
        if expires is not None:
            try:
                datetime.date.fromisoformat(expires)
            except (TypeError, ValueError):
                raise UsageError("entry %d: expires must be YYYY-MM-DD" % n)
        key = (entry["path"], entry["category"], entry["fingerprint"], entry["match"])
        if key in index:
            raise UsageError("entry %d: duplicate of entry for %s" % (n, entry["path"]))
        index[key] = entry
    return index


def evaluate(findings, index, scope=None, today=None):
    """Split findings into (reported, allowed counts). scope=None checks every entry for staleness."""
    today = today or datetime.date.today()
    grouped = collections.defaultdict(list)
    for f in findings:
        grouped[f.key()].append(f)
    reported = []
    allowed = collections.Counter()
    for key, group in grouped.items():
        entry = index.get(key)
        if entry is None:
            reported.extend(group)
            continue
        limit = entry.get("count", 1)
        group.sort(key=lambda f: f.line)
        allowed[key[1]] += min(limit, len(group))
        for f in group[limit:]:
            f.note = "exceeds recorded count %d" % limit
            reported.append(f)
        if len(group) < limit:
            reported.append(
                Finding(key[0], 0, "stale_exception", key[3], entry["id"], key[2],
                        "recorded %d occurrence(s), found %d" % (limit, len(group)))
            )
        expires = entry.get("expires")
        if expires and datetime.date.fromisoformat(expires) < today:
            reported.append(
                Finding(key[0], 0, "expired_exception", key[3], entry["id"], key[2], "expired " + expires)
            )
    for key, entry in index.items():
        if key in grouped:
            continue
        if scope is not None and key[0] not in scope:
            continue
        reported.append(
            Finding(key[0], 0, "stale_exception", key[3], entry["id"], key[2], "text no longer present")
        )
        expires = entry.get("expires")
        if expires and datetime.date.fromisoformat(expires) < today:
            reported.append(
                Finding(key[0], 0, "expired_exception", key[3], entry["id"], key[2], "expired " + expires)
            )
    reported.sort(key=lambda f: f.sort_key())
    return reported, allowed


def run(root, base=None, scan_all=False, allowlist=None, today=None):
    """Returns (reported, allowed, total_findings, files_scanned)."""
    root = os.path.abspath(root)
    index = load_allowlist(root, allowlist)
    tracked = set(list_all(root))
    if scan_all:
        paths, scope = sorted(p for p in tracked if included(p)), None
    else:
        changed, scope = changed_files(root, base)
        paths = set(p for p in changed if included(p))
        for p in sorted(paths):
            text = read_text(root, p)
            if text is not None:
                paths |= set(r for r in referenced_files(root, p, text, tracked) if included(r))
        scope = set(scope) | paths
        paths = sorted(paths)
    findings = []
    scanned = 0
    for p in paths:
        text = read_text(root, p)
        if text is None:
            continue
        scanned += 1
        findings.extend(scan_text(p, text))
    reported, allowed = evaluate(findings, index, scope, today)
    return reported, allowed, len(findings), scanned


def format_text(reported, allowed, total, scanned, mode):
    lines = []
    for f in reported:
        where = "%s:%d" % (f.path, f.line) if f.line else f.path
        extra = " (%s)" % f.note if f.note else ""
        lines.append("%s: [%s] %s%s" % (where, f.category, f.match, extra))
        if f.line:
            lines.append("    | " + f.context[:160])
    counts = collections.Counter(f.category for f in reported)
    lines.append(
        "%s: %d file(s) scanned, %d finding(s), %d allowed, %d reported"
        % (mode, scanned, total, sum(allowed.values()), len(reported))
    )
    lines.append("allowed by category: " + (", ".join("%s=%d" % kv for kv in sorted(allowed.items())) or "none"))
    if counts:
        lines.append("reported by category: " + ", ".join("%s=%d" % kv for kv in sorted(counts.items())))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("root", nargs="?", default=DEFAULT_ROOT, help="repository root (default: this checkout)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--base", metavar="REV", help="scan files changed against this commit")
    mode.add_argument("--all", action="store_true", help="scan the whole tree")
    mode.add_argument("--name-digest", metavar="NAME", help="print the catalogue digest of a game name and exit")
    parser.add_argument("--allowlist", help="allow-list file (default: %s)" % ALLOWLIST_PATH)
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    parser.add_argument("--exact-case", action="store_true", help="with --name-digest: digest for the exact-case set")
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return 2 if exc.code else 0
    if args.name_digest is not None:
        print(name_digest(args.name_digest, args.exact_case))
        return 0
    if not args.base and not args.all:
        print("error: give --base REV or --all", file=sys.stderr)
        return 2
    if not os.path.isdir(args.root):
        print("error: not a directory: %s" % args.root, file=sys.stderr)
        return 2
    try:
        reported, allowed, total, scanned = run(args.root, args.base, args.all, args.allowlist)
    except UsageError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    mode_name = "all" if args.all else "base " + args.base
    if args.json:
        print(
            json.dumps(
                {
                    "mode": "all" if args.all else "base",
                    "base": args.base,
                    "files_scanned": scanned,
                    "findings_total": total,
                    "allowed": dict(sorted(allowed.items())),
                    "reported": [f.as_dict() for f in reported],
                },
                indent=2,
                sort_keys=True,
            )
        )
    else:
        print(format_text(reported, allowed, total, scanned, mode_name))
    return 1 if reported else 0


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
