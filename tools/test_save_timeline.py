#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
import tempfile
import unittest
from pathlib import Path

from save_timeline import fork_data


class SaveTimelineTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.live = self.root / 'live'
        self.slot = Path('user/save/account/test/slot')
        (self.live / self.slot.parent).mkdir(parents=True)
        (self.live / self.slot).write_bytes(b'A')
        (self.live / 'user/Contents').mkdir()
        (self.live / 'user/Contents/private').write_bytes(b'not a save')
        self.gen = fork_data(self.live, self.root / 'snapshots', False)
        self.branches = self.root / 'branches'

    def test_each_load_has_its_own_writable_save(self):
        (self.live / self.slot).write_bytes(b'newer')
        first = fork_data(self.gen, self.branches, True)
        (first / self.slot).write_bytes(b'autosave')
        second = fork_data(self.gen, self.branches, True)
        self.assertEqual((second / self.slot).read_bytes(), b'A')
        self.assertEqual((first / self.slot).read_bytes(), b'autosave')
        self.assertEqual((self.live / self.slot).read_bytes(), b'newer')
        self.assertFalse((second / 'user/Contents').exists())

    def test_corrupt_generation_does_not_publish_or_clobber_a_branch(self):
        good = fork_data(self.gen, self.branches, True)
        (self.gen / 'data' / self.slot).write_bytes(b'X')
        with self.assertRaisesRegex(ValueError, 'integrity'):
            fork_data(self.gen, self.branches, True)
        self.assertEqual(list(self.branches.iterdir()), [good.parent])
        self.assertEqual((good / self.slot).read_bytes(), b'A')

    def test_output_inside_the_input_is_refused_before_any_write(self):
        dest = self.live / 'user/save/recursive'
        with self.assertRaisesRegex(ValueError, 'disjoint'):
            fork_data(self.live, dest, False)
        self.assertFalse(dest.exists())


if __name__ == '__main__':
    unittest.main()
