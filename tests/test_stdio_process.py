import subprocess
import sys
import unittest

try:
    from stdio_process import stop
except ImportError:
    from tests.stdio_process import stop


class StdioProcessCleanup(unittest.TestCase):
    def child(self, source):
        child = subprocess.Popen([sys.executable, "-u", "-c", source],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        self.addCleanup(stop, child)
        return child

    def assert_closed(self, child):
        self.assertIsNotNone(child.poll())
        for pipe in (child.stdin, child.stdout, child.stderr):
            self.assertTrue(pipe.closed)

    def test_running_child_is_reaped_and_pipes_are_closed(self):
        child = self.child("import sys; sys.stdin.read()")
        stop(child)
        self.assert_closed(child)
        stop(child)  # Repeated cleanup is safe after a failed test's finally.
        self.assert_closed(child)

    def test_already_exited_child_is_drained_and_closed(self):
        child = self.child("print('remaining output')")
        child.wait(timeout=10)
        stop(child)
        self.assert_closed(child)
