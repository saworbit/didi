# MCP instructions and recent capability exploration — 2026-09-25

## Scope and isolation

Tested `feat/mcp-instructions` from commit `1cc86bd`, plus the harness changes in this report, in the dedicated `D:/didi-mcp-instructions` worktree. The concurrent `D:/didi` checkout was not edited. Live tests used disposable projects, project-specific session directories and owned editor processes. This is a Windows/MSVC/Python 3.14 run, not a claim of Linux or macOS coverage.

Coverage includes initialization instructions, optional `safe-v1` argument normalization, reimport overlap, audio bus authoring and routing, music-loop workarounds, export/settings file locks, and managed-editor failure recovery. The full live integration harness also exercised the existing scene, script, project, runtime, UI, export, resource and diagnostic workflows.

## Findings and fixes

- **Recovery harness import failure:** `python -m unittest tests.test_managed_recovery_adversarial` could not import its shared fixture, although discovery with `-t tests` worked. Reproduced in a failing regression test, fixed with package-aware import selection, and verified in both modes. The three previously blocked real-editor adversarial scenarios then passed.
- **Fixture process/pipe leaks:** YOLO, elicitation and output-schema fixtures killed children without consistently reaping them or closing stdin/stdout. Python reported running subprocesses and unclosed files. A bounded shared cleanup helper now drains/closes pipes and waits for exit. Tests cover a running child, an already-exited child and repeated cleanup. All 23 affected tests passed without those warnings.
- No new server regression was found in the exercised cases. Existing limitations below remain visible; passing this run does not mean those capabilities exist or those older defects are fixed.

## Automated harness additions

Ten tests join ordinary unittest discovery; no additional CI job or external dependency is needed:

- Five real-stdio instruction tests cover version fallback/discovery parity, malformed handshake recovery, duplicate initialization, 65 pipelined/batched responses, eight concurrent clients, untrusted client metadata, and the documented offline read workflow after an invalid request. Each exchange has a 20-second timeout and uses a temporary project with isolated discovery.
- Two normalization tests cover per-request profile isolation, recovery after refusal and numeric boundary/format cases, including Unicode digits, booleans, null, exponent notation and out-of-range depth. Both compile-time profiles pass. These additions cost less than a second across the existing 13-test profile suite on this machine.
- One import regression checks both supported unittest invocation modes.
- Two process-cleanup regressions check child exit and closure of every pipe.

## Executed validation

| Area | Result |
| --- | --- |
| Default and `DIDI_ELASTIC_INGRESS=ON` Release builds | Passed |
| Native suite, both profiles | 863 tests passed per profile |
| Python suite, both profiles | 446 discovered per profile: 439 passed, seven opt-in live tests skipped and exercised separately; no ResourceWarnings |
| Normalization wire suite, both profiles | 13 tests passed per profile |
| Live integration, Godot 4.5.1 and 4.7.2 | Both passed on the first attempt |
| Managed recovery, Godot 4.7.2 | Three acceptance and three adversarial scenarios passed; enabled-profile live normalization passed separately |
| Lock holder and release/retry | Expected five-second wait, retryable `409 project_file_busy`, unchanged file, then success after release |
| Two-server preset/settings races | 40/40 writes retained for each tool; no lost or falsely failed writes |
| Reimport overlap | First import, immediate repeat and a pending filesystem scan all succeeded; no editor errors/warnings |
| Music workflow | Ordinary asset stopped; script and sidecar/reimport workarounds both looped in a running game |
| Audio workflow | Names, read-only/moved layouts, initial properties, invalid dry-run, second-server refusal and editor/game routing matched expectations |
| Protocol/lifecycle/schema probes | Expected refusals and recovery; all 401 advertised parameters documented; no required output fields missing or type mismatches |
| Content coherence | Successful JSON text/structured payloads agreed; errors and image results were distinguished |

The full live harness emitted only its documented expected diagnostics and the known undo/redo defect. Raw editor logs for the focused reimport, music and audio runs contained no ERROR/WARNING lines. Focused audio testing also inspected the log after each call.

## Ergonomics and cost

The handshake guide is 3,881 UTF-8 bytes. Twenty local process-start-plus-handshake samples measured median **25.05 ms**, p95 **38.86 ms**, maximum **39.75 ms**. Fifty sequential calls per tool in a persistent offline session measured:

| Tool | Median | p95 |
| --- | ---: | ---: |
| `scene_get_hierarchy` | 0.67 ms | 0.74 ms |
| `script_get_symbols` | 0.81 ms | 0.92 ms |
| `project_get_setting` | 0.40 ms | 0.48 ms |

These are observations on a warm local machine, not CI performance thresholds or live-engine latency claims. The new guide did not introduce a perceptible handshake stall in these samples. Refusals named the actionable recovery: omit stale cursors, restart for a new initialized session, release a busy project-file lock, or attach the required session kind. Offline results remained explicitly separate from live state.

## Existing limits confirmed

**Follow-up, 2026-09-26:** both issues below were fixed before this report merged, and the two entries describe the tested commit, `1cc86bd`. #966 fixed #913: `editor_undo` and `editor_redo` run the editor's own Undo and Redo, and the live harness no longer allows the `Inconsistent redo history` line. #963 fixed #958 with `asset_configure_import`, which sets a WAV, OGG or MP3 import's loop options and checks what the engine then loads.

- [#913](https://github.com/saworbit/didi/issues/913): the integration harness still observes Godot's `Inconsistent redo history` diagnostic around undo/redo. This predates the tested instructions/normalization changes and remains a tracked engine-integration defect.
- [#958](https://github.com/saworbit/didi/issues/958): the surface cannot directly inspect or change a music asset's import-loop option. The existing script and sidecar/reimport workarounds function. The guide correctly routes unsupported asset work to files/Godot tooling; this run did not add an import-configuration API.
- Local native and Python runs do not replace cross-platform CI or visual/artistic review of a game. Rendering checks here are the live harness's viewport/UI assertions, not a subjective visual approval.

Local raw transcripts and latency samples are retained under the worktree's ignored `build/` directories (`exploratory-*` and `explore-*`); they are not committed as generated log bulk.
