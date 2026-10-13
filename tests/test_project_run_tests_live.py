"""Focused GUT selections and timeout answers, opt-in with the real framework.

Set DIDI_TEST_BINARY, DIDI_TEST_GODOT and DIDI_TEST_GUT_PATH. Use the native
Godot executable rather than the Windows console launcher so the test owns
the process it waits for. Each run is small and finishes in under 60 seconds.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tools.vibe.mcp_client import Session


@unittest.skipUnless(all(os.environ.get(name) for name in
                        ('DIDI_TEST_BINARY', 'DIDI_TEST_GODOT', 'DIDI_TEST_GUT_PATH')),
                     'real GUT selection tests are opt-in')
class ProjectRunTestsLive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = Path(os.environ.get('DIDI_PROBE_TEMP', tempfile.gettempdir())).resolve()
        cls.temporary = tempfile.TemporaryDirectory(prefix='didi-gut-selection-', dir=directory)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        assert cls.root.resolve().parent == directory
        shutil.copytree(os.environ['DIDI_TEST_GUT_PATH'], cls.root / 'addons' / 'gut')
        (cls.root / 'project.godot').write_text(
            'config_version=5\n[application]\nconfig/name="GUT selection"\n'
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n', encoding='utf-8')
        for folder, source in {
            'selected': 'extends "res://addons/gut/test.gd"\nfunc test_selected():\n\tassert_true(true)\n',
            'ignored': 'extends "res://addons/gut/test.gd"\nfunc test_configured():\n\tassert_true(false)\n',
            'timeout': 'extends "res://addons/gut/test.gd"\nfunc test_wait():\n\tawait get_tree().create_timer(30).timeout\n\tassert_true(true)\n',
        }.items():
            (cls.root / folder).mkdir()
            (cls.root / folder / ('test_' + folder + '.gd')).write_text(source, encoding='utf-8')
        (cls.root / '.gutconfig.json').write_text(
            '{"dirs":["res://ignored"],"tests":["res://ignored/test_ignored.gd"],'
            '"include_subdirs":true,"log_level":2}', encoding='utf-8')
        engine = os.environ['DIDI_TEST_GODOT']
        imported = subprocess.run([engine, '--headless', '--editor', '--path', str(cls.root), '--import'],
                                  capture_output=True, timeout=30)
        if imported.returncode:
            raise RuntimeError(imported.stderr.decode(errors='replace'))
        cls.session = Session(project=cls.root, binary=os.environ['DIDI_TEST_BINARY'],
                              env={'GODOT_BIN': engine}, editor_log=False)
        cls.addClassCleanup(cls.session.close)

    def run_tests(self, name, paths=None, timeout=15):
        arguments = {'framework': 'gut', 'name': name, 'timeout_seconds': timeout}
        if paths is not None:
            arguments['paths'] = paths
        return self.session.call('project_run_tests', arguments)

    def assert_focused(self, report, failed):
        self.assertFalse(failed, report)
        self.assertEqual(report['verdict'], 'pass', report)
        self.assertEqual(report['counts']['total'], 1)
        self.assertTrue(report['configuration']['ignored'])
        recorded = {entry['path'] for entry in report['files']}
        self.assertIn('res://selected/test_selected.gd', recorded)
        self.assertNotIn('res://ignored/test_ignored.gd', recorded)
        self.assertNotIn('res://.gutconfig.json', recorded)

    def test_file_selection_ignores_configured_directories_and_tests(self):
        report, failed = self.run_tests('selected-file', ['res://selected/test_selected.gd'])
        self.assert_focused(report, failed)

    def test_directory_selection_ignores_configured_tests(self):
        report, failed = self.run_tests('selected-directory', ['res://selected'])
        self.assert_focused(report, failed)

    def test_omitted_paths_honor_the_project_config(self):
        report, failed = self.run_tests('configured')
        self.assertTrue(failed)
        self.assertEqual(report['verdict'], 'fail')
        self.assertFalse(report['configuration']['ignored'])
        self.assertIn('res://.gutconfig.json', {entry['path'] for entry in report['files']})

    def test_timeout_reports_progress_without_file_hashes_or_ansi(self):
        report, failed = self.run_tests('timeout', ['res://timeout/test_timeout.gd'], timeout=5)
        self.assertTrue(failed)
        self.assertEqual(report.get('reason'), 'timeout', report)
        self.assertNotIn('files', report)
        self.assertNotIn('files_truncated', report)
        self.assertLessEqual(len(report['output_tail']), 512)
        self.assertNotIn('\x1b', report['output_tail'])
        self.assertEqual(report['progress']['scripts_started'], 1)
        self.assertEqual(report['progress']['last_script'], 'res://timeout/test_timeout.gd')
        # Compact transport does not discard the record kept for inspection.
        record = json.loads((self.root / report['record']).read_text(encoding='utf-8'))
        self.assertIn('files', record)
