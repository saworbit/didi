from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
PLAN = ROOT / "docs" / "PHASE_7_PARTIAL_IMPLEMENTATION_PLAN.md"
PATH_SUFFIXES = (".cpp", ".hpp", ".py", ".ps1", ".gd", ".tscn", ".json",
                 ".yml", ".md", ".txt", ".godot")
SPECIAL_PATHS = {"CMakeLists.txt", "README.md", "CHANGELOG.md", "SECURITY.md"}


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
            owners.extend(int(value) for value in re.findall(r"Task (\d+):", cell))
            if "*" in cell or "glob" in cell.lower() or "family" in cell.lower():
                raise ValueError("non_exact_handoff_boundary")
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


if __name__ == "__main__":
    unittest.main()
