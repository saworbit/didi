# Changelog

All notable changes to **Didi** (`godot-mcp-native`) will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical entries describe the surface advertised by those releases. For the executable status of each current registration, use [docs/CAPABILITIES.md](docs/CAPABILITIES.md) or runtime `tools/list` metadata.

---

## [Unreleased]

Nothing yet. The surface below is what 1.5.0 shipped and is current.

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `91/94`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Discovery now exposes 94 canonical tools plus 10 legacy registrations (104 total). 91 canonical tools are implemented and 3 remain unimplemented.

---

## [1.5.0] - 2026-09-03

### Added

- Added `scene_get_selection`, which reports the nodes selected in the Godot editor. It answers what a person means by "this node", and nothing in the surface could answer it before. Discovery now exposes 94 canonical tools plus 10 legacy registrations (104 total). 91 canonical tools are implemented and 3 remain unimplemented. The implementation remains 91/94 canonical tools, and all 3 Phase 7 names remain registered but unimplemented. Live and editor only: a selection exists only in a running editor, so an empty list read from a file would be a fabricated fact rather than a fallback. Entries carry the node path relative to the edited scene root plus class and name, capped at 256, with `selected_total` and `truncated` reported separately because two cases are deliberately counted and not named: a node selected in a scene other than the edited one, which has no path from the edited root, and a node freed between the engine building the list and Didi reading it. The `EditorInterface` and `EditorSelection` method binds were verified against extension API dumps from Godot 4.5.1 and 4.7.2 rather than assumed stable, and the live harness exercises the call on both.
- Exposed boards as subscribable MCP resources: `blackboard://<board>/state` and `blackboard://<board>/tasks`, with `resources/subscribe`, `resources/unsubscribe`, and `notifications/resources/updated`. The `resources.subscribe` capability moves to `true` because the handlers now exist. An agent waiting on another agent's work no longer polls `blackboard_read` on turns it pays for. The writer is a different `didi` process, so the server watches the board file's size and modified time on a background thread: still polling, but polling that costs no request, no token and no turn, and only while something is subscribed. The first tick records the current state rather than announcing it. A notification carries the URI and never the contents, so reading still goes through `resources/read` and its bounds. Only `blackboard://` URIs are subscribable, since nothing else changes without a call from the same client. Every write to stdout is now serialised through one lock, which `runStdio` did not need when it was the only writer. Tool counts are unchanged at 94 canonical, 90 implemented, 104 total; the listed resource count moves from 3 to 5.
- Added task allocation on the blackboard: `blackboard_task_create`, `blackboard_task_claim`, `blackboard_task_update`, `blackboard_task_complete`, and `blackboard_task_list`, recorded as an accepted amendment. Discovery now exposes 94 canonical tools plus 10 legacy registrations (104 total). 91 canonical tools are implemented and 3 remain unimplemented. The implementation remains 91/94 canonical tools, and all 3 Phase 7 names remain registered but unimplemented. Claiming is atomic: reading that a task is free and writing that it is yours happen under one board lock, so agents racing for the same task produce one winner and a clean refusal for the rest. A lease is the crash story, and the only record of a claim: when it lapses the task returns to the pool, and nothing renews one on an agent's behalf. Dependencies gate readiness, must already exist, and cannot form a cycle. Only the lease holder may update or complete a task, because completing someone else's releases its dependents on work that is still half done; reopening a reviewed or failed task is the deliberate exception. Tasks live in a board section `blackboard_write` cannot address, so a write cannot corrupt the queue.
- Added a shared blackboard: `blackboard_write`, `blackboard_read`, `blackboard_patch`, `blackboard_list_keys`, and `blackboard_clear`, recorded as an accepted amendment in [docs/SURFACE_AMENDMENTS.md](docs/SURFACE_AMENDMENTS.md). Discovery now exposes 94 canonical tools plus 10 legacy registrations (104 total). 91 canonical tools are implemented and 3 remain unimplemented. The implementation remains 91/94 canonical tools, and all 3 Phase 7 names remain registered but unimplemented. Two agents are two processes, so the board is a file under `.didi/blackboard/` in the project rather than process memory: an in-memory board would pass every single-agent test and be empty for the second agent, which is the only case that motivated it. Every read-modify-write runs under an OS-backed exclusive lock and saves through an atomic rename. Paths are dot or slash separated and reject traversal, empty segments and control characters; a write refuses to run through an existing value rather than silently turning another agent's number into a container. Entries take an optional `ttl_seconds` and disappear from reads, listings and the file once it lapses. Patches are RFC 6902 and all or nothing. `blackboard_clear` always requires a confirmation token, because there is no non-destructive clear. Board content is data and never instruction: values are stored and returned verbatim and Didi never interprets or executes them. Bounds ship in the response: 256 KiB a value, 4 MiB a board, 10,000 keys, 32 levels deep.
- Extended `project_audit_assets` with bounded, read-only Godot `.import` health evidence. Existing metadata can now report invalid metadata, missing source assets, missing generated outputs, and source timestamps newer than outputs, with metadata/source/target provenance and a total count independent of the response cap. The scanner reads at most 20,000 regular sidecars, 256 KiB, and 1,024 declared outputs per sidecar; skips generated build trees plus directory/file symlinks; validates sidecar/source identity and project containment; and retains only the requested top findings. Timestamp findings are deliberately named `source_newer_than_output`: Didi does not claim to reproduce Godot's checksum, importer-version, or settings-validity decisions.
- Started Phase 8 with exact static node-path blast-radius analysis in `project_analyze_impact`. Relative, `/root/...`, `%...`, and `$...` targets now resolve as `node_path`; scene connection endpoints, animation tracks, serialized `NodePath` properties, and direct GDScript references are classified without matching similarly named siblings. Dynamic paths remain explicitly outside the evidence boundary, and no mutation or refactoring cascade is implied.
- Hardened the Phase 7 delivery after independent red/purple review: unsigned JSON integers above `INT64_MAX` can no longer wrap into valid tile/grid coordinates or the GridMap clear sentinel; every mutating Phase 7 request now reports a non-retryable unknown outcome after an ambiguous post-dispatch transport failure; tile/grid rollback method binds are verified before mutation; generated prompts and current documentation now match the 80/83 manifest; and the validator rejects the stale capability claims that exposed the drift.
- Delivered `tilemap_set_cells`, `tilemap_get_used_rect`, and `gridmap_set_cells`, taking the surface to 80/83 implemented with only the three API-blocked Phase 7 contracts reserved. TileMapLayer and GridMap mutations validate every record and referenced resource before creating one UndoRedo action, snapshot every old cell, reread exact post-state, roll back a mismatch, and report no-op batches without adding undo history. The used-rect read returns exact integer position, size, and end coordinates. The live fixture covers set, erase/clear, no-op, wrong class, invalid-last-record atomicity, and real TileSetAtlasSource/MeshLibrary validation.
- Delivered `viewport_set_camera_transform` and `viewport_toggle_debug_draw` as the viewport half of Phase 7A, taking the surface to 77/83 implemented with 6 names still reserved. Camera edits target an in-scene `Camera3D`, validate finite bounded vectors and FOV, commit one editor UndoRedo action, and verify observed state. Debug control supports only public collision/navigation SceneTree hints for future games run from the editor; it rejects the retained wireframe request, preserves omitted values, rereads both hints, and restores both original values on any failed setter or postcondition.
- Delivered `anim_list_tracks` and `anim_play_track` as the second half of Phase 7B, taking the surface to 75/83 implemented with 8 names still reserved. An agent could see an AnimationPlayer in the tree and nothing about what it held. The list returns every animation in the player's library, sorted by name, with length, loop mode, and each track's type, path and key times, in the editor or a game, capped at 128 animations, 128 tracks, 256 keys and 256 KiB with a cursor that says where it stopped. It never touches a key. The play is game-only: it checks the name exists, calls `AnimationPlayer.play` once with the requested speed and direction, and rereads `is_playing` and `current_animation` rather than trusting the call; `dispatched` is not completion. A negative speed without `from_end` is rejected because it would play nothing from time zero. Current animation is read through `Object.get` because `get_current_animation` changes hash between Godot 4.5 and 4.7. The live harness lists the fixture's animation in both sessions and plays it in the game with the key count unchanged afterwards.
- Delivered `physics_raycast_query` and `nav_query_path` as Phase 7B partial delivery, taking the surface to 73/83 implemented with 10 names still reserved. Line of sight and reachability were guesswork from screenshots and transform arithmetic. The raycast fires one segment through the root viewport's existing World2D or World3D with the contract's fixed flags (bodies and areas on, hit from inside off, back faces on in 3D) and returns the hit point, normal, collider path and class, and collision layer, or every detail field as null on a miss. The path query asks the same world's navigation map for a path and returns the ordered points, capped at 256, with `reachable` false on an empty path. Neither creates a world, a map or a body, and neither bakes anything. Both run in the editor or a game; in the editor the root viewport's world is the editor's own, not the edited scene's, so the live harness proves a real hit and a real path in a game session and an honest miss in the editor.
- Delivered `runtime_inject_input` (legacy alias `inject_input_event`) as Phase 7C partial delivery, taking the surface to 71/83 implemented with 12 names still reserved. Until now an agent could launch a game and watch it, but not press anything in it. A call carries 1 to 32 explicit events across the five allow-listed classes: action, key, mouse button, joypad button and joypad motion. Every event is constructed and fully configured before the first is dispatched, so a bad event in position five fails the batch with nothing sent, and press and release are separate events with no timer and no implied release. Dispatch is `Input.parse_input_event`, which returns void, so the result counts calls made rather than events accepted; the live harness proves delivery through a fixture that observes `_input`. Game sessions only: the standalone policy refuses editor sessions and the extension refuses again before the bridge. A mutation with `dry_run` and no confirmation token, since an input event is not reversible and not destructive.
- Delivered `runtime_read_profiler` as Phase 7C partial delivery, taking the surface to 70/83 implemented with 13 names still reserved. Until now an agent chasing a stutter could read one frame at a time and nothing across time. The tool samples ten `Performance` monitors on the Godot main thread over a window of up to 5 seconds and 120 samples, driven by the frame callback so nothing blocks, and returns min, max, mean and last per metric. Availability is the pinned `Performance.get_monitor` bind existing and nothing else: a zero reading is a legitimate sample, which matters because most monitors are legitimately zero in an idle editor. A non-finite reading counts as invalid, and a metric with no valid sample reports explicit nulls rather than dropping the fields. One collector runs per session; a second request while one is active gets `423`, and shutdown mid-window returns `504` with the outcome rather than a partial window. The live harness collects a window in the editor and in a game session and checks the shape, the count and the order.
- Added `audio_configure_bus`, which sets a bus volume, mute or solo on the running engine. It is live only on purpose: writing the layout file would change what the project loads next time and not what anyone is listening to now. A bus can be named or numbered, and a name is resolved through the engine so a bus added at runtime is addressable. A `volume_db` outside -80 to 24 is rejected rather than clamped, because outside that range a caller is either confusing decibels with a linear gain or has slipped a digit and clamping hides both. Bus state is not part of the edited scene, so the editor undo stack does not carry it; the result says so and returns the values it replaced, which are the only way back. The live harness changes a bus, reads it back through a separate call rather than trusting the response that made the change, and puts it back.
- Added `audio_list_buses`. A muted bus is invisible: the game runs, nothing errors, and no sound comes out, and nothing in Didi could read the bus layout at all, so the question could not be asked. Live it reports each bus's volume, mute, solo, bypass, routing and effect chain from `AudioServer`; every method it calls carries the same hash on Godot 4.5.1, 4.6.2 and 4.7.2, so there is no per-version branch, and the live harness proves it on all three. Offline it reads the project bus layout, following `audio/buses/default_bus_layout` and falling back the way Godot does. A project with no layout file is reported as exactly that rather than as an error, and the offline result says effect chains were not read, because an empty list would otherwise read as "no effects".
- Added `project_analyze_impact`, which answers what else changes if this changes. Renaming a variable or a signal can break a scene that wired it, an animation track that keyframes it, or an autoload that loads it, and Godot reports none of that until the game runs. A lexical search does not report it either: the connection lives in a `.tscn` as an attribute and the keyframe lives inside a quoted `NodePath`, so the agent edits the script, sees a clean search, and ships a project that is broken at runtime. Every place the target is named is returned with the form it takes, whole word so tracing `health` does not report every `max_health`, plus where the name is declared. A target that is neither a path nor a single identifier is rejected rather than answered with an empty report, because "nothing depends on this" and "you asked the wrong question" must not look the same to a caller about to delete something.
- Added `project_audit_assets`, an offline pass over the whole project that reports three things no single file can show: assets nothing references, references that resolve to no file, and signals nothing emits or connects. It follows every reference form Godot writes, including the uid-only `ext_resource` that Godot has been writing since 4.4, so an asset named only by uid is not called an orphan. Orphan detection is restricted to asset types because a scene nothing references is usually a level you open by hand, and a tool that reports those is a tool people learn to ignore. The findings are evidence, not a delete list, and the response says so: a path a script builds at runtime cannot be followed, and neither can a connection made through a variable name. Both limits ship in the payload, not only in the docs.
- Closed Phase 6 without expanding the protocol surface: mandatory explicit Godot project selection, project-keyed runtime endpoints, one-client OS session locks, mutation dry-runs, and exact confirm-before-write tokens.
- Closed Phase 5 with six canonical tools: C# build diagnostics, real shader compilation diagnostics, secret-redacted export-preset discovery, guarded headless export, deterministic GridMap MeshLibrary generation, and live non-injecting UI hit-testing.
- Added a cross-platform argv-only process runner with finite deadlines, child-group termination, a 1 MiB combined-output cap, and Windows command-line quoting coverage.
- Added the approved Phase 7-12 roadmap, including canonical-surface completion and governance requirements for all future phases.
- Completed the 2026-08-29 Phase 7 feasibility gate on Godot 4.5.1 and 4.7.2. The reproducible [evidence](docs/PHASE_7_API_FEASIBILITY.md) found 15/18 implementation-feasible and 3/18 API-blocked under the approved contracts; the [executable plan](docs/PHASE_7_IMPLEMENTATION_PLAN.md) stopped before Tasks 2-13.

