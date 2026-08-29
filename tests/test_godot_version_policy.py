import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == "nt", "Godot workflow scripts are Windows-only")
class GodotVersionWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def fake_godot(self, version: str) -> Path:
        executable = self.root / f"godot-{version}.cmd"
        executable.write_text(
            "@echo off\n"
            f'if "%~1"=="--version" (echo {version}.stable.official.test& exit /b 0)\n'
            "echo PHASE7_CONTRACT^|signal_flag_combinations=1^|key_identity_combinations=7\n"
            "exit /b 0\n",
            encoding="ascii",
        )
        return executable

    def run_script(self, relative_path: str, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["pwsh", "-NoLogo", "-NoProfile", "-File", str(REPOSITORY_ROOT / relative_path), *arguments],
            cwd=REPOSITORY_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
        )

    def test_current_contract_probe_accepts_exactly_one_godot_472_engine(self):
        result = self.run_script(
            "tests/phase7_contract_probe/run_phase7_contract_probe.ps1",
            "-Godot472",
            str(self.fake_godot("4.7.2")),
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("ENGINE_CONTRACT|4.7.2|"), 1, result.stdout)
        self.assertNotIn("4.5.1", result.stdout + result.stderr)
        self.assertNotIn("4.6.2", result.stdout + result.stderr)

    def test_current_contract_probe_rejects_godot_451(self):
        result = self.run_script(
            "tests/phase7_contract_probe/run_phase7_contract_probe.ps1",
            "-Godot472",
            str(self.fake_godot("4.5.1")),
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires Godot 4.7.2", result.stdout + result.stderr)

    def test_current_integration_rejects_godot_462_before_launch(self):
        fake_didi = self.root / "didi.exe"
        fake_didi.write_bytes(b"")
        result = self.run_script(
            "tests/run_godot_integration.ps1",
            "-GodotExecutable",
            str(self.fake_godot("4.6.2")),
            "-McpExecutable",
            str(fake_didi),
            "-StartupTimeoutSeconds",
            "1",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires Godot 4.7.2", result.stdout + result.stderr)

    def test_version_guard_does_not_change_caller_strict_mode(self):
        command = (
            f". '{REPOSITORY_ROOT / 'tests' / 'assert_godot_472.ps1'}'; "
            "$response = [pscustomobject]@{}; $null = $response.session_id"
        )
        result = subprocess.run(
            ["pwsh", "-NoLogo", "-NoProfile", "-Command", command],
            cwd=REPOSITORY_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=15,
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
