"""Exercise probe session ownership with real, test-owned stdio children."""

import subprocess
import sys
import tempfile
import time
from pathlib import Path
import unittest
from unittest.mock import patch

from tools.vibe.mcp_client import Session
try:
    from stdio_process import stop
except ImportError:
    from tests.stdio_process import stop


PEER = '''import json, sys, time
request = json.loads(sys.stdin.readline())
print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": {"ready": True}}), flush=True)
sys.stdin.readline()
'''


class ProbeSessionCleanup(unittest.TestCase):
    @staticmethod
    def cleanup_child(child):
        if all(pipe.closed for pipe in (child.stdin, child.stdout, child.stderr)):
            child.wait(timeout=2)
        else:
            stop(child)

    def launch(self, source):
        original = subprocess.Popen

        def child(_argv, **options):
            self.child = original([sys.executable, "-u", "-c", source], **options)
            self.addCleanup(self.cleanup_child, self.child)
            return self.child

        return patch("tools.vibe.mcp_client.subprocess.Popen", side_effect=child)

    def session(self, source):
        with self.launch(PEER + source):
            return Session(binary=sys.executable, editor_log=False)

    def assert_closed(self):
        self.assertIsNotNone(self.child.poll())
        for pipe in (self.child.stdin, self.child.stdout, self.child.stderr):
            self.assertTrue(pipe.closed)

    def test_graceful_close_reaps_and_closes_all_pipes(self):
        session = self.session('sys.stdin.read(); print("shutdown diagnostic", file=sys.stderr)')
        session.close(timeout=1)
        self.assert_closed()
        self.assertEqual(session.stderr().strip(), "shutdown diagnostic")
        session.close(timeout=1)
        self.assert_closed()
        self.assertEqual(session.stderr().strip(), "shutdown diagnostic")

    def test_shutdown_drains_output_larger_than_pipe_capacity(self):
        session = self.session('sys.stdin.read(); sys.stdout.write("x" * 262144); '
                               'sys.stderr.write("y" * 262144)')
        session.close(timeout=2)
        self.assert_closed()
        self.assertEqual(self.child.returncode, 0)
        self.assertEqual(session.stderr(), "y" * 262144)

    def test_timeout_kills_reaps_and_retains_partial_stderr(self):
        session = self.session('sys.stdin.read(); print("before hang", file=sys.stderr, flush=True); time.sleep(30)')
        session.close(timeout=0.2)
        self.assert_closed()
        self.assertNotEqual(self.child.returncode, 0)
        self.assertEqual(session.stderr().strip(), "before hang")

    def test_stderr_read_before_close_is_retained(self):
        session = self.session('print("already exited", file=sys.stderr)')
        self.child.wait(timeout=2)
        self.assertEqual(session.stderr().strip(), "already exited")
        session.close(timeout=1)
        self.assert_closed()
        self.assertEqual(session.stderr().strip(), "already exited")

    def test_pipe_inspection_error_still_reaps_and_closes(self):
        session = self.session('sys.stdin.read(); time.sleep(30)')
        with patch("tools.vibe.mcp_client._read_pipe_now", side_effect=OSError("pipe probe failed")):
            with self.assertRaisesRegex(OSError, "pipe probe failed"):
                session.close(timeout=0.2)
        self.assert_closed()

    def test_failed_handshake_cleans_up_the_owned_process(self):
        with self.launch('import sys; print("bad startup", file=sys.stderr); sys.exit(7)'):
            with self.assertRaisesRegex(RuntimeError, "closed stdout"):
                Session(binary=sys.executable, editor_log=False)
        self.assert_closed()
        self.assertEqual(self.child.returncode, 7)

    def test_inherited_writer_does_not_extend_the_close_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "descendant-finished"
            descendant = "import time; from pathlib import Path; time.sleep(3); Path(%r).touch()" % str(marker)
            source = ('import subprocess\n'
                'sys.stdin.read()\n'
                'print("before descendant", file=sys.stderr, flush=True)\n'
                'subprocess.Popen([sys.executable, "-c", %r], stdin=subprocess.DEVNULL)\n'
                'time.sleep(30)\n') % descendant
            session = self.session(source)
            try:
                started = time.monotonic()
                session.close(timeout=0.2)
                self.assertLess(time.monotonic() - started, 1.5)
                self.assert_closed()
                self.assertEqual(session.stderr().strip(), "before descendant")
            finally:
                # This separate child is owned by the fixture, and must finish
                # before its marker directory is removed, even on a red test.
                limit = time.monotonic() + 5
                while not marker.exists() and time.monotonic() < limit:
                    time.sleep(0.01)
                self.assertTrue(marker.exists())


if __name__ == "__main__":
    unittest.main()