- The documentation validator now requires every `tests/test_*.py` module to be named by some workflow. CI runs named `unittest` modules rather than discovering the tests directory, so a new test file ran nowhere until someone remembered to add a step -- and passed locally, so nothing looked wrong. `tests/test_phase7_signal_admission.py` was in exactly that state: it asserts the Phase 7 signal test seam never reaches a shipping build, and it executed in no job at all. It is now registered, and the rule prevents a recurrence.
- Added YOLO mode: `--yolo`, or `DIDI_YOLO=1`, skips confirmation on destructive tools for unattended runs. It is a launch flag only, chosen by the person starting the process; nothing reachable from a tool call can set it, and a test asserts no tool exposes such an argument. It skips confirmation, not validation or authentication. The open gate is visible at startup, in `server/discover` as `_meta.didi.confirmationsSkipped`, and on every affected result as `confirmation: skipped` -- distinct from `human` and `agent`, because nobody confirmed anything.
- Confirmation for destructive tools can now reach a human. When a client declares the `elicitation` capability, a confirmation-gated call without a token returns an `input_required` result carrying an `elicitation/create` and the real dry-run preview, so a person sees what will change rather than a tool name. `accept` executes; `decline` and `cancel` both refuse and stay distinguishable. Previously the agent received the confirmation token and echoed it back, which is the agent confirming to itself.
- A client that cannot elicit is not silently downgraded: the token flow remains, and every confirmed mutation now records `_meta.didi.confirmation` as `human` or `agent` so a caller can tell what the confirmation was worth.
- Didi now serves MCP revision `2026-07-28` alongside `2024-11-05`. Every result carries `resultType`, and cacheable operations carry `ttlMs` and `cacheScope`. The freshness values are deliberately conservative: `tools/list`, `resources/list` and `resources/read` embed live session availability that flips when an editor starts or stops, so they report `ttlMs: 0` -- immediately stale. A cache that serves a stale availability claim is worse than no cache. Only `server/discover` and `prompts/list`, which are compile-time constants, claim a real freshness window.
- Discovery advertises only revisions Didi actually serves, and that is enforced rather than asserted: a test drives a real request at every version discovery advertises and requires it to succeed, so the advertised list cannot outrun the implementation.
- Added `server/discover`, making Didi dual-era. MCP revision `2026-07-28` removed the `initialize` handshake: a modern client declares its protocol version in `_meta` on every request, and servers must implement discovery. Didi still serves `2024-11-05` result shapes and so advertises only that revision, but a modern client now receives `-32022 Unsupported protocol version` naming what it can retry with, instead of the silence a legacy-only stdio server gives it. A request carrying a supported version is self-contained and needs no prior `initialize`. Legacy clients are unaffected.
- Delivered the four signal tools -- `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit` -- as Phase 7 partial delivery, taking the surface to 69/83 implemented with 14 names still reserved. They were admitted only after the production-configuration extension passed the raw signal bridge trial on Godot 4.5.1, 4.6.2 and 4.7.2. That trial had never been run: one compile flag controlled both admission and the failure-injection test seams, so the only binary that could serve a signal request was one no user would ever run. Separating the two is what made the trial possible.
- The signal test seams remain compiled out of every shipping build, and a test now asserts the seam configurator is absent from production rather than asserting the whole feature is.
- Added `runtime_read_output`, which reads what the **engine** printed rather than what Didi recorded: `print()` from a running game, `push_warning`, `push_error`, and GDScript parse and runtime errors, the last carrying the originating script file and line. Didi registers a custom `Logger` class and subscribes it through `OS.add_logger`; this is the first class the extension registers with the engine rather than only calling into. The stream is a separate 2,000-record ring with the same cursor contract as `runtime_read_logs`, so heavy engine output cannot evict Didi's own diagnostics. Verified end to end against Godot 4.5.1, 4.6.2, and 4.7.2. Where an engine does not expose the class-registration interface the extension still loads, warns at startup, and the tool returns no records rather than failing.
- Added `didi --dump-tool-manifest`, which emits the registered tool surface as sorted, byte-stable JSON with counts and names. Documentation and the CI MCP smoke are now validated against it, so a published count can never disagree with the software.
- Added `kLegacyToolNames` as the single declaration of which registrations are legacy. The canonical/legacy split previously existed only in prose and could not be verified.
- Added `--list` and `--filter=<substring>` to the native test runner, so a single case can be run in isolation.
- Added [docs/SURFACE_AMENDMENTS.md](docs/SURFACE_AMENDMENTS.md), the record through which the canonical tool surface may grow.
- Added specification tool `annotations` to every registered tool. `readOnlyHint` is derived from the same mutation classification that drives `dry_run` and confirmation, so 41 of the 89 registrations are identifiable as safe to auto-approve without splitting any tool into read and edit pairs. `destructiveHint` is true for every mutation, and `openWorldHint` is true for the six tools that start a subprocess against the project and false for the rest.
- Added `outputSchema` to the tools whose result shape has been observed, covering script diagnostics, both project searches, resource listing, session listing, viewport capture, scene hierarchy, and their legacy aliases. A contract test exercises each one through the built binary and validates its real payload against the published schema. Tools that cannot be exercised, and every unimplemented name, declare none.
- Added `structuredContent` to successful JSON tool results, carrying the same payload as the text block after execution-mode and session attribution. The text block is unchanged.

