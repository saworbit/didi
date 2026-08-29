# Surface Amendments

The canonical tool surface is stable by default. It is not frozen.

This document is the record of every change to it. An amendment must be accepted
here before a name is registered. The rule it replaces — "do not increase the
canonical count" — was written to stop success stubs from proliferating, and it
did that job. It also made a capability with no existing name impossible to add,
which is not what it was for. The no-stub rule stays absolute; the surface grows
through this record.

## Why a record and not a rule

Published tool counts are derived from the built binary by
`didi --dump-tool-manifest` and verified against the documentation by
`tools/validate_documentation.py --tool-manifest`. Adding a name therefore
updates every published count automatically. What the counts cannot tell you is
whether the name should exist at all. That judgment is what this document
records.

## What an amendment must state

| Field | Requirement |
| :--- | :--- |
| **Name** | The exact canonical tool name, in `domain_verb_object` form. |
| **Failing workflow** | The concrete agent task that cannot be completed without it. Not a capability description — an actual task that currently forces the agent to guess, poll, or go blind. |
| **Execution modes** | `live`, `offline_fallback`, or both, and the session kinds allowed. |
| **Safety class** | `read` (no `dry_run`), `create/set` (dry-run, idempotent), or `remove/overwrite` (dry-run plus confirmation token). |
| **Proving test** | The native or Godot integration test that will demonstrate the behavior, named before implementation starts. |
| **Reviewer** | Who accepted the amendment. Mutations additionally require a security review. |

An amendment that cannot name a failing workflow is refused. That is the whole
point of the record: it forces the question the previous rule made unaskable.

## How a failing workflow is found

Candidate names come from the workflow validation exercise, not from
brainstorming domains. Before each phase gate, run one real agent task end to
end and log every point where the agent had to guess, poll, or go blind. Each
logged blind spot is a candidate amendment.

The standing exercise is deliberately mundane: **make the player double-jump and
prove it works.** It touches scripting, input, runtime launch, live observation,
and verification, and it fails visibly wherever the surface has a hole.

## Status values

- `PROPOSED` — recorded, not yet accepted.
- `ACCEPTED` — approved; implementation may begin.
- `IMPLEMENTED` — registered, tested, and reflected in the tool manifest.
- `WITHDRAWN` — refused or superseded, with the reason retained.

## Amendment log

No amendments have been accepted yet. The first four candidates come from the
August 2026 competitive review and are proposed, not accepted, in
[Realignment Implementation Plan](REALIGNMENT_IMPLEMENTATION_PLAN.md):
`runtime_read_output`, `ui_list_controls`, `godot_api_reference`, and an `until`
parameter on the existing `runtime_step` (a change, not a new name).

## Deprecating a name

Removal is also an amendment. Didi follows the MCP feature lifecycle: a name
scheduled for removal is marked deprecated, documents its migration path, and
remains registered for at least twelve months before it may be removed. The ten
v1.0 legacy names in `kLegacyToolNames` have no deprecation date set; setting one
requires an amendment.
