"""Exercise the real CMake identity setup in disposable Git work trees."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cmake") and shutil.which("git"), "CMake and Git are required")
class BuildIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="didi identity ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.source.mkdir()
        # Keep the production identity wiring and templates. Compilers and the
        # rest of Didi are unnecessary for observing the generated source.
        prefix = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8").split(
            "set(CMAKE_CXX_STANDARD 20)", 1)[0]
        prefix = re.sub(r"LANGUAGES C CXX", "LANGUAGES NONE", prefix, count=1)
        (self.source / "CMakeLists.txt").write_text(
            prefix + '\nadd_custom_target(identity_probe ALL DEPENDS '
            '"${CMAKE_BINARY_DIR}/generated/src/didi_build_id.cpp")\n',
            encoding="utf-8")
        shutil.copytree(ROOT / "cmake", self.source / "cmake")
        self.run_command("git", "init", cwd=self.source)
        self.run_command("git", "config", "user.name", "Build Test", cwd=self.source)
        self.run_command("git", "config", "user.email", "build@example.invalid", cwd=self.source)
        self.run_command("git", "config", "commit.gpgsign", "false", cwd=self.source)
        self.run_command("git", "config", "core.hooksPath", str(self.root / "hooks"), cwd=self.source)
        self.commit("initial")

    def run_command(self, *arguments, cwd=None, env=None):
        result = subprocess.run(arguments, cwd=cwd, capture_output=True, text=True, timeout=10,
                                env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def commit(self, text, date=None):
        (self.source / "revision.txt").write_text(text, encoding="utf-8")
        self.run_command("git", "add", ".", cwd=self.source)
        env = None
        if date:
            env = dict(os.environ, GIT_COMMITTER_DATE=date, GIT_AUTHOR_DATE=date)
        self.run_command("git", "commit", "-m", text, cwd=self.source, env=env)
        return self.run_command("git", "rev-parse", "--short=12", "HEAD", cwd=self.source)

    def configure(self, source=None, build=None, *options):
        generator = ("-G", "Ninja") if shutil.which("ninja") else ()
        self.run_command("cmake", "-S", str(source or self.source), "-B", str(build or self.build),
                         *generator, *options)

    def build_identity(self, build=None):
        build = build or self.build
        self.run_command("cmake", "--build", str(build), "--target", "identity_probe")
        output = build / "generated/src/didi_build_id.cpp"
        match = re.search(r'kBuildId = "([^"]+)";', output.read_text(encoding="utf-8"))
        self.assertIsNotNone(match)
        return match.group(1), output

    def test_commit_updates_without_reconfigure_and_noop_keeps_output(self):
        self.configure()
        initial, _ = self.build_identity()
        # Dated before the configure, so the configure's stamp stands.
        commit = self.commit("next", date="2001-02-03T04:05:06Z")
        updated, output = self.build_identity()
        self.assertIn("+" + commit + ".", updated)
        self.assertNotEqual(initial, updated)
        self.assertEqual(initial.rsplit(".", 1)[1], updated.rsplit(".", 1)[1])
        modified = output.stat().st_mtime_ns
        self.assertEqual(self.build_identity()[0], updated)
        self.assertEqual(output.stat().st_mtime_ns, modified)
        output.unlink()
        self.assertEqual(self.build_identity()[0], updated)

    def test_a_commit_later_than_the_configure_raises_the_stamp(self):
        # Two commits built in one build directory carried the configure's
        # stamp, so setup could not tell which was newer (#1450). The later of
        # the configure time and the commit time orders them along history.
        self.configure()
        configured = self.build_identity()[0].rsplit(".", 1)[1]
        self.commit("from the past", date="2001-02-03T04:05:06Z")
        self.assertEqual(self.build_identity()[0].rsplit(".", 1)[1], configured)
        later = self.commit("from the future", date="2099-01-02T03:04:05Z")
        identity, output = self.build_identity()
        self.assertTrue(identity.endswith("+" + later + ".20990102T030405"), identity)
        modified = output.stat().st_mtime_ns
        self.assertEqual(self.build_identity()[0], identity)
        self.assertEqual(output.stat().st_mtime_ns, modified)

    def test_checkout_detached_and_packed_refs_refresh_the_commit(self):
        original = self.run_command("git", "rev-parse", "--short=12", "HEAD", cwd=self.source)
        next_commit = self.commit("next")
        self.run_command("git", "branch", "packed", "HEAD~1", cwd=self.source)
        self.run_command("git", "pack-refs", "--all", "--prune", cwd=self.source)
        self.configure()
        self.build_identity()
        self.run_command("git", "checkout", "--detach", "HEAD~1", cwd=self.source)
        self.assertIn("+" + original + ".", self.build_identity()[0])
        self.run_command("git", "checkout", "--detach", next_commit, cwd=self.source)
        self.assertIn("+" + next_commit + ".", self.build_identity()[0])
        self.run_command("git", "checkout", "packed", cwd=self.source)
        self.assertIn("+" + original + ".", self.build_identity()[0])

    def test_linked_worktree_reports_its_own_head(self):
        first = self.run_command("git", "rev-parse", "--short=12", "HEAD", cwd=self.source)
        self.commit("next")
        linked = self.root / "linked worktree"
        self.run_command("git", "worktree", "add", "--detach", str(linked), "HEAD~1", cwd=self.source)
        linked_build = self.root / "linked build"
        self.configure(linked, linked_build)
        self.assertIn("+" + first + ".", self.build_identity(linked_build)[0])
        # A worktree's HEAD has its own reflog; use the main source explicitly.
        current = self.run_command("git", "rev-parse", "--short=12", "HEAD", cwd=self.source)
        self.run_command("git", "checkout", "--detach", current, cwd=linked)
        self.assertIn("+" + current + ".", self.build_identity(linked_build)[0])

    def test_without_git_keeps_the_archive_identity(self):
        self.configure(None, None, "-DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE")
        identity, output = self.build_identity()
        self.assertIn("+nogit.", identity)
        modified = output.stat().st_mtime_ns
        self.assertEqual(self.build_identity()[0], identity)
        self.assertEqual(output.stat().st_mtime_ns, modified)


if __name__ == "__main__":
    unittest.main()
