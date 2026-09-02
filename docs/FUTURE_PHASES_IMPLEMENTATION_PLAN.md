# Future Phases Documentation Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Phases 7-12 and the rules for every later phase part of the repository's authoritative, validated documentation surface.

**Architecture:** `docs/ROADMAP.md` remains the delivery-status index, while `docs/FUTURE_PHASES_DESIGN.md` remains the detailed scope and gate authority. The README and capability documentation provide concise navigation, and the existing Python documentation validator prevents phase coverage, status vocabulary, and canonical-tool counts from drifting.

**Tech Stack:** Markdown, Python 3 standard library, `unittest`, existing `tools/validate_documentation.py` validation framework.

## Execution note

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `80/83`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

This is the approved historical plan that integrated the future-phases documentation. Its original task wording is preserved below. Current Phase 7 status is `PARTIAL_DELIVERY`: the 2026-08-29 Godot 4.5.1/4.7.2 gate found 15/18 implementation-feasible and 3/18 API-blocked under the approved contracts. All 15 feasible names, including the three TileMapLayer/GridMap tools, are delivered. The 3 API-blocked names remain registered but unimplemented. The surface is 80/83.

Current evidence is [PHASE_7_API_FEASIBILITY.md](PHASE_7_API_FEASIBILITY.md), and the approved executable plan is [PHASE_7_IMPLEMENTATION_PLAN.md](PHASE_7_IMPLEMENTATION_PLAN.md). Governance authorized partial delivery; the signal names shipped under it. Further Phase 7 work requires the same treatment: a production trial per capability, not a feasibility note. Under the third path, all three blockers must re-enter Task 1 and prove `GO` on both pinned engines before Task 2; contract weakening is a separate explicit contract amendment.

## Global Constraints

- Phase 7 closes the canonical MCP surface from 60 implemented tools to all 78; it adds no new public tool names.
- Phases 8-12 remain future work and must not be described as implemented.
- Roadmap phase status uses only `PLANNED`, `IN PROGRESS`, or `COMPLETE`.
- Every future phase must define scope, explicit exclusions, security and mutation classifications, exit evidence, completion date, and pull request before it can become `COMPLETE`.
- A successful stub does not count as implementation.
- `docs/ROADMAP.md` is the authoritative delivery-status index; `docs/FUTURE_PHASES_DESIGN.md` is the detailed approved design.
- Keep the README's canonical protocol table at exactly 78 tool names and do not duplicate that table in roadmap documents.
- Preserve the repository's existing documentation validation command and standard-library-only implementation.
- Do not create files beneath `docs/superpowers/`; repository validation forbids that path.

---

### Task 1: Add Failing Roadmap-Continuity Tests

**Files:**
- Modify: `tests/test_documentation_validator.py`
- Test: `tests/test_documentation_validator.py`

**Interfaces:**
- Consumes: `tools.validate_documentation.validate_repository(root: Path) -> list[str]`
- Produces: regression tests requiring Phases 7-12, controlled status values, and the future-phase governance contract

- [ ] **Step 1: Add a failing test for complete future-phase coverage**

Add this test method to the existing documentation validator test case:

```python
def test_repository_requires_future_phase_roadmap(self):
    errors = validate_repository(REPOSITORY_ROOT)
    future_phase_errors = [
        error for error in errors
        if "future phase" in error.lower() or "phase status" in error.lower()
    ]
    self.assertEqual([], future_phase_errors)
```

- [ ] **Step 2: Add focused fixture tests for missing phases and invalid statuses**

Create minimal temporary repository fixtures using the test module's existing helper pattern. Assert these exact validator messages:

```python
self.assertIn(
    "docs/ROADMAP.md must declare Phase 12",
    errors,
)
self.assertIn(
    "docs/ROADMAP.md Phase 8 has invalid status 'FUTURE'",
    errors,
)
self.assertIn(
    "docs/FUTURE_PHASES_DESIGN.md must define future-phase governance",
    errors,
)
```

- [ ] **Step 3: Run the focused tests and confirm they fail**

Run:

```powershell
python -m unittest tests.test_documentation_validator -v
```

Expected: FAIL because `validate_repository` does not yet enforce Phase 7-12 declarations, the three-value status vocabulary, or future-phase governance.

- [ ] **Step 4: Commit the failing tests**

```powershell
git add -- tests/test_documentation_validator.py
git commit -m "test: require future phase roadmap continuity"
```

---

### Task 2: Publish Phases 7-12 in the Authoritative Roadmap

**Files:**
- Modify: `docs/ROADMAP.md`

**Interfaces:**
- Consumes: approved scope and gates from `docs/FUTURE_PHASES_DESIGN.md`
- Produces: one authoritative status entry for each of Phases 7, 8, 9, 10, 11, and 12

- [ ] **Step 1: Add the roadmap status legend**

Near the top of `docs/ROADMAP.md`, add:

