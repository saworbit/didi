"""Real-editor crash and checkpoint attacks; uses only children owned by this test."""
import hashlib
import json
from pathlib import Path
import threading
import time
import unittest

import test_managed_recovery_live as live


class ManagedRecoveryAdversarial(live.ManagedRecoveryLive):
    # This module reuses the transport fixture without repeating its acceptance test.
    test_saved_edit_crash_reconnect_restore_and_restart_limit = None
    test_local_rejection_does_not_require_restore = None
    test_failed_scene_reopen_cannot_be_bypassed_by_next_edit = None

    def source_digest(self):
        return {str(path.relative_to(self.source)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in self.source.rglob('*') if path.is_file()}

    def prepare_delayed_signal(self, delay_ms=15000):
        project = self.workspace / 'project'
        (project / 'adversarial.gd').write_text('''@tool
extends Node
signal hold_requested

func _hold() -> void:
    var mode = FileAccess.READ_WRITE if FileAccess.file_exists("res://attempts.txt") else FileAccess.WRITE
    var marker = FileAccess.open("res://attempts.txt", mode)
    marker.seek_end()
    marker.store_line("started")
    marker.flush()
    marker.close()
    OS.delay_msec(DELAY_MS)
    marker = FileAccess.open("res://attempts.txt", FileAccess.READ_WRITE)
    marker.seek_end()
    marker.store_line("completed")
    marker.close()
'''.replace('DELAY_MS', str(delay_ms)), encoding='utf-8')
        (project / 'adversarial.tscn').write_text('''[gd_scene load_steps=2 format=3]
[ext_resource type="Script" path="res://adversarial.gd" id="1"]
[node name="CrashFixture" type="Node"]
[node name="Probe" type="Node" parent="."]
script = ExtResource("1")
[connection signal="hold_requested" from="Probe" to="Probe" method="_hold"]
''', encoding='utf-8')
        result, _ = self.tool('scene_open', scene_path='res://adversarial.tscn')
        self.assertFalse(result['isError'], result)
        result, ready = self.tool('runtime_checkpoint')
        self.assertFalse(result['isError'], result)
        return ready

    def test_inflight_death_latches_mutations_without_replaying_and_restore_reconciles(self):
        original_source = self.source_digest()
        project = self.workspace / 'project'
        ready = self.prepare_delayed_signal()
        checkpoint_id = ready['last_checkpoint']['id']
        owned_pid = ready['pid']
        responses = []
        errors = []

        def emit():
            try:
                responses.append(self.tool('signal_emit', target_node='Probe', signal_name='hold_requested', arguments=[]))
            except BaseException as error:
                errors.append(error)

        operation = threading.Thread(target=emit, daemon=True)
        operation.start()
        marker = project / 'attempts.txt'
        deadline = time.monotonic() + 20
        while not marker.exists() and operation.is_alive() and time.monotonic() < deadline:
            time.sleep(0.02)
        if not marker.exists():
            operation.join(timeout=45)
            self.fail(f'Godot mutation did not reach its durable marker: responses={responses}, errors={errors}')
        self.assertTrue(operation.is_alive(), 'Mutation must still be in flight when the editor is killed')
        journal = json.loads((self.workspace / 'recovery.json').read_text())
        self.assertEqual(journal['operation']['tool'], 'signal_emit')
        self.assertEqual(journal['operation']['outcome'], 'pending')
        self.assertTrue(journal['requires_reconciliation'])
        self.kill_editor(owned_pid)
        operation.join(timeout=45)
        self.assertFalse(operation.is_alive(), 'Dead editor must release the outstanding tool request')
        self.assertFalse(errors, errors)
        self.assertEqual(len(responses), 1)
        result, failed = responses[0]
        self.assertTrue(result['isError'], result)
        self.assertTrue(failed['recovery']['requires_reconciliation'])
        self.assertEqual(marker.read_text().splitlines(), ['started'])

        # Status and dry runs observe the dead child without launching one.
        result, _ = self.tool('runtime_recover_editor', dry_run=True)
        self.assertFalse(result['isError'], result)
        _, dry_recovery = self.tool('runtime_recovery_status')
        self.assertEqual(dry_recovery['pid'], owned_pid)
        self.assertFalse(dry_recovery['editor_running'])
        self.assertFalse(dry_recovery['automatic_restart_used'])
        self.assertTrue(dry_recovery['requires_reconciliation'])
        result, _ = self.tool('runtime_recover_editor')
        self.assertFalse(result['isError'], result)
        _, reconnected = self.tool('runtime_recovery_status')
        self.assertNotEqual(reconnected['pid'], owned_pid)
        self.assertTrue(reconnected['automatic_restart_used'])
        self.assertTrue(reconnected['requires_reconciliation'])
        self.assertEqual(marker.read_text().splitlines(), ['started'])
        result, _ = self.tool('scene_instantiate_node', parent_path='/root', node_type='Node', name='MustBeBlocked')
        self.assertTrue(result['isError'], result)
        self.assertNotIn('MustBeBlocked', (project / 'adversarial.tscn').read_text())
        result, _ = self.tool('scene_get_hierarchy')
        self.assertFalse(result['isError'], result)
        result, _ = self.tool('runtime_checkpoint')
        self.assertTrue(result['isError'], 'An unknown outcome must require explicit checkpoint acceptance')
        result, accepted = self.tool('runtime_checkpoint', accept_current_files=True)
        self.assertFalse(result['isError'], 'After the old editor is proven dead, inspected saved files can be accepted')
        self.assertFalse(accepted['requires_reconciliation'])

        result, restored = self.tool('runtime_restore_checkpoint', checkpoint_id=checkpoint_id)
        self.assertFalse(result['isError'], result)
        self.assertFalse(restored['requires_reconciliation'])
        self.assertFalse(marker.exists(), 'Restoring the pre-mutation checkpoint must remove the interrupted side effect')
        preserved = Path(restored['last_operation']['preserved_workspace'])
        self.assertEqual((preserved / 'attempts.txt').read_text().splitlines(), ['started'])
        result, _ = self.tool('scene_open', scene_path='res://adversarial.tscn')
        self.assertFalse(result['isError'], result)
        result, _ = self.tool('scene_instantiate_node', parent_path='/root', node_type='Node', name='AllowedAfterRestore')
        self.assertFalse(result['isError'], result)
        self.assertIn('AllowedAfterRestore', (project / 'adversarial.tscn').read_text())
        self.kill_editor(restored['pid'])
        result, _ = self.tool('runtime_recover_editor')
        self.assertTrue(result['isError'], 'Explicit recovery cannot bypass the already consumed restart budget')
        _, stopped = self.tool('runtime_recovery_status')
        self.assertEqual(stopped['pid'], restored['pid'])
        self.assertFalse(stopped['editor_running'])
        self.assertTrue(stopped['automatic_restart_used'])
        self.assertEqual(self.source_digest(), original_source)

    def test_accepting_files_cannot_clear_uncertainty_while_editor_is_still_executing(self):
        original_source = self.source_digest()
        ready = self.prepare_delayed_signal(delay_ms=30000)
        result, _ = self.tool('signal_emit', target_node='Probe', signal_name='hold_requested', arguments=[])
        self.assertTrue(result['isError'], 'The bounded IPC deadline must expire before the delayed callback finishes')
        _, pending = self.tool('runtime_recovery_status')
        self.assertEqual(pending['pid'], ready['pid'])
        self.assertTrue(pending['editor_running'])
        self.assertTrue(pending['requires_reconciliation'])
        marker = self.workspace / 'project' / 'attempts.txt'
        self.assertEqual(marker.read_text().splitlines(), ['started'], 'The callback must still be incomplete')
        journal = (self.workspace / 'recovery.json').read_bytes()
        checkpoints = sorted(path.name for path in (self.workspace / 'checkpoints').iterdir())
        result, _ = self.tool('runtime_checkpoint', accept_current_files=True)
        self.assertTrue(result['isError'], 'Explicit file acceptance cannot settle an operation still running in the editor')
        _, blocked = self.tool('runtime_recovery_status')
        self.assertTrue(blocked['requires_reconciliation'])
        self.assertEqual(blocked['pid'], ready['pid'])
        self.assertEqual((self.workspace / 'recovery.json').read_bytes(), journal)
        self.assertEqual(sorted(path.name for path in (self.workspace / 'checkpoints').iterdir()), checkpoints)
        self.assertEqual(marker.read_text().splitlines(), ['started'])
        result, restored = self.tool('runtime_restore_checkpoint', checkpoint_id=ready['last_checkpoint']['id'])
        self.assertFalse(result['isError'], result)
        self.assertNotEqual(restored['pid'], ready['pid'])
        self.assertFalse(restored['requires_reconciliation'])
        self.assertEqual(self.source_digest(), original_source)

    def test_restore_dry_run_and_corrupt_checkpoint_preserve_running_editor_and_files(self):
        original_source = self.source_digest()
        _, initial = self.tool('runtime_recovery_status')
        checkpoint_id = initial['last_checkpoint']['id']
        result, _ = self.tool('scene_open', scene_path='res://main.tscn')
        self.assertFalse(result['isError'], result)
        result, _ = self.tool('scene_instantiate_node', parent_path='/root', node_type='Node', name='MustSurvive')
        self.assertFalse(result['isError'], result)
        project_scene = self.workspace / 'project' / 'main.tscn'
        edited = project_scene.read_bytes()
        _, before = self.tool('runtime_recovery_status')
        journal = (self.workspace / 'recovery.json').read_bytes()
        checkpoints = sorted(path.name for path in (self.workspace / 'checkpoints').iterdir())
        result, _ = self.tool('runtime_restore_checkpoint', checkpoint_id=checkpoint_id, dry_run=True)
        self.assertFalse(result['isError'], result)
        _, dry = self.tool('runtime_recovery_status')
        self.assertEqual(dry['pid'], before['pid'])
        self.assertTrue(dry['editor_running'])
        self.assertEqual(dry['last_operation'], before['last_operation'])
        self.assertEqual(project_scene.read_bytes(), edited)
        self.assertEqual((self.workspace / 'recovery.json').read_bytes(), journal)
        self.assertEqual(sorted(path.name for path in (self.workspace / 'checkpoints').iterdir()), checkpoints)
        self.assertFalse(list(self.workspace.glob('preserved-*')))
        self.assertFalse(list(self.workspace.glob('restore-*')))

        damaged = self.workspace / 'checkpoints' / checkpoint_id / 'files' / 'main.tscn'
        valid_bytes = damaged.read_bytes()
        self.assertIn(b'Root', valid_bytes)
        damaged.write_bytes(valid_bytes.replace(b'Root', b'Evil'))  # Same size: digest must detect it.
        result, _ = self.tool('runtime_restore_checkpoint', checkpoint_id=checkpoint_id)
        self.assertTrue(result['isError'], 'A tampered checkpoint must be rejected')
        _, rejected = self.tool('runtime_recovery_status')
        self.assertEqual(rejected['pid'], before['pid'])
        self.assertTrue(rejected['editor_running'], 'Validate the checkpoint before stopping the owned editor')
        self.assertEqual(project_scene.read_bytes(), edited)
        self.assertFalse(list(self.workspace.glob('preserved-*')))
        result, hierarchy = self.tool('scene_get_hierarchy')
        self.assertFalse(result['isError'], result)
        self.assertIn('MustSurvive', json.dumps(hierarchy))
        self.assertEqual(self.source_digest(), original_source)


if __name__ == '__main__':
    unittest.main()
