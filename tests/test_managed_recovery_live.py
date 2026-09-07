"""Real Godot recovery acceptance. Opt in with DIDI_TEST_BINARY and DIDI_RECOVERY_GODOT."""
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest


@unittest.skipUnless(os.environ.get('DIDI_TEST_BINARY') and os.environ.get('DIDI_RECOVERY_GODOT'), 'real Godot recovery test is opt-in')
class ManagedRecoveryLive(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='didi-recovery-live-')
        self.root = Path(self.temp.name)
        # unittest runs cleanups even if initialization or the first RPC fails.
        self.addCleanup(self._cleanup_fixture)
        self._job = None
        self._reader = None
        self.source = self.root / 'source'
        self.source.mkdir()
        self.workspace = self.root / 'managed'
        self.binary = Path(os.environ['DIDI_TEST_BINARY']).resolve()
        addon = self.binary.parent / 'addons' / 'didi'
        if not addon.is_dir():
            addon = self.binary.parent.parent / 'addons' / 'didi'
        shutil.copytree(addon, self.source / 'addons' / 'didi')
        (self.source / 'project.godot').write_text('config_version=5\n[application]\nconfig/name="Recovery acceptance"\nrun/main_scene="res://main.tscn"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/didi/plugin.cfg")\n')
        (self.source / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Root" type="Node"]\n')
        self.original = (self.source / 'main.tscn').read_bytes()
        env = os.environ.copy()
        env['DIDI_SESSION_DIR'] = str(self.root / 'sessions')
        Path(env['DIDI_SESSION_DIR']).mkdir()
        self.stderr = open(self.root / 'host.log', 'w')
        self.host = subprocess.Popen([str(self.binary), '--project', str(self.source), '--managed-editor', os.environ['DIDI_RECOVERY_GODOT'], '--recovery-workspace', str(self.workspace), '--yolo'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr, text=True, encoding='utf-8', env=env, start_new_session=os.name != 'nt')
        if os.name == 'nt':
            self._own_host_job()
        self.lines = queue.Queue()
        def reader():
            try:
                with open(self.root / 'stdout.jsonl', 'w', encoding='utf-8') as transcript:
                    for line in self.host.stdout:
                        transcript.write(line)
                        transcript.flush()
                        self.lines.put(line)
            except BaseException as error:
                self.lines.put(error)
            finally:
                self.lines.put(None)
        self._reader = threading.Thread(target=reader, daemon=True)
        self._reader.start()
        self.seq = 0
        self.request('initialize', {'protocolVersion': '2025-03-26', 'capabilities': {}, 'clientInfo': {'name': 'recovery-test', 'version': '1'}})
        self.host.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
        self.host.stdin.flush()

    def tearDown(self):
        # Cleanup is registered during setUp so setup failures use the same path.
        pass

    def _own_host_job(self):
        """Keep a kernel-owned boundary around this host and its test editors."""
        import ctypes
        from ctypes import wintypes
        class BasicLimits(ctypes.Structure):
            _fields_ = [('ProcessTime', ctypes.c_int64), ('JobTime', ctypes.c_int64),
                        ('Flags', wintypes.DWORD), ('MinWorkingSet', ctypes.c_size_t),
                        ('MaxWorkingSet', ctypes.c_size_t), ('ActiveProcesses', wintypes.DWORD),
                        ('Affinity', ctypes.c_size_t), ('Priority', wintypes.DWORD),
                        ('Scheduling', wintypes.DWORD)]
        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in ('ReadOps', 'WriteOps', 'OtherOps', 'ReadBytes', 'WriteBytes', 'OtherBytes')]
        class ExtendedLimits(ctypes.Structure):
            _fields_ = [('Basic', BasicLimits), ('Io', IoCounters), ('ProcessMemory', ctypes.c_size_t),
                        ('JobMemory', ctypes.c_size_t), ('PeakProcessMemory', ctypes.c_size_t),
                        ('PeakJobMemory', ctypes.c_size_t)]
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateJobObjectW.argtypes = (ctypes.c_void_p, wintypes.LPCWSTR)
        kernel.CreateJobObjectW.restype = wintypes.HANDLE
        kernel.SetInformationJobObject.argtypes = (wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD)
        kernel.AssignProcessToJobObject.argtypes = (wintypes.HANDLE, wintypes.HANDLE)
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel.TerminateJobObject.argtypes = (wintypes.HANDLE, wintypes.UINT)
        self._kernel = kernel
        self._job = kernel.CreateJobObjectW(None, None)
        if not self._job:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = ExtendedLimits()
        limits.Basic.Flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel.SetInformationJobObject(self._job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())
        if not kernel.AssignProcessToJobObject(self._job, int(self.host._handle)):
            raise ctypes.WinError(ctypes.get_last_error())

    def _diagnostics(self):
        sections = []
        for path in [self.root / 'host.log', *sorted((self.root / 'managed').glob('editor-*.log'))]:
            if path.is_file():
                try:
                    sections.append(f'--- {path.name} ---\n{path.read_text(encoding="utf-8", errors="replace")[-6000:]}')
                except OSError as error:
                    sections.append(f'{path.name}: {error}')
        return '\n'.join(sections)

    def _cleanup_fixture(self):
        cleanup_error = None
        try:
            if hasattr(self, 'host'):
                try:
                    self.host.stdin.close()
                except (BrokenPipeError, OSError):
                    pass
                try:
                    self.host.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    if self._job:
                        self._kernel.TerminateJobObject(self._job, 137)
                    elif os.name != 'nt':
                        os.killpg(self.host.pid, signal.SIGKILL)
                    else:
                        self.host.kill()
                    self.host.wait(timeout=10)
        except BaseException as error:
            cleanup_error = error
        finally:
            if self._job:
                self._kernel.CloseHandle(self._job)
                self._job = None
            if self._reader:
                self._reader.join(timeout=5)
            if hasattr(self, 'host'):
                self.host.stdout.close()
            if hasattr(self, 'stderr'):
                self.stderr.close()
            outcome = getattr(self, '_outcome', None)
            result = getattr(outcome, 'result', None)
            failures = list(getattr(result, 'errors', [])) + list(getattr(result, 'failures', []))
            failed = cleanup_error is not None or any(test is self or getattr(test, 'test_case', None) is self for test, _ in failures)
            artifact_root = os.environ.get('DIDI_RECOVERY_ARTIFACT_DIR')
            if failed and artifact_root:
                destination = Path(artifact_root).resolve() / self.root.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copytree(self.root, destination)
                print(f'Recovery failure artifacts: {destination}', file=sys.stderr)
            self.temp.cleanup()
        if cleanup_error:
            raise cleanup_error

    def request(self, method, params):
        self.seq += 1
        request_id = self.seq
        try:
            self.host.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': request_id, 'method': method, 'params': params}) + '\n')
            self.host.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise AssertionError(f'Host input failed: {error}\n{self._diagnostics()}') from error
        deadline = time.monotonic() + 60
        while True:
            try:
                line = self.lines.get(timeout=max(0, deadline - time.monotonic()))
            except queue.Empty as error:
                raise AssertionError(f'RPC {method} {params} timed out\n{self._diagnostics()}') from error
            if line is None:
                raise AssertionError(f'Host stdout closed during {method}\n{self._diagnostics()}')
            if isinstance(line, BaseException):
                raise AssertionError(f'Host stdout reader failed: {line}\n{self._diagnostics()}') from line
            try:
                result = json.loads(line)
            except json.JSONDecodeError as error:
                raise AssertionError(f'Invalid MCP output: {line!r}\n{self._diagnostics()}') from error
            if result.get('id') == request_id:
                self.assertNotIn('error', result, result)
                return result['result']

    def tool(self, tool_name, **args):
        result = self.request('tools/call', {'name': tool_name, 'arguments': args})
        payload = result.get('structuredContent')
        if payload is None:
            payload = json.loads(result['content'][0]['text'])
        return result, payload

    def kill_editor(self, pid):
        if os.name == 'nt':
            # This PID came from this test's owned child and remains held by the host.
            import ctypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.TerminateProcess.argtypes = (ctypes.c_void_p, ctypes.c_uint)
            kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
            kernel.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint)
            handle = kernel.OpenProcess(1 | 0x100000, False, pid)
            self.assertTrue(handle)
            try:
                self.assertTrue(kernel.TerminateProcess(handle, 137))
                self.assertEqual(kernel.WaitForSingleObject(handle, 5000), 0)
            finally:
                kernel.CloseHandle(handle)
        else:
            os.kill(pid, signal.SIGKILL)

    def test_failed_scene_reopen_cannot_be_bypassed_by_next_edit(self):
        result, _ = self.tool('scene_open', scene_path='res://main.tscn')
        self.assertFalse(result['isError'], result)
        _, status = self.tool('runtime_recovery_status')
        project = self.workspace / 'project'
        (project / 'replacement.tscn').write_bytes(self.original)
        (project / 'main.tscn').unlink()
        self.kill_editor(status['pid'])
        result, _ = self.tool('runtime_recover_editor')
        self.assertTrue(result['isError'], result)
        _, failed = self.tool('runtime_recovery_status')
        self.assertEqual(failed['state'], 'scene_reopen_failed')
        result, _ = self.tool('scene_open', scene_path='res://replacement.tscn')
        self.assertTrue(result['isError'], 'A failed recovery must not admit edits')
        _, stopped = self.tool('runtime_recovery_status')
        self.assertEqual(stopped['last_operation'], failed['last_operation'])

    def test_local_rejection_does_not_require_restore(self):
        result, _ = self.tool('scene_remove_node')
        self.assertTrue(result['isError'])
        _, status = self.tool('runtime_recovery_status')
        self.assertFalse(status['requires_reconciliation'])
        result, _ = self.tool('scene_open', scene_path='res://does-not-exist.tscn')
        self.assertTrue(result['isError'])
        _, status = self.tool('runtime_recovery_status')
        self.assertFalse(status['requires_reconciliation'])
        result, _ = self.tool('scene_open', scene_path='res://main.tscn')
        self.assertFalse(result['isError'], result)

    def test_saved_edit_crash_reconnect_restore_and_restart_limit(self):
        _, initial = self.tool('runtime_recovery_status')
        first_pid = initial['pid']
        self.assertTrue(initial['enabled'])
        point = initial['last_checkpoint']['id']
        result, _ = self.tool('scene_open', scene_path='res://main.tscn')
        self.assertFalse(result['isError'], result)
        result, edit = self.tool('scene_instantiate_node', parent_path='/root', node_type='Node', name='RecoveredNode')
        self.assertFalse(result['isError'], result)
        point = edit['recovery']['operation']['before_checkpoint']
        self.assertIn('RecoveredNode', (self.workspace / 'project' / 'main.tscn').read_text())
        self.assertEqual((self.source / 'main.tscn').read_bytes(), self.original)
        self.kill_editor(first_pid)
        # Dry run and status cannot restart a process.
        self.tool('scene_instantiate_node', parent_path='/root', node_type='Node', name='NeverCreated', dry_run=True)
        _, status = self.tool('runtime_recovery_status')
        self.assertEqual(status['pid'], first_pid)
        self.assertFalse(status['automatic_restart_used'])
        result, tree = self.tool('scene_get_hierarchy')
        self.assertFalse(result['isError'], result)
        _, status = self.tool('runtime_recovery_status')
        self.assertNotEqual(status['pid'], first_pid)
        self.assertTrue(status['automatic_restart_used'])
        self.assertEqual((self.workspace / 'project' / 'main.tscn').read_text().count('name="RecoveredNode"'), 1)
        result, restored = self.tool('runtime_restore_checkpoint', checkpoint_id=point)
        self.assertFalse(result['isError'], result)
        self.assertEqual((self.workspace / 'project' / 'main.tscn').read_bytes(), self.original)
        preserved = Path(restored['last_operation']['preserved_workspace'])
        self.assertIn('RecoveredNode', (preserved / 'main.tscn').read_text())
        self.kill_editor(restored['pid'])
        result, _ = self.tool('scene_instantiate_node', parent_path='/root', node_type='Node', name='NotReplayed')
        self.assertTrue(result['isError'])
        _, stopped = self.tool('runtime_recovery_status')
        self.assertEqual(stopped['pid'], restored['pid'])
        self.assertEqual((self.source / 'main.tscn').read_bytes(), self.original)


if __name__ == '__main__':
    unittest.main()
