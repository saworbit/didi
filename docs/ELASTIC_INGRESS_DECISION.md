# Decision record: bounded argument normalization experiment

**Status:** implemented on an isolated branch; proposed for review with the build
option OFF by default. Not a decision to enable it in releases or adopt the
original three-tier architecture. Date: 2026-09-25.

## Problem and hypothesis

A caller can express an unambiguous read using a representation outside the
published schema: `max_nodes: "10"` instead of `10`, or `point: [40, 40]` instead
of `{ "x": 40, "y": 40 }`. Didi correctly refuses the original request. Correcting
that representation may require another agent/tool round trip even though the
intended read did not change.

This experiment tests whether a small, explicit conversion policy can remove
some of those repair turns while preserving the canonical handler contract.
It does not establish how often this problem occurs in real use. The supplied
comparison paper is a source of hypotheses, not verified evidence about another
repository, a crash rate, agent productivity, or the safety of script evaluation.

The architectural change is significant because it introduces a second accepted
wire representation before strict validation. Its implementation is deliberately
limited: three existing read-only tools, no new engine operation, and no implicit
opt-in. Adding a compatibility path still creates maintenance obligations even
when its feature flag defaults OFF.

## Decision and boundaries

Retain the implementation as a default-disabled experiment for review. The server
advertises `safe-v1` only on the three supported tools when built with
`DIDI_ELASTIC_INGRESS=ON`. Each call must request the profile in metadata.

| Original proposal | Decision in this increment | Reason |
| --- | --- | --- |
| Parameter normalization | Implement selected integer limits and 2D coordinates | These representations can be mapped by a small, explicit policy and compared directly with canonical reads. |
| Broad semantic coercion and fuzzy paths | Exclude | Choosing a target, unit, boolean, or missing component requires assumptions about intent. |
| ClassDB reflection | Defer to an independent design | Engine thread affinity, object lifetime, authorization, and advertised capabilities need separate treatment. |
| Generic method invocation and mutation fallback | Exclude | Conversion must not select a different operation or retry execution through a different route. |
| Ephemeral GDScript evaluation | Exclude | Source filtering in the editor process does not establish a general execution sandbox. |

Read-only classification limits consequences; it does not prove zero risk.
Read operations can still consume resources or reveal project information.

## Alternatives considered

| Approach | Benefit | Cost or limitation |
| --- | --- | --- |
| Keep strict calls and improve examples/errors | No extra accepted representation or server policy | Caller still performs every repair; may be sufficient if these errors are rare. |
| Normalize in the client adapter | Canonical traffic reaches Didi; no server conversion needed | Each adapter must implement and version the policy; this may nevertheless be the better solution for a single client. |
| Widen published input schemas | Schema-validating clients can accept the alternate shapes | Changes the advertised contract for every consumer and spreads normalization requirements into tool contracts. |
| Explicit server profile, selected here for the experiment | One versioned policy, bounded before dispatch, comparable with strict calls | Requires client support for metadata and for sending the alternate arguments; adds validation and compatibility work. |
| Automatic coercion or execution fallback | Minimal caller setup | Can hide mistakes or change intent; exceeds the demonstrated evidence and current scope. |

A schema-validating client may reject an array before any request reaches Didi.
The server profile cannot fix that. Discovery metadata is not proof that an
existing assistant or adapter understands it. A real client integration must be
shown before claiming practical adoption value. If client-side conversion solves
the same problem more simply, retaining server normalization needs fresh justification.

## Preserved contracts and new costs

The conversion occurs before execution, using a temporary JSON object. A
cumulative budget is checked before copying. The existing schema validator
remains authoritative and normal dispatch validates again. Failure returns an
error; it never executes a partial conversion or selects another tool.

Ordinary calls retain strict behavior. The canonical names and input schemas
are unchanged. Profile calls may refuse values accepted by ordinary calls because
they also enforce the profile's bounds and precision rules. This is intentional;
`safe-v1` is not a promise to accept every canonical payload.

