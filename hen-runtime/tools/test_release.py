#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Release helper regressions: merge correctness, refusal and safe extraction."""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('hen_release', Path(__file__).with_name('release.py'))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hen-helper-test-')
        self.root = Path(self.temp.name)
        self.previous_root = release.ROOT
        release.ROOT = self.root / 'ours'
        self.base = self.root / 'base'
        self.next = self.root / 'next'
        for tree in (self.base, release.ROOT, self.next):
            self.write(tree, 'Makefile', 'all:\n\ttrue\n')
            self.write(tree, 'hen/source/server.cpp', 'base\n' * 12)
            self.write(tree, 'ro/source/session.cpp', 'ro base\n')
        self.write(release.ROOT, 'hotupdate-controller/source/release_version.hpp', 'old\n')
        self.write(release.ROOT, 'release.json', json.dumps({
            'upstream_revision': 'old', 'upstream_label': 'old', 'review_required': False}))
        (release.ROOT / 'tools').mkdir()
        release.archive(self.base, release.ROOT / 'tools/upstream-base.zip')

    def tearDown(self):
        release.ROOT = self.previous_root
        self.temp.cleanup()

    @staticmethod
    def write(root, name, text):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def port(self):
        args = argparse.Namespace(upstream=str(self.next), revision='test-new',
                                  out=str(self.root / 'result'), git=None)
        with contextlib.redirect_stdout(io.StringIO()):
            release.rebase(args)
        return self.root / 'result'

    def test_preserve_custom_additions_and_upstream_changes(self):
        self.write(release.ROOT, 'added.txt', 'updater addition\n')
        self.write(self.next, 'new-upstream.txt', 'upstream addition\n')
        result = self.port()
        self.assertEqual((result / 'added.txt').read_text(), 'updater addition\n')
        self.assertEqual((result / 'new-upstream.txt').read_text(), 'upstream addition\n')
        self.assertTrue(json.loads((result / 'release.json').read_text())['review_required'])

    def test_non_overlapping_source_changes_merge(self):
        self.write(release.ROOT, 'hen/source/server.cpp', 'ours\n' + 'base\n' * 11)
        self.write(self.next, 'hen/source/server.cpp', 'base\n' * 11 + 'theirs\n')
        result = self.port()
        merged = (result / 'hen/source/server.cpp').read_text()
        self.assertIn('ours\n', merged)
        self.assertIn('theirs\n', merged)
        self.assertNotIn('<<<<<<<', merged)

    def test_overlapping_source_changes_stop_with_conflict(self):
        self.write(release.ROOT, 'hen/source/server.cpp', 'ours\n')
        self.write(self.next, 'hen/source/server.cpp', 'theirs\n')
        with self.assertRaises(ValueError):
            self.port()
        report = json.loads((self.root / 'result/PORT-REVIEW.json').read_text())
        self.assertIn('hen/source/server.cpp', report['conflicts'])
        self.assertEqual((self.next / 'hen/source/server.cpp').read_text(), 'theirs\n')

    def test_deleted_upstream_file_is_not_silently_resurrected(self):
        self.write(self.base, 'obsolete.txt', 'before\n')
        self.write(release.ROOT, 'obsolete.txt', 'changed\n')
        release.archive(self.base, release.ROOT / 'tools/upstream-base.zip')
        with self.assertRaises(ValueError):
            self.port()
        self.assertFalse((self.root / 'result/obsolete.txt').exists())
        self.assertEqual((self.root / 'result/obsolete.txt.updater-version').read_text(), 'changed\n')

    def test_existing_output_is_never_overwritten(self):
        result = self.root / 'result'
        result.mkdir()
        self.write(result, 'keep.txt', 'keep\n')
        with self.assertRaises(ValueError):
            self.port()
        self.assertEqual((result / 'keep.txt').read_text(), 'keep\n')

    def test_zip_traversal_rejected(self):
        archive = self.root / 'bad.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('../outside.txt', 'bad')
        with self.assertRaises(ValueError):
            release.unpack(archive, self.root / 'extract')
        self.assertFalse((self.root / 'outside.txt').exists())

    def test_case_collisions_rejected(self):
        archive = self.root / 'bad.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('Makefile', 'one')
            z.writestr('makefile', 'two')
        with self.assertRaises(ValueError):
            release.unpack(archive, self.root / 'extract')

    def test_build_requires_review_for_new_ports(self):
        self.write(release.ROOT, 'release.json', json.dumps({'review_required': True}))
        with self.assertRaisesRegex(ValueError, 'Review PORT-REVIEW'):
            release.build(argparse.Namespace(reviewed=False))


if __name__ == '__main__':
    unittest.main()
