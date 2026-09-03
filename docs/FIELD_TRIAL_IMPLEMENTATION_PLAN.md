# Field Trial Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the harness that seeds, briefs, and scores a Didi field trial, so a cold agent can be pointed at a bare Godot project and its every friction point recorded.

**Architecture:** Three artifacts under `tools/field-trial/`. A seed script writes the working directory, the minimal `project.godot`, the MCP client configuration, and a baseline record. A briefing document is the prompt handed to the tester. A coverage script reads the tester's session transcript afterwards and reports which implemented tools were reached and which never occurred to it. One Python test module covers the two scripts.

**Tech Stack:** Python 3 standard library only, matching the existing `tools/*.py`. No new dependencies. Markdown for the briefing.

## Global Constraints

- Plans and specs live in `docs/`, named `FOO_DESIGN.md` and `FOO_IMPLEMENTATION_PLAN.md`. `docs/superpowers/` must not exist: `tools/validate_documentation.py` fails the build if it does (`FILESYSTEM_FORBIDDEN_ARTIFACT_PATHS`).
- `python tools/validate_documentation.py` must pass after every task that touches Markdown. Every relative Markdown link must resolve to a real file.
- Every `tests/test_*.py` must be named by a step in `.github/workflows/*.yml` or the validator fails. A test that runs nowhere is a validator error, not a warning.
- Pushing a branch that changes `.github/workflows/` needs `env -u GH_TOKEN -u GITHUB_TOKEN git push`; the configured token lacks the `workflow` scope.
- Never `git add build-ninja/`. It is an untracked local build tree.
- No commit message, branch name, pull request, issue, or comment may name a model, an assistant, or an agent, or say the work was generated. Write in first person.
- Godot 4.7.2 is the trial engine: `C:\Godot\Godot_v4.7.2-stable_win64.exe` and `..._console.exe`.
- The built server is `D:\didi\build-ninja\didi.exe`; its manifest is `D:\didi\build-ninja\tool-manifest.json`.
- Branch from `main` after `git fetch origin`. Other agents merge to `main` independently.

---

### Task 1: Coverage extractor

Didi logs `Method: tools/call` at DEBUG (`src/mcp/mcp_server.cpp:287`) and names the tool only in an ERROR line when a call throws (`src/mcp/tool_registry.cpp:746`). The server log therefore cannot answer which tools were used. The client transcript can: every invocation is a `tool_use` block named `mcp__didi__<tool>` inside `record["message"]["content"]`.

**Files:**
- Create: `tools/field-trial/coverage.py`
- Create: `tests/test_field_trial.py`
- Modify: `.github/workflows/lint.yml:48-51`

**Interfaces:**
- Produces: `extract_invocations(transcript_lines: Iterable[str], server: str = "didi") -> dict[str, int]`, `build_report(counts: dict[str, int], implemented_names: Iterable[str]) -> dict`, and a `main(argv: list[str] | None = None) -> int` CLI entry point.
- Consumes: `build-ninja/tool-manifest.json`, whose shape is `{"counts": {...}, "names": {"canonical": [...], "implemented": [...], "legacy": [...], "unimplemented": [...]}, "schema": 1}`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_field_trial.py`:

```python
import importlib.util
import json
from pathlib import Path
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
COVERAGE_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "coverage.py"
SPEC = importlib.util.spec_from_file_location("field_trial_coverage", COVERAGE_PATH)
if SPEC is None or SPEC.loader is None:
    raise ImportError(f"Cannot load coverage reporter from {COVERAGE_PATH}")
COVERAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COVERAGE)


def transcript_line(*tool_names):
    """One assistant turn holding a tool_use block per name."""
    return json.dumps(
        {
            "message": {
                "content": [
                    {"type": "tool_use", "id": f"toolu_{index}", "name": name, "input": {}}
                    for index, name in enumerate(tool_names)
                ]
            }
        }
    )


