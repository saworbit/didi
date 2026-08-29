from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
PLAN = ROOT / "docs" / "PHASE_7_PARTIAL_IMPLEMENTATION_PLAN.md"
PATH_SUFFIXES = (".cpp", ".hpp", ".py", ".ps1", ".gd", ".tscn", ".json",
                 ".yml", ".md", ".txt", ".godot")
SPECIAL_PATHS = {"CMakeLists.txt", "README.md", "CHANGELOG.md", "SECURITY.md"}
EXACT_PROSE_BOUNDARIES = {
    "optional session kind, shared guard declaration, test access",
    "profiler state and scheduler declarations",
    "top-of-queue and direct-dispatch guards",
    "profiler scheduler/interception",
    "external method admission table only",
    "generated schema and alias resolution",
    "enable exactly 15 capability rows only",
    "historical 60/18 and alias/schema assertions",
    "current 75/3 capability assertions only",
    "parent-compatible registry and identity RED",
    "public list-tools 75/3 assertions only",
    "session guard/zero-dispatch matrix",
    "authenticated admission and route matrix",
    "raw authenticated method matrix",
    "public registered-tool matrix and count assertions",
    "generator/contract-probe jobs and path filters",
    "activation validator/public integration gates only",
}
GODOT_BRIDGE_METHODS = {
    2: {"signal.listConnections", "signal.connect", "signal.disconnect", "signal.emit"},
    3: {"vision.setCameraTransform", "vision.toggleDebugDraw"},
    4: {"tilemap.setCells", "tilemap.getUsedRect", "gridmap.setCells"},
    5: {"physics.raycast"},
    6: {"nav.queryPath"},
    7: {"anim.listTracks", "anim.playTrack"},
    9: {"profiler.sample"},
}


def is_path(token: str) -> bool:
    if token.startswith("// "):
        return False
    return "/" in token or token in SPECIAL_PATHS or token.endswith(PATH_SUFFIXES)


def task_sections(text: str):
    matches = list(re.finditer(r"^### Task (\d+):", text, re.MULTILINE))
    sections = {}
    for index, match in enumerate(matches):
        number = int(match.group(1))
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections[number] = text[match.start():end]
    return sections


def declared_paths(section: str):
    start = section.index("**Files:**")
    end = section.index("**Interfaces", start)
    files = section[start:end]
    allowed = set()
    denied = set()
    for line in files.splitlines():
        match = re.match(r"^- (Create|Modify(?: only)?|Test|Do not modify):?\s*(.*)$", line)
        if not match:
            continue
        target = denied if match.group(1) == "Do not modify" else allowed
        target.update(token for token in re.findall(r"`([^`]+)`", match.group(2))
                      if is_path(token))
    return allowed, denied


def staged_paths(section: str, declared: set[str]):
    commands = re.findall(r"^git add (.+)$", section, re.MULTILINE)
    commits = re.findall(r'^git commit -m ".+"$', section, re.MULTILINE)
    if len(commands) != 1:
        raise ValueError("staging_command_count")
    if len(commits) != 1:
        raise ValueError("commit_command_count")
    staged = set(commands[0].split())
    expanded = set()
    for path in staged:
        descendants = {item for item in declared if item.startswith(path.rstrip("/") + "/")}
        expanded.update(descendants or {path})
    return expanded


