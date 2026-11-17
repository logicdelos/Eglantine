<!--
SPDX-FileCopyrightText: Copyright 2026 logicdelos
SPDX-License-Identifier: GPL-3.0-or-later
-->
<!-- lang: en-GB -->

# Accuracy policy: no per-game code

The emulator is meant to behave like the console. A new game should run because the console behaviour it relies on is modelled, not because the code knows the game. The two games tested in this fork are Fire Emblem: Fortune's Weave and Xenoblade Chronicles 2. They are test cases for the emulator, never inputs to it.

`tools/check_accuracy_policy.py` turns this into a mechanical check. It freezes the exceptions that already exist and fails any change that adds one.

## What counts as per-game code

Code or data is per-game when it makes the emulator behave differently because of which game is running, or because of a property that only one game has. In particular:

- a program id, title id, build id, NSO build or shader hash that selects emulation behaviour, defaults, clocks, pacing or patches;
- named presets or prompts, and settings chosen or applied automatically for a title;
- a constant-buffer offset, image size, instruction fingerprint, count or threshold whose only justification is one game or one library workload;
- an external compatibility table that the emulator loads, and automatic selection of user mods;
- a fix or default described in terms of a game, even when the code looks neutral.

Renaming a game to an anonymous label, hashing its name, or moving the branch into a data or configuration file does not make it general. The check looks for those forms too.

## What is allowed

- Branches on the real console model, firmware, ABI, opcodes, formats and capabilities.
- Global enhancements that the user selects, clearly off the accuracy baseline.
- Host vendor or driver workarounds justified by a general capability or a structural failure, not by a game.
- Title ids used as storage, metadata or access-control identity, for example save paths, content registries and play-time records. A system applet or archive id (the ids after the `0100`/`0400` prefix are all zero except the last four digits) is not treated as a game id.
- External user mods through the existing generic facilities.
- Lawful synthetic conformance fixtures.

Game names, ids, scenes, picture tolerances and routes belong in lab-only files (the `fe-lab` tooling), which the emulator does not link or load. Nothing that is a ROM, key or firmware may be added to the source tree or to a test fixture.

## Running the check

```
python tools/check_accuracy_policy.py --all                 # the whole tree
python tools/check_accuracy_policy.py --base <commit>       # files changed since <commit>
python tools/check_accuracy_policy.py --all --json          # machine-readable output
python tools/integration/test_accuracy_policy.py            # the tests of the check
```

Only the Python standard library is used. The check reads files inside the repository root and asks git only for file lists and diffs. It fetches nothing and never edits a file.

Exit codes: 0 no findings, 1 findings, 2 usage or configuration error (including a malformed allow-list).

`--base` scans the files that differ from the commit in the working tree (renames included, untracked files included) and, in addition, any repository file that a changed source file names in a quoted path or include (for example a table the code loads). Whole files are scanned, not only changed lines, so a nearby game comment is visible to the reviewer.

Each finding prints as `path:line: [category] matched text` followed by the source line. Output is sorted and stable.

## What the check looks for

| Category | Finds |
|---|---|
| `title_id_literal` | A title or program id as a hex literal (`0100` or `0400` followed by 12 hex digits, with or without `0x`, with any integer suffix) outside the system range. |
| `title_id_encoded` | The same id written as a decimal literal or as an eight-byte array in the context of a title or program id. |
| `program_id_flow` | A program, title, application or build id (or a variable assigned from one) compared with something other than zero or another id, passed to `switch`, `find`, `count`, `contains`, `at` or similar, or used as an index. |
| `identity_hash` | A 64-bit hex literal near the words hash, key, crc, digest or build id, and quoted 16 or 40 digit hex strings (shader hashes, native keys, build ids). Constants of well-known hash functions are ignored. |
| `game_name` | A game name or abbreviation, or a `title1`/`title 4` style alias, in code, a comment, a string or a data file. The catalogue is kept as digests so that the tool does not list games; see below. |
| `workaround_knob` | An `EDEN_`, `EGLANTINE_` or `YUZU_` environment name containing FIX, HACK, WORKAROUND, QUIRK, TWEAK or OVERRIDE, and a setting named as a fix, hack, workaround, quirk or tweak. |
| `layout_constant` | In GPU, shader and loader code: a width, height, pitch or extent compared with a literal of 256 or more, and a constant-buffer index or offset compared with a literal. |
| `patch_constant` | In the loader and patch manager: a hex address or offset constant next to the word patch or offset. |
| `override_path` | A shader replacement or override file path. |
| `data_policy` | A data file (`json`, `toml`, `ini`, `yaml`, `csv`, `cfg`) under `src/` or `dist/`, and a policy-like key (`program_id`, `title_id`, `build_id`, `presets`, `profiles`, `patches`, `workarounds`, `quirks`, `overrides`, ...) in any data file. |

