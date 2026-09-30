"""A scene opened straight after an editor starts stays the edited scene.

Opt in with DIDI_TEST_BINARY (the server) and DIDI_STARTUP_GODOT (a Godot
editor binary).

An editor publishes its session before it has finished starting. It opens the
scenes it restores, or the project's main scene, once its first scan of the
project is applied, and makes one of them current. A scene_open answered before
then said opened: true and was replaced a moment later, so every later scene
call acted on the main scene (#1069).
"""
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import subprocess
import tempfile
import threading
import time
import unittest

try:
    import didi_binary
    import stdio_process
except ImportError:
    from tests import didi_binary, stdio_process

READS = 10


@unittest.skipUnless(os.environ.get('DIDI_TEST_BINARY') and os.environ.get('DIDI_STARTUP_GODOT'),
                     'real Godot startup test is opt-in')
class EditorStartupSceneLive(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='didi-startup-live-')
        self.addCleanup(self._cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / 'project'
        self.project.mkdir()
        self.binary = didi_binary.resolve().resolve()
        addon = self.binary.parent / 'addons' / 'didi'
        if not addon.is_dir():
            addon = self.binary.parent.parent / 'addons' / 'didi'
        shutil.copytree(addon, self.project / 'addons' / 'didi')
        (self.project / 'project.godot').write_text(
            'config_version=5\n[application]\nconfig/name="Startup scene"\n'
            'run/main_scene="res://main.tscn"\n[rendering]\n'
            'renderer/rendering_method="gl_compatibility"\n[editor_plugins]\n'
            'enabled=PackedStringArray("res://addons/didi/plugin.cfg")\n')
        (self.project / 'main.tscn').write_text('[gd_scene format=3]\n[node name="Main" type="Node2D"]\n')
        (self.project / 'tab.tscn').write_text('[gd_scene format=3]\n[node name="Tab" type="Node2D"]\n')
        self.godot = os.environ['DIDI_STARTUP_GODOT']
        self.env = os.environ.copy()
        self.env['DIDI_SESSION_DIR'] = str(self.root / 'sessions')
        Path(self.env['DIDI_SESSION_DIR']).mkdir()
        self.editor = None
        self.session_pid = None
        self.host = None
        # Imported once headless, so the editor below is the first to open the
        # project and its startup opens the main scene.
        subprocess.run([self.godot, '--headless', '--path', str(self.project), '--import'],
                       capture_output=True, timeout=600, env=self.env)

    def _cleanup(self):
        if self.host is not None:
            try:
                self.host.stdin.close()
            except OSError:
                pass
            try:
                self.host.wait(timeout=20)
            except subprocess.TimeoutExpired:
                pass
            stdio_process.stop(self.host)
        if self.editor is not None:
            # A console build is a launcher and the editor is its child, so the
            # tree goes, and the pid the session published after it.
            if os.name == 'nt':
                subprocess.run(['taskkill', '/T', '/F', '/PID', str(self.editor.pid)], capture_output=True)
            elif self.editor.poll() is None:
                os.killpg(self.editor.pid, signal.SIGKILL)
            if self.session_pid:
                try:
                    os.kill(self.session_pid, signal.SIGTERM)
                except OSError:
                    pass
            try:
                self.editor.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.editor.kill()
        for _ in range(20):
            try:
                self.temp.cleanup()
                return
            except OSError:
                time.sleep(0.5)

    def _start_host(self):
        self.host = subprocess.Popen([str(self.binary), '--project', str(self.project)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, encoding='utf-8', env=self.env)
        self.lines = queue.Queue()

        def reader():
            for line in self.host.stdout:
                self.lines.put(line)
            self.lines.put(None)

        threading.Thread(target=reader, daemon=True).start()
        self.seq = 0
        self.request('initialize', {'protocolVersion': '2025-03-26', 'capabilities': {},
                                    'clientInfo': {'name': 'startup-test', 'version': '1'}})
        self.host.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'}) + '\n')
        self.host.stdin.flush()

    def request(self, method, params):
        self.seq += 1
        self.host.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': self.seq, 'method': method,
                                          'params': params}) + '\n')
        self.host.stdin.flush()
        deadline = time.monotonic() + 60
        while True:
            line = self.lines.get(timeout=max(0, deadline - time.monotonic()))
            self.assertIsNotNone(line, f'the server closed its output during {method}')
            message = json.loads(line)
            # Notifications carry no id and are interleaved with the replies.
            if message.get('id') == self.seq:
                self.assertNotIn('error', message, message)
                return message['result']

    def tool(self, name, **arguments):
        result = self.request('tools/call', {'name': name, 'arguments': arguments})
        payload = result.get('structuredContent')
        if payload is None:
            payload = json.loads(result['content'][0]['text'])
        return result.get('isError', False), payload

    def test_a_scene_opened_at_startup_stays_current(self):
        self._start_host()
        launcher_options = {'start_new_session': True} if os.name != 'nt' else {}
        self.editor = subprocess.Popen([self.godot, '--editor', '--path', str(self.project)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       env=self.env, **launcher_options)
        session = None
        deadline = time.monotonic() + 180
        while session is None and time.monotonic() < deadline:
            _, listed = self.tool('runtime_list_sessions')
            session = next((entry for entry in listed.get('sessions', [])
                            if entry.get('kind') == 'editor' and entry.get('alive') is not False), None)
            if session is None:
                time.sleep(0.25)
        self.assertIsNotNone(session, 'the editor published no session in 180 s')
        self.session_pid = int(session['pid'])
        errored, attached = self.tool('runtime_attach_session', session_id=session['session_id'])
        self.assertFalse(errored, attached)

        # A caller may be told to retry while the editor is still starting;
        # what it may not be told is that the scene is open when it will not be.
        deadline = time.monotonic() + 120
        while True:
            errored, opened = self.tool('scene_open', scene_path='res://tab.tscn')
            data = opened.get('data', {}) if errored else {}
            if not errored or data.get('outcome') != 'not_started' or time.monotonic() > deadline:
                break
            time.sleep(1)
        self.assertFalse(errored, opened)
        self.assertTrue(opened['opened'], opened)

        reads = []
        for _ in range(READS):
            time.sleep(1)
            errored, hierarchy = self.tool('scene_get_hierarchy', max_depth=1)
            reads.append(hierarchy if errored else hierarchy.get('scene_file_path'))
        self.assertEqual(reads, ['res://tab.tscn'] * READS)


if __name__ == '__main__':
    unittest.main()