### Changed

- Took the version out of the C++ sources. `project(VERSION ...)` in `CMakeLists.txt` now generates `didi/common/version.hpp` into the build tree, and `mcp_protocol.hpp` and `main.cpp` read it instead of spelling `1.4.0` out three times between them. The documentation validator no longer compares those two files, because they cannot drift; it rejects a literal version appearing in either of them instead. The contributing instructions were also incomplete: they listed eight files to update and omitted `demo/addons/didi/plugin.cfg`, which the validator has been checking all along.
- Mutating tool schemas now advertise `dry_run`; editor reload, script patching, and overwrite-enabled offline writers require a 120-second single-use token bound to the exact arguments, project, and runtime route.
- The documentation validator now derives the Phase 7 status block's implementation ratio, and the spelled-out forms of published counts, from the tool manifest as well. Both were still literals, so registering any new canonical tool failed CI until the validator itself was edited.
- Corrected the published read-only registration count. It was stated as 43 and the binary reports 41; the figure had never been checked against the software.
- The documentation validator derives every published tool count from the tool manifest instead of matching hard-coded numbers in prose. It previously enforced that documents agreed with each other rather than with the binary, and implementing any reserved tool would have failed CI until the validator itself was edited.
- The CI MCP smoke verifies the live `tools/list` surface against the manifest emitted by the same build, and now asserts every `implemented` flag rather than a sample.
- Split the fused surface rule: "no success stubs" remains absolute, while new tool names are added through a recorded surface amendment.
- Documented that Godot 4.5 and 4.6 expose no read-side scene dirty state through GDExtension and that `EditorInterface.get_unsaved_scenes()` arrives in 4.7, which Didi does not yet consume. The previous wording named only 4.5 and read as a permanent engine limitation.
- The live integration harness runs on Windows PowerShell 5.1. It previously required PowerShell 7 solely because of `ConvertFrom-Json -Depth`, which does not exist on 5.1 and is unnecessary on either host.
- Discovery now exposes 83 canonical tools plus 10 legacy registrations (93 total). 80 canonical tools are implemented and 3 remain unimplemented.
- Phase 7 status is `PARTIAL_DELIVERY`. All 15 implementation-feasible names are delivered. The remaining `physics_simulate_step`, `nav_bake_mesh`, and `runtime_get_call_stack` contracts stay registered but unimplemented because no supported public API/semantics satisfying the exact approved contract was found on either tested version.

