# Draft PR: Experiment with explicit normalization of read-only arguments

A read request using `max_nodes: "10"` or `point: [40, 40]` currently needs a
representation repair before it can reach the existing handler. This experiment
lets a client explicitly select `safe-v1` for those representations on three
read-only tools, then applies the canonical schema and ordinary dispatch.
`DIDI_ELASTIC_INGRESS` remains OFF by default.

## Why introduce this boundary?

The hypothesis is fewer argument-repair round trips for clients that can use the
profile. Local tests establish equivalent results for selected conversions;
they do not establish error frequency, token savings, or faster agent tasks.
Schema-validating clients may block the alternate inputs before they reach Didi.
An actual client integration and controlled trial are adoption gates.

The [decision record](ELASTIC_INGRESS_DECISION.md) compares strict-only operation,
client-side normalization, wider schemas, and this server profile. It records
maintenance costs, rejection/removal criteria, and the broader proposal's
excluded reflection, generic mutation, fuzzy targeting, and script evaluation.
This PR proposes a bounded experiment, not default enablement of a new architecture.

## Scope and compatibility

- Only `scene_get_hierarchy`, `ui_list_controls`, and `ui_hit_test` advertise the
  profile when enabled. Every request must opt in explicitly.
- Canonical names and schemas remain unchanged. A temporary result is strictly
  validated before execution; failed conversion never triggers fallback dispatch.
- Cumulative budgets precede copying. Numeric precision limits and fixed,
  non-echoing diagnostics are part of the [wire contract](ELASTIC_INGRESS.md).
- No ordinary-call breaking change is intended. Profile-specific limits are
  stricter in some cases; zero regression risk is not claimed.

The branch also contains an independently reviewable Windows timeout correction:
job accounting could reach zero before a child handle signaled. The runner now
waits on verified captured handles and reports uncertainty when coverage cannot
be established. Keep this in its own commit or PR. It applies with the profile
OFF, and disabling normalization does not revert it.

## Evidence

- Enabled and disabled builds and all 15 focused normalization checks passed.
- Full native suite on `5e41c7d` plus these changes: 862 passed, zero failed.
- Final Python suite after adding live coverage: 436 run, 11 skipped, successful;
  existing subprocess/file ResourceWarnings remain in the general suite.
- Exploratory live session: 232 calls, 93 matching canonical/normalized pairs,
  and recovery after 22 malformed requests.
- Retained live regression passed on Windows with Godot 4.6.2 and 4.7.2; it checks
  expected hits, no injected input, and unchanged scene/source bytes.
- Original timeout regression passed three consecutive additional runs.
- Documentation contract (54 Markdown files) and diff checks passed after the
  rationale update. Generated inventory passed at 1298 tests after rebasing onto current main.

[Local verification](ELASTIC_INGRESS_VALIDATION.md) distinguishes historical
failures, configurations, follow-up fixes, and the limits of the smoke timings.

## Gates and rollback

The branch was rebased onto `5e41c7d` and locally reverified, including the
enabled live workflow on Godot 4.7.2. Before merge, pass enabled/disabled checks
on the supported platforms and reverify if the source or target branch changes. Before default
adoption, demonstrate client compatibility and measured task/repair benefits
against strict calls using pre-agreed thresholds. These gates remain open.

Rebuild with the option OFF and restart to disable normalization. Clients then
need canonical arguments without the profile; explicit profile requests are
refused. Reverting the independent timeout correction is a separate decision.
No rollback undoes already completed operations.

This document is the review-description template for this branch. Publication
status is tracked by its GitHub pull request. No release or version bump is
proposed here.
