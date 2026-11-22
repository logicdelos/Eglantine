#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Snapshot a stopped emulator's SaveDir data; fork independent writable copies for testing.

This is only the persistent save-data component, not a running-machine checkpoint. The
caller must own the directories and stop all writers to the source before capture.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import stat
import tempfile

# SaveDataFactory's roots. SaveDir may also be NANDDir: never archive firmware or keys.
SAVE_TREES = ('system/save', 'user/save', 'temp')
FORMAT = 'eglantine-save-data-v1'


def kind(path):
    s = path.lstat()
    if getattr(s, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise ValueError('reparse point not supported in a save timeline')
    if stat.S_ISDIR(s.st_mode):
        return 'dir'
    if stat.S_ISREG(s.st_mode):
        return 'file'
    raise ValueError('links and special files are not supported in a save timeline')


def copy_and_hash(source, dest=None):
    """Hash names, directory entries and contents in a stable order as they are copied."""
    tree = hashlib.sha256()

    def visit(rel):
        p = source / rel
        t = kind(p)
        name = os.fsencode(rel.as_posix())
        tree.update(t.encode() + len(name).to_bytes(8, 'big') + name)
        if t == 'dir':
            if dest is not None:
                (dest / rel).mkdir(parents=True)
            for child in sorted(p.iterdir(), key=lambda x: x.name):
                visit(rel / child.name)
        else:
            with p.open('rb') as f:
                if dest is None:
                    digest = hashlib.file_digest(f, 'sha256')
                else:
                    digest = hashlib.sha256()
                    with (dest / rel).open('xb') as out:
                        while chunk := f.read(1 << 20):
                            digest.update(chunk)
                            out.write(chunk)
                tree.update(digest.digest())

    found = False
    for folder in SAVE_TREES:
        rel = Path(folder)
        try:
            # Check intermediate directories too: user or system could be a junction.
            if rel.parent != Path('.') and kind(source / rel.parent) != 'dir':
                raise ValueError('expected save parent directory')
            t = kind(source / rel)
        except FileNotFoundError:
            continue
        if t != 'dir':
            raise ValueError('expected save directory')
        visit(rel)
        found = True
    if not found:
        raise ValueError('no save trees found; supply the configured SaveDir, not a title folder')
    return tree.hexdigest()


def fork_data(source, store, restoring):
    if kind(source) != 'dir':
        raise ValueError('expected a directory')
    source = source.resolve(strict=True)
    store = store.resolve()
    if source == store or source in store.parents or store in source.parents:
        raise ValueError('source and destination stores must be disjoint')
    expected = None
    if restoring:
        # The receipt contains no user paths, names or guest data. Validate before publishing.
        receipt = source / 'receipt'
        if kind(receipt) != 'file' or receipt.stat().st_size != len(FORMAT) + 66:
            raise ValueError('invalid or incomplete save-data generation')
        version, expected = receipt.read_text(encoding='ascii').splitlines()
        if version != FORMAT or len(expected) != 64:
            raise ValueError('unsupported save-data receipt')
        source = source / 'data'
        if kind(source) != 'dir':
            raise ValueError('invalid save-data generation')
    store.mkdir(parents=True, exist_ok=True)
    # Each publication owns a fresh private directory. No path ever names an existing save
    # as a write destination. Only the final rename publishes a usable generation or branch.
    work = Path(tempfile.mkdtemp(prefix='timeline-', dir=store))
    try:
        stage = work / 'pending'
        stage.mkdir()
        data = stage if restoring else stage / 'data'
        digest = copy_and_hash(source, data)
        if not restoring:
            expected = copy_and_hash(source)
        if digest != expected:
            raise ValueError('save data changed during capture or failed integrity verification')
        if not restoring:
            (stage / 'receipt').write_bytes(f'{FORMAT}\n{digest}\n'.encode('ascii'))
        published = work / ('branch' if restoring else 'generation')
        stage.rename(published)
        return published
    except BaseException:
        shutil.rmtree(work)
        raise


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='op', required=True)
    for op, helptext in [('capture', 'capture a quiescent SaveDir into a new generation'),
                         ('branch', 'verify a generation and create a new writable SaveDir')]:
        cmd = sub.add_parser(op, help=helptext)
        cmd.add_argument('source', type=Path)
        cmd.add_argument('store', type=Path, help='separate output directory')
    args = p.parse_args()
    try:
        print(fork_data(args.source, args.store, args.op == 'branch'))
    except (ValueError, OSError) as exc:
        p.exit(1, f'{p.prog}: {exc}\n')


if __name__ == '__main__':
    main()