New costs include two build configurations, profile/version compatibility, numeric
boundary cases, custom discovery metadata, and maintaining tests when the three
schemas evolve. Fixed error prose avoids echoing supplied values but can be less
helpful than ordinary schema diagnostics. Budgets apply after transport parsing;
they do not replace parser or transport limits.

For exact conversions, budgets, errors, setup, and rollback, see the
[wire contract](ELASTIC_INGRESS.md). No breaking change is intended for ordinary
calls; a zero-percent regression-risk claim would be unsupported.

## Evidence and its limits

[Local verification](ELASTIC_INGRESS_VALIDATION.md) records configurations,
historical failures, fixes, and reproducible commands in the linked guide.

- Integrated Windows native suite: 862 passed after the separate timeout fix
  and rebase onto `5e41c7d`.
- Final Python suite: 436 run successfully, 11 skipped; targeted live tests ran separately.
- Exploratory session: 232 calls, 93 equal canonical/normalized response pairs,
  and recovery after 22 malformed requests.
- Retained live regression: passed with Godot 4.6.2 and 4.7.2 on Windows;
  verifies the expected button hit, no injected input, and unchanged scene bytes.
- Enabled/disabled protocol checks passed. Local smoke timings are not a formal
  benchmark and do not measure agent task completion or token savings.

These observations support the narrow implementation's local correctness. They
do not justify enabling it by default, adding more conversions, reflection, or an
evaluator. Linux/macOS validation and a controlled client/agent trial remain open.

## Review, trial, and adoption gates

Before merging the default-disabled experiment, reviewers should verify the
allowlist and failure-before-dispatch behavior, rerun checks on the integrated
revision, and obtain passing enabled/disabled CI on the supported platforms.
The current branch's historical green runs do not prove a later merge is green.

Before proposing default enablement or broader scope:

1. Demonstrate one actual client that discovers and deliberately selects the
   profile. Record whether its own schema validation permits the alternate input.
2. Compare strict and profile-enabled runs on identical project snapshots and
   task briefs, with fixed model/client settings and repeated trials. Include
   ordinary canonical calls, near-miss representations, and genuine invalid input.
3. Report task completion, argument-repair calls per task, total tool calls,
   available token/cost measures, and end-to-end latency. Include failures and
   unchanged tasks; do not infer productivity from hand-authored conversion tests.
4. Compare canonical-only strict dispatch with profile dispatch using interleaved
   warm runs; report latency tails and resource use at representative request
   sizes, including budget boundaries. Set numerical acceptance thresholds with
   maintainers before seeing trial results, not after.
5. Require no observed incorrect target, changed result, unintended mutation, or
   compatibility regression. Any such event blocks adoption pending diagnosis.
   Positive repair savings must not come at the expense of task completion.

The existing [field-trial design](FIELD_TRIAL_DESIGN.md) can inform the task and
ledger format; the controlled comparison above has not been run. The project
maintainers own the eventual adoption decision and numerical thresholds.

Keep the option OFF while evidence is incomplete. Revise or remove the experiment
if clients cannot use it, repair savings are negligible, costs outweigh benefits,
or a client adapter offers the same benefit with less shared complexity. More
fallback machinery is not the default response to an unsuccessful trial.

## Separate Windows timeout correction

Investigation also found a pre-existing process-tree teardown race: job accounting
could report no active processes before a child's handle signaled. The fix waits
on verified captured handles and reports uncertainty when complete coverage is
unprovable. It changes the Windows runner independently of normalization and
applies even when `DIDI_ELASTIC_INGRESS=OFF`.

Keep this correction independently reviewable in its own commit (or separate PR).
It should stand on its own regression evidence. The normalization build switch
does not revert it; reverting its own change is a separate operation.

For normalization rollback, rebuild with the option OFF and restart the server.
Profile-aware clients must stop sending the metadata or use canonical arguments
without the profile; disabled builds explicitly refuse profile requests. This
removes future normalization support and cannot undo already completed operations.
