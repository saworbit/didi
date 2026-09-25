# Elastic ingress: local verification

Date: 2026-09-25. Initial base: `89998a9`. Publication base: `5e41c7d`
(`origin/main`). Branch: `feature/elastic-ingress`.
Platform: Windows x64, MSVC 19.44, Debug, Python 3.14.0.
Local jsonschema version: 4.25.1; CI installs the repository's pinned version.

The first increment is implemented. After fixing a reproduced pre-existing
Windows timeout race, the full disabled native and Python suites pass locally.
Cross-platform and performance release gates remain open. The initial local
phase used no GitHub operations; publication preparation subsequently fetched
the current remote base with user authorization. The table below records the
initial phase, before rebasing onto the publication base.

| Check | Observed result |
| --- | --- |
| Baseline native suite before implementation | 855 passed, 1 failed, 856 total |
| Enabled build, all native targets | Compiled successfully |
| Disabled build, server and native tests | Compiled successfully |
| New native tests, enabled and disabled | 4 passed in each configuration |
| New stdio tests, enabled and disabled | 11 passed in each configuration |
| Final source-scoped CMake option, toggled ON then OFF with timeout fix | Rebuilt successfully; 4 native and 11 stdio tests passed in each state |
| Initial full enabled native suite | 858 passed, 2 failed, 860 total |
| Initial full enabled Python suite | 435 run, successful with 10 skips |
| Final disabled native suite, including timeout fix | 860 passed, zero failed |
| Final disabled Python suite, including timeout fix | 435 run, successful with 10 skips |
| Discovery compatibility | All 129 entries equal after removing the enabled profile advertisement |
| Strict response compatibility | Five successful/error response fixtures equal across enabled and disabled builds |
| Documentation contract | Passed against the built tool manifest |
| Generated test inventory | Passed; 1296 registered tests after the live regression was added |
| Whitespace/diff check | Passed |
| Independent automated code review | Normalization and timeout findings fixed; no remaining material findings in follow-up review |

## Initial native suite failures and follow-up

`TestRunner.TimeoutKillsTheWholeProcessTree` failed on the baseline and the
feature build, then failed again in isolation at `!processAlive(grandchild)`.
Follow-up instrumentation showed the job reporting zero active processes while
its captured child process handle remained nonsignaled for another 353 ms.
The runner previously reported `TreeExited` during this teardown interval.

The Windows timeout path now captures and verifies job member handles before
termination, then requires both empty job accounting and signaled handles within
its existing bounded wait. It reports `QueryFailed` if membership or lifetime
coverage cannot be established, including previously departed children absent
from the snapshot. This conservatively avoids claiming completion without
observable evidence; it can report uncertainty after older children exited.
The original regression test is unchanged and passed after the fix.

`IPC.Win32WriteDeadline` failed the `elapsed < 300ms` assertion in the full
feature run while a second build was compiling. It passed in isolation after
compilation stopped. That retry does not erase the full-run failure.

The initial full CTest run therefore returned failure even though its Python suite and
all new feature tests passed. The final disabled full-suite rerun passes both
native and Python suites, including the IPC deadline test. Python emitted
subprocess/file ResourceWarnings at shutdown despite its successful result;
this is not a warning-free run. Local build/test logs remain under `out/` in the
worktree; they are intentionally not source-controlled.

## Review fixes and evidence

- Bound opted-in request arguments by reference before the first copy. The
  normalizer checks cumulative limits before producing its temporary result.
- Reject parsed numeric strings at or beyond magnitude 2^53, including exponent
  spellings. Both native and stdio regression tests failed before the fix and
  passed afterward. JSON integer coordinates retain the documented closed interval.

The final CMake definition is scoped to the normalizer translation unit. The
secondary build was toggled ON and back OFF to verify that configuration path;
its final state is OFF. The original enabled build remains separately available.

## Remaining release gates

Cross-platform CI, formal performance budgets, and measured agent-loop
improvements have not been executed in this task. Targeted Windows live-editor
coverage is recorded below; it is not the complete engine integration suite.
Reflection and script evaluation remain deferred.

## Exploratory live sessions and iteration

The 2026-09-25 exploratory pass used the enabled Debug server with a disposable
managed editor and a two-Control scene. Godot 4.6.2 completed 232 calls in one
session: 93 canonical/normalized response pairs matched exactly, 22 malformed
requests were refused, and each refusal was followed by a successful hit on
`/root/UI/Go`. An additional unprofiled array request remained strict.

Inputs covered plus signs, whitespace, units, integer fractions/exponents,
range violations, booleans/null, malformed vectors, NaN/overflow, inexact large
integers, unknown fields, uncoerced booleans, and cumulative input budgets.
No new production defect was observed. The iteration retains this workflow in
`ManagedRecoveryLive.test_elastic_ingress_live_read_only_roundtrip` and clarifies
numeric syntax in the profile documentation. The retained test additionally
checks bounded non-echoing errors, no injected input, and unchanged scene/source
bytes. It passed on Windows with Godot 4.6.2 and 4.7.2 (one test on each).

A smoke timing sample measured medians of 6.938 ms for 93 successful canonical
reads and 6.829 ms for 115 successful normalized reads. These unequal samples
include warm-up and recovery calls, use a tiny scene, and are not a controlled
benchmark or evidence of an agent productivity gain.

The original process-tree timeout regression passed three consecutive reruns.
Exploratory transcripts and logs are under `out/vibe-*`; the permanent regression
and its invocation are documented in [the profile guide](ELASTIC_INGRESS.md).

Final Python discovery ran 436 tests successfully with 11 skips against the
enabled server after the retained test was added. The live scenario was run
separately with Godot as recorded above. The 11 disabled-profile stdio tests
also passed. Inventory (1296 registered tests), documentation, and diff checks
passed. The ordinary Python suite still emits existing subprocess/file cleanup
ResourceWarnings; these do not occur in the targeted live test results.

## Publication verification on current main

The branch was rebased onto `5e41c7d` (`origin/main`) before publication, preserving
the upstream reimport changes. On that integrated source revision:

| Check | Result |
| --- | --- |
| Default-OFF full Debug build | Passed, all targets |
| Default-OFF CTest native suite | 862 passed, zero failed |
| Default-OFF CTest Python suite | 436 run, successful, 11 skips |
| Enabled native profile checks | 4 passed |
| Enabled stdio profile checks | 11 passed |
| Enabled live regression, Godot 4.7.2 | Passed |
| Generated inventory | 1298 tests; includes the upstream additions |

Logs are `out/pr-*.log` and the CTest log under
`out/elastic-disabled/Testing/Temporary/LastTest.log`. The secondary directory
was used to toggle the option ON for focused/live checks, then restored OFF.
These are local Windows results; remote CI and adoption trials remain separate
gates. The timeout fix and the experiment are separate commits for review.