```markdown
## Phase Status

Roadmap phases use exactly three states: `PLANNED`, `IN PROGRESS`, and `COMPLETE`.
Detailed scope and acceptance gates for all post-Phase-6 work are defined in
[Future Phases Design](FUTURE_PHASES_DESIGN.md).
```

- [ ] **Step 2: Add Phase 7 as the canonical-surface completion phase**

Add a `PLANNED` Phase 7 entry that states:

```markdown
## Phase 7: Canonical Surface Completion (`PLANNED`)

**Objective:** Implement the remaining 18 canonical tools, moving the protocol surface from 61/79 to 83/83 without adding public tool names.

**Delivery slices:**
- 7A: signals, viewport camera/debug, and tile/grid operations (9 tools)
- 7B: physics, navigation, and animation operations (6 tools)
- 7C: input injection, call-stack inspection, and profiling (3 tools)

**Exit gate:** All 79 canonical tools have real implementations and cross-platform, native bridge, Godot, security, mutation-policy, and documentation evidence. Successful placeholders do not satisfy this gate.
```

- [ ] **Step 3: Add concise Phase 8-12 entries**

Add one `PLANNED` section per approved phase with these exact objectives:

```markdown
## Phase 8: Deep Project Intelligence and Asset Pipeline (`PLANNED`)
**Objective:** Add dependency-aware project analysis, import health, asset provenance, and safe bulk asset workflows after canonical completion.

## Phase 9: Advanced Visual, UI, and Authoring Workflows (`PLANNED`)
**Objective:** Add higher-level scene, UI, visual validation, and authoring workflows built from stable canonical primitives.

## Phase 10: Gogo Parallel Godot Orchestration (`PLANNED`)
**Objective:** Coordinate isolated Godot work across parallel workers with deterministic ownership, conflict prevention, and auditable integration.

## Phase 11: MCP Protocol and Workflow Evolution (`PLANNED`)
**Objective:** Evolve protocol ergonomics, workflow composition, capability negotiation, and compatibility without destabilizing canonical behavior.

## Phase 12: Distribution and Ecosystem Maturity (`PLANNED`)
**Objective:** Mature packaging, release channels, extension governance, compatibility guarantees, and operator-facing distribution workflows.
```

For every section, include the explicit exclusions and exit-gate summary from `docs/FUTURE_PHASES_DESIGN.md`; do not mark any deliverable implemented.

- [ ] **Step 4: Add the permanent roadmap-extension rule**

Append:

```markdown
## Adding Future Phases

Phase 13 and later must be documented before implementation begins. Each phase requires scope, explicit exclusions, security and mutation classifications, measurable exit evidence, and a roadmap status. A phase may move to `COMPLETE` only when its completion date and pull request are recorded with its evidence.
```

- [ ] **Step 5: Commit the authoritative roadmap update**

```powershell
git add -- docs/ROADMAP.md
git commit -m "docs: publish phases 7 through 12"
```

---

### Task 3: Align README and Supporting Documentation

**Files:**
- Modify: `README.md`
- Modify: `docs/CAPABILITIES.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: phase names, statuses, and ownership established in `docs/ROADMAP.md`
- Produces: concise public navigation without duplicating the detailed design or canonical tool table

- [ ] **Step 1: Add a roadmap summary to README**

Add a short section after the current implementation-status material:

```markdown
## Delivery Roadmap

Phases 1-6 established the current implementation baseline. Phase 7 is planned to complete the canonical surface from 61/79 to 83/83 tools; Phases 8-12 cover project intelligence, advanced authoring, parallel orchestration, protocol evolution, and ecosystem maturity.

See the [Roadmap](docs/ROADMAP.md) for delivery status and the [Future Phases Design](docs/FUTURE_PHASES_DESIGN.md) for approved scope and exit gates.
```

Do not change the `Protocol Surface (78 Canonical Tools)` table or imply that the remaining 18 tools are implemented.

- [ ] **Step 2: Add roadmap semantics to capabilities documentation**

In `docs/CAPABILITIES.md`, add:

```markdown
## Planned Capability Growth