### Fixed

- Packaged the addon from the build directory instead of the source tree. Release packaging copied `addons/didi/*` verbatim, which meant an archive contained whatever a POST_BUILD step or a tool had left in a tracked directory: `bin/.gitkeep` and, on any machine where `viewport_create_test_lab` had run, a generated `test_lab_sandbox.tscn`. The build now assembles the complete addon at `build/addons/didi/` and packaging reads that. `cmake --install` also shipped an addon without `didi_plugin.gd`, the script `plugin.cfg` names, so an installed plugin could not start; the addon file list is now declared once and used by both the staged copy and the install rules. The extension is still copied into the demo and smoke Godot projects, which can only load it from inside their own `res://`.
- Corrected documentation that had drifted from the build. The architecture diagram and the integration guide still said 78 canonical tools when the manifest emits 83, the README badge and two documents named only the legacy MCP revision when `server/discover` advertises `2026-07-28` alongside `2024-11-05`, and nothing described the launch arguments at all. The administrator guide now carries a command line option table and the exit `2` refusal behaviour, and the quickstart, capability matrix, and agent instructions say that a malformed option is refused rather than ignored.
- Refused unknown and malformed command-line options at startup instead of ignoring them. The parser had no final else, so a misspelled option or a log level outside the documented enum started the server as though the launch had succeeded, and a value-taking option would consume the flag after it: `--log-level --yolo` came up without YOLO mode and without the warning that says confirmations are off. Unknown options, missing values, option-shaped values, empty values, unknown log levels, and stray arguments now exit 2 with the reason and the relevant help line, before project resolution or any server startup. Accepted forms are unchanged.
- Quarantined the runtime route only on transport failure. `sendPhase7LiveRequest` retired the route on any error before classifying it, so an ordinary rejection from the engine left every later live call in the session unable to dispatch, including unrelated tools. Its contract test never covered this: both cases were transport failures and one only looked like one, using a bare `Error(502)` that carries no transport state.
- Reaped orphaned session descriptor tombstones. Retirement is move-then-delete, so an owner that died between the two steps left a `.didi-retired-*` file that nothing ever removed and the registry grew without bound. Discovery now removes such an entry only when its contents parse as a descriptor, the session id in the filename matches the session id inside it, and the owning process is provably gone; an alive or unverifiable owner, unreadable contents, or a name that disagrees with its contents all retain it. POSIX still always retains, because no portable unlink primitive is bound to a verified open file.
- Reconciled all current operating documentation with Phase 6: completed the roadmap's 79-tool table, documented project-root startup, session lock `423`, mutation preview/confirmation semantics, and labeled historical design records so they are not mistaken for current behavior.
- Gave four order-dependent native tests their own setup. `Tools.CaptureViewportWithIpc` was intermittently failing because it registered none of the tools or resources it called and borrowed them from whichever test ran before it; the assertion that failed depended on execution order. Every native test passes in isolation.
- Preserved ordinary comments when replacing GDScript symbols.
- Preserved explicit `null` JSON-RPC success results.
- Failed closed before creating a Windows session pipe when the owner-and-Administrators security descriptor cannot be built.
- Rejected malformed MCP/JSON-RPC parameter types (including scalar `params`), request-only methods sent without an ID, JSON numeric overflow, and unsupported `Content-Length` framing without terminating the server or dispatching hidden mutations.
- Protected `resource_create` and visual test-lab files from replacement unless callers pass `overwrite: true`.
- Enforced one reconnect-and-I/O IPC deadline on POSIX, exact response-ID correlation on both transports, and distinct handler-exception responses that preserve the parsed request ID.
- Bounded `runtime_launch` to 1–120 seconds, treated Windows exit code 259 as completed, broadened Godot 4.5.1/4.6.2/4.7.2 and POSIX/macOS discovery, and clarified that `break_on_error` classifies output after exit.
- Declared explicit x86_64, arm64, and universal macOS GDExtension keys; launched Windows Godot batch wrappers, including non-ASCII paths, through the trusted System32 `cmd.exe`; accepted arithmetic `+`; parsed Godot 4 multiline compiler diagnostics; limited the `else` colon rule to the complete keyword; and stopped advertising the unimplemented MCP logging capability.
- Made lightweight GDScript diagnostics and both symbol APIs string/comment-aware, recognized annotated/static/inner declarations, added bounded and format-validated Godot `.uid` sidecars across resource types, and removed unsafe `demo/` and recursive scene-path fallbacks in favor of UTF-8-safe project-root-confined files.
- Parsed Godot 4.5 dummy-renderer shader diagnostics, used the supported four-argument `find_children` API for generated MeshLibrary scripts, restored editor routing after long offline work, and applied Control's documented rectangle fallback when `_has_point` has no callable override.
- Updated the Linux, macOS, and Windows fast MCP smoke to lock the 79-canonical/89-total Phase 5 surface and all six new execution-mode/schema contracts.

