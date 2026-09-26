"""Executable contract for opt-in managed recovery. Set DIDI_TEST_BINARY to exercise a build."""
import json
import os
from pathlib import Path
import sys
import subprocess
import unittest

try:
    import didi_binary
except ImportError:
    from tests import didi_binary


@unittest.skipUnless(os.environ.get('DIDI_TEST_BINARY'), 'set DIDI_TEST_BINARY')
class ManagedRecoverySurface(unittest.TestCase):
    def test_recovery_tools_are_implemented(self):
        result = subprocess.run([str(didi_binary.resolve()), '--dump-tool-manifest'], capture_output=True, text=True, check=True)
        manifest = json.loads(result.stdout)
        implemented = manifest['names']['implemented']
        if isinstance(implemented, dict):
            implemented = implemented.get('names', [])
        for name in ('runtime_checkpoint', 'runtime_recovery_status', 'runtime_restore_checkpoint', 'runtime_recover_editor'):
            self.assertIn(name, implemented)

    def test_managed_options_require_each_other_before_project_access(self):
        for option in ('--managed-editor', '--recovery-workspace'):
            result = subprocess.run([str(didi_binary.resolve()), option, 'missing'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('must be supplied together', result.stderr)


class RecoveryHarnessImports(unittest.TestCase):
    def test_adversarial_fixture_imports_in_both_unittest_modes(self):
        root = Path(__file__).resolve().parents[1]
        for module, directory in (("tests.test_managed_recovery_adversarial", root),
                                  ("test_managed_recovery_adversarial", root / "tests")):
            with self.subTest(module=module):
                result = subprocess.run(
                    [sys.executable, "-c", "import importlib; importlib.import_module(" + repr(module) + ")"],
                    cwd=directory, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