class ExtractInvocationsTests(unittest.TestCase):
    def test_counts_repeated_didi_calls_by_bare_tool_name(self):
        lines = [
            transcript_line("mcp__didi__scene_create", "Bash"),
            transcript_line("mcp__didi__scene_create"),
        ]
        self.assertEqual(COVERAGE.extract_invocations(lines), {"scene_create": 2})

    def test_ignores_other_servers_and_native_tools(self):
        lines = [transcript_line("mcp__github__list_issues", "Read", "Edit")]
        self.assertEqual(COVERAGE.extract_invocations(lines), {})

    def test_skips_blank_and_malformed_lines_without_raising(self):
        lines = ["", "   ", "{not json", transcript_line("mcp__didi__editor_undo")]
        self.assertEqual(COVERAGE.extract_invocations(lines), {"editor_undo": 1})

    def test_ignores_records_with_no_content_list(self):
        lines = [json.dumps({"message": {"content": "summarised"}}), json.dumps({"type": "summary"})]
        self.assertEqual(COVERAGE.extract_invocations(lines), {})

    def test_honours_a_non_default_server_alias(self):
        lines = [transcript_line("mcp__godot__scene_open")]
        self.assertEqual(
            COVERAGE.extract_invocations(lines, server="godot"), {"scene_open": 1}
        )