### Verified

- Extended the disposable Godot 4.5.1 integration harness through valid/invalid shader compilation, pack export, deterministic two-item MeshLibrary generation, and ordered live UI hit-testing with and without ignored controls. The native suite contains 162 passing tests.

## [1.4.0] - 2026-08-28

### Added

- Closed Phase 4 with four canonical tools: bounded literal `project_search_text`, lexical GDScript/C# `project_search_symbols`, editor-backed `asset_reimport`, and exact live `viewport_diff_capture`.
- Added 32-lowercase-hex live capture IDs backed by an 8-entry/64 MiB process-local RGBA LRU cache with a 2,048 × 2,048 per-image limit.
- Added reversible `node_isolation_path` capture with optional transparent background, instance-ID-safe reverse restoration, forced redraws, and explicit restoration metadata.
- Added exact-dimension RGBA diff metrics and transparent PNG output, including threshold, pixel count/ratio, per-channel mean error, maximum delta, and nullable bounding box.

### Changed

- Version is now `1.4.0`; discovery exposes 72 canonical tools plus 10 legacy registrations (82 total). Fifty-four canonical tools are implemented and 14 remain unimplemented.
- Project search enforces canonical containment, allowlisted `.gd`/`.cs`/`.tscn`/`.tres` formats, symlink/generated-tree exclusion, UTF-8 validation, deterministic order, and file/byte/result/preview limits.
- Asset reimport validates the complete source batch before mutation, permits one active request, and requires two consecutive editor-idle callbacks before success.
- Carried forward automated version, release-fact, support-policy, and Markdown-link drift validation from the Phase 3 documentation reconciliation.
- Removed agent-internal workflow reports, plans, and specifications from the project tree, ignored their former paths, and added validation to prevent them from being committed again.