def authoritative_rows(text: str):
    start = text.index("### Authoritative Multi-Owner Handoff Table")
    end = text.index("## Governance Decision", start)
    rows = {}
    for line in text[start:end].splitlines():
        if not line.startswith("| `"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        path = cells[0].strip("`")
        owners = []
        for cell in cells[1:]:
            matches = list(re.finditer(r"Task (\d+):", cell))
            for index, match in enumerate(matches):
                task = int(match.group(1))
                end = matches[index + 1].start() if index + 1 < len(matches) else len(cell)
                boundary = cell[match.end():end].strip()
                boundary = re.sub(r"(?:;\s*)?then\s*$", "", boundary).strip(" ;")
                if not boundary:
                    raise ValueError("empty_handoff_boundary")
                tokens = re.findall(r"`([^`]+)`", boundary)
                prose = re.sub(r"`[^`]+`", "", boundary)
                if (re.search(r"[*?\[\]{}]", prose) or
                        re.search(r"\b(?:glob|family)\b", boundary, re.IGNORECASE)):
                    raise ValueError("non_exact_handoff_boundary")
                task1_test_case = (
                    path.startswith("tests/test_phase7") and task == 1 and
                    len(tokens) == 1 and tokens[0].startswith("TEST_CASE("))
                for token in tokens:
                    if (re.search(r"[*?{}]", token) or
                            (not task1_test_case and re.search(r"[\[\]]", token))):
                        raise ValueError("non_exact_handoff_boundary")
                if path == "src/gdextension/godot_bridge.cpp":
                    required = GODOT_BRIDGE_METHODS.get(task, set())
                    if not required.issubset(tokens):
                        raise ValueError("non_exact_handoff_boundary")
                    if task == 9 and "Performance bind/sample block" not in boundary:
                        raise ValueError("non_exact_handoff_boundary")
                elif path.startswith("tests/test_phase7") and task != 1:
                    if (len(tokens) != 2 or "BEGIN" not in tokens[0] or
                            "END" not in tokens[1] or " through " not in boundary):
                        raise ValueError("non_exact_handoff_boundary")
                elif not tokens and boundary not in EXACT_PROSE_BOUNDARIES:
                    raise ValueError("non_exact_handoff_boundary")
                owners.append(task)
        if not owners:
            raise ValueError("empty_handoff_boundary")
        if path in rows:
            raise ValueError("duplicate_handoff_row")
        rows[path] = owners
    return rows


def audit_plan(text: str):
    sections = task_sections(text)
    if set(range(1, 12)) - set(sections):
        raise ValueError("missing_task")
    owners_by_path = {}
    for task in range(1, 12):
        declared, denied = declared_paths(sections[task])
        staged = staged_paths(sections[task], declared)
        if staged != declared:
            raise ValueError(f"task_{task}_modified_staged_mismatch")
        if staged & denied:
            raise ValueError(f"task_{task}_denied_path_staged")
        for path in declared:
            owners_by_path.setdefault(path, []).append(task)

    shared = {path: owners for path, owners in owners_by_path.items() if len(owners) > 1}
    rows = authoritative_rows(text)
    if set(rows) != set(shared):
        raise ValueError("authoritative_handoff_set_mismatch")
    for path, owners in shared.items():
        if rows[path] != owners or owners != sorted(set(owners)):
            raise ValueError("authoritative_owner_order_mismatch")
    return True


class Phase7PlanOwnershipTests(unittest.TestCase):
    def test_plan_file_boundaries_staging_and_handoffs_are_equal(self):
        self.assertTrue(audit_plan(PLAN.read_text(encoding="utf-8")))

    def test_omitted_task1_domain_handoff_is_rejected(self):
        text = PLAN.read_text(encoding="utf-8")
        text = re.sub(r"^\| `tests/test_phase7a_signals\.cpp`.*\n", "", text,
                      count=1, flags=re.MULTILINE)
        with self.assertRaisesRegex(ValueError, "authoritative_handoff_set_mismatch"):
            audit_plan(text)

    def test_declared_task1_domain_handoff_is_accepted(self):
        self.assertTrue(audit_plan(PLAN.read_text(encoding="utf-8")))

    def test_wildcard_handoff_is_rejected(self):
        text = PLAN.read_text(encoding="utf-8")
        text = text.replace("Task 1: file preamble, fixtures, and `TEST_CASE",
                            "Task 1: * file preamble, fixtures, and `TEST_CASE", 1)
        with self.assertRaisesRegex(ValueError, "non_exact_handoff_boundary"):
            audit_plan(text)

    def test_empty_task_handoff_boundary_is_rejected(self):
        text = PLAN.read_text(encoding="utf-8")
        text = re.sub(
            r"(\| `tests/test_phase7a_signals\.cpp`[^\n]*?\|) Task 1: [^|]*",
            r"\1 Task 1: ", text, count=1)
        with self.assertRaisesRegex(ValueError, "empty_handoff_boundary"):
            audit_plan(text)

    def test_prose_only_handoff_boundary_is_rejected(self):
        text = PLAN.read_text(encoding="utf-8")
        text = re.sub(
            r"(\| `tests/test_phase7a_signals\.cpp`[^\n]*?\|) Task 1: [^|]*",
            r"\1 Task 1: some changes ", text, count=1)
        with self.assertRaisesRegex(ValueError, "non_exact_handoff_boundary"):
            audit_plan(text)

    def test_domain_handoff_requires_both_begin_and_end_markers(self):
        text = PLAN.read_text(encoding="utf-8")
        text = text.replace(
            "Task 2: append-only section `// TASK 2 SIGNAL BEHAVIOR BEGIN` through "
            "`// TASK 2 SIGNAL BEHAVIOR END`",
            "Task 2: append-only section `// TASK 2 SIGNAL BEHAVIOR BEGIN`", 1)
        with self.assertRaisesRegex(ValueError, "non_exact_handoff_boundary"):
            audit_plan(text)

    def test_godot_bridge_handoff_rejects_incomplete_method_list(self):
        text = PLAN.read_text(encoding="utf-8")
        table_start = text.index("### Authoritative Multi-Owner Handoff Table")
        table_end = text.index("## Governance Decision", table_start)
        table = text[table_start:table_end].replace(
            "`signal.emit`", "`signal.emit.removed`", 1)
        text = text[:table_start] + table + text[table_end:]
        with self.assertRaisesRegex(ValueError, "non_exact_handoff_boundary"):
            audit_plan(text)

    def test_phase7_domain_handlers_have_one_binding_aware_forwarding_path(self):
        handlers = {
            "src/tools/signal_tools.cpp": [
                "handleSignalListConnections", "handleSignalConnect",
                "handleSignalDisconnect", "handleSignalEmit"],
            "src/tools/visual_tools.cpp": [
                "handleViewportSetCameraTransform", "handleViewportToggleDebugDraw"],
            "src/tools/tilemap_grid_tools.cpp": [
                "handleTilemapSetCells", "handleTilemapGetUsedRect", "handleGridmapSetCells"],
            "src/tools/physics_nav_tools.cpp": [
                "handlePhysicsRaycastQuery", "handlePhysicsSimulateStep", "handleNavBakeMesh",
                "handleNavQueryPath", "handleAnimListTracks", "handleAnimPlayTrack"],
            "src/tools/runtime_tools.cpp": [
                "handleInjectInputEvent", "handleRuntimeGetCallStack",
                "handleRuntimeReadProfiler"],
        }
        for path, names in handlers.items():
            source = (ROOT / path).read_text(encoding="utf-8")
            self.assertNotRegex(
                source,
                r"sendRequest\(\s*\"(?:signal\.(?:listConnections|connect|disconnect|emit)|"
                r"vision\.(?:setCameraTransform|toggleDebugDraw)|"
                r"tilemap\.(?:setCells|getUsedRect)|gridmap\.setCells|physics\.raycast|"
                r"nav\.queryPath|anim\.(?:listTracks|playTrack)|"
                r"runtime\.(?:injectInput|getCallStack|readProfiler))\"")
            for name in names:
                self.assertRegex(
                    source,
                    rf"{name}\s*\(\s*const\s+ResolvedToolBinding&\s+binding[\s\S]*?sendPhase7LiveRequest\(\s*binding",
                    path + ":" + name)

    def test_process_queue_validates_session_before_dequeue_or_start(self):
        source = (ROOT / "src/gdextension/editor_hook.cpp").read_text(encoding="utf-8")
        process_queue = source[source.index("void EditorHook::processQueue()"):
                               source.index("void EditorHook::cancelPendingCommands")]
        validation_match = re.search(
            r"validateSessionKindForMethod\(\s*m_commandQueue\.front\(\)\.method",
            process_queue)
        self.assertIsNotNone(validation_match)
        validation = validation_match.start()
        self.assertLess(validation, process_queue.index("m_commandQueue.pop()"))
        self.assertLess(validation, process_queue.index("tryStart()"))


if __name__ == "__main__":
    unittest.main()