class BuildReportTests(unittest.TestCase):
    def test_splits_called_uncalled_and_unknown_names(self):
        report = COVERAGE.build_report(
            {"scene_create": 3, "not_a_tool": 1},
            ["scene_create", "scene_open", "editor_undo", "editor_redo"],
        )
        self.assertEqual(report["called"], {"scene_create": 3})
        self.assertEqual(report["uncalled"], ["editor_redo", "editor_undo", "scene_open"])
        self.assertEqual(report["unknown"], ["not_a_tool"])

    def test_totals_count_distinct_tools_and_invocations_separately(self):
        report = COVERAGE.build_report(
            {"scene_create": 3, "scene_open": 1}, ["scene_create", "scene_open"]
        )
        self.assertEqual(report["totals"]["implemented"], 2)
        self.assertEqual(report["totals"]["distinct_called"], 2)
        self.assertEqual(report["totals"]["invocations"], 4)
        self.assertEqual(report["totals"]["coverage_percent"], 100.0)

    def test_unknown_names_do_not_inflate_coverage(self):
        report = COVERAGE.build_report(
            {"not_a_tool": 9}, ["scene_create", "scene_open"]
        )
        self.assertEqual(report["totals"]["distinct_called"], 0)
        self.assertEqual(report["totals"]["invocations"], 0)
        self.assertEqual(report["totals"]["coverage_percent"], 0.0)

    def test_empty_manifest_reports_zero_rather_than_dividing_by_zero(self):
        report = COVERAGE.build_report({}, [])
        self.assertEqual(report["totals"]["coverage_percent"], 0.0)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python -m unittest tests.test_field_trial -v`
Expected: FAIL at import with `ImportError: Cannot load coverage reporter from ...tools\field-trial\coverage.py`, because the module does not exist yet.

- [ ] **Step 3: Write the implementation**

Create `tools/field-trial/coverage.py`:

```python
"""Report which Didi tools a field trial actually invoked.

The server logs `Method: tools/call` at DEBUG and names the tool only when a
call throws, so the server log cannot say which tools a tester reached for. The
client transcript can: every invocation is a `tool_use` block named
`mcp__<server>__<tool>`. This reads that transcript and compares it against the
manifest the trial was seeded with, so the interesting number is not how much
worked but which implemented tools never occurred to the tester at all.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable


def _tool_use_blocks(record: object) -> list[dict]:
    if not isinstance(record, dict):
        return []
    message = record.get("message")
    if not isinstance(message, dict):
        return []
    content = message.get("content")
    if not isinstance(content, list):
        return []
    return [
        block
        for block in content
        if isinstance(block, dict) and block.get("type") == "tool_use"
    ]


def extract_invocations(
    transcript_lines: Iterable[str], server: str = "didi"
) -> dict[str, int]:
    """Count invocations by bare tool name, keyed off the MCP server alias.

    A transcript is appended to while a session runs and can hold partial or
    non-message records, so an unreadable line is skipped rather than fatal.
    """
    prefix = f"mcp__{server}__"
    counts: dict[str, int] = {}
    for line in transcript_lines:
        stripped = line.strip()
        if not stripped:
            continue
        try:
            record = json.loads(stripped)
        except json.JSONDecodeError:
            continue
        for block in _tool_use_blocks(record):
            name = block.get("name")
            if isinstance(name, str) and name.startswith(prefix):
                tool = name[len(prefix) :]
                counts[tool] = counts.get(tool, 0) + 1
    return counts


def build_report(counts: dict[str, int], implemented_names: Iterable[str]) -> dict:
    """Split observed calls against the manifest's implemented set.

    `unknown` is deliberately kept out of every total. A name the manifest does
    not know is either a legacy alias or a typo, and letting either raise the
    coverage number would make the one figure people quote the least reliable.
    """
    implemented = set(implemented_names)
    called = {name: count for name, count in sorted(counts.items()) if name in implemented}
    unknown = sorted(name for name in counts if name not in implemented)
    uncalled = sorted(implemented - set(called))
    coverage = (100.0 * len(called) / len(implemented)) if implemented else 0.0
    return {
        "called": called,
        "uncalled": uncalled,
        "unknown": unknown,
        "totals": {
            "implemented": len(implemented),
            "distinct_called": len(called),
            "invocations": sum(called.values()),
            "coverage_percent": round(coverage, 1),
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--transcript", required=True, type=Path, help="Session transcript .jsonl"
    )
    parser.add_argument(
        "--manifest", required=True, type=Path, help="tool-manifest.json from the build"
    )
    parser.add_argument("--server", default="didi", help="MCP server alias in the client config")
    parser.add_argument("--output", type=Path, help="Write the report here instead of stdout")
    args = parser.parse_args(argv)

    with args.transcript.open(encoding="utf-8", errors="replace") as handle:
        counts = extract_invocations(handle, server=args.server)

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    report = build_report(counts, manifest["names"]["implemented"])
    rendered = json.dumps(report, indent=2, sort_keys=True)

    if args.output is None:
        print(rendered)
    else:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python -m unittest tests.test_field_trial -v`
Expected: PASS, 9 tests.

- [ ] **Step 5: Register the test in CI**

The validator fails if a `tests/test_*.py` is named by no workflow. Edit `.github/workflows/lint.yml`, replacing the `Validate Documentation Contract` step's run block with:

```yaml
      - name: Validate Documentation Contract
        run: |
          python -m unittest tests.test_documentation_validator -v
          python -m unittest tests.test_field_trial -v
          python tools/validate_documentation.py
```

- [ ] **Step 6: Verify the validator passes**

Run: `python tools/validate_documentation.py`
Expected: `Documentation contract valid (...)` and exit 0. A failure naming `tests.test_field_trial` means step 5 was not saved.

- [ ] **Step 7: Commit**

```bash
git add tools/field-trial/coverage.py tests/test_field_trial.py .github/workflows/lint.yml
git commit -m "Count the tools a trial actually reached for"
```

Push needs `env -u GH_TOKEN -u GITHUB_TOKEN git push -u origin <branch>` because this touches `.github/workflows/`.

---

### Task 2: Seed script

**Files:**
- Create: `tools/field-trial/seed_trial.py`
- Modify: `tests/test_field_trial.py` (append the class below; the CI step from Task 1 already names the module)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `seed(target: Path, didi_exe: Path, godot_exe: Path, repository: Path, manifest: Path | None = None) -> dict` returning the baseline record it wrote, plus `main(argv: list[str] | None = None) -> int`.

The seed is bare on purpose. Didi exits `2` when `--project` names a directory without a `project.godot`, so the project file is forced; nothing else is. No addon is copied, the plugin is not enabled, and the editor is not started. Finding those is the tester's first test.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_field_trial.py`, above the `if __name__` block:

```python
import tempfile

SEED_PATH = REPOSITORY_ROOT / "tools" / "field-trial" / "seed_trial.py"
SEED_SPEC = importlib.util.spec_from_file_location("field_trial_seed", SEED_PATH)
if SEED_SPEC is None or SEED_SPEC.loader is None:
    raise ImportError(f"Cannot load seed script from {SEED_PATH}")
SEED = importlib.util.module_from_spec(SEED_SPEC)
SEED_SPEC.loader.exec_module(SEED)


class SeedTests(unittest.TestCase):
    def setUp(self):
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.addCleanup(self._temp.cleanup)
        self.didi = self.root / "didi.exe"
        self.didi.write_text("binary", encoding="utf-8")
        self.godot = self.root / "godot.exe"
        self.godot.write_text("binary", encoding="utf-8")

    def seed(self, target):
        return SEED.seed(
            target=target,
            didi_exe=self.didi,
            godot_exe=self.godot,
            repository=REPOSITORY_ROOT,
        )

    def test_writes_a_project_godot_the_server_will_accept(self):
        target = self.root / "trial"
        self.seed(target)
        project = (target / "project.godot").read_text(encoding="utf-8")
        self.assertIn("config_version=5", project)
        self.assertIn("[application]", project)

    def test_seed_carries_no_addon_and_no_enabled_plugin(self):
        target = self.root / "trial"
        self.seed(target)
        self.assertFalse((target / "addons").exists())
        self.assertNotIn(
            "editor_plugins", (target / "project.godot").read_text(encoding="utf-8")
        )

    def test_mcp_config_launches_the_built_server_at_debug_on_this_project(self):
        target = self.root / "trial"
        self.seed(target)
        config = json.loads((target / ".mcp.json").read_text(encoding="utf-8"))
        server = config["mcpServers"]["didi"]
        self.assertEqual(server["command"], str(self.didi))
        self.assertEqual(
            server["args"], ["--project", str(target), "--log-level", "DEBUG"]
        )

    def test_baseline_records_what_the_tester_was_handed(self):
        target = self.root / "trial"
        baseline = self.seed(target)
        written = json.loads((target / "baseline.json").read_text(encoding="utf-8"))
        self.assertEqual(written, baseline)
        for field in ("commit", "godot_executable", "didi_executable", "seeded_utc"):
            self.assertIn(field, written)
        self.assertRegex(written["commit"], r"^[0-9a-f]{7,40}$")

    def test_refuses_to_overwrite_an_existing_trial(self):
        target = self.root / "trial"
        self.seed(target)
        with self.assertRaises(FileExistsError):
            self.seed(target)

    def test_refuses_a_server_path_that_does_not_exist(self):
        with self.assertRaises(FileNotFoundError):
            SEED.seed(
                target=self.root / "trial",
                didi_exe=self.root / "absent.exe",
                godot_exe=self.godot,
                repository=REPOSITORY_ROOT,
            )
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python -m unittest tests.test_field_trial -v`
Expected: FAIL at import with `ImportError: Cannot load seed script from ...tools\field-trial\seed_trial.py`.

- [ ] **Step 3: Write the implementation**

Create `tools/field-trial/seed_trial.py`:

```python
"""Seed a Didi field trial working directory.

