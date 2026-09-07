"""Executable contract for opt-in managed recovery. Set DIDI_TEST_BINARY to exercise a build."""
import json
import os
import subprocess
import unittest


@unittest.skipUnless(os.environ.get('DIDI_TEST_BINARY'), 'set DIDI_TEST_BINARY')
class ManagedRecoverySurface(unittest.TestCase):
    def test_recovery_tools_are_implemented(self):
        result = subprocess.run([os.environ['DIDI_TEST_BINARY'], '--dump-tool-manifest'], capture_output=True, text=True, check=True)
        manifest = json.loads(result.stdout)
        implemented = manifest['names']['implemented']
        if isinstance(implemented, dict):
            implemented = implemented.get('names', [])
        for name in ('runtime_checkpoint', 'runtime_recovery_status', 'runtime_restore_checkpoint', 'runtime_recover_editor'):
            self.assertIn(name, implemented)

    def test_managed_options_require_each_other_before_project_access(self):
        for option in ('--managed-editor', '--recovery-workspace'):
            result = subprocess.run([os.environ['DIDI_TEST_BINARY'], option, 'missing'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('must be supplied together', result.stderr)


if __name__ == '__main__':
    unittest.main()