Scope: source, build, configuration and data files that the emulator can build in or load. Prose documentation (`*.md`, `docs/`, `fork-notes/`), translations (`dist/languages/`), `LICENSES/`, third-party trees (`externals/`), the dependency manifest `cpmfile.json`, and the three files of this check are not scanned.

A text scan cannot prove that no branch classifies a game by its content. The check finds the forms listed above. A new heuristic branch, for example one keyed on a texture shape or a command sequence, still needs an architectural review. The check is a guard against the common forms, not a semantic proof.

### Game name catalogue

The catalogue of game names and abbreviations is stored as short digests in `tools/check_accuracy_policy.py`, so that the file does not list games. To add a name, run `python tools/check_accuracy_policy.py --name-digest "<name>"` and add the printed value to `NAME_DIGESTS`. Names are matched on lower-case words, so `Some Name`, `some_name` and `SomeName` all match. A multi-word name also needs the digest of its first word (run `--name-digest` on that word) in `NAME_FIRST_DIGESTS`. Abbreviations that are also ordinary words are matched in exact case: use `--name-digest "<word>" --exact-case` and add the value to `NAME_DIGESTS_CS`.

## Recording an exception

Existing exceptions are listed in `tools/accuracy_policy_allowlist.json`. The file has two parts:

- `inventory`: one record per inventory item, with a plain `summary` and a `removal` condition. Items `A1` to `D18` come from the accuracy inventory. `M1` to `M4` are metadata and lab artefacts that are not behaviour (storage identity, display aliases, lab files, instruction encodings). `T1` is test fixtures. `U1` is what the check found that the inventory did not list.
- `entries`: one record per finding. An entry matches by `path`, `category`, `match` and `fingerprint` (a hash of the whitespace-normalised source line), so moving a line or re-indenting it keeps the entry valid, while editing it does not. `count` (default 1) is how many identical findings the file may contain. `line_at_base` is only a hint for the reader. `origin` is the commit that introduced the exact line text (`git log -S`), or `upstream (before shallow boundary)` when the history of this shallow repository ends first; it is never guessed. An optional `expires` date (`YYYY-MM-DD`) makes the entry fail after that day.

A change fails the check when it:

- adds a finding with no matching entry, or more findings than `count` allows (a new branch, or an expansion of an allowed one);
- edits a line that has an entry (the fingerprint changes, so the old entry is stale and the new line is a new finding);
- leaves an entry whose text is no longer present (a stale entry), moves or deletes its file, or reaches its `expires` date.

A new exception needs a review of why the code is a generic protocol or console-semantics rule and not another game fix. If it is, add an inventory record with a summary and a removal condition, and add the entries. If it is not, remove the code instead.

## Removing an exception

When the code of an exception is deleted or made general, delete its entries in the same change. The check reports them as stale until then, which is the intended reminder. When all entries of an inventory record are gone, delete the record. A legitimate rename of a file needs its entries updated to the new path in the same change.

## Known limits

- Detection is lexical. It follows a few simple assignments of ids to local names, not general data flow.
- The check cannot see a fix that names no game, no id and no hash, and encodes the game only through a magic number whose context is not covered above.
- Allow-list entries freeze text, not intent. An entry is a debt record, not an approval.