Deliberately minimal. Didi exits 2 when --project names a directory without a
project.godot, so the project file is forced by the architecture. Everything
past it -- copying the addon, enabling the plugin, starting the editor -- is
the tester's job, because that is exactly where real users get stuck.
"""

from __future__ import annotations

import argparse
import datetime
import json
from pathlib import Path
import shutil
import subprocess

PROJECT_GODOT = """; Seeded by tools/field-trial/seed_trial.py. Bare on purpose.

config_version=5

[application]

config/name="Didi Field Trial"
config/features=PackedStringArray("4.7", "Forward Plus")
"""


def _commit(repository: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def seed(
    target: Path,
    didi_exe: Path,
    godot_exe: Path,
    repository: Path,
    manifest: Path | None = None,
) -> dict:
    """Create the trial directory and return the baseline record written into it."""
    for label, path in (("Didi executable", didi_exe), ("Godot executable", godot_exe)):
        if not path.is_file():
            raise FileNotFoundError(f"{label} not found: {path}")
    if target.exists():
        raise FileExistsError(
            f"{target} already exists. A trial is scored against its seed, so reusing "
            "a directory silently mixes two runs. Delete it or pick another path."
        )

    target.mkdir(parents=True)
    (target / "project.godot").write_text(PROJECT_GODOT, encoding="utf-8")
    (target / ".mcp.json").write_text(
        json.dumps(
            {
                "mcpServers": {
                    "didi": {
                        "command": str(didi_exe),
                        "args": ["--project", str(target), "--log-level", "DEBUG"],
                    }
                }
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    if manifest is not None and manifest.is_file():
        shutil.copyfile(manifest, target / "tool-manifest.baseline.json")

    baseline = {
        "commit": _commit(repository),
        "didi_executable": str(didi_exe),
        "godot_executable": str(godot_exe),
        "repository": str(repository),
        "seeded_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "manifest_copied": manifest is not None and manifest.is_file(),
    }
    (target / "baseline.json").write_text(
        json.dumps(baseline, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return baseline


def main(argv: list[str] | None = None) -> int:
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, type=Path)
    parser.add_argument(
        "--didi-exe", type=Path, default=repository / "build-ninja" / "didi.exe"
    )
    parser.add_argument(
        "--godot-exe",
        type=Path,
        default=Path(r"C:\Godot\Godot_v4.7.2-stable_win64_console.exe"),
    )
    parser.add_argument(
        "--manifest", type=Path, default=repository / "build-ninja" / "tool-manifest.json"
    )
    args = parser.parse_args(argv)

    baseline = seed(
        target=args.target,
        didi_exe=args.didi_exe,
        godot_exe=args.godot_exe,
        repository=repository,
        manifest=args.manifest,
    )
    print(json.dumps(baseline, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `python -m unittest tests.test_field_trial -v`
Expected: PASS, 15 tests.

- [ ] **Step 5: Commit**

```bash
git add tools/field-trial/seed_trial.py tests/test_field_trial.py
git commit -m "Seed a trial with the bare minimum the server will start on"
```

---

### Task 3: The briefing

This is the artifact the trial actually turns on. It has no unit test; its gate is the checklist in Step 2, run against [Field Trial Design](FIELD_TRIAL_DESIGN.md).

**Files:**
- Create: `tools/field-trial/TRIAL_BRIEF.md`

**Interfaces:**
- Consumes: the directory layout Task 2 seeds (`project.godot`, `.mcp.json`, `baseline.json`).
- Produces: `TRIAL_LOG.md` in the working directory, which Task 4 collects.

- [ ] **Step 1: Write the briefing**

Create `tools/field-trial/TRIAL_BRIEF.md`:

```markdown
# Field Trial Briefing

Read all of this before you start.

## Your situation

- Your working directory is the Godot project you are building in. Everything you make goes there.
- An MCP server named `didi` is connected to this session. It is the tool you are here to use.
- Didi's source repository is at `D:\didi`. Read anything in it you like. Do not write to it, and do not run any git command inside it.
- Godot 4.7.2 is at `C:\Godot\Godot_v4.7.2-stable_win64.exe`, and `C:\Godot\Godot_v4.7.2-stable_win64_console.exe` when you need to capture output.
- You have never used Didi before. Work out what it can do.

## This run is unattended

Nobody will answer questions, so do not ask any. When you are blocked, write it down, pick the most reasonable interpretation, and keep going. Do not stop early because something is unclear.

## What to build

A single-screen 2D arena survival game.

1. The player is a `CharacterBody2D` moving in four directions through project input actions. Do not hardcode keycodes.
2. The arena is a TileMapLayer with solid walls the player cannot cross.
3. Three enemies come from a packed scene and chase the player. Each has an AnimationPlayer with at least one animation that plays.
4. Score lives on an autoload singleton and reaches a HUD Control label by signal. Do not poll for it.
5. Clearing all three enemies wins. A third hit on the player loses. Show a screen for each.
6. It runs. Launch the game, drive the player with injected input, capture a frame of the editor viewport and one of the running game, and show the logs are clean.

Get as far as you can. Partial is fine. Stopping without recording why is not.

## Use Didi first

Attempt every action through Didi first, including the ones where editing a file directly would obviously be faster. That is the point of the run.

When a tool fails or cannot do what you need, you may fall back to writing files directly or driving Godot yourself. Every fallback goes in the ledger.

## The ledger

Keep `TRIAL_LOG.md` in your working directory. Append an entry every time something does not go smoothly:

    ## [timestamp] Short title
    Intent:    what I was trying to achieve
    Attempt:   tool name and exact arguments
    Result:    exact response or error
    Verdict:   worked | worked-with-friction | failed
    Fallback:  what I did instead, or none
    Issue:     issue number, or none and why

Also record architectural decisions and node paths on Didi's blackboard, as its own documentation recommends.

## Filing issues

Issues go to the `saworbit/didi` repository with `gh`. Filing is deliberately expensive. Before you open one:

1. Re-read the relevant part of `D:\didi\docs`. Behaviour that is documented and wrong is still worth reporting, under a different label.
2. Search open and closed issues for a duplicate.
3. Reduce it to a minimal reproduction: exact tool name, exact arguments, exact response.
4. Choose a label. `bug` when behaviour contradicts the documentation. `documentation` when the documentation is wrong or missing. `enhancement` when the capability is simply absent.

Add the `field-trial` label to every issue on top of that classification.

One issue per root cause, never one per occurrence. Stop after twenty. Later findings go in the ledger with a note that the cap was reached.

Fill in the fields the bug report template asks for: Didi version, Godot version, operating system, reproduction, expected, actual.

Write in first person and in plain sentences. No em dashes, no emoji. Do not describe the report as generated, and do not name a model, an assistant, an agent, or a tool as its author.

Issues only. Do not commit, branch, push, or open a pull request anywhere.

## When you finish

Add a summary to the end of `TRIAL_LOG.md`: features completed, features abandoned and why, issues filed, and the one change to Didi that would have helped you most.
```

- [ ] **Step 2: Check the briefing against the design**

Read [Field Trial Design](FIELD_TRIAL_DESIGN.md) and confirm each of these appears in the briefing. Fix any that do not:

- Repository readable, not writable, no git inside it (design section 2).
- Unattended, no questions (design section 1).
- All six build-target features, worded as features and never as tool names (design section 4).
- Didi-first mandate with fallback permitted and logged (design section 5).
- The exact seven-field ledger format, and the blackboard instruction (design section 6).
- All four pre-filing steps, the three labels, the `field-trial` label, one-issue-per-root-cause, the cap of twenty, the template fields, the voice rule, and issues-only (design section 7).

- [ ] **Step 3: Verify the validator still passes**

Run: `python tools/validate_documentation.py`
Expected: exit 0. The new file adds one Markdown document; the link to `FIELD_TRIAL_DESIGN.md` in this plan must resolve.

- [ ] **Step 4: Commit**

```bash
git add tools/field-trial/TRIAL_BRIEF.md
git commit -m "Write the briefing the trial hands its tester"
```

---

### Task 4: Label, dry run, and the real seed

The end-to-end gate. Everything here is a command, not a code change, and it runs before any tester session starts.

**Files:**
- No repository files change.

**Interfaces:**
- Consumes: `seed()` from Task 2 and the briefing from Task 3.
- Produces: the `field-trial` label on `saworbit/didi`, and the seeded `D:\didi-trials\trial-01`.

- [ ] **Step 1: Create the label**

```bash
gh label create field-trial --repo saworbit/didi --color 5319e7 --description "Raised during a field trial run"
```

Expected: the label is created. If it already exists `gh` says so and that is fine; do not delete and recreate it, because that would strip it from issues already carrying it.

- [ ] **Step 2: Confirm the build the trial will use**

```bash
ls build-ninja/didi.exe build-ninja/tool-manifest.json
```

Expected: both exist. If either is missing, rebuild with VsDevCmd plus Ninja into `build-ninja` before continuing; a trial scored against a stale manifest reports the wrong uncalled set.

- [ ] **Step 3: Dry-run the seed into a throwaway directory**

```bash
python tools/field-trial/seed_trial.py --target D:/didi-trials/dry-run
```

Expected: prints a baseline JSON object with a `commit` matching `git rev-parse HEAD`. Confirm `D:/didi-trials/dry-run` holds `project.godot`, `.mcp.json`, `baseline.json`, and `tool-manifest.baseline.json`, and holds no `addons` directory.

- [ ] **Step 4: Prove the server starts on the seed**

```bash
build-ninja/didi.exe --project D:/didi-trials/dry-run --log-level DEBUG --dump-tool-manifest
```

Expected: a manifest on stdout rather than exit `2`. Exit `2` means the seeded `project.godot` is not acceptable to the server, which blocks the trial and is itself the first finding.

- [ ] **Step 5: Remove the dry run**

```bash
rm -rf D:/didi-trials/dry-run
```

Expected: gone. The seed refuses to overwrite an existing directory, so leaving it behind blocks a later re-run of the same name.

- [ ] **Step 6: Seed the real trial**

```bash
python tools/field-trial/seed_trial.py --target D:/didi-trials/trial-01
```

Expected: baseline printed. Record the `commit` value; the review compares the run against exactly that build.

- [ ] **Step 7: Verify the whole suite and the validator**

Run: `python -m unittest tests.test_field_trial -v` then `python tools/validate_documentation.py`
Expected: 15 tests pass, validator exits 0.

- [ ] **Step 8: Open the pull request**

```bash
env -u GH_TOKEN -u GITHUB_TOKEN git push -u origin <branch>
gh pr create --repo saworbit/didi --fill
```

The push must unset the tokens because Task 1 changed `.github/workflows/`. Write the pull request body in first person with no mention of a model, an assistant, or generation.

---

## After the plan

The tester session is started by hand in `D:\didi-trials\trial-01` with the contents of `tools/field-trial/TRIAL_BRIEF.md` as its opening message. When it stops, score the run:

```bash
python tools/field-trial/coverage.py \
  --transcript "$HOME/.claude/projects/D--didi-trials-trial-01/<session>.jsonl" \
  --manifest D:/didi-trials/trial-01/tool-manifest.baseline.json \
  --output D:/didi-trials/trial-01/coverage.json
```

Review reads four things: `TRIAL_LOG.md`, `coverage.json`, the issues carrying the `field-trial` label, and the working tree the tester left behind.