The capability matrix describes current behavior only. Planned work is tracked separately in [ROADMAP.md](ROADMAP.md), with detailed post-Phase-6 scope in [FUTURE_PHASES_DESIGN.md](FUTURE_PHASES_DESIGN.md). A planned capability must not appear as supported in this document until its implementation and acceptance evidence are complete.
```

- [ ] **Step 3: Record the documentation change in the changelog**

Under the current unreleased documentation section, add:

```markdown
- Added the approved Phase 7-12 roadmap, including canonical-surface completion and governance requirements for all future phases.
```

- [ ] **Step 4: Commit the public-documentation alignment**

```powershell
git add -- README.md docs/CAPABILITIES.md CHANGELOG.md
git commit -m "docs: align public guidance with future roadmap"
```

---

### Task 4: Enforce Future-Phase Documentation Contracts

**Files:**
- Modify: `tools/validate_documentation.py`
- Modify: `tests/test_documentation_validator.py`

**Interfaces:**
- Consumes: repository root `Path`, Markdown text from `docs/ROADMAP.md` and `docs/FUTURE_PHASES_DESIGN.md`
- Produces: deterministic validation errors returned by `validate_repository(root: Path) -> list[str]`

- [ ] **Step 1: Define exact validation constants**

Add these module-level constants beside the existing documentation invariants:

```python
FUTURE_PHASE_RANGE = range(7, 13)
VALID_PHASE_STATUSES = {"PLANNED", "IN PROGRESS", "COMPLETE"}
FUTURE_PHASE_GOVERNANCE_TERMS = (
    "explicit exclusions",
    "security",
    "mutation",
    "exit evidence",
    "completion date",
    "pull request",
)
```

- [ ] **Step 2: Implement roadmap phase extraction**

Add:

```python
PHASE_HEADING_PATTERN = re.compile(
    r"^## Phase (?P<number>\d+):.*?\(`(?P<status>[^`]+)`\)\s*$",
    re.MULTILINE,
)


def validate_future_phase_roadmap(roadmap_text: str) -> list[str]:
    errors: list[str] = []
    phases = {
        int(match.group("number")): match.group("status")
        for match in PHASE_HEADING_PATTERN.finditer(roadmap_text)
    }
    for phase in FUTURE_PHASE_RANGE:
        if phase not in phases:
            errors.append(f"docs/ROADMAP.md must declare Phase {phase}")
            continue
        status = phases[phase]
        if status not in VALID_PHASE_STATUSES:
            errors.append(
                f"docs/ROADMAP.md Phase {phase} has invalid status '{status}'"
            )
    return errors
```

- [ ] **Step 3: Implement governance validation**

Add:

```python
def validate_future_phase_governance(design_text: str) -> list[str]:
    normalized = design_text.lower()
    missing = [
        term for term in FUTURE_PHASE_GOVERNANCE_TERMS
        if term not in normalized
    ]
    if missing:
        return [
            "docs/FUTURE_PHASES_DESIGN.md must define future-phase governance"
        ]
    return []
```

Call both validators from `validate_repository`, preserving the existing aggregation order and missing-file behavior.

- [ ] **Step 4: Complete fixture coverage**

Add positive tests for all three valid statuses and a governance document containing every required term. Add negative tests that remove each future phase independently and parameterize invalid values `FUTURE`, `DONE`, and `ACTIVE`.

- [ ] **Step 5: Run focused tests and confirm they pass**

Run:

```powershell
python -m unittest tests.test_documentation_validator -v
```

Expected: all documentation validator tests pass, including missing-phase, invalid-status, and governance cases.

- [ ] **Step 6: Commit validator enforcement**

```powershell
git add -- tools/validate_documentation.py tests/test_documentation_validator.py
git commit -m "feat: validate future phase documentation"
```

---

### Task 5: Verify the Integrated Documentation Set

**Files:**
- Verify: `README.md`
- Verify: `CHANGELOG.md`
- Verify: `docs/ROADMAP.md`
- Verify: `docs/FUTURE_PHASES_DESIGN.md`
- Verify: `docs/CAPABILITIES.md`
- Verify: `tools/validate_documentation.py`
- Verify: `tests/test_documentation_validator.py`

**Interfaces:**
- Consumes: all deliverables from Tasks 1-4
- Produces: reproducible evidence that the roadmap is complete, internally aligned, and validator-enforced

- [ ] **Step 1: Run the documentation test suite**

```powershell
python -m unittest tests.test_documentation_validator -v
```

Expected: all tests pass.

- [ ] **Step 2: Run repository documentation validation**

```powershell
python tools/validate_documentation.py
```

Expected: exit code 0 and no validation errors.

- [ ] **Step 3: Run whitespace validation**

```powershell
git diff --check
```

Expected: exit code 0; line-ending conversion warnings are acceptable, whitespace errors are not.

- [ ] **Step 4: Confirm the canonical count remains unchanged**

Use the existing semantic audit or validator output to confirm the README declares 79 canonical tools, 60 implemented tools, and 18 planned Phase 7 implementations. Do not derive a new canonical count from roadmap prose.

- [ ] **Step 5: Commit any integration-only corrections**

If verification requires a correction, stage only the files changed for that correction and use:

```powershell
git commit -m "docs: correct future roadmap integration"
```

If no correction is required, do not create an empty commit.

---

## Completion Criteria

- `docs/ROADMAP.md` contains Phases 7-12 with controlled statuses and links to the approved design.
- Phase 7 explicitly accounts for all 18 currently unimplemented canonical tools.
- Phases 8-12 are documented as planned and are not represented as implemented capabilities.
- README, capabilities, changelog, roadmap, and design documents agree on status and ownership.
- The validator rejects missing future phases, invalid phase statuses, and absent future-phase governance.
- Documentation tests, repository validation, and whitespace validation pass.