### Fixed

- Prevented synchronous `EditorFileSystem.reimport_files` callbacks from deadlocking the pending-reimport lifecycle lock.

### Verified

- Extended the Godot 4.5.1 disposable integration harness through real search, SVG reimport, reversible node isolation, a non-empty visual mutation diff, and an exact post-undo diff while preserving fixture and session cleanup.

## [1.3.0] - 2026-08-27

### Added

- Closed Phase 3 with ten canonical tools: local session discovery/attach/detach/get plus live structured logs, pause/step/stop, runtime tree inspection, and bounded `eval_gdscript`.
- Added atomic schema-1 descriptors and process-unique same-user IPC endpoints for concurrent Godot editor and game sessions. Authenticated protocol-1.3 attach uses a 3-second handshake and preserves the previous route on failure.
- Added a 2,000-record cursor log ring with deterministic gap/filter behavior, 16 KiB messages, 64 KiB details, and token/expression-source redaction.
- Added exact paused game stepping, single-pending-step enforcement, shutdown cancellation, pause verification, 10,000-node plus 256 KiB runtime-tree bounds with UTF-8-safe field truncation, and PID-plus-process-start identity checks across Windows, Linux, and macOS.
- Added strict read-only expression evaluation with a receiver-aware allowlist, ClassDB-prebound scalar property reads, in-subtree contexts/results, cooperative deadlines, depth/element/size limits, and adversarial scanner/callback coverage.

### Changed

