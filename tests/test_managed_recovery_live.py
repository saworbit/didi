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

try:
    import didi_binary
except ImportError:
    from tests import didi_binary


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
        self.binary = didi_binary.resolve().resolve()
        addon = self.binary.parent / 'addons' / 'didi'
        if not addon.is_dir():
            addon = self.binary.parent.parent / 'addons' / 'didi'
        shutil.copytree(addon, self.source / 'addons' / 'didi')
        (self.source / 'project.godot').write_text('config_version=5\n[application]\nconfig/name="Recovery acceptance"\nrun/main_scene="res://main.tscn"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n[editor_plugins]\nenabled=PackedStringArray("res://addons/didi/plugin.cfg")\n')
        (self.source / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Root" type="Node"]\n')
        if self._testMethodName.startswith('test_observed_'):
            self._observed_checked = set()
            self.addCleanup(self.assert_observation_executed)
            shutil.copyfile(Path(__file__).parent / 'godot_smoke' / 'observed_witness.gd', self.source / 'observed_witness.gd')
            if self._testMethodName == 'test_observed_runtime_run_scenario':
                shutil.copytree(Path(__file__).parent / 'godot_smoke/scenario_runner', self.source / 'scenario_runner')
                counter = self.source / 'scenario_runner/frame_counter.gd'
                counter.write_text(counter.read_text(encoding='utf-8') + '\nfunc _ready() -> void:\n\tvar file := FileAccess.open("res://.didi/observed_process.json", FileAccess.WRITE)\n\tfile.store_string(JSON.stringify({"pid": OS.get_process_id()}))\n', encoding='utf-8')
            (self.source / 'main.tscn').write_text('[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://observed_witness.gd" id="1"]\n[node name="Root" type="Node"]\nscript=ExtResource("1")\n', encoding='utf-8')
        self.original = (self.source / 'main.tscn').read_bytes()
        env = os.environ.copy()
        env['DIDI_SESSION_DIR'] = str(self.root / 'sessions')
        if self._testMethodName.startswith('test_observed_'):
            env['GODOT_BIN'] = os.environ['DIDI_RECOVERY_GODOT']
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
            self._remove_fixture_tree()
        if cleanup_error:
            raise cleanup_error

    def _remove_fixture_tree(self):
        # The workspace is not free the moment the host is. A managed editor is
        # killed when the job handle closes, which is asynchronous by
        # construction: the process that owned it is already gone, so nothing
        # in the product can wait for it. Windows refuses to delete a file
        # another process still has open, and Godot's hot-reload shadow copy of
        # the extension is the one it holds longest.
        #
        # Deleting immediately happened to win against one editor process and
        # lost against two, so the console build turned four passing tests into
        # four teardown errors (#678). Waiting is the honest answer; the
        # deadline is what keeps a real leak a failure rather than a hang.
        deadline = time.monotonic() + (20 if self._testMethodName.startswith('test_observed_') else 60)
        while True:
            try:
                self.temp.cleanup()
                return
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.25)

    def request(self, method, params):
        self.seq += 1
        request_id = self.seq
        try:
            self.host.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': request_id, 'method': method, 'params': params}) + '\n')
            self.host.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise AssertionError(f'Host input failed: {error}\n{self._diagnostics()}') from error
        deadline = time.monotonic() + (20 if self._testMethodName.startswith('test_observed_') else 60)
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

    def witness(self, method, *arguments):
        result, payload = self.tool('scene_call_method', target_node='/root/Root', method_name=method, arguments=list(arguments))
        self.assertFalse(result['isError'], payload)
        self.assertFalse(payload.get('engine_diagnostics'), payload)
        return payload['returned']

    def open_observed_scene(self):
        result, payload = self.tool('scene_open', scene_path='res://main.tscn')
        self.assertFalse(result['isError'], payload)

    def owned_process(self, pid):
        # Holding the process object and its creation time avoids PID reuse.
        # Every editor is a descendant of the host's kill-on-close job.
        if os.name != 'nt':
            self.skipTest('Observed lifecycle ownership uses Windows process handles')
        import ctypes
        from ctypes import wintypes
        kernel = self._kernel
        kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.GetProcessTimes.argtypes = (wintypes.HANDLE,) + (ctypes.POINTER(wintypes.FILETIME),) * 4
        kernel.IsProcessInJob.argtypes = (wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL))
        kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        handle = kernel.OpenProcess(0x1000 | 0x100000, False, pid)
        self.assertTrue(handle, f'Cannot open owned process {pid}')
        self.addCleanup(kernel.CloseHandle, handle)
        owned = wintypes.BOOL()
        self.assertTrue(kernel.IsProcessInJob(handle, self._job, ctypes.byref(owned)))
        self.assertTrue(owned.value, f'Process {pid} is outside this test job')
        times = [wintypes.FILETIME() for _ in range(4)]
        self.assertTrue(kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)))
        created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        self.assertGreater(created, 0)
        return handle, created

    def owned_running(self, process):
        result = self._kernel.WaitForSingleObject(process[0], 0)
        self.assertIn(result, (0, 258), 'The owned process query failed')
        return result == 258

    def assert_owned_gone(self, process):
        self.assertEqual(self._kernel.WaitForSingleObject(process[0], 5000), 0, 'The previous owned process is still running')

    def assert_observation_executed(self):
        tool = self._testMethodName.removeprefix('test_observed_')
        self.assertIn(tool, self._observed_checked, f'{tool} independent observation never executed')
        print(f'Observed external case completed: {tool}')

    def check_observed(self, tool, payload, witness):
        try:
            from observed_case_registry import assert_observed, field_value
        except ImportError:
            from tests.observed_case_registry import assert_observed, field_value
        self.assertFalse(payload.get('engine_diagnostics'), payload)
        assert_observed(self, tool, payload, witness)
        # Each real witness must reject a wrong answer, rather than agreeing
        # vacuously. The actual tool response and actual witness remain intact.
        self._observed_checked.add(tool)
        import copy
        registry = json.loads((Path(__file__).parent / 'observed_post_state.json').read_text())['tools'][tool]
        for field in registry['observed']:
            wrong = copy.deepcopy(payload)
            target = wrong
            parts = field.split('.')
            for part in parts[:-1]:
                target = target[part]
            value = field_value(payload, field)
            target[parts[-1]] = not value if isinstance(value, bool) else value + ['res://wrong_observed_file.gd'] if isinstance(value, list) else value + 1
            with self.assertRaises(AssertionError, msg=f'{tool}.{field} did not reject a mutation'):
                assert_observed(self, tool, wrong, witness)

    def test_observed_editor_reload_project(self):
        self.open_observed_scene()
        project = self.workspace / 'project'
        resource = 'res://observed_new_folder/new_shape.tres'
        self.assertFalse(self.witness('indexed', resource))
        (project / 'observed_new_folder').mkdir()
        (project / 'observed_new_folder/new_shape.tres').write_text('[gd_resource type="BoxShape3D" format=3]\n[resource]\nsize=Vector3(2,3,4)\n', encoding='utf-8')
        result, payload = self.tool('editor_reload_project')
        self.assertFalse(result['isError'], payload)
        indexed = self.witness('indexed', resource)
        self.assertTrue(indexed, 'The scan did not discover a new folder')
        idle = self.witness('scan_idle')
        self.assertTrue(idle, 'The scan was still running when reload answered')
        self.check_observed('editor_reload_project', payload, {'scan_applied': indexed and idle})

    def test_observed_runtime_checkpoint(self):
        self.open_observed_scene()
        result, payload = self.tool('runtime_checkpoint')
        self.assertFalse(result['isError'], payload)
        point = payload['last_checkpoint']['id']
        folder = self.workspace / 'checkpoints' / point / 'files'
        self.assertTrue(folder.is_dir())
        inventory = self.witness('snapshot_files', str(folder))
        self.assertGreater(inventory['files'], 0)
        self.assertGreater(inventory['bytes'], 0)
        self.check_observed('runtime_checkpoint', payload, {'last_checkpoint': inventory})

    def test_observed_runtime_recover_editor(self):
        self.open_observed_scene()
        _, before = self.tool('runtime_recovery_status')
        self.assertEqual(before['editor_pid'], self.witness('process_id'))
        editor = self.owned_process(before['editor_pid'])
        launcher = self.owned_process(before['pid'])
        self.kill_editor(before['editor_pid'])
        self.assert_owned_gone(editor)
        self.assert_owned_gone(launcher)
        result, payload = self.tool('runtime_recover_editor')
        self.assertFalse(result['isError'], payload)
        self.assertNotEqual(payload['editor_pid'], before['editor_pid'])
        fresh = self.owned_process(payload['editor_pid'])
        self.assertNotEqual(fresh[1], editor[1])
        engine_pid = self.witness('process_id')
        running = self.owned_running(self.owned_process(payload['pid']))
        self.check_observed('runtime_recover_editor', payload, {'editor_pid': engine_pid, 'editor_running': running})

    def test_observed_runtime_restore_checkpoint(self):
        self.open_observed_scene()
        _, checkpoint = self.tool('runtime_checkpoint')
        before = self.owned_process(checkpoint['editor_pid'])
        point = checkpoint['last_checkpoint']['id']
        file = self.workspace / 'project/main.tscn'
        file.write_bytes(self.original + b'\n[node name="Discarded" type="Node" parent="."]\n')
        result, payload = self.tool('runtime_restore_checkpoint', checkpoint_id=point)
        self.assertFalse(result['isError'], payload)
        self.assert_owned_gone(before)
        fresh = self.owned_process(payload['editor_pid'])
        self.assertNotEqual(fresh[1], before[1])
        self.assertEqual(file.read_bytes(), self.original)
        preserved = Path(payload['last_operation']['preserved_workspace'])
        self.assertIn(b'Discarded', (preserved / 'main.tscn').read_bytes())
        self.open_observed_scene()
        self.assertEqual(self.witness('scene_nodes', 'res://main.tscn'), ['Root'])
        self.check_observed('runtime_restore_checkpoint', payload,
                            {'editor_pid': self.witness('process_id'), 'editor_running': self.owned_running(self.owned_process(payload['pid']))})

    def assert_powershell_observation(self, case, data):
        path = self.root / ('observed-' + case + '.json')
        path.write_text(json.dumps(data), encoding='utf-8')
        checked = subprocess.run(['powershell', '-NoProfile', '-File', str(Path(__file__).parent / 'run_observed_external.ps1'), '-Case', case, '-InputPath', str(path)], capture_output=True, text=True, timeout=5)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_observed_project_apply_changes(self):
        self.open_observed_scene()
        project = self.workspace / 'project'
        resource = 'res://apply_observed.gd'
        (project / 'apply_observed.gd').write_text('extends Node\nfunc beta():\n\treturn 2\n', encoding='utf-8')
        self.assertTrue(self.witness('hold', resource))
        (project / '.gitignore').write_text('.godot/\n.didi/\n.worktrees/\n', encoding='utf-8')
        # Inherit the real user's Git identity; this disposable repository
        # never overrides user.name or user.email.
        for args in (['init', '--quiet'], ['add', '.'], ['-c', 'core.autocrlf=false', 'commit', '--quiet', '-m', 'Observed apply fixture']):
            done = subprocess.run(['git'] + args, cwd=project, capture_output=True, text=True, timeout=5)
            self.assertEqual(done.returncode, 0, done.stderr)
        expected = 'extends Node\nfunc delta():\n\treturn 4\n'
        result, payload = self.tool('project_apply_changes', changes=[{'path': resource, 'content': expected}], timeout_seconds=15)
        self.assertFalse(result['isError'], payload)
        held = self.witness('held_copy', resource)
        text = self.witness('file_text', resource)
        self.assert_powershell_observation('Apply', {'answer': payload, 'held': held, 'path': resource, 'expected': expected, 'file_text': text})
        current = 'delta' in held['methods'] and 'beta' not in held['methods']
        self.check_observed('project_apply_changes', payload, {'editor_copies_reloaded': [resource] if current else []})

    def test_observed_runtime_run_scenario(self):
        self.open_observed_scene()
        editor_pid = self.witness('process_id')
        editor = self.owned_process(editor_pid)
        # The game records its own PID before stepping. Hold its native
        # process object while alive, including creation time and job identity,
        # so teardown reads the same process even if its PID is later reused.
        record = self.workspace / 'project/.didi/observed_process.json'
        record.parent.mkdir(exist_ok=True)
        captured = queue.Queue()
        cancelled = threading.Event()
        def capture_game():
            deadline = time.monotonic() + 18
            while not cancelled.is_set() and time.monotonic() < deadline:
                try:
                    pid = json.loads(record.read_text())['pid']
                    captured.put((pid, self.owned_process(pid)))
                    return
                except (FileNotFoundError, json.JSONDecodeError):
                    cancelled.wait(.01)
                except BaseException as error:
                    captured.put(error)
                    return
        reader = threading.Thread(target=capture_game, daemon=True)
        reader.start()
        try:
            result, payload = self.tool('runtime_run_scenario', name='observed_teardown', scene_path='res://scenario_runner/frame_counter.tscn', timeout_seconds=15, steps=[
                {'kind': 'wait', 'frames': 1},
                {'kind': 'assert', 'expression': "node.get('position').x", 'context_node': '/root/FrameCounter', 'minimum': 1, 'maximum': 1}])
            self.assertFalse(result['isError'], payload)
            identity = captured.get(timeout=2)
            if isinstance(identity, BaseException):
                raise identity
            game_pid, game = identity
            self.assertEqual(payload['game']['pid'], game_pid, 'The answer did not name the game that recorded its own PID')
            game_running = self.owned_running(game)
            editor_running = self.owned_running(editor)
        finally:
            cancelled.set()
            reader.join(timeout=2)
            self.assertFalse(reader.is_alive(), 'The game identity reader did not stop')
        self.assertEqual(editor_pid, self.witness('process_id'))
        self.assertEqual(editor[1], self.owned_process(editor_pid)[1])
        self.assert_powershell_observation('Scenario', {'answer': payload, 'game_running': game_running, 'editor_running': editor_running})
        self.check_observed('runtime_run_scenario', payload, {'teardown': {'session_gone': not game_running}})

    def test_elastic_ingress_live_read_only_roundtrip(self):
        tools = self.request('tools/list', {})['tools']
        hit_test = next(tool for tool in tools if tool['name'] == 'ui_hit_test')
        profile = hit_test['_meta']['didi'].get('argumentNormalization')
        if profile is None:
            self.skipTest('elastic ingress is disabled in this build')
        self.assertEqual(profile['profile'], 'safe-v1')
        scene = self.workspace / 'project' / 'elastic_ui.tscn'
        scene.write_text('[gd_scene format=3]\n[node name="UI" type="Control"]\n'
                         'offset_right = 640.0\noffset_bottom = 480.0\n'
                         '[node name="Go" type="Button" parent="."]\n'
                         'offset_left = 20.0\noffset_top = 20.0\n'
                         'offset_right = 180.0\noffset_bottom = 80.0\ntext = "Go"\n')
        opened, _ = self.tool('scene_open', scene_path='res://elastic_ui.tscn')
        self.assertFalse(opened['isError'], opened)
        before = scene.read_bytes()

        def call(name, args, normalized=True):
            params = {'name': name, 'arguments': args}
            if normalized:
                params['_meta'] = {profile['requestMetaKey']: profile['profile']}
            return self.request('tools/call', params)

        pairs = [
            ('scene_get_hierarchy', {'max_depth': '2', 'max_nodes': '10'},
             {'max_depth': 2, 'max_nodes': 10}),
            ('ui_list_controls', {'max_results': '10'}, {'max_results': 10}),
            ('ui_hit_test', {'point': ['40', '40'], 'max_results': '5'},
             {'point': {'x': 40, 'y': 40}, 'max_results': 5}),
        ]
        for name, loose, canonical in pairs:
            with self.subTest(tool=name):
                result = call(name, loose)
                self.assertFalse(result['isError'], result)
                self.assertTrue(result['structuredContent']['is_live_engine'])
                self.assertEqual(result, call(name, canonical, False))

        invalid = [('ui_list_controls', {'max_results': value}) for value in
                   ('+2', ' 2', '2 ', '2px', '2.0', '1e1', '0', '257', True, None)]
        invalid += [('ui_hit_test', {'point': point}) for point in
                    ([], [1], [1, 2, 3], [True, 2], [1, 'NaN'], [1, '1e999'],
                     [1, 'secret-value'], {'x': 1}, [1, '9007199254740993'])]
        invalid += [('ui_list_controls', args) for args in
                    ({'extra': 'secret-value'}, {'max_results': '5', 'include_text': 'true'},
                     {'extra': [0] * 4100})]
        for index, (name, args) in enumerate(invalid):
            with self.subTest(malformed_case=index):
                result = call(name, args)
                self.assertTrue(result['isError'], result)
                self.assertNotIn('secret-value', json.dumps(result))
                self.assertLess(len(json.dumps(result)), 2048)
                recovered = call('ui_hit_test', {'point': ['40', '40']})
                self.assertFalse(recovered['isError'], recovered)
                payload = recovered['structuredContent']
                self.assertEqual(payload['topmost']['node_path'], '/root/UI/Go')
                self.assertFalse(payload['input_injected'])
        self.assertTrue(call('ui_hit_test', {'point': ['40', '40']}, False)['isError'])
        self.assertEqual(scene.read_bytes(), before)
        self.assertEqual((self.source / 'main.tscn').read_bytes(), self.original)


if __name__ == '__main__':
    unittest.main()