- Version is now `1.3.0`; discovery exposes 68 canonical tools plus 10 legacy registrations (78 total). Fifty canonical tools are implemented and 18 remain honestly unimplemented.
- Deterministic same-project auto-attach selects an unambiguous sole session or unique editor; ambiguity remains detached. `runtime_get_session` performs a fresh bounded authenticated identity handshake and quarantines the failed route without disturbing a concurrently superseding route.
- Capability metadata is session-kind-aware: `sessionKind` identifies the selected editor/game, `editorConnected` is true only for an editor, and `liveAvailable` requires that the selected kind is allowed for that exact tool or resource.
- Live main-thread work now has a 15-second extension deadline with explicit `not_started` versus `unknown_outcome` results. Public live calls use a 17-second outer deadline and quarantine only the exact failed route generation.
- POSIX session discovery now uses `$XDG_RUNTIME_DIR/didi-sessions` when XDG provides an absolute path, otherwise the effective-UID-qualified temporary fallback. Proof-safe POSIX retirement retains a non-`.json` tombstone that discovery ignores; Windows deletes the exact verified object through its open handle.
- CI smoke now locks the 78-registration surface, Phase 3 execution metadata, cursor schema, evaluator limits, and the still-unimplemented runtime input/call-stack/profiler tools.
- Runtime logging is explicitly scoped to structured Didi events. It does not capture arbitrary external `print()` output; `runtime_launch` remains the bounded child stdout/stderr path.

### Verification

- The v1.3.0 release matrix runs the complete native suite plus concurrent editor/game integration coverage on Godot 4.5.1 and 4.7.2. The test runner's reported total remains authoritative as the suite evolves; the live harness preserves the 119-request Phase 1/2 baseline and adds the Phase 3 session, routing, tree, log, control, and evaluation sequences.

## [1.2.0] - 2026-08-27

### Added
- **Phase 1 live engine substrate** for Godot 4.5+: native main-loop dispatch, real edited `SceneTree` traversal, scalar property access, UndoRedo-backed node mutations, editor undo/redo/save/rescan, and real editor viewport PNG capture.
- **Phase 2 project wiring** with 18 new canonical live tools for script attachment, autoloads, typed InputMap events, bounded project settings, scene groups, and scene create/open/close/branch packing.
- **Atomic project persistence** through `ProjectSettings.save()` with snapshot rollback and live `InputMap` reload.
- **Disposable 119-request Godot integration fixture** covering Phase 1 and Phase 2 success, undo/redo, persistence failure and rollback, resource ownership, overwrite, malformed input, and unsafe-path cases.
- **Honest capability discovery**: every `tools/list` and `resources/list` entry now reports `_meta.didi.executionModes`, `implemented`, and an explanatory `reason` when unavailable. Dynamic metadata also reports the current live/offline state.
- **Cross-version integration harness** covering Godot 4.5.1, 4.6.2, and 4.7.2.

### Fixed & Hardened
- Removed the non-functional GDScript singleton pump and all live-success stubs.
- Prevented timed-out queued commands from mutating the editor later and bounded main-thread work to 64 commands per frame.
- Made timeout cancellation state-aware for queued commands. Phase 3 later replaced the already-running indefinite wait with bounded `unknown_outcome` handling and route quarantine.
- Removed the original outer timeout race; Phase 3 subsequently made the public live-call deadline finite and generation-safe.
- Made cross-thread bridge readiness atomic and resolved pending IPC promises during editor shutdown.
- Kept scene mutations in the edited scene's UndoRedo history, used undo-side references for removed nodes, and preserved node lifetimes across history pruning.
- Rejected unknown or type-incompatible scalar properties and restored exact sibling order after remove and reparent undo.
- Confined node resolution and mutations to the edited scene subtree, protected its root, rejected cyclic reparenting, and rejected non-`Node` ClassDB objects before UndoRedo registration.
- Preserved live viewport provenance and dimensions at the public MCP boundary; only real GPU-backed captures report `is_live_frame: true`.
- Centralized result-level execution provenance and kept offline-only filesystem/parser work out of Godot's main-thread command queue.
- Updated pull-request CI assertions to cover the complete 68-registration surface, dynamic execution modes, resources, and canonical scene hierarchy output.
- Made `scene_close` conservative on Godot 4.5: explicit `discard_unsaved: true` is required because that API cannot expose active-scene dirty state.
- Made explicit scene overwrite replace the ResourceLoader cache and reload existing editor tabs before verification.
- Raised the minimum supported Godot version to 4.5, where the required native main-loop callback API is available.
- Reconciled README, quickstart, capability, tool, protocol, architecture, operations, LLM, resource/prompt, developer, roadmap, contribution, and security documentation with the verified implementation.

---

## [1.1.0] - 2026-08-26

### Added
- **Exhaustive 40-Tool Canonical Surface across 9 Functional Domains**:
  - *Domain 1 (Scene Tree & Nodes)*: `scene_get_hierarchy`, `scene_instantiate_node`, `scene_remove_node`, `scene_reparent_node`, `scene_set_property`, `scene_get_property`, `scene_duplicate_node`.
  - *Domain 2 (Signals & Events)*: `signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`.
  - *Domain 3 (Scripting & Reflection)*: `script_check_syntax`, `script_reflect_class` (built-in Godot 4 class reflection), `script_get_symbols` (AST parser), `script_patch_method`.
  - *Domain 4 (Vision & Render)*: `viewport_capture_frame`, `viewport_set_camera_transform`, `viewport_create_test_lab`, `viewport_toggle_debug_draw`.
  - *Domain 5 (Physics, Animation & Navigation)*: `physics_raycast_query`, `physics_simulate_step`, `nav_bake_mesh`, `nav_query_path`, `anim_list_tracks`, `anim_play_track`.
  - *Domain 6 (Tilemaps & GridMaps)*: `tilemap_set_cells`, `tilemap_get_used_rect`, `gridmap_set_cells`.
  - *Domain 7 (Resources & Project Files)*: `resource_create`, `resource_inspect`, `project_list_resources`, `project_get_uid_map`.
  - *Domain 8 (Execution, Input & Debug)*: `runtime_launch`, `runtime_inject_input`, `runtime_get_call_stack`, `runtime_read_profiler`.
  - *Domain 9 (Editor Lifecycle & Undo/Redo)*: `editor_undo`, `editor_redo`, `editor_save_scene`, `editor_reload_project`.
- **Roadmap Specification**: Added `docs/ROADMAP.md` documenting the full 9-domain matrix and architectural vision.
- **Backwards Compatibility**: Preserved all 10 legacy v1.0 names (`capture_viewport`, `get_scene_hierarchy`, etc.) as registered compatibility surface.
- **Enhanced Error Reading**: Structured error capture for GDScript compiler errors, runtime crashes, and engine log buffers.

### Fixed & Hardened
- Restrict named pipe DACL strictly to Owner and Local Administrators (`D:(A;;GA;;;BA)(A;;GA;;;OW)`), removing `WD`.
- Enforce `0600` permissions on POSIX Unix domain sockets.
- Fix recursive mutex deadlock in `PosixIpcClient`.
- Implement non-blocking I/O cancellation (`CancelIoEx` on Win32, `shutdown()` on POSIX) for graceful server shutdown.
- Windows binary stdio mode (`_setmode(_O_BINARY)`) and `cin.gcount()` framing checks.
- Project root path traversal boundary confinement on file modifications.
- Atomic log verbosity level management (`std::atomic<LogLevel>`).

---

## [1.0.0] - 2026-08-26

### Added
- **Unified C++20 Dual Architecture**: Single CMake build producing standalone stdio executable (`didi.exe`) and in-engine GDExtension shared library (`didi_extension.dll`).
- **MCP 2024-11-05 Protocol Support**: Fully compliant JSON-RPC 2.0 transport supporting newline-delimited messages and HTTP-style `Content-Length` headers over `stdin`/`stdout`.
- **Historical v1.0 IPC**: Ultra-low-latency OS Named Pipe transport (`\\.\pipe\godot_didi_ipc` on Windows, UNIX domain sockets on POSIX) with 4-byte little-endian length framing. Phase 3 and Phase 6 later replaced the fixed endpoint name with authenticated, project-keyed, process/session-unique endpoints.
- **10 Domain Tools Across 5 Functional Areas**:
  - *Visual & Vision*: `capture_viewport` (SubViewport off-screen PNG memory blit + RFC 4648 Base64 output), `create_visual_test_lab` (multi-camera sandbox generator).
  - *Scene Tree*: `get_scene_hierarchy` (hierarchical AST parser and live tree reflection), `mutate_scene_tree` (with Godot `EditorUndoRedoManager` transaction safety).
  - *Scripting & Code*: `analyze_script_diagnostics` (GDScript 2.0 static linter + headless compiler validator), `patch_script_symbols` (safe regex-escaped symbol replacer).
  - *Runtime & Debug*: `execute_test_session` (headless engine subprocess runner with timeout enforcement and structured log capture), `inject_input_event`.
  - *Asset Pipeline*: `query_project_resources` (UID & `res://` dependency scanner with deny-list pruning), `instantiate_asset`.
- **Dynamic MCP Resources**: Registered `godot://project/tree`, `godot://editor/state`, and `godot://runtime/logs` URIs.
- **Turnkey Prompt Templates**:
  - `godot_debug_visual_anomaly`: Guided 5-step visual inspection and correction loop.
  - `godot_generate_gameplay_slice`: End-to-end mechanic construction and validation workflow.
- **Offline Fallback Engine**: Enables code diagnostics, asset indexing, scene parsing, and headless test sessions even when the Godot Editor GUI is closed.
- **Security & Safety Hardening**:
  - Restricted SDDL security descriptor for Windows Named Pipes (Current User & Administrators only).
  - GDExtension IPC restricted to `GDEXTENSION_INITIALIZATION_EDITOR` level.
  - Viewport dimension clamping (16x16 to 4096x4096) and 128 MB frame buffer safety limits.
  - Parameterized CLI process arguments preventing shell injection.
  - Async-signal safe shutdown mechanism.
- **Automated Test Suite**: 16 unit and integration tests passing with 100% success rate (`didi_tests.exe`).
- **Comprehensive Documentation Suite**: Architecture guide, tool reference manual, dynamic resources/prompts guide, integration guide, developer guide, API protocol specification, admin guide, and LLM system prompt instructions.
