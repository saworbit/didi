# Changelog

Every notable change to **Didi** (`godot-mcp-native`), newest first.

- **One line per change.** Each starts with the day its pull request merged into
  `main` and ends with the issue it settled and the pull request that made it.
  The reasoning, the evidence and the tests behind a change are in that pull request.
- **Grouped the way [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) groups them.**
  Each release lists Breaking, Added, Changed and Fixed, each running newest first.
  Read Breaking before upgrading a client.
- **Entries merged before 4 October 2026 were full paragraphs.** Every word of them
  is kept in [the changelog as it stood then](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md),
  and each release below links to its own part of it.

Versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html), and
[Stability](#stability) says what a version number does and does not promise.
Historical entries describe the surface advertised by those releases. For the
executable status of each current registration, use
[docs/CAPABILITIES.md](docs/CAPABILITIES.md) or runtime `tools/list` metadata.
[CONTRIBUTING.md](CONTRIBUTING.md#changelog-entries) says how to add an entry.

## Releases at a glance

| Version | Released | What it brought | Added | Changed | Fixed | Breaking | Tools implemented |
| :--- | :--- | :--- | ---: | ---: | ---: | ---: | :--- |
| [Unreleased](#unreleased) | not yet | Everything merged since 2.0.1 |  |  |  |  | 119 of 122 |
| [2.0.1](#201---2026-09-23) | 2026-09-23 | Correctness fixes to 2.0.0, and release archives that stand on their own | 10 | 3 | 182 | 6 | 113 of 116 |
| [2.0.0](#200---2026-09-13) | 2026-09-13 | Error codes, handshake checks and schema strictness corrected across the surface | 4 | 0 | 93 | 10 | 113 of 116 |
| [1.8.0](#180---2026-09-10) | 2026-09-10 | Test and CI reliability, fuzz targets, and faster impact analysis | 4 | 7 | 13 | 0 | 112 of 115 |
| [1.7.0](#170---2026-09-09) | 2026-09-09 | Signed releases, the Control Room and managed editor recovery | 6 | 6 | 14 | 0 | 112 of 115 |
| [1.6.0](#160---2026-09-06) | 2026-09-06 | Editor console, change verification, spatial queries and shader tools | 24 | 0 | 29 | 0 | 106 of 109 |
| [1.5.0](#150---2026-09-03) | 2026-09-03 | Phase 7 tools arrive, the shared blackboard, YOLO mode and MCP 2026-07-28 | 39 | 12 | 18 | 0 | 91 of 94 |
| [1.4.0](#140---2026-08-28) | 2026-08-28 | Phase 4: project search, asset reimport, node isolation and visual diffs | 4 | 6 | 1 | 0 | 54 of 72 |
| [1.3.0](#130---2026-08-27) | 2026-08-27 | Phase 3: sessions, runtime logs, game stepping and expression evaluation | 5 | 8 | 0 | 0 | 50 of 68 |
| [1.2.0](#120---2026-08-27) | 2026-08-27 | The live engine: Phase 1 substrate and Phase 2 project wiring | 6 | 0 | 15 | 0 | 40 of 58 |
| [1.1.0](#110---2026-08-26) | 2026-08-26 | The 40-tool canonical surface and IPC hardening | 4 | 0 | 7 | 0 | 40 named |
| [1.0.0](#100---2026-08-26) | 2026-08-26 | First release: ten tools, the standalone server and the GDExtension | 10 | 0 | 0 | 0 | 10 |

---

## [Unreleased]

The status block below states the current surface rather than anything this
release changed, which is why it lives here and not in a version section.

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `119/122`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Discovery now exposes 122 canonical tools plus 10 legacy registrations (132 total). 119 canonical tools are implemented and 3 remain unimplemented.
The three Phase 7 blockers are unchanged; the newest name is `project_run_tests`, recorded in [Surface Amendments](docs/SURFACE_AMENDMENTS.md).

Full write-ups for these entries: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#unreleased).

### Added

- `2026-10-10` `ui_hit_test` takes a game's window pixels with `space: "screen"`, and a game's hits carry `screen_rect`, so list, hit test and click share one space. [#1223](https://github.com/saworbit/didi/issues/1223) · [PR #1260](https://github.com/saworbit/didi/pull/1260)
- `2026-10-10` `--text-copy once` leaves out the text that repeats `structuredContent`, for a host that reads it and cannot declare `didi/responseEconomy`. [#1238](https://github.com/saworbit/didi/issues/1238) · [PR #1259](https://github.com/saworbit/didi/pull/1259)
- `2026-10-10` `runtime_read_profiler`'s verdict carries `slow_frames`: frames over twice the budget, the worst three each judged on its own parts. [#1230](https://github.com/saworbit/didi/issues/1230) · [PR #1258](https://github.com/saworbit/didi/pull/1258)
- `2026-10-09` `runtime_read_profiler` says whether the frame is bound on the CPU, the GPU or physics, and `self_check` proves it sees a known stall. [#1228](https://github.com/saworbit/didi/issues/1228) · [PR #1229](https://github.com/saworbit/didi/pull/1229)
- `2026-10-09` `runtime_inject_input` takes a `window_focus` event, which runs a game's own focus-loss handlers and says it was synthetic. [#1197](https://github.com/saworbit/didi/issues/1197)
- `2026-10-09` `scene_set_property` and `scene_instantiate_node` write `Dictionary` properties, typed ones included, and an int key reads back as its digits. [#1195](https://github.com/saworbit/didi/issues/1195)
- `2026-10-09` `ui_list_controls` in a game reports `screen_rect`, the window pixels a click is aimed in, which a stretched project scales. [#1189](https://github.com/saworbit/didi/issues/1189)
- `2026-10-09` `runtime_inject_input` takes `paused_delivery: "now"`, which operates a pause menu that processes while paused without stepping the game. [#1191](https://github.com/saworbit/didi/issues/1191)
- `2026-10-09` `runtime_launch` takes `fixed_fps`, and `runtime_step` answers `fixed_fps` and the `physics_ticks` its frames ran. [#1209](https://github.com/saworbit/didi/issues/1209)
- `2026-10-09` `project_run_tests` runs the project's GUT or GdUnit4 tests headless and judges the JUnit report, so a run that tested nothing cannot pass. [#1206](https://github.com/saworbit/didi/issues/1206) · [PR #1212](https://github.com/saworbit/didi/pull/1212)
- `2026-10-08` `runtime_run_scenario` proves a behaviour in one call: it drives its own game frame by frame, checks it, stops it, and records the files a pass was true for. [#1206](https://github.com/saworbit/didi/issues/1206) · [PR #1210](https://github.com/saworbit/didi/pull/1210)
- `2026-10-04` Transforms, rectangles, packed arrays and typed arrays can be read and written as JSON, so none of them is patched into scene text. [#1133](https://github.com/saworbit/didi/issues/1133) · [PR #1182](https://github.com/saworbit/didi/pull/1182)
- `2026-10-04` A `source_text` check beside an open editor asks its language server. [#1142](https://github.com/saworbit/didi/issues/1142) · [PR #1176](https://github.com/saworbit/didi/pull/1176)
- `2026-10-04` `editor_reload_project` runs as a job, so a slow scan can be waited out. [#1157](https://github.com/saworbit/didi/issues/1157) · [PR #1174](https://github.com/saworbit/didi/pull/1174)
- `2026-10-04` Nine more mutating tools say what they replaced. [#1151](https://github.com/saworbit/didi/issues/1151) · [PR #1173](https://github.com/saworbit/didi/pull/1173)
- `2026-10-04` A path write says which other nodes share what it changes, and can write into copies instead. [#1134](https://github.com/saworbit/didi/issues/1134) · [PR #1172](https://github.com/saworbit/didi/pull/1172)
- `2026-10-03` A long export no longer makes every other helper launch wait (Q8 part 3). [#1137](https://github.com/saworbit/didi/issues/1137) · [PR #1158](https://github.com/saworbit/didi/pull/1158)
- `2026-10-03` `asset_reimport` runs as a job, so a scan of many new scripts can be waited out (Q8 part 2). [#996](https://github.com/saworbit/didi/issues/996) · [PR #1156](https://github.com/saworbit/didi/pull/1156)
- `2026-10-03` A change journal, and undo of one change on its own (Q15 part 1). [#1149](https://github.com/saworbit/didi/issues/1149) · [PR #1155](https://github.com/saworbit/didi/pull/1155)
- `2026-10-02` `didi setup` and `didi doctor` (Q12 part 1). [#1144](https://github.com/saworbit/didi/issues/1144) · [PR #1147](https://github.com/saworbit/didi/pull/1147)
- `2026-10-02` Script diagnostics come from the open editor's GDScript language server (Q11 part 1). [#1139](https://github.com/saworbit/didi/issues/1139) · [PR #1140](https://github.com/saworbit/didi/pull/1140)
- `2026-10-02` `project_export` and `csharp_check_build` can run as jobs, and a retried call no longer runs twice (Q8 part 1). [#1137](https://github.com/saworbit/didi/issues/1137) · [PR #1138](https://github.com/saworbit/didi/pull/1138)
- `2026-10-02` A property inside a node's sub-resource can be read and written, several at a time, as one undo step (Q7 part 1). [#1133](https://github.com/saworbit/didi/issues/1133) · [PR #1136](https://github.com/saworbit/didi/pull/1136)
- `2026-10-01` The contract snapshots record four more shapes a client meets. [#1025](https://github.com/saworbit/didi/issues/1025) · [PR #1119](https://github.com/saworbit/didi/pull/1119)
- `2026-10-01` A research report on what Didi costs an agent in tokens. [#1107](https://github.com/saworbit/didi/issues/1107) · [PR #1109](https://github.com/saworbit/didi/pull/1109)
- `2026-10-01` resource_create can name which number it writes. [#1003](https://github.com/saworbit/didi/issues/1003) · [PR #1102](https://github.com/saworbit/didi/pull/1102)
- `2026-10-01` A field trial can hold its findings for review. [#1008](https://github.com/saworbit/didi/issues/1008) · [PR #1101](https://github.com/saworbit/didi/pull/1101)
- `2026-09-29` A mutation that leaves work undone names it as a step (Q6 part 3). [#1040](https://github.com/saworbit/didi/issues/1040) · [PR #1048](https://github.com/saworbit/didi/pull/1048)
- `2026-09-29` No failure answers as a bare sentence (Q6 part 1, finished). [#1040](https://github.com/saworbit/didi/issues/1040) · [PR #1046](https://github.com/saworbit/didi/pull/1046)
- `2026-09-28` The same call failing the same way twice says so (Q6 part 2). [#1040](https://github.com/saworbit/didi/issues/1040) · [PR #1042](https://github.com/saworbit/didi/pull/1042)
- `2026-09-28` Every refusal names what fixes it (Q6 part 1). [#1040](https://github.com/saworbit/didi/issues/1040) · [PR #1041](https://github.com/saworbit/didi/pull/1041)
- `2026-09-28` Every bounded read says whether it is complete, and the two largest take `fields` (Q5 part 2). [#776](https://github.com/saworbit/didi/issues/776) · [PR #1039](https://github.com/saworbit/didi/pull/1039)
- `2026-09-28` `--session-descriptor once` gives the session half of response economy to hosts that cannot declare it. [#1031](https://github.com/saworbit/didi/issues/1031) · [PR #1034](https://github.com/saworbit/didi/pull/1034)
- `2026-09-28` A client that reads `structuredContent` can decline what it already has (Q5 part 1). [#776](https://github.com/saworbit/didi/issues/776) · [PR #1030](https://github.com/saworbit/didi/pull/1030)
- `2026-09-28` `--tools core` lists under half the bytes, and both tool lists have a budget (Q4). [#1012](https://github.com/saworbit/didi/issues/1012) · [PR #1029](https://github.com/saworbit/didi/pull/1029)
- `2026-09-28` What a client is shown is committed, and CI fails when it moves (Q3). [#1011](https://github.com/saworbit/didi/issues/1011) · [PR #1024](https://github.com/saworbit/didi/pull/1024)
- `2026-09-28` Every mutating tool is checked for what it observed (Q2). [#1010](https://github.com/saworbit/didi/issues/1010) · [PR #1021](https://github.com/saworbit/didi/pull/1021)
- `2026-09-27` A field trial is scored on how it called tools, not only which. [PR #1006](https://github.com/saworbit/didi/pull/1006)
- `2026-09-26` Operational guidance in the MCP handshake. [PR #962](https://github.com/saworbit/didi/pull/962)
- `2026-09-26` End-to-end protocol and normalization coverage. [PR #962](https://github.com/saworbit/didi/pull/962)
- `2026-09-25` `asset_configure_import` makes a track loop. [#958](https://github.com/saworbit/didi/issues/958) · [PR #963](https://github.com/saworbit/didi/pull/963)
- `2026-09-25` Experimental argument normalization, disabled by default. [PR #960](https://github.com/saworbit/didi/pull/960)
- `2026-09-24` `audio_add_bus` gives a game its Music and SFX buses. [#771](https://github.com/saworbit/didi/issues/771) · [PR #942](https://github.com/saworbit/didi/pull/942)
- `2026-09-24` `project_add_export_preset` makes a game shippable through the surface. [#779](https://github.com/saworbit/didi/issues/779) · [PR #926](https://github.com/saworbit/didi/pull/926)
- `2026-09-24` Every live answer carries what the engine printed while it ran. [PR #915](https://github.com/saworbit/didi/pull/915)
- `2026-09-24` The live harness reads the engine's own log. [PR #915](https://github.com/saworbit/didi/pull/915)
- `2026-09-24` `anim_add_library` gives an AnimationPlayer an animation. [#770](https://github.com/saworbit/didi/issues/770) · [PR #912](https://github.com/saworbit/didi/pull/912)
- `2026-09-23` A release archive can be installed and checked in one command. [PR #911](https://github.com/saworbit/didi/pull/911)

### Changed

- `2026-10-11` The architecture guide lists never calling into a freed object as a bridge guarantee, and the developer guide says how a bridge error keeps its data. [#1266](https://github.com/saworbit/didi/issues/1266) · [PR #1272](https://github.com/saworbit/didi/pull/1272)
- `2026-10-10` The live harness fails, naming the node, when a fixture scene holds a freed object after a scene change. [#1268](https://github.com/saworbit/didi/issues/1268) · [PR #1270](https://github.com/saworbit/didi/pull/1270)
- `2026-10-10` A bridge refusal rebuilt from a helper's error keeps its `data`, and the refusal scan fails a rebuild that drops it. [#1267](https://github.com/saworbit/didi/issues/1267) · [PR #1270](https://github.com/saworbit/didi/pull/1270)
- `2026-10-10` The MCP preflight and the game bridge check a runtime `root_path` with one rule. [#1254](https://github.com/saworbit/didi/issues/1254) · [PR #1270](https://github.com/saworbit/didi/pull/1270)
- `2026-10-09` The sanitizer job has 45 minutes, the build matrix's limit, because a cold build after a header change took its whole 30. [#1224](https://github.com/saworbit/didi/issues/1224)
- `2026-10-05` A release's notes are its changelog section, line for line, instead of GitHub's list of pull request titles. [PR #1199](https://github.com/saworbit/didi/pull/1199)
- `2026-10-05` The tool reference says when `resource_create` and `project_apply_changes` answer `editor_index_pending`, as it already did for `script_create`. [#1177](https://github.com/saworbit/didi/issues/1177) · [PR #1192](https://github.com/saworbit/didi/pull/1192)
- `2026-10-05` The live harness compares `project_rename_references`' `updated_files` with the scene file it rewrote, so ten mutating tools are left unchecked, not eleven. [#1020](https://github.com/saworbit/didi/issues/1020) · [PR #1187](https://github.com/saworbit/didi/pull/1187)
- `2026-10-04` The changelog is one dated line per change, newest first, with a table of every release at the top. [PR #1178](https://github.com/saworbit/didi/pull/1178)
- `2026-10-01` project_list_input_actions lists the project's own actions by default. [#1108](https://github.com/saworbit/didi/issues/1108) · [PR #1112](https://github.com/saworbit/didi/pull/1112)
- `2026-10-01` The argument validation paragraphs sit with the schema rules. [#1037](https://github.com/saworbit/didi/issues/1037) · [PR #1101](https://github.com/saworbit/didi/pull/1101)
- `2026-09-27` The roadmap says what to build next, and why. [PR #1013](https://github.com/saworbit/didi/pull/1013)
- `2026-09-26` The roadmap and the amendment log describe the surface as it stands. [#990](https://github.com/saworbit/didi/issues/990) · [PR #991](https://github.com/saworbit/didi/pull/991)
- `2026-09-26` The vendored JSON parser is nlohmann/json 3.12.0. [#796](https://github.com/saworbit/didi/issues/796) · [PR #985](https://github.com/saworbit/didi/pull/985)
- `2026-09-25` The README and the website list every tool. [PR #965](https://github.com/saworbit/didi/pull/965)
- `2026-09-23` The website and the quickstart start from the download. [PR #910](https://github.com/saworbit/didi/pull/910)

### Fixed

- `2026-10-10` A property holding a freed node reads as `null` with `freed: true`, and nothing calls into the freed object. [#1266](https://github.com/saworbit/didi/issues/1266) · [PR #1270](https://github.com/saworbit/didi/pull/1270)
- `2026-10-10` The developer guide and build queue name the new test-run, profiler and text-copy pieces, and the rule a harness fixture's `@tool` script keeps. [#1227](https://github.com/saworbit/didi/issues/1227) · [PR #1265](https://github.com/saworbit/didi/pull/1265)
- `2026-10-10` The harness fixture's editor probe no longer leaves a freed node in its metadata, which crashed the 4.5.1 editor on a tab switch. [#1227](https://github.com/saworbit/didi/issues/1227) · [PR #1264](https://github.com/saworbit/didi/pull/1264)
- `2026-10-10` `scene_call_method` takes a call that leaves out parameters with defaults, and runs it with them. [#1251](https://github.com/saworbit/didi/issues/1251) · [PR #1263](https://github.com/saworbit/didi/pull/1263)
- `2026-10-10` `editor_reload_project` without `request_id` drops the server's cached resource index, as the job and offline paths do. [#1250](https://github.com/saworbit/didi/issues/1250) · [PR #1263](https://github.com/saworbit/didi/pull/1263)
- `2026-10-10` An untyped Dictionary's `int` keys stay `int` when a read is written back, so `table[1]` still finds its value. [#1249](https://github.com/saworbit/didi/issues/1249) · [PR #1263](https://github.com/saworbit/didi/pull/1263)
- `2026-10-10` A test report over 16 MiB is refused after reading 16 MiB of it, not after reading all of it. [#1247](https://github.com/saworbit/didi/issues/1247) · [PR #1262](https://github.com/saworbit/didi/pull/1262)
- `2026-10-10` A GUT run records `.gutconfig.json` and the tests it lists, so its pass goes stale when one of them changes. [#1246](https://github.com/saworbit/didi/issues/1246) · [PR #1262](https://github.com/saworbit/didi/pull/1262)
- `2026-10-10` A `make_unique` write whose setter refuses the copy no longer changes the resource it shares, and the answer says the node kept its own. [#1245](https://github.com/saworbit/didi/issues/1245) · [PR #1261](https://github.com/saworbit/didi/pull/1261)
- `2026-10-10` TOOL_REFERENCE gives the answer a profiler read gets when its game exits mid-window: `502` `live_session_ended`, not `504`. [#1230](https://github.com/saworbit/didi/issues/1230) · [PR #1258](https://github.com/saworbit/didi/pull/1258)
- `2026-10-10` 26 bridge refusals that reached clients as a bare `conflict` carry a `data.code` and its fix, and a codeless `Error(4xx)` fails the refusal scan. [#1222](https://github.com/saworbit/didi/issues/1222) · [PR #1253](https://github.com/saworbit/didi/pull/1253)
- `2026-10-10` A `project_run_tests` run whose output passed its 1 MiB cap answers `error` `output_truncated`, not `pass`: a load error could be in what was dropped. [#1243](https://github.com/saworbit/didi/issues/1243) · [PR #1248](https://github.com/saworbit/didi/pull/1248)
- `2026-10-10` Two `project_run_tests` runs of one name at once each read their own report, in a directory of the run's own. [#1242](https://github.com/saworbit/didi/issues/1242) · [PR #1248](https://github.com/saworbit/didi/pull/1248)
- `2026-10-10` A scenario's `timeout_seconds` and job cancel stop a `wait_until` or `wait` already running, and the run answers `error`, not `pass`. [#1241](https://github.com/saworbit/didi/issues/1241) · [PR #1244](https://github.com/saworbit/didi/pull/1244)
- `2026-10-09` A scenario's game starts paused before its first physics frame, and `game.frames_before_pause` says how many ran. [#1208](https://github.com/saworbit/didi/issues/1208)
- `2026-10-09` Six `scene_call_method` refusals carry codes of their own, and a codeless `fail(4xx)` in the bridge fails the refusal scan. [#1196](https://github.com/saworbit/didi/issues/1196)
- `2026-10-09` `script_attach_to_node` on a node that holds a script answers `script_already_attached`, naming that script and `script_detach_from_node`. [#1193](https://github.com/saworbit/didi/issues/1193)
- `2026-10-09` `script_check_syntax` accepts inline suites such as `if ready: start()`, and a lexical error beside a clean engine verdict is a warning, not an error. [#1188](https://github.com/saworbit/didi/issues/1188)
- `2026-10-09` SECURITY.md records the re-read behind the second re-raise of the two process alerts. [#1201](https://github.com/saworbit/didi/issues/1201)
- `2026-10-09` The Quickstart, admin and integration guides say `didi setup` and `didi doctor` are not in the 2.0.1 archive, and how to set up with it. [#1190](https://github.com/saworbit/didi/issues/1190)
- `2026-10-09` The tool manifest publishes the `openWorldHint` and job tools, and the docs validator fails a list of either that misses one. [#1215](https://github.com/saworbit/didi/issues/1215)
- `2026-10-09` A file replace that fails names the reason the system gave, and `project_settings_file.concurrent_writers` names each write that failed. [#1214](https://github.com/saworbit/didi/issues/1214)
- `2026-10-09` `tools/test_inventory.py --test-binary` wins over `DIDI_TEST_BINARY`, and a binary with no test registry is refused with the override named. [#1194](https://github.com/saworbit/didi/issues/1194)
- `2026-10-05` `didi setup` keeps a table a person adds under Codex's `mcp_servers.didi`, such as its `env`, instead of refusing the file. [#1144](https://github.com/saworbit/didi/issues/1144) · [PR #1186](https://github.com/saworbit/didi/pull/1186)
- `2026-10-05` Eight refusals name the fix that applies instead of another, and the route failure and session-kind refusals read as proper sentences. [#1184](https://github.com/saworbit/didi/issues/1184) · [PR #1185](https://github.com/saworbit/didi/pull/1185)
- `2026-10-04` A 422 from `script_attach_to_node` or `scene_call_method` names its own fix, not `export_presets.cfg`. [#1181](https://github.com/saworbit/didi/issues/1181) · [PR #1183](https://github.com/saworbit/didi/pull/1183)
- `2026-10-04` A Godot that cannot be run is reported at once on Linux and macOS, and CI runs the live `didi setup` test on each engine line. [#1144](https://github.com/saworbit/didi/issues/1144) · [PR #1180](https://github.com/saworbit/didi/pull/1180)
- `2026-10-04` A `class_name` written into a new folder is known to the next check, because the writer waits for the editor to list it. [#1177](https://github.com/saworbit/didi/issues/1177) · [PR #1179](https://github.com/saworbit/didi/pull/1179)
- `2026-10-04` A rolled-back write no longer steps the scene's history behind the editor (part 2). [#1152](https://github.com/saworbit/didi/issues/1152) · [PR #1175](https://github.com/saworbit/didi/pull/1175)
- `2026-10-04` Every Godot object Didi constructs is finished or not handed back. [#1166](https://github.com/saworbit/didi/issues/1166) · [PR #1171](https://github.com/saworbit/didi/pull/1171)
- `2026-10-04` A failed undo registration no longer leaves the editor mid-action (part 1). [#1152](https://github.com/saworbit/didi/issues/1152) · [PR #1171](https://github.com/saworbit/didi/pull/1171)
- `2026-10-04` A detached runtime_launch no longer takes another project's game. [#1167](https://github.com/saworbit/didi/issues/1167) · [PR #1170](https://github.com/saworbit/didi/pull/1170)
- `2026-10-04` A helper process can no longer hang between fork and exec. [#1168](https://github.com/saworbit/didi/issues/1168) · [PR #1170](https://github.com/saworbit/didi/pull/1170)
- `2026-10-04` Running jobs are cancelled and joined when the server stops. [#1169](https://github.com/saworbit/didi/issues/1169) · [PR #1170](https://github.com/saworbit/didi/pull/1170)
- `2026-10-04` The visual test lab no longer carries a uid Godot cannot read. [#1164](https://github.com/saworbit/didi/issues/1164) · [PR #1165](https://github.com/saworbit/didi/pull/1165)
- `2026-10-04` resource_create writes a class_name sub-resource the way Godot does. [#1131](https://github.com/saworbit/didi/issues/1131) · [PR #1165](https://github.com/saworbit/didi/pull/1165)
- `2026-10-04` The harness holds every writer to the editor's index. [#1154](https://github.com/saworbit/didi/issues/1154) · [PR #1165](https://github.com/saworbit/didi/pull/1165)
- `2026-10-04` No helper launch can forget to suppress its session. [#1161](https://github.com/saworbit/didi/issues/1161) · [PR #1163](https://github.com/saworbit/didi/pull/1163)
- `2026-10-04` The offline contract snapshot no longer fails beside an open editor. [#1145](https://github.com/saworbit/didi/issues/1145) · [PR #1163](https://github.com/saworbit/didi/pull/1163)
- `2026-10-04` A project.godot the editor cannot write is the caller's to fix, not a server fault. [#1153](https://github.com/saworbit/didi/issues/1153) · [PR #1162](https://github.com/saworbit/didi/pull/1162)
- `2026-10-04` A synchronous `asset_reimport` timeout no longer says to retry in five seconds. [#1159](https://github.com/saworbit/didi/issues/1159) · [PR #1162](https://github.com/saworbit/didi/pull/1162)
- `2026-10-03` A script Didi creates no longer draws `Unrecognized UID` on Godot 4.5 and 4.6. [PR #1150](https://github.com/saworbit/didi/pull/1150)
- `2026-10-03` A refused project settings save says the rollback worked, and the engine says the save failed once. [PR #1150](https://github.com/saworbit/didi/pull/1150)
- `2026-10-03` The harness editor says its red lines are meant. [PR #1150](https://github.com/saworbit/didi/pull/1150)
- `2026-10-01` project_export and gridmap_export_mesh_library are checked against the engine. [#1020](https://github.com/saworbit/didi/issues/1020) · [PR #1129](https://github.com/saworbit/didi/pull/1129)
- `2026-10-01` resource_create writes a class_name type the way Godot does. [#1125](https://github.com/saworbit/didi/issues/1125) · [PR #1128](https://github.com/saworbit/didi/pull/1128)
- `2026-10-01` scene_reparent_node gives a clashing node a readable name and says so. [#1126](https://github.com/saworbit/didi/issues/1126) · [PR #1128](https://github.com/saworbit/didi/pull/1128)
- `2026-10-01` scene_call_method and signal_emit are exempt for good from the observed check. [#1020](https://github.com/saworbit/didi/issues/1020) · [PR #1127](https://github.com/saworbit/didi/pull/1127)
- `2026-10-01` The observed post-state check sends inputs the engine stores differently. [#1022](https://github.com/saworbit/didi/issues/1022) · [PR #1127](https://github.com/saworbit/didi/pull/1127)
- `2026-10-01` asset_reimport waits for a scan the editor started to be applied. [#995](https://github.com/saworbit/didi/issues/995) · [PR #1124](https://github.com/saworbit/didi/pull/1124)
- `2026-10-01` deferred_scene_uid.py creates scenes during a scan again. [#1122](https://github.com/saworbit/didi/issues/1122) · [PR #1124](https://github.com/saworbit/didi/pull/1124)
- `2026-10-01` The Phase 7 success contracts are checked against real answers. [#861](https://github.com/saworbit/didi/issues/861) · [PR #1120](https://github.com/saworbit/didi/pull/1120)
- `2026-10-01` project_list_resources answers in path order on every platform. [#1121](https://github.com/saworbit/didi/issues/1121) · [PR #1119](https://github.com/saworbit/didi/pull/1119)
- `2026-10-01` A not_found names where the missing thing is listed. [#1117](https://github.com/saworbit/didi/issues/1117) · [PR #1118](https://github.com/saworbit/didi/pull/1118)
- `2026-10-01` editor_reload_project answers once the editor has applied a full scan. [#1114](https://github.com/saworbit/didi/issues/1114) · [PR #1116](https://github.com/saworbit/didi/pull/1116)
- `2026-10-01` A field trial's brief names the checkout and Godot it was given. [#1105](https://github.com/saworbit/didi/issues/1105) · [PR #1113](https://github.com/saworbit/didi/pull/1113)
- `2026-10-01` runtime_inject_input answers with what Input holds. [#1019](https://github.com/saworbit/didi/issues/1019) · [PR #1111](https://github.com/saworbit/didi/pull/1111)
- `2026-10-01` A new scene's uid is indexed before scene_create answers. [#1004](https://github.com/saworbit/didi/issues/1004) · [#995](https://github.com/saworbit/didi/issues/995) · [PR #1110](https://github.com/saworbit/didi/pull/1110)
- `2026-10-01` scene_close answers with the tabs it left. [#1019](https://github.com/saworbit/didi/issues/1019) · [PR #1104](https://github.com/saworbit/didi/pull/1104)
- `2026-10-01` The project writers answer with what project.godot holds after the save. [#1019](https://github.com/saworbit/didi/issues/1019) · [PR #1103](https://github.com/saworbit/didi/pull/1103)
- `2026-10-01` resource_create answers with the file it wrote. [#1019](https://github.com/saworbit/didi/issues/1019) · [PR #1102](https://github.com/saworbit/didi/pull/1102)
- `2026-10-01` An empty answer object no longer agrees with the engine in the observed post-state check. [#1097](https://github.com/saworbit/didi/issues/1097) · [PR #1100](https://github.com/saworbit/didi/pull/1100)
- `2026-09-30` The harness recognizes the [#285](https://github.com/saworbit/didi/issues/285) engine crash on Godot 4.6.2 too. [#1098](https://github.com/saworbit/didi/issues/1098) · [PR #1099](https://github.com/saworbit/didi/pull/1099)
- `2026-09-30` The editor startup test retries a held open and names the engine crash it can meet. [#1069](https://github.com/saworbit/didi/issues/1069) · [#285](https://github.com/saworbit/didi/issues/285) · [PR #1094](https://github.com/saworbit/didi/pull/1094)
- `2026-09-30` `tilemap_set_cells` and `gridmap_set_cells` answer with the cells they read back. [#1019](https://github.com/saworbit/didi/issues/1019) · [PR #1093](https://github.com/saworbit/didi/pull/1093)
- `2026-09-30` `editor_save_scene` reads the file it saved. [#1019](https://github.com/saworbit/didi/issues/1019) · [PR #1092](https://github.com/saworbit/didi/pull/1092)
- `2026-09-30` An offline `project_set_setting` says when text reads as another type. [#1016](https://github.com/saworbit/didi/issues/1016) · [PR #1091](https://github.com/saworbit/didi/pull/1091)
- `2026-09-30` The local CI lanes run from a git worktree. [#1070](https://github.com/saworbit/didi/issues/1070) · [PR #1090](https://github.com/saworbit/didi/pull/1090)
- `2026-09-30` Undo, redo and project code say whether they left the scene unsaved. [#1049](https://github.com/saworbit/didi/issues/1049) · [PR #1088](https://github.com/saworbit/didi/pull/1088)
- `2026-09-30` A writer that rebuilds an open tab says whether the tab had unsaved changes. [#1082](https://github.com/saworbit/didi/issues/1082) · [PR #1087](https://github.com/saworbit/didi/pull/1087)
- `2026-09-30` `runtime_launch` with a Godot that cannot be started says so. [#1076](https://github.com/saworbit/didi/issues/1076) · [PR #1086](https://github.com/saworbit/didi/pull/1086)
- `2026-09-30` `csharp_check_build` no longer calls a slow .NET SDK missing. [#1078](https://github.com/saworbit/didi/issues/1078) · [PR #1085](https://github.com/saworbit/didi/pull/1085)
- `2026-09-30` `scene_create` over a scene open in another tab opens it on Godot 4.5 and 4.6. [#1079](https://github.com/saworbit/didi/issues/1079) · [#1073](https://github.com/saworbit/didi/issues/1073) · [PR #1084](https://github.com/saworbit/didi/pull/1084)
- `2026-09-30` A scene opened straight after an editor starts stays the edited scene. [#1069](https://github.com/saworbit/didi/issues/1069) · [PR #1083](https://github.com/saworbit/didi/pull/1083)
- `2026-09-30` A pack over a scene open in another tab no longer comes undone on the next save. [#1072](https://github.com/saworbit/didi/issues/1072) · [PR #1080](https://github.com/saworbit/didi/pull/1080)
- `2026-09-30` A write to a scene open in the editor no longer comes undone on the next save. [#1068](https://github.com/saworbit/didi/issues/1068) · [PR #1071](https://github.com/saworbit/didi/pull/1071)
- `2026-09-30` Removing and moving a node answer with what the tree now holds (in part). [#1019](https://github.com/saworbit/didi/issues/1019) · [PR #1066](https://github.com/saworbit/didi/pull/1066)
- `2026-09-30` Group edits answer with the membership they left (in part). [#1019](https://github.com/saworbit/didi/issues/1019) · [PR #1065](https://github.com/saworbit/didi/pull/1065)
- `2026-09-29` The local clang lane builds main again. [#1026](https://github.com/saworbit/didi/issues/1026) · [PR #1064](https://github.com/saworbit/didi/pull/1064)
- `2026-09-29` The live harness copies only the fixture it tracks. [#1036](https://github.com/saworbit/didi/issues/1036) · [PR #1063](https://github.com/saworbit/didi/pull/1063)
- `2026-09-29` The seven blackboard writers answer with what they saved (in part). [#1019](https://github.com/saworbit/didi/issues/1019) · [PR #1062](https://github.com/saworbit/didi/pull/1062)
- `2026-09-29` The local asan lane is no longer killed for memory. [#1050](https://github.com/saworbit/didi/issues/1050) · [PR #1061](https://github.com/saworbit/didi/pull/1061)
- `2026-09-29` A field trial keeps its evidence and pins its tester. [#1005](https://github.com/saworbit/didi/issues/1005) · [#1007](https://github.com/saworbit/didi/issues/1007) · [PR #1060](https://github.com/saworbit/didi/pull/1060)
- `2026-09-29` Engine output and helper answers carry text, not terminal noise. [#1028](https://github.com/saworbit/didi/issues/1028) · [PR #1059](https://github.com/saworbit/didi/pull/1059)
- `2026-09-29` The advice for a new autoload no longer offers a rescan that does not work. [#1002](https://github.com/saworbit/didi/issues/1002) · [PR #1058](https://github.com/saworbit/didi/pull/1058)
- `2026-09-29` Two failures that skipped the error floor now name their fix. [#1043](https://github.com/saworbit/didi/issues/1043) · [PR #1057](https://github.com/saworbit/didi/pull/1057)
- `2026-09-29` A harness run that fails reports the failure, not the teardown guard. [#1044](https://github.com/saworbit/didi/issues/1044) · [PR #1056](https://github.com/saworbit/didi/pull/1056)
- `2026-09-29` A Godot that will not start reads as engine_unavailable, not a server fault. [#1045](https://github.com/saworbit/didi/issues/1045) · [PR #1055](https://github.com/saworbit/didi/pull/1055)
- `2026-09-29` A command no longer runs inside the editor's own scan work (part one). [#995](https://github.com/saworbit/didi/issues/995) · [PR #1054](https://github.com/saworbit/didi/pull/1054)
- `2026-09-29` Six file writers no longer leave an attached editor holding the old copy. [#1047](https://github.com/saworbit/didi/issues/1047) · [PR #1053](https://github.com/saworbit/didi/pull/1053)
- `2026-09-29` A stopped game no longer counts against the eight held sessions. [#1001](https://github.com/saworbit/didi/issues/1001) · [PR #1052](https://github.com/saworbit/didi/pull/1052)
- `2026-09-28` `project_verify_changes` could pass a proposal whose errors it never read. [PR #1039](https://github.com/saworbit/didi/pull/1039)
- `2026-09-28` Seventeen reads that could be cut short said so partly or not at all (Q5). [PR #1039](https://github.com/saworbit/didi/pull/1039)
- `2026-09-27` The phase design no longer calls Phase 13 planned while the roadmap says it is in progress. [PR #1018](https://github.com/saworbit/didi/pull/1018)
- `2026-09-27` Every argument declares a JSON type, so a Claude host can send an int, a bool or an array. [#1000](https://github.com/saworbit/didi/issues/1000) · [PR #1015](https://github.com/saworbit/didi/pull/1015)
- `2026-09-26` `asset_reimport` reimports an asset it batches with a new file. [PR #994](https://github.com/saworbit/didi/pull/994)
- `2026-09-26` `project_set_setting` checks the files an array names. [#989](https://github.com/saworbit/didi/issues/989) · [PR #993](https://github.com/saworbit/didi/pull/993)
- `2026-09-26` `ui_list_controls` reports what a translated control draws. [#988](https://github.com/saworbit/didi/issues/988) · [PR #992](https://github.com/saworbit/didi/pull/992)
- `2026-09-26` A refusal for an argument under the wrong name carries the fix. [#784](https://github.com/saworbit/didi/issues/784) · [PR #986](https://github.com/saworbit/didi/pull/986)
- `2026-09-26` `project_audit_assets` reports a scene connection to a method nothing declares. [#781](https://github.com/saworbit/didi/issues/781) · [PR #984](https://github.com/saworbit/didi/pull/984)
- `2026-09-26` `project_list_input_actions` can ask for less. [#775](https://github.com/saworbit/didi/issues/775) · [PR #983](https://github.com/saworbit/didi/pull/983)
- `2026-09-26` An open code scanning alert is reported. [#807](https://github.com/saworbit/didi/issues/807) · [PR #982](https://github.com/saworbit/didi/pull/982)
- `2026-09-26` `emitter_node` names the emitter on every signal tool. [#769](https://github.com/saworbit/didi/issues/769) · [PR #981](https://github.com/saworbit/didi/pull/981)
- `2026-09-26` The Python suites that drive the server now run on the Windows CI leg. [PR #980](https://github.com/saworbit/didi/pull/980)
- `2026-09-26` `DIDI_TEST_BINARY` pointed at `didi_tests` is refused by name. [#846](https://github.com/saworbit/didi/issues/846) · [PR #980](https://github.com/saworbit/didi/pull/980)
- `2026-09-26` The end to end MCP check runs before a push. [#845](https://github.com/saworbit/didi/issues/845) · [PR #980](https://github.com/saworbit/didi/pull/980)
- `2026-09-26` A timed-out test session reports `tree_exited` again when one of its processes had already finished. [#859](https://github.com/saworbit/didi/issues/859) · [PR #979](https://github.com/saworbit/didi/pull/979)
- `2026-09-26` Recovery test imports and stdio fixture cleanup. [PR #962](https://github.com/saworbit/didi/pull/962)
- `2026-09-25` Three more refusals carry the argument that fixes them in `retry_with`. [#902](https://github.com/saworbit/didi/issues/902) · [PR #977](https://github.com/saworbit/didi/pull/977)
- `2026-09-25` `audio_list_buses` reads a quoted or boolean `volume_db` the way Godot does. [#907](https://github.com/saworbit/didi/issues/907) · [PR #976](https://github.com/saworbit/didi/pull/976)
- `2026-09-25` `project_add_export_preset` says when the preset's export folder does not exist. [#932](https://github.com/saworbit/didi/issues/932) · [PR #975](https://github.com/saworbit/didi/pull/975)
- `2026-09-25` `audio_add_bus` refuses a bad argument before it asks for an editor. [#949](https://github.com/saworbit/didi/issues/949) · [PR #974](https://github.com/saworbit/didi/pull/974)
- `2026-09-25` `project_export` and `gridmap_export_mesh_library` refuse control characters in `output_path`. [#939](https://github.com/saworbit/didi/issues/939) · [PR #973](https://github.com/saworbit/didi/pull/973)
- `2026-09-25` `project_list_export_presets` reads the runnable preset a Godot 4.7 editor saved. [#922](https://github.com/saworbit/didi/issues/922) · [PR #972](https://github.com/saworbit/didi/pull/972)
- `2026-09-25` `runtime_*` control tools and the live runtime-log resource share one deadline rule. [#856](https://github.com/saworbit/didi/issues/856) · [PR #971](https://github.com/saworbit/didi/pull/971)
- `2026-09-25` A string sent to the engine keeps a leading byte-order mark, and one holding a NUL is refused. [#948](https://github.com/saworbit/didi/issues/948) · [PR #970](https://github.com/saworbit/didi/pull/970)
- `2026-09-25` A checkpoint or a file write no longer fails on Windows because something held a file for a moment. [#937](https://github.com/saworbit/didi/issues/937) · [PR #969](https://github.com/saworbit/didi/pull/969)
- `2026-09-25` Two `script_patch_method` calls on one script no longer lose a method. [#954](https://github.com/saworbit/didi/issues/954) · [PR #968](https://github.com/saworbit/didi/pull/968)
- `2026-09-25` A board subscribed while the watcher already ran is no longer announced as changed. [#139](https://github.com/saworbit/didi/issues/139) · [PR #967](https://github.com/saworbit/didi/pull/967)
- `2026-09-25` `editor_undo` and `editor_redo` no longer leave the editor's history inconsistent. [#913](https://github.com/saworbit/didi/issues/913) · [PR #966](https://github.com/saworbit/didi/pull/966)
- `2026-09-25` Windows test-session timeout completion. [PR #960](https://github.com/saworbit/didi/pull/960)
- `2026-09-25` `asset_reimport` no longer answers, or starts, inside the editor's own import pass. [#914](https://github.com/saworbit/didi/issues/914) · [PR #957](https://github.com/saworbit/didi/pull/957)
- `2026-09-24` Two servers writing one project file could lose an update. [#929](https://github.com/saworbit/didi/issues/929) · [PR #953](https://github.com/saworbit/didi/pull/953)
- `2026-09-24` A blackboard resource could say a board did not exist while showing its state. [#514](https://github.com/saworbit/didi/issues/514) · [PR #953](https://github.com/saworbit/didi/pull/953)
- `2026-09-24` Three refusals said something other than what they meant. [PR #947](https://github.com/saworbit/didi/pull/947)
- `2026-09-24` `scene_instantiate_node` did not say when an initial property failed to land. [PR #946](https://github.com/saworbit/didi/pull/946)
- `2026-09-24` `audio_list_buses` could not ask a running game about its own mix. [PR #945](https://github.com/saworbit/didi/pull/945)
- `2026-09-24` A bus layout the editor could not write was reported as one it would. [PR #944](https://github.com/saworbit/didi/pull/944)
- `2026-09-24` Four handlers counted a published length bound in bytes. [PR #943](https://github.com/saworbit/didi/pull/943)
- `2026-09-24` The bus layout setting was followed only when it was a `res://` path. [#935](https://github.com/saworbit/didi/issues/935) · [PR #942](https://github.com/saworbit/didi/pull/942)
- `2026-09-24` A bus name with a quote, a backslash or a control character came back with Godot's escapes still in it. [#934](https://github.com/saworbit/didi/issues/934) · [PR #941](https://github.com/saworbit/didi/pull/941)
- `2026-09-24` `user://` and `RES://` were read as folders inside the project. [PR #931](https://github.com/saworbit/didi/pull/931)
- `2026-09-24` `runtime_inject_input` reported an action the game never declared as pressed. [PR #930](https://github.com/saworbit/didi/pull/930)
- `2026-09-24` `project_export` lost a preset name with a space at either end. [PR #928](https://github.com/saworbit/didi/pull/928)
- `2026-09-24` A release build with no export templates was a `500 internal_error`. [PR #928](https://github.com/saworbit/didi/pull/928)
- `2026-09-24` `project_add_export_preset`'s `did_you_mean` never reached a caller. [PR #928](https://github.com/saworbit/didi/pull/928)
- `2026-09-24` A presets file with a byte-order mark was blamed on a key nobody could see. [PR #928](https://github.com/saworbit/didi/pull/928)
- `2026-09-24` `project_add_export_preset` stored a directory as `export_path`. [PR #928](https://github.com/saworbit/didi/pull/928)
- `2026-09-24` Writing an input action broke navigation in the editor's 3D view. [#925](https://github.com/saworbit/didi/issues/925) · [PR #927](https://github.com/saworbit/didi/pull/927)
- `2026-09-24` `project_list_export_presets` listed presets Godot never loads, and `project_export` tried to export them. [#921](https://github.com/saworbit/didi/issues/921) · [PR #924](https://github.com/saworbit/didi/pull/924)
- `2026-09-24` Upgrading the addon printed ten "Missing .uid file" warnings. [PR #917](https://github.com/saworbit/didi/pull/917)
- `2026-09-24` `resource_create` wrote `.res` files Godot cannot load. [PR #916](https://github.com/saworbit/didi/pull/916)
- `2026-09-24` An overwrite left the editor answering from its old copy. [PR #916](https://github.com/saworbit/didi/pull/916)
- `2026-09-24` `asset_reimport` reported a failed import as imported. [PR #915](https://github.com/saworbit/didi/pull/915)
- `2026-09-24` Refusals no longer let the engine print first. [PR #915](https://github.com/saworbit/didi/pull/915)

---

## [2.0.1] - 2026-09-23

Correctness work on 2.0.0, and release archives that stand on their own. Most
of what follows fixes answers that were wrong. Some of those fixes change what
a client sees: two renamed result fields, one field whose type changed, a patch
tool that now refuses instead of appending, new refusals for input that used to
be accepted, and refusals that now carry a different status or `data.code`.
They are listed under Breaking below. Read that list before upgrading a client
that branches on any of them. No tool or argument was removed or renamed, and
about fifty optional arguments and result fields are new.

Each archive now carries a README written for the download, the third-party
notices for the code compiled into it, and on Windows a build that needs no
Visual C++ Redistributable.

Full write-ups for 2.0.1: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#201---2026-09-23).

### Breaking

- **Two result fields are renamed.** `runtime_read_logs` and
  `runtime_read_output` report `sequence_overflowed` where they reported
  `exhausted`, and every page carries `has_more` (#598). The offline
  `scene_get_hierarchy` names an instanced scene in `instance_of` where it used
  `instance`, so both routes now use the same name (#591).
- **`mutation_preview.changes[].target` names what the change is about**, such
  as a path and a symbol, instead of repeating the arguments object. The
  arguments are still in `mutation_preview.arguments` (#574).
- **`script_patch_method` refuses a symbol the script does not declare** with
  `404`, where it appended one and reported the same success as a replacement.
  `create_if_missing: true` keeps the append (#569). `symbol_type` accepts only
  `function`, `variable`, `constant`, `signal`, `enum` and `class`, so a value
  such as `method` that 2.0.0 let through is refused (#570).
- **Status codes that changed.**
  - A failure the engine itself reports is `502` with `data.code:
    "engine_refused"`. It was `503` with `not_connected`, which read as a lost
    session (#625).
  - The managed-recovery tools on a server started without `--managed-editor`
    answer `409`. They answered `501` (#599).
  - A capture from a headless editor or game answers `409`. It answered `404`
    (#676, #777).
  - `signal_connect` to a handler on a script that did not compile answers
    `409`. It answered `404` (#729).
  - A script this process may not read answers `403` with `code: "forbidden"`.
    It answered `400`, or a success carrying a diagnostic (#653).
  - A file whose name JSON cannot carry answers `500`. It answered `400`
    (#650).
- **`data.code` values that changed.** A colliding output says
  `already_exists` rather than `conflict`, and names the argument to resend
  under `retry_with`. The bridge's 409 and 422 refusals say which conflict they
  are rather than `conflict` or `unprocessable`. A command cancelled before it
  ran says `command_cancelled` rather than the 504 floor's `timeout`, and a
  command cut off by an ended session says `live_session_ended` with
  `retryable: false` (#862, #865, #867, #890, #892).
- **New refusals for input that used to be accepted.** A second `initialize`
  (`-32600`, #552). An empty string for a required string parameter (#553,
  #554). Exploring a paused game (`409`). A shader uniform value outside its
  declared `hint_range` (#620). An edit to a node the edited scene does not
  own, to an inherited node, or an instance of the scene inside itself (`409`,
  #588, #589, #590). `script_get_symbols` returns at most 2000 symbols unless
  `max_symbols` says otherwise, and says when it stopped.

### Added

- `2026-09-23` The fuzz target lists cannot drift apart. [#884](https://github.com/saworbit/didi/issues/884) · [PR #889](https://github.com/saworbit/didi/pull/889)
- `2026-09-20` `project_audit_assets` reports what is wrong with `project.godot` itself. [#818](https://github.com/saworbit/didi/issues/818) · [#817](https://github.com/saworbit/didi/issues/817) · [PR #819](https://github.com/saworbit/didi/pull/819)
- `2026-09-20` The vendored headers have a watcher. [PR #795](https://github.com/saworbit/didi/pull/795)
- `2026-09-20` Dependabot watches the base image, and groups security fixes. [PR #795](https://github.com/saworbit/didi/pull/795)
- `2026-09-19` The live editor matrix covers the whole supported range. [#759](https://github.com/saworbit/didi/issues/759) · [PR #763](https://github.com/saworbit/didi/pull/763)
- `2026-09-19` `runtime_launch` can leave the game running. [#733](https://github.com/saworbit/didi/issues/733) · [PR #760](https://github.com/saworbit/didi/pull/760)
- `2026-09-16` Every tool publishes a title. [#686](https://github.com/saworbit/didi/issues/686) · [PR #698](https://github.com/saworbit/didi/pull/698)
- `2026-09-16` A blackboard writer can say "only if this has not changed". [#682](https://github.com/saworbit/didi/issues/682) · [PR #697](https://github.com/saworbit/didi/pull/697)
- `2026-09-14` Platform support is stated where people look. [PR #587](https://github.com/saworbit/didi/pull/587)
- `2026-09-14` A project website. [PR #584](https://github.com/saworbit/didi/pull/584)

### Changed

- `2026-09-23` The release archives stand on their own. [PR #908](https://github.com/saworbit/didi/pull/908)
- `2026-09-22` A connection that arrives is read when it arrives, rather than when the previous one goes quiet. [#873](https://github.com/saworbit/didi/issues/873) · [PR #875](https://github.com/saworbit/didi/pull/875)
- `2026-09-20` Every security alert now has a disposition written down, including the ones Scorecard raises. [PR #804](https://github.com/saworbit/didi/pull/804)

### Fixed

- `2026-09-23` `audio_list_buses` offline lists the buses the engine loads from the files. [#903](https://github.com/saworbit/didi/issues/903) · [#905](https://github.com/saworbit/didi/issues/905) · [PR #906](https://github.com/saworbit/didi/pull/906)
- `2026-09-23` A bus muted with `mute = 1` is reported as muted. [#853](https://github.com/saworbit/didi/issues/853) · [PR #904](https://github.com/saworbit/didi/pull/904)
- `2026-09-23` A refusal that names the argument that fixes it carries that argument. [#900](https://github.com/saworbit/didi/issues/900) · [PR #901](https://github.com/saworbit/didi/pull/901)
- `2026-09-23` `retry_with` has one shape again. [#897](https://github.com/saworbit/didi/issues/897) · [PR #899](https://github.com/saworbit/didi/pull/899)
- `2026-09-23` A rolled back mutation says `rolled_back` everywhere. [#896](https://github.com/saworbit/didi/issues/896) · [PR #898](https://github.com/saworbit/didi/pull/898)
- `2026-09-23` Twenty-seven bridge refusals say which conflict they are. [#894](https://github.com/saworbit/didi/issues/894) · [PR #895](https://github.com/saworbit/didi/pull/895)
- `2026-09-23` Every refusal above 400 names itself, in all five files that emit one. [#892](https://github.com/saworbit/didi/issues/892) · [PR #893](https://github.com/saworbit/didi/pull/893)
- `2026-09-23` An authorization refusal keeps its structured half. [#890](https://github.com/saworbit/didi/issues/890) · [PR #891](https://github.com/saworbit/didi/pull/891)
- `2026-09-22` One frame reader, called from both ends of both transports. [#880](https://github.com/saworbit/didi/issues/880) · [#881](https://github.com/saworbit/didi/issues/881) · [#882](https://github.com/saworbit/didi/issues/882) · [PR #883](https://github.com/saworbit/didi/pull/883)
- `2026-09-22` A frame the server has not been sent is no longer a buffer it has already allocated. [#876](https://github.com/saworbit/didi/issues/876) · [PR #879](https://github.com/saworbit/didi/pull/879)
- `2026-09-22` Four server threads no longer spin silently when the process runs out of file descriptors. [#877](https://github.com/saworbit/didi/issues/877) · [PR #879](https://github.com/saworbit/didi/pull/879)
- `2026-09-22` A Windows slot that cannot create its pipe instance says so. [#878](https://github.com/saworbit/didi/issues/878) · [PR #879](https://github.com/saworbit/didi/pull/879)
- `2026-09-22` The two deadlines for opening a connection stop being flat 2,000 ms numbers, and the comment on one of them stops stating something that is not true. [#874](https://github.com/saworbit/didi/issues/874) · [PR #875](https://github.com/saworbit/didi/pull/875)
- `2026-09-22` A handshake deadline now allows for the wait to be accepted, so the attach that `runtime_launch --detach` documents works on macOS and Linux. [#782](https://github.com/saworbit/didi/issues/782) · [PR #872](https://github.com/saworbit/didi/pull/872)
- `2026-09-22` A process that has exited no longer reads as present on macOS and Linux. [#869](https://github.com/saworbit/didi/issues/869) · [PR #871](https://github.com/saworbit/didi/pull/871)
- `2026-09-22` A detached game is handed to init rather than left as the server's own child. [#786](https://github.com/saworbit/didi/issues/786) · [PR #870](https://github.com/saworbit/didi/pull/870)
- `2026-09-22` Every refusal the extension emits names itself. [#865](https://github.com/saworbit/didi/issues/865) · [#867](https://github.com/saworbit/didi/issues/867) · [PR #868](https://github.com/saworbit/didi/pull/868)
- `2026-09-22` A paused game is refused rather than explored. [#778](https://github.com/saworbit/didi/issues/778) · [PR #866](https://github.com/saworbit/didi/pull/866)
- `2026-09-22` The Phase 7 signal bridge harness runs, and it passes. [#862](https://github.com/saworbit/didi/issues/862) · [PR #864](https://github.com/saworbit/didi/pull/864)
- `2026-09-22` Two signal refusals say the identifier where every other one says it. [PR #864](https://github.com/saworbit/didi/pull/864)
- `2026-09-22` A `scene_set_property` write that did not land says which of the two it was. [#767](https://github.com/saworbit/didi/issues/767) · [PR #863](https://github.com/saworbit/didi/pull/863)
- `2026-09-22` `signal_connect` writes the flags the editor's own Connect dialog writes. [#852](https://github.com/saworbit/didi/issues/852) · [PR #860](https://github.com/saworbit/didi/pull/860)
- `2026-09-22` The same rule was wrong in four more places in the same call. [PR #860](https://github.com/saworbit/didi/pull/860)
- `2026-09-22` `resource_create`'s type guard asks the engine that will load the file. [#766](https://github.com/saworbit/didi/issues/766) · [PR #858](https://github.com/saworbit/didi/pull/858)
- `2026-09-21` A project.godot the engine will not open no longer reads as one it will. [#826](https://github.com/saworbit/didi/issues/826) · [PR #857](https://github.com/saworbit/didi/pull/857)
- `2026-09-21` Every live route now gives the same account of a failed engine. [#854](https://github.com/saworbit/didi/issues/854) · [PR #855](https://github.com/saworbit/didi/pull/855)
- `2026-09-21` `origin: scene` means the connection is in the scene, not that the receiver happens to live there. [#768](https://github.com/saworbit/didi/issues/768) · [PR #851](https://github.com/saworbit/didi/pull/851)
- `2026-09-21` A headless game is refused as a game, and the renderer stopped dropping the facts a refusal carries. [#777](https://github.com/saworbit/didi/issues/777) · [PR #850](https://github.com/saworbit/didi/pull/850)
- `2026-09-21` The two readers of `project.godot` answer offline, like its writer does. [#780](https://github.com/saworbit/didi/issues/780) · [PR #849](https://github.com/saworbit/didi/pull/849)
- `2026-09-21` `runnable` is read the way the engine reads it, and one rule now covers both readers. [#842](https://github.com/saworbit/didi/issues/842) · [PR #848](https://github.com/saworbit/didi/pull/848)
- `2026-09-21` The bus layout is read the way Godot writes it. [#844](https://github.com/saworbit/didi/issues/844) · [PR #847](https://github.com/saworbit/didi/pull/847)
- `2026-09-21` The export presets refusal says which of the six causes it is. [#828](https://github.com/saworbit/didi/issues/828) · [PR #843](https://github.com/saworbit/didi/pull/843)
- `2026-09-21` The test runner refuses an argument it does not understand. [#803](https://github.com/saworbit/didi/issues/803) · [PR #841](https://github.com/saworbit/didi/pull/841)
- `2026-09-21` CONTRIBUTING says how to run the Python suite. [#798](https://github.com/saworbit/didi/issues/798) · [PR #841](https://github.com/saworbit/didi/pull/841)
- `2026-09-21` The Markdown link check is one implementation, and it runs before you push. [#824](https://github.com/saworbit/didi/issues/824) · [PR #840](https://github.com/saworbit/didi/pull/840)
- `2026-09-21` A session lock file is swept up once nobody holds it. [#787](https://github.com/saworbit/didi/issues/787) · [PR #839](https://github.com/saworbit/didi/pull/839)
- `2026-09-21` The offline bus reader follows the manifest, and answers with the bus the project actually has. [#836](https://github.com/saworbit/didi/issues/836) · [#837](https://github.com/saworbit/didi/issues/837) · [PR #838](https://github.com/saworbit/didi/pull/838)
- `2026-09-21` The import record is looked for where Godot writes it. [#833](https://github.com/saworbit/didi/issues/833) · [PR #835](https://github.com/saworbit/didi/pull/835)
- `2026-09-21` The freshness reproduction is tested against records Godot wrote. [#834](https://github.com/saworbit/didi/issues/834) · [PR #835](https://github.com/saworbit/didi/pull/835)
- `2026-09-20` The audit reads both halves of the record, and says so when it reads neither. [#830](https://github.com/saworbit/didi/issues/830) · [PR #832](https://github.com/saworbit/didi/pull/832)
- `2026-09-20` `source_newer_than_output` meant two things, and the documented remedy fixed one of them. [#831](https://github.com/saworbit/didi/issues/831) · [PR #832](https://github.com/saworbit/didi/pull/832)
- `2026-09-20` Import freshness is read from the record Godot wrote, not from modification times. [#827](https://github.com/saworbit/didi/issues/827) · [PR #829](https://github.com/saworbit/didi/pull/829)
- `2026-09-20` A `.import` and an `export_presets.cfg` the engine refuses were both read as though they had loaded. [#823](https://github.com/saworbit/didi/issues/823) · [PR #825](https://github.com/saworbit/didi/pull/825)
- `2026-09-20` A balanced `project.godot` is not a loadable one, and a line can hold two settings. [#820](https://github.com/saworbit/didi/issues/820) · [PR #822](https://github.com/saworbit/didi/pull/822)
- `2026-09-20` Two settings on one line are two settings. [#821](https://github.com/saworbit/didi/issues/821) · [PR #822](https://github.com/saworbit/didi/pull/822)
- `2026-09-20` A key the engine built by joining the line above into this one is refused for the same reason. [PR #822](https://github.com/saworbit/didi/pull/822)
- `2026-09-20` An identifier does not end a value. [PR #822](https://github.com/saworbit/didi/pull/822)
- `2026-09-20` A value is what the engine reads, not the rest of one line. [#816](https://github.com/saworbit/didi/issues/816) · [PR #819](https://github.com/saworbit/didi/pull/819)
- `2026-09-20` A `project.godot` Godot refuses to parse is no longer read, previewed and written as though it loaded. [#817](https://github.com/saworbit/didi/issues/817) · [PR #819](https://github.com/saworbit/didi/pull/819)
- `2026-09-20` A key is the tokens joined, not the text before the first `=`. [#813](https://github.com/saworbit/didi/issues/813) · [PR #815](https://github.com/saworbit/didi/pull/815)
- `2026-09-20` `export_presets.cfg` is read by the same rules as every other ConfigFile. [#812](https://github.com/saworbit/didi/issues/812) · [PR #815](https://github.com/saworbit/didi/pull/815)
- `2026-09-20` A spaced header hides nothing from the last two readers. [#814](https://github.com/saworbit/didi/issues/814) · [PR #815](https://github.com/saworbit/didi/pull/815)
- `2026-09-20` A `#` line in `project.godot` is a setting, not a comment. [#810](https://github.com/saworbit/didi/issues/810) · [PR #811](https://github.com/saworbit/didi/pull/811)
- `2026-09-20` A spaced section header is the same section. [#809](https://github.com/saworbit/didi/issues/809) · [PR #811](https://github.com/saworbit/didi/pull/811)
- `2026-09-20` A spaced `[autoload]` key is the same key. [#802](https://github.com/saworbit/didi/issues/802) · [PR #808](https://github.com/saworbit/didi/pull/808)
- `2026-09-20` A second merge no longer cancels the first one's CodeQL run on `main`. [PR #805](https://github.com/saworbit/didi/pull/805)
- `2026-09-20` `project_rename_references` reports the `[autoload]` line that defines the name. [#792](https://github.com/saworbit/didi/issues/792) · [PR #801](https://github.com/saworbit/didi/pull/801)
- `2026-09-20` `project_audit_assets` follows the resources `project.godot` names. [#774](https://github.com/saworbit/didi/issues/774) · [PR #791](https://github.com/saworbit/didi/pull/791)
- `2026-09-20` `runtime_launch` names the process and the engine the rest of its answer means. [#773](https://github.com/saworbit/didi/issues/773) · [#772](https://github.com/saworbit/didi/issues/772) · [PR #790](https://github.com/saworbit/didi/pull/790)
- `2026-09-19` A resource slot takes a list of classes, so materials can be assigned again. [#783](https://github.com/saworbit/didi/issues/783) · [PR #789](https://github.com/saworbit/didi/pull/789)
- `2026-09-19` `resource_create` stops writing files Godot cannot load or silently empties. [#765](https://github.com/saworbit/didi/issues/765) · [#764](https://github.com/saworbit/didi/issues/764) · [PR #788](https://github.com/saworbit/didi/pull/788)
- `2026-09-19` The Unix socket server stops without pulling a descriptor out from under its own thread. [#757](https://github.com/saworbit/didi/issues/757) · [PR #762](https://github.com/saworbit/didi/pull/762)
- `2026-09-19` A timeout that could not finish the kill says so. [#755](https://github.com/saworbit/didi/issues/755) · [PR #761](https://github.com/saworbit/didi/pull/761)
- `2026-09-19` The offline process tools take the whole tree down on timeout. [#758](https://github.com/saworbit/didi/issues/758) · [PR #761](https://github.com/saworbit/didi/pull/761)
- `2026-09-19` An asset the editor has never seen gets imported, and the answer says whether it did. [#731](https://github.com/saworbit/didi/issues/731) · [PR #751](https://github.com/saworbit/didi/pull/751)
- `2026-09-18` `scene_create` makes the scene you meant. [#740](https://github.com/saworbit/didi/issues/740) · [PR #754](https://github.com/saworbit/didi/pull/754)
- `2026-09-18` A coordinate is an object anywhere a vector is. [#738](https://github.com/saworbit/didi/issues/738) · [PR #753](https://github.com/saworbit/didi/pull/753)
- `2026-09-18` `LLM_INSTRUCTIONS` no longer forbids what the surface does. [#739](https://github.com/saworbit/didi/issues/739) · [PR #753](https://github.com/saworbit/didi/pull/753)
- `2026-09-18` A property write that landed says so. [PR #753](https://github.com/saworbit/didi/pull/753)
- `2026-09-18` A `oneOf` refusal picks the branch by type. [PR #753](https://github.com/saworbit/didi/pull/753)
- `2026-09-18` One InputEvent vocabulary, spelled the engine's way, and published. [#737](https://github.com/saworbit/didi/issues/737) · [PR #752](https://github.com/saworbit/didi/pull/752)
- `2026-09-18` The InputEvent vocabulary is published, not just enforced. [#736](https://github.com/saworbit/didi/issues/736) · [PR #752](https://github.com/saworbit/didi/pull/752)
- `2026-09-18` A refusal about an event says which entry and which property. [#737](https://github.com/saworbit/didi/issues/737) · [PR #752](https://github.com/saworbit/didi/pull/752)
- `2026-09-18` `runtime_launch` finishes its own kill before it answers. [#732](https://github.com/saworbit/didi/issues/732) · [PR #750](https://github.com/saworbit/didi/pull/750)
- `2026-09-18` A handler on a script that did not compile is reported as that, not as a missing method. [#729](https://github.com/saworbit/didi/issues/729) · [PR #749](https://github.com/saworbit/didi/pull/749)
- `2026-09-18` A crash comes back with somewhere to go. [#744](https://github.com/saworbit/didi/issues/744) · [PR #748](https://github.com/saworbit/didi/pull/748)
- `2026-09-18` A run that crashed says so in its summary. [#744](https://github.com/saworbit/didi/issues/744) · [PR #748](https://github.com/saworbit/didi/pull/748)
- `2026-09-18` A syntax check says whether it asked a compiler. [#728](https://github.com/saworbit/didi/issues/728) · [PR #747](https://github.com/saworbit/didi/pull/747)
- `2026-09-18` A vector is written as the type the property is declared, not the type its JSON looks like. [#730](https://github.com/saworbit/didi/issues/730) · [PR #746](https://github.com/saworbit/didi/pull/746)
- `2026-09-18` `resource_create` says which engine its property check was not run against. [#735](https://github.com/saworbit/didi/issues/735) · [PR #746](https://github.com/saworbit/didi/pull/746)
- `2026-09-18` A raycast in the editor asks the edited scene's own world, and answers with a path the surface takes. [#743](https://github.com/saworbit/didi/issues/743) · [#742](https://github.com/saworbit/didi/issues/742) · [PR #745](https://github.com/saworbit/didi/pull/745)
- `2026-09-18` `collision_mask` takes Godot's whole 32-bit range. [#743](https://github.com/saworbit/didi/issues/743) · [PR #745](https://github.com/saworbit/didi/pull/745)
- `2026-09-17` The last uncached build in CI is cached, and every platform now configures the same way. [PR #727](https://github.com/saworbit/didi/pull/727)
- `2026-09-17` The live Godot jobs cache their compile instead of repeating it. [PR #726](https://github.com/saworbit/didi/pull/726)
- `2026-09-17` CI's critical path was a cold compile of a tree that was already cached. [PR #725](https://github.com/saworbit/didi/pull/725)
- `2026-09-17` A push to main no longer cancels the run that was checking the last merge. [PR #725](https://github.com/saworbit/didi/pull/725)
- `2026-09-16` `project_rename_references`'s preview names the sites it will leave behind. [#716](https://github.com/saworbit/didi/issues/716) · [PR #724](https://github.com/saworbit/didi/pull/724)
- `2026-09-16` Three semantic failures answer with the error envelope rather than a bare string. [#705](https://github.com/saworbit/didi/issues/705) · [PR #723](https://github.com/saworbit/didi/pull/723)
- `2026-09-16` Every unbound name in `eval_gdscript` is refused by its own name. [#712](https://github.com/saworbit/didi/issues/712) · [PR #723](https://github.com/saworbit/didi/pull/723)
- `2026-09-16` Two parameter descriptions stop offering a value their own enum refuses. [#708](https://github.com/saworbit/didi/issues/708) · [PR #722](https://github.com/saworbit/didi/pull/722)
- `2026-09-16` The two legacy names with no canonical replacement say so. [#709](https://github.com/saworbit/didi/issues/709) · [PR #722](https://github.com/saworbit/didi/pull/722)
- `2026-09-16` Two tools stop advertising a set the answer is not a member of. [#713](https://github.com/saworbit/didi/issues/713) · [PR #722](https://github.com/saworbit/didi/pull/722)
- `2026-09-16` The listings say they move, and say when. [#701](https://github.com/saworbit/didi/issues/701) · [PR #721](https://github.com/saworbit/didi/pull/721)
- `2026-09-16` `--ui-app off` stops declaring the MCP Apps extension. [#717](https://github.com/saworbit/didi/issues/717) · [PR #721](https://github.com/saworbit/didi/pull/721)
- `2026-09-16` A refused ghost preview leaves the screen as it found it. [#707](https://github.com/saworbit/didi/issues/707) · [PR #720](https://github.com/saworbit/didi/pull/720)
- `2026-09-16` `signal_disconnect` stops reporting a method signature problem. [#714](https://github.com/saworbit/didi/issues/714) · [PR #720](https://github.com/saworbit/didi/pull/720)
- `2026-09-16` `scene_instantiate_node` says when the engine did not use the name it was given. [#710](https://github.com/saworbit/didi/issues/710) · [PR #720](https://github.com/saworbit/didi/pull/720)
- `2026-09-16` A runtime endpoint too long for `sockaddr_un` says so. [#711](https://github.com/saworbit/didi/issues/711) · [PR #719](https://github.com/saworbit/didi/pull/719)
- `2026-09-16` `csharp_check_build` now reports the build it actually ran. [#702](https://github.com/saworbit/didi/issues/702) · [#703](https://github.com/saworbit/didi/issues/703) · [#704](https://github.com/saworbit/didi/issues/704) · [#706](https://github.com/saworbit/didi/issues/706) · [PR #718](https://github.com/saworbit/didi/pull/718)
- `2026-09-16` A session test reads its own directory, not the machine's. [PR #700](https://github.com/saworbit/didi/pull/700)
- `2026-09-16` `viewport_create_test_lab`'s preview names the file it replaces. [#685](https://github.com/saworbit/didi/issues/685) · [PR #699](https://github.com/saworbit/didi/pull/699)
- `2026-09-16` `--yolo` shows up where a client reads before it calls. [#684](https://github.com/saworbit/didi/issues/684) · [PR #698](https://github.com/saworbit/didi/pull/698)
- `2026-09-16` The blackboard records who removed a value, not only who wrote one. [#681](https://github.com/saworbit/didi/issues/681) · [PR #697](https://github.com/saworbit/didi/pull/697)
- `2026-09-16` A blackboard key that expired does not read like one nobody wrote. [#680](https://github.com/saworbit/didi/issues/680) · [PR #696](https://github.com/saworbit/didi/pull/696)
- `2026-09-16` `blackboard_patch` says which operation failed, in its own words. [#679](https://github.com/saworbit/didi/issues/679) · [PR #696](https://github.com/saworbit/didi/pull/696)
- `2026-09-16` A headless editor is a state Didi can name. [#676](https://github.com/saworbit/didi/issues/676) · [PR #695](https://github.com/saworbit/didi/pull/695)
- `2026-09-16` `editor_save_scene` reports what the engine printed while saving. [#683](https://github.com/saworbit/didi/issues/683) · [PR #695](https://github.com/saworbit/didi/pull/695)
- `2026-09-16` `script_check_syntax` refuses when the compiler never ran. [#677](https://github.com/saworbit/didi/issues/677) · [PR #694](https://github.com/saworbit/didi/pull/694)
- `2026-09-16` The engine-mismatch check works without an explicit attach. [#687](https://github.com/saworbit/didi/issues/687) · [PR #694](https://github.com/saworbit/didi/pull/694)
- `2026-09-16` `--managed-editor` starts against Godot's Windows console build. [#678](https://github.com/saworbit/didi/issues/678) · [PR #693](https://github.com/saworbit/didi/pull/693)
- `2026-09-16` `--log-level DEBUG` answers a client that does not read stderr. [#689](https://github.com/saworbit/didi/issues/689) · [PR #692](https://github.com/saworbit/didi/pull/692)
- `2026-09-16` The editor exits cleanly on macOS and Linux. [#688](https://github.com/saworbit/didi/issues/688) · [PR #691](https://github.com/saworbit/didi/pull/691)
- `2026-09-16` The Linux release starts on a distro whose glibc meets the stated floor. [#647](https://github.com/saworbit/didi/issues/647) · [PR #675](https://github.com/saworbit/didi/pull/675)
- `2026-09-16` The macOS archive's `.gdextension` declares only what the archive holds. [#648](https://github.com/saworbit/didi/issues/648) · [PR #675](https://github.com/saworbit/didi/pull/675)
- `2026-09-16` The Diagnostics page checks the architecture, not the filename. [#648](https://github.com/saworbit/didi/issues/648) · [PR #675](https://github.com/saworbit/didi/pull/675)
- `2026-09-16` The addon folds path case where the filesystem does, not only on Windows. [#655](https://github.com/saworbit/didi/issues/655) · [PR #674](https://github.com/saworbit/didi/pull/674)
- `2026-09-16` A `GODOT_BIN` that cannot be used is reported, not discarded in silence. [#656](https://github.com/saworbit/didi/issues/656) · [PR #673](https://github.com/saworbit/didi/pull/673)
- `2026-09-16` `runtime_list_sessions` says which directory it read. [#649](https://github.com/saworbit/didi/issues/649) · [PR #673](https://github.com/saworbit/didi/pull/673)
- `2026-09-16` `maxLength` counts what it says it counts. [#663](https://github.com/saworbit/didi/issues/663) · [PR #672](https://github.com/saworbit/didi/pull/672)
- `2026-09-16` A parameter pinned to one value says why. [#654](https://github.com/saworbit/didi/issues/654) · [PR #672](https://github.com/saworbit/didi/pull/672)
- `2026-09-16` The export family answers with an envelope and previews what it will do. [#651](https://github.com/saworbit/didi/issues/651) · [PR #671](https://github.com/saworbit/didi/pull/671)
- `2026-09-16` `project_export`'s preview reads the presets file. [PR #671](https://github.com/saworbit/didi/pull/671)
- `2026-09-16` `gridmap_export_mesh_library`'s preview describes the file it replaces. [PR #671](https://github.com/saworbit/didi/pull/671)
- `2026-09-16` A script this process may not read is not reported as bad code. [#653](https://github.com/saworbit/didi/issues/653) · [PR #670](https://github.com/saworbit/didi/pull/670)
- `2026-09-16` A file whose name JSON cannot carry is named, not blamed on the caller. [#650](https://github.com/saworbit/didi/issues/650) · [PR #669](https://github.com/saworbit/didi/pull/669)
- `2026-09-15` `project_rename_references` previews the plan it is about to carry out. [#662](https://github.com/saworbit/didi/issues/662) · [PR #668](https://github.com/saworbit/didi/pull/668)
- `2026-09-15` A match in a scene or a resource is not called a source code reference. [#665](https://github.com/saworbit/didi/issues/665) · [PR #668](https://github.com/saworbit/didi/pull/668)
- `2026-09-15` The whole-project readers answer on a project with a baked mesh in it. [#661](https://github.com/saworbit/didi/issues/661) · [PR #667](https://github.com/saworbit/didi/pull/667)
- `2026-09-15` Those two readers are bounded, and say when they read less than the whole project. [#664](https://github.com/saworbit/didi/issues/664) · [PR #667](https://github.com/saworbit/didi/pull/667)
- `2026-09-15` A duplicated branch survives the save. [#659](https://github.com/saworbit/didi/issues/659) · [PR #666](https://github.com/saworbit/didi/pull/666)
- `2026-09-15` Undoing `scene_remove_from_group` puts a persistent group back persistent. [#660](https://github.com/saworbit/didi/issues/660) · [PR #666](https://github.com/saworbit/didi/pull/666)
- `2026-09-15` `audio_configure_bus` says where the change ends up. [#622](https://github.com/saworbit/didi/issues/622) · [PR #646](https://github.com/saworbit/didi/pull/646)
- `2026-09-15` `script_check_syntax` and `shader_check_compile` name the engine that answered. [#617](https://github.com/saworbit/didi/issues/617) · [PR #645](https://github.com/saworbit/didi/pull/645)
- `2026-09-15` The fix cycle spawns the interpreter it is running, not a command called `python`. [#635](https://github.com/saworbit/didi/issues/635) · [PR #644](https://github.com/saworbit/didi/pull/644)
- `2026-09-15` Both Python floors are written down. [#634](https://github.com/saworbit/didi/issues/634) · [PR #644](https://github.com/saworbit/didi/pull/644)
- `2026-09-15` A reconfigure with no source change recompiles one file, not 57 targets. [#633](https://github.com/saworbit/didi/issues/633) · [PR #643](https://github.com/saworbit/didi/pull/643)
- `2026-09-15` The checkpoint file-count boundary is tested with eleven files, not ten thousand. [#627](https://github.com/saworbit/didi/issues/627) · [PR #643](https://github.com/saworbit/didi/pull/643)
- `2026-09-15` The tilemap and gridmap rules a schema cannot state answer with a sentence. [#619](https://github.com/saworbit/didi/issues/619) · [PR #642](https://github.com/saworbit/didi/pull/642)
- `2026-09-15` A live-only tool with no engine attached says what to do about it. [#615](https://github.com/saworbit/didi/issues/615) · [PR #641](https://github.com/saworbit/didi/pull/641)
- `2026-09-15` A script the engine cannot read is not a script with nothing in it. [#614](https://github.com/saworbit/didi/issues/614) · [PR #640](https://github.com/saworbit/didi/pull/640)
- `2026-09-15` `script_check_syntax` fails a script Godot refuses to load. [#613](https://github.com/saworbit/didi/issues/613) · [PR #640](https://github.com/saworbit/didi/pull/640)
- `2026-09-15` Emitting a signal nothing is connected to is a no-op, and says so. [#624](https://github.com/saworbit/didi/issues/624) · [PR #639](https://github.com/saworbit/didi/pull/639)
- `2026-09-15` An engine that answered is no longer reported as a session that is gone. [#625](https://github.com/saworbit/didi/issues/625) · [PR #639](https://github.com/saworbit/didi/pull/639)
- `2026-09-15` `signal_emit`'s dry run runs the argument-value rules the confirmed call runs. [#616](https://github.com/saworbit/didi/issues/616) · [PR #639](https://github.com/saworbit/didi/pull/639)
- `2026-09-15` A preview says what its probe actually read. [#621](https://github.com/saworbit/didi/issues/621) · [PR #639](https://github.com/saworbit/didi/pull/639)
- `2026-09-15` A float shader uniform set to a whole number survives the save. [#612](https://github.com/saworbit/didi/issues/612) · [PR #638](https://github.com/saworbit/didi/pull/638)
- `2026-09-15` A colour or vector write that landed says it landed. [#618](https://github.com/saworbit/didi/issues/618) · [PR #638](https://github.com/saworbit/didi/pull/638)
- `2026-09-15` `shader_list_uniforms` reports each uniform's declared hint, and `shader_set_uniform` honours it. [#620](https://github.com/saworbit/didi/issues/620) · [PR #638](https://github.com/saworbit/didi/pull/638)
- `2026-09-15` `shader_set_uniform` says the change is not on disk yet. [#623](https://github.com/saworbit/didi/issues/623) · [PR #638](https://github.com/saworbit/didi/pull/638)
- `2026-09-15` The schema gate refuses arguments that are not an object. [#629](https://github.com/saworbit/didi/issues/629) · [PR #637](https://github.com/saworbit/didi/pull/637)
- `2026-09-15` `viewport_diff_capture` refuses a malformed capture id with the error envelope. [#628](https://github.com/saworbit/didi/issues/628) · [PR #637](https://github.com/saworbit/didi/pull/637)
- `2026-09-15` The server starts under a project root with an accent in it. [#611](https://github.com/saworbit/didi/issues/611) · [PR #636](https://github.com/saworbit/didi/pull/636)
- `2026-09-14` A game stopped on request is reported as the exit it is. [#595](https://github.com/saworbit/didi/issues/595) · [PR #610](https://github.com/saworbit/didi/pull/610)
- `2026-09-14` What discovery advertises is what a call gets, for hit-testing a game and for managed recovery. [#592](https://github.com/saworbit/didi/issues/592) · [#599](https://github.com/saworbit/didi/issues/599) · [PR #609](https://github.com/saworbit/didi/pull/609)
- `2026-09-14` Three places where the published contract and the handler disagreed. [#596](https://github.com/saworbit/didi/issues/596) · [#598](https://github.com/saworbit/didi/issues/598) · [#593](https://github.com/saworbit/didi/issues/593) · [PR #608](https://github.com/saworbit/didi/pull/608)
- `2026-09-14` The extension no longer prints engine errors at startup and on every dashboard read, and its log stays out of a game's output. [#600](https://github.com/saworbit/didi/issues/600) · [#601](https://github.com/saworbit/didi/issues/601) · [PR #607](https://github.com/saworbit/didi/pull/607)
- `2026-09-14` Injected input reaches a paused game's nodes, and a click lands where it is aimed. [#594](https://github.com/saworbit/didi/issues/594) · [#597](https://github.com/saworbit/didi/issues/597) · [#602](https://github.com/saworbit/didi/issues/602) · [PR #606](https://github.com/saworbit/didi/pull/606)
- `2026-09-14` Scene edits the file cannot hold are refused before they happen, and the hierarchy says who owns what. [#588](https://github.com/saworbit/didi/issues/588) · [#589](https://github.com/saworbit/didi/issues/589) · [#590](https://github.com/saworbit/didi/issues/590) · [#603](https://github.com/saworbit/didi/issues/603) · [#591](https://github.com/saworbit/didi/issues/591) · [PR #605](https://github.com/saworbit/didi/pull/605)
- `2026-09-14` A bounded reader publishes `max_response_bytes`, and both new ones use the same figure. [PR #586](https://github.com/saworbit/didi/pull/586)
- `2026-09-14` The social preview banner named the wrong surface size. [PR #585](https://github.com/saworbit/didi/pull/585)
- `2026-09-14` Every required string parameter carries a declared length. [#573](https://github.com/saworbit/didi/issues/573) · [PR #583](https://github.com/saworbit/didi/pull/583)
- `2026-09-14` A refusal names both halves of the mistake. [#577](https://github.com/saworbit/didi/issues/577) · [PR #583](https://github.com/saworbit/didi/pull/583)
- `2026-09-14` `case_sensitive` describes what it does. [#576](https://github.com/saworbit/didi/issues/576) · [PR #583](https://github.com/saworbit/didi/pull/583)
- `2026-09-14` `script_get_symbols` publishes a limit and says what it left out. [#575](https://github.com/saworbit/didi/issues/575) · [PR #582](https://github.com/saworbit/didi/pull/582)
- `2026-09-14` A confirmation token is bound to what the preview saw, not only to the call. [#572](https://github.com/saworbit/didi/issues/572) · [PR #581](https://github.com/saworbit/didi/pull/581)
- `2026-09-14` A dry run runs the argument checks the real call runs. [#571](https://github.com/saworbit/didi/issues/571) · [PR #581](https://github.com/saworbit/didi/pull/581)
- `2026-09-14` A preview carries its arguments once. [#574](https://github.com/saworbit/didi/issues/574) · [PR #581](https://github.com/saworbit/didi/pull/581)
- `2026-09-14` `viewport_diff_capture` makes the viewport render before it compares. [#568](https://github.com/saworbit/didi/issues/568) · [PR #580](https://github.com/saworbit/didi/pull/580)
- `2026-09-14` `script_patch_method` refuses a symbol the script does not declare. [#569](https://github.com/saworbit/didi/issues/569) · [PR #579](https://github.com/saworbit/didi/pull/579)
- `2026-09-14` `symbol_type` publishes the six kinds it models, and refuses the rest. [#570](https://github.com/saworbit/didi/issues/570) · [PR #579](https://github.com/saworbit/didi/pull/579)
- `2026-09-14` `viewport_create_test_lab` checks the target first and writes the lab where the audit can see it. [#564](https://github.com/saworbit/didi/issues/564) · [PR #567](https://github.com/saworbit/didi/pull/567)
- `2026-09-14` `viewport_create_test_lab` says whether it instanced the target. [#565](https://github.com/saworbit/didi/issues/565) · [PR #567](https://github.com/saworbit/didi/pull/567)
- `2026-09-14` Live scene mutations say the change is unsaved. [#557](https://github.com/saworbit/didi/issues/557) · [PR #566](https://github.com/saworbit/didi/pull/566)
- `2026-09-14` A second `initialize` is refused. [#552](https://github.com/saworbit/didi/issues/552) · [PR #563](https://github.com/saworbit/didi/pull/563)
- `2026-09-14` `script_reflect_class` compares the pinned dump to the project when no session is selected. [#555](https://github.com/saworbit/didi/issues/555) · [PR #562](https://github.com/saworbit/didi/pull/562)
- `2026-09-14` `project_search_symbols` reads a `.GD` script. [#549](https://github.com/saworbit/didi/issues/549) · [PR #561](https://github.com/saworbit/didi/pull/561)
- `2026-09-14` Search columns count code points. [#556](https://github.com/saworbit/didi/issues/556) · [PR #561](https://github.com/saworbit/didi/pull/561)
- `2026-09-14` Writers report the path they resolved, not the argument. [#546](https://github.com/saworbit/didi/issues/546) · [#551](https://github.com/saworbit/didi/issues/551) · [PR #560](https://github.com/saworbit/didi/pull/560)
- `2026-09-14` `script_patch_method` keeps the file's line endings. [#550](https://github.com/saworbit/didi/issues/550) · [PR #560](https://github.com/saworbit/didi/pull/560)
- `2026-09-14` Every required string parameter carries `minLength: 1` unless the schema says otherwise, stamped where `additionalProperties` is stamped. [#553](https://github.com/saworbit/didi/issues/553) · [#554](https://github.com/saworbit/didi/issues/554) · [PR #559](https://github.com/saworbit/didi/pull/559)
- `2026-09-14` Four failures behind valid arguments answer with the error envelope instead of prose. [#548](https://github.com/saworbit/didi/issues/548) · [PR #559](https://github.com/saworbit/didi/pull/559)
- `2026-09-14` `project_set_setting`'s descriptions say where the `create` guard runs. [#547](https://github.com/saworbit/didi/issues/547) · [PR #559](https://github.com/saworbit/didi/pull/559)

---

## [2.0.0] - 2026-09-13

A major because the surface changed, not because the project grew up. See
[Stability](#stability) for what the number does and does not promise.

Almost all of this release is correctness work on answers that were already
wrong. The reason it is a major rather than a patch is that a client written
against 1.8.0 branched on those wrong answers, and several of them are now
different. Read the list below before upgrading.

Full write-ups for 2.0.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#200---2026-09-13).

### Breaking

- **`initialize` requires `protocolVersion`.** It is a required string in the
  MCP schema and every value was previously accepted, including a missing key.
  A client that omitted it now fails the handshake with `-32602` rather than
  connecting. This is the one change that can break a whole client rather than
  one call (#531).
- **Every published `inputSchema` is closed.** `additionalProperties: false` is
  enforced, so an argument a tool does not declare is refused instead of being
  accepted and ignored. A call that carried a misspelled parameter used to
  succeed while doing something other than what was asked (#418, #397).
- **`prompts/get` refuses an argument the prompt does not declare**, for the
  same reason (#511).
- **`blackboard_task_claim` answers a lost race with `409`.** Naming a
  `task_id` that cannot be claimed returns `404` when the task does not exist
  and `409` when its state stands in the way. It used to return `isError:
  false` with `claimed: false`, so a caller branching on `isError` read a lost
  race as a win. An unnamed claim finding nothing is still a success (#529).
- **`blackboard_task_complete` and `blackboard_task_update` answer an
  already-completed task with `409`, not `400`** (#530).
- **`execution_mode` changed value on the definitions with no live path.**
  `blackboard://<board>/state`, `blackboard://<board>/tasks` and
  `godot://project/tree` report `local`; `ui://didi/control-room` reports
  `local_status`; and `tools/list` advertises the word each tool actually
  answers with. Anything branching on `offline_fallback` for these sees a
  different string (#503, #533).
- **Tool annotations are decided per tool.** `destructiveHint`,
  `idempotentHint` and `openWorldHint` were four names for one bit and now
  carry their own values, which changes what a client may auto-approve. In
  particular `runtime_attach_session` and `runtime_detach_session` are no
  longer read-only (#505, #507).
- **`runtime_detach_session` succeeds when nothing is attached**, answering
  `detached: false` instead of `503`. A caller treating any error as fatal saw
  a failure where there was none; a caller asserting on the error now sees a
  success (#537).
- **Error payloads carry `data.code`, `data.tool` and `data.retryable`
  everywhere**, and roughly seventy call sites that answered with a bare JSON
  string now answer with the envelope. Substring-matching the old prose no
  longer works (#420, #460, #486, #487, #492, #526).
- **Paths are validated by resolving them.** `res://nested/../ok.gd` is now
  written rather than refused, because it lands inside the project root, and a
  path holding a NUL or any other control character is refused rather than
  written somewhere else (#525, #534).

### Added

- `2026-09-12` Nine more tools publish an `outputSchema`, and the rule for which do is written down. [#509](https://github.com/saworbit/didi/issues/509) · [PR #523](https://github.com/saworbit/didi/pull/523)
- `2026-09-10` `scene_call_method` runs a method the target node's own script declares, and returns what it returned. [#389](https://github.com/saworbit/didi/issues/389) · [PR #395](https://github.com/saworbit/didi/pull/395)
- `2026-09-10` `viewport_capture_frame` can select the editor main screen it needs. [#381](https://github.com/saworbit/didi/issues/381) · [PR #393](https://github.com/saworbit/didi/pull/393)
- `2026-09-10` `resource_create` can express a reference to another resource, so the composite resources are authorable at last. [#380](https://github.com/saworbit/didi/issues/380) · [PR #392](https://github.com/saworbit/didi/pull/392)

### Fixed

- `2026-09-13` `initialize` reads the `protocolVersion` it is sent. [#531](https://github.com/saworbit/didi/issues/531) · [PR #543](https://github.com/saworbit/didi/pull/543)
- `2026-09-13` `resources/subscribe` refuses with a reason that is true. [#532](https://github.com/saworbit/didi/issues/532) · [PR #543](https://github.com/saworbit/didi/pull/543)
- `2026-09-13` Resources with no live path say what they are, not what they fell back from. [#533](https://github.com/saworbit/didi/issues/533) · [PR #543](https://github.com/saworbit/didi/pull/543)
- `2026-09-13` A second MCP server on a held editor is told so. [#527](https://github.com/saworbit/didi/issues/527) · [PR #542](https://github.com/saworbit/didi/pull/542)
- `2026-09-13` An engine crash survives the call that discovered it. [#536](https://github.com/saworbit/didi/issues/536) · [PR #542](https://github.com/saworbit/didi/pull/542)
- `2026-09-13` `runtime_detach_session` is idempotent. [#537](https://github.com/saworbit/didi/issues/537) · [PR #542](https://github.com/saworbit/didi/pull/542)
- `2026-09-13` `blackboard_task_update` says what it takes for `progress`. [#528](https://github.com/saworbit/didi/issues/528) · [PR #541](https://github.com/saworbit/didi/pull/541)
- `2026-09-13` `blackboard_task_claim` answers a conflict like its siblings. [#529](https://github.com/saworbit/didi/issues/529) · [PR #541](https://github.com/saworbit/didi/pull/541)
- `2026-09-13` Completing an already-completed task is `409 conflict`. [#530](https://github.com/saworbit/didi/issues/530) · [PR #541](https://github.com/saworbit/didi/pull/541)
- `2026-09-13` The schema-enforcement test brings its own project. [PR #540](https://github.com/saworbit/didi/pull/540)
- `2026-09-13` A path holding a NUL is refused rather than written somewhere else. [#525](https://github.com/saworbit/didi/issues/525) · [PR #539](https://github.com/saworbit/didi/pull/539)
- `2026-09-13` `script_create` answers a bad path with a code. [#526](https://github.com/saworbit/didi/issues/526) · [PR #539](https://github.com/saworbit/didi/pull/539)
- `2026-09-13` `res://nested/../ok2.gd` is accepted, because it lands inside the project. [#534](https://github.com/saworbit/didi/issues/534) · [PR #539](https://github.com/saworbit/didi/pull/539)
- `2026-09-13` A Godot `Error` reaches the caller with its name. [#535](https://github.com/saworbit/didi/issues/535) · [PR #539](https://github.com/saworbit/didi/pull/539)
- `2026-09-12` `runtime_detach_session` reports `server_build_id` again. [PR #523](https://github.com/saworbit/didi/pull/523)
- `2026-09-12` `godot://editor/state` names a scene root the scene tools accept. [#502](https://github.com/saworbit/didi/issues/502) · [PR #522](https://github.com/saworbit/didi/pull/522)
- `2026-09-12` Every blackboard board is served as `application/json`. [#513](https://github.com/saworbit/didi/issues/513) · [PR #521](https://github.com/saworbit/didi/pull/521)
- `2026-09-12` A blackboard board says whether it exists, and the parameterised shape is discoverable. [#514](https://github.com/saworbit/didi/issues/514) · [PR #521](https://github.com/saworbit/didi/pull/521)
- `2026-09-12` A malformed blackboard URI names the part that was wrong. [#515](https://github.com/saworbit/didi/issues/515) · [PR #521](https://github.com/saworbit/didi/pull/521)
- `2026-09-12` `prompts/get` refuses an argument the prompt does not declare. [#511](https://github.com/saworbit/didi/issues/511) · [PR #520](https://github.com/saworbit/didi/pull/520)
- `2026-09-12` A prompt has one description. [#512](https://github.com/saworbit/didi/issues/512) · [PR #520](https://github.com/saworbit/didi/pull/520)
- `2026-09-12` Every published `inputSchema` carries the `additionalProperties: false` the server enforces. [#508](https://github.com/saworbit/didi/issues/508) · [PR #519](https://github.com/saworbit/didi/pull/519)
- `2026-09-12` `scene_get_hierarchy` declares the fields it returns. [#510](https://github.com/saworbit/didi/issues/510) · [PR #519](https://github.com/saworbit/didi/pull/519)
- `2026-09-12` `tools/list` says the mode a tool actually answers with. [#503](https://github.com/saworbit/didi/issues/503) · [PR #518](https://github.com/saworbit/didi/pull/518)
- `2026-09-12` `project_get_uid_map` and `project_audit_assets` stop calling an authoritative answer a fallback. [#504](https://github.com/saworbit/didi/issues/504) · [PR #518](https://github.com/saworbit/didi/pull/518)
- `2026-09-12` Tool annotations are decided per tool instead of being four names for one bit. [#505](https://github.com/saworbit/didi/issues/505) · [#507](https://github.com/saworbit/didi/issues/507) · [PR #517](https://github.com/saworbit/didi/pull/517)
- `2026-09-12` `runtime_detach_session` says what it did. [#506](https://github.com/saworbit/didi/issues/506) · [PR #517](https://github.com/saworbit/didi/pull/517)
- `2026-09-12` The expression sandbox names the read that works. [#488](https://github.com/saworbit/didi/issues/488) · [PR #501](https://github.com/saworbit/didi/pull/501)
- `2026-09-12` `tools/list` says which names are legacy. [#493](https://github.com/saworbit/didi/issues/493) · [PR #500](https://github.com/saworbit/didi/pull/500)
- `2026-09-12` `project_apply_changes` stops issuing a token for a call it cannot apply. [#491](https://github.com/saworbit/didi/issues/491) · [PR #499](https://github.com/saworbit/didi/pull/499)
- `2026-09-12` A `oneOf` branch behind a `$ref` says what it needs. [#489](https://github.com/saworbit/didi/issues/489) · [PR #498](https://github.com/saworbit/didi/pull/498)
- `2026-09-12` The project writers check what they are about to write. [#485](https://github.com/saworbit/didi/issues/485) · [#490](https://github.com/saworbit/didi/issues/490) · [PR #497](https://github.com/saworbit/didi/pull/497)
- `2026-09-12` Every error says what kind of failure it is, in the same place. [#486](https://github.com/saworbit/didi/issues/486) · [#487](https://github.com/saworbit/didi/issues/487) · [#492](https://github.com/saworbit/didi/issues/492) · [PR #496](https://github.com/saworbit/didi/pull/496)
- `2026-09-12` `scene_get_hierarchy` answers the question it was asked. [#482](https://github.com/saworbit/didi/issues/482) · [#483](https://github.com/saworbit/didi/issues/483) · [#484](https://github.com/saworbit/didi/issues/484) · [PR #495](https://github.com/saworbit/didi/pull/495)
- `2026-09-12` Every tool parameter says what it is. [#462](https://github.com/saworbit/didi/issues/462) · [PR #481](https://github.com/saworbit/didi/pull/481)
- `2026-09-12` A confirmation skipped by YOLO mode, or offered to a person, reports the preview's own refusal. [#463](https://github.com/saworbit/didi/issues/463) · [PR #480](https://github.com/saworbit/didi/pull/480)
- `2026-09-12` `scene_call_method`'s dry run reads the call instead of a property. [#463](https://github.com/saworbit/didi/issues/463) · [PR #480](https://github.com/saworbit/didi/pull/480)
- `2026-09-12` `signal_list_connections` marks the editor's own listeners. [#461](https://github.com/saworbit/didi/issues/461) · [PR #479](https://github.com/saworbit/didi/pull/479)
- `2026-09-12` `ui_list_controls` and `ui_hit_test` name the subtree they actually covered. [#470](https://github.com/saworbit/didi/issues/470) · [PR #479](https://github.com/saworbit/didi/pull/479)
- `2026-09-12` `scene_get_group_members` returns the group names a scene actually uses. [#472](https://github.com/saworbit/didi/issues/472) · [PR #479](https://github.com/saworbit/didi/pull/479)
- `2026-09-12` `project_set_setting` checks the setting name against the engine. [#464](https://github.com/saworbit/didi/issues/464) · [PR #478](https://github.com/saworbit/didi/pull/478)
- `2026-09-12` `scene_instantiate_node` refuses a request that names nothing to instantiate. [#471](https://github.com/saworbit/didi/issues/471) · [PR #477](https://github.com/saworbit/didi/pull/477)
- `2026-09-12` `res://.didi/` is not listed or searched as project content. [#468](https://github.com/saworbit/didi/issues/468) · [PR #476](https://github.com/saworbit/didi/pull/476)
- `2026-09-12` `project_search_symbols` counts a file it reached and could not read symbols from. [#469](https://github.com/saworbit/didi/issues/469) · [PR #476](https://github.com/saworbit/didi/pull/476)
- `2026-09-12` `resource_create` refuses a `resource_type` Godot does not know. [#465](https://github.com/saworbit/didi/issues/465) · [PR #475](https://github.com/saworbit/didi/pull/475)
- `2026-09-12` `resource_create`'s `property_check` says which engine it checked against. [#466](https://github.com/saworbit/didi/issues/466) · [PR #475](https://github.com/saworbit/didi/pull/475)
- `2026-09-12` `resource_inspect` reads the type out of the file. [#467](https://github.com/saworbit/didi/issues/467) · [PR #475](https://github.com/saworbit/didi/pull/475)
- `2026-09-12` `resource_create` answers a bad `save_path` with the error envelope, the defect [#460](https://github.com/saworbit/didi/issues/460) fixed elsewhere. [PR #475](https://github.com/saworbit/didi/pull/475)
- `2026-09-12` Eight tools answer a path-validation failure with the error envelope. [#460](https://github.com/saworbit/didi/issues/460) · [PR #474](https://github.com/saworbit/didi/pull/474)
- `2026-09-12` Live scene answers name the scene they describe, and `scene_create` says it changed which one that is. [#448](https://github.com/saworbit/didi/issues/448) · [PR #459](https://github.com/saworbit/didi/pull/459)
- `2026-09-12` Every semantic failure in the Phase 7 bridge says a sentence, and a node of the wrong type is told apart from a path that resolves to nothing. [#441](https://github.com/saworbit/didi/issues/441) · [#443](https://github.com/saworbit/didi/issues/443) · [PR #458](https://github.com/saworbit/didi/pull/458)
- `2026-09-12` The verification sandbox says which repository it used, and refuses one that merely encloses the project. [#450](https://github.com/saworbit/didi/issues/450) · [PR #457](https://github.com/saworbit/didi/pull/457)
- `2026-09-12` `project_apply_changes` fails in the same shape `project_verify_changes` does. [#449](https://github.com/saworbit/didi/issues/449) · [PR #457](https://github.com/saworbit/didi/pull/457)
- `2026-09-12` A number no float property can hold is refused rather than written as `inf`. [#437](https://github.com/saworbit/didi/issues/437) · [PR #455](https://github.com/saworbit/didi/pull/455)
- `2026-09-12` `resource_create` checks property names against the type before it writes anything. [#444](https://github.com/saworbit/didi/issues/444) · [PR #454](https://github.com/saworbit/didi/pull/454)
- `2026-09-12` `script_patch_method` reads the replacement before it writes it, and keeps the declaration where it found it. [#438](https://github.com/saworbit/didi/issues/438) · [#439](https://github.com/saworbit/didi/issues/439) · [#440](https://github.com/saworbit/didi/issues/440) · [PR #452](https://github.com/saworbit/didi/pull/452)
- `2026-09-11` The schema layer enforces the shapes it publishes, so `tilemap_set_cells` names the field the way `gridmap_set_cells` always has. [#442](https://github.com/saworbit/didi/issues/442) · [PR #456](https://github.com/saworbit/didi/pull/456)
- `2026-09-11` Three request edges below `tools/call` answer the way the specification says. [#445](https://github.com/saworbit/didi/issues/445) · [#446](https://github.com/saworbit/didi/issues/446) · [#447](https://github.com/saworbit/didi/issues/447) · [PR #453](https://github.com/saworbit/didi/pull/453)
- `2026-09-11` A dry run reads its target, so a preview of a mutation that cannot succeed is no longer shaped like a preview of one that will. [#417](https://github.com/saworbit/didi/issues/417) · [PR #436](https://github.com/saworbit/didi/pull/436)
- `2026-09-11` Work that was never engine work is no longer reported as a fallback. [#419](https://github.com/saworbit/didi/issues/419) · [PR #435](https://github.com/saworbit/didi/pull/435)
- `2026-09-11` `project_audit_assets` does not call third-party addon files orphans. [#427](https://github.com/saworbit/didi/issues/427) · [PR #434](https://github.com/saworbit/didi/pull/434)
- `2026-09-11` Every semantic failure carries a code. [#420](https://github.com/saworbit/didi/issues/420) · [PR #433](https://github.com/saworbit/didi/pull/433)
- `2026-09-11` `viewport_toggle_debug_draw` and `viewport_set_camera_transform` say what was wrong instead of returning a C++ identifier. [#424](https://github.com/saworbit/didi/issues/424) · [PR #433](https://github.com/saworbit/didi/pull/433)
- `2026-09-11` `resource_inspect` tells a directory from a path with nothing behind it. [#426](https://github.com/saworbit/didi/issues/426) · [PR #433](https://github.com/saworbit/didi/pull/433)
- `2026-09-11` `project_analyze_impact` reads every `project.godot` setting that holds a path, not only `[autoload]`. [#421](https://github.com/saworbit/didi/issues/421) · [PR #432](https://github.com/saworbit/didi/pull/432)
- `2026-09-11` `project_search_text` reads the text formats a project keeps references in, and counts what it did not read. [#422](https://github.com/saworbit/didi/issues/422) · [PR #432](https://github.com/saworbit/didi/pull/432)
- `2026-09-11` The overwrite gate arms on the target, not on the flag. [#425](https://github.com/saworbit/didi/issues/425) · [PR #431](https://github.com/saworbit/didi/pull/431)
- `2026-09-11` A tool's arguments are closed by default, so a typo'd property name is refused rather than ignored. [#418](https://github.com/saworbit/didi/issues/418) · [PR #430](https://github.com/saworbit/didi/pull/430)
- `2026-09-11` `pattern` and `uniqueItems` are enforced, having been published at 22 sites and checked at none. [#423](https://github.com/saworbit/didi/issues/423) · [PR #430](https://github.com/saworbit/didi/pull/430)
- `2026-09-11` Symbol scanning keeps a name that is not spelled in ASCII. [#416](https://github.com/saworbit/didi/issues/416) · [PR #429](https://github.com/saworbit/didi/pull/429)
- `2026-09-11` `script_reflect_class` no longer gives advice it cannot honour, and says when the pinned API is not the engine you are running. [#405](https://github.com/saworbit/didi/issues/405) · [PR #414](https://github.com/saworbit/didi/pull/414)
- `2026-09-11` `viewport_create_test_lab` names `runtime_launch` in the message it returns. [#408](https://github.com/saworbit/didi/issues/408) · [PR #412](https://github.com/saworbit/didi/pull/412)
- `2026-09-11` Offline `scene_get_hierarchy` no longer answers a different question than the one asked. [#401](https://github.com/saworbit/didi/issues/401) · [PR #411](https://github.com/saworbit/didi/pull/411)
- `2026-09-11` `project_list_export_presets` reports no presets instead of a file error. [#403](https://github.com/saworbit/didi/issues/403) · [PR #411](https://github.com/saworbit/didi/pull/411)
- `2026-09-11` `project_analyze_impact` says whether the target exists. [#404](https://github.com/saworbit/didi/issues/404) · [PR #411](https://github.com/saworbit/didi/pull/411)
- `2026-09-11` A confirmation token is no longer consumed by an attempt that failed its own binding check. [#398](https://github.com/saworbit/didi/issues/398) · [PR #410](https://github.com/saworbit/didi/pull/410)
- `2026-09-11` The mutation gate no longer calls its preview exact. [#407](https://github.com/saworbit/didi/issues/407) · [PR #410](https://github.com/saworbit/didi/pull/410)
- `2026-09-11` `tools/call` now checks the `inputSchema` each tool publishes before anything dispatches. [#397](https://github.com/saworbit/didi/issues/397) · [PR #409](https://github.com/saworbit/didi/pull/409)
- `2026-09-11` `viewport_capture_passes` publishes the segmentation pass it has always drawn. [PR #409](https://github.com/saworbit/didi/pull/409)
- `2026-09-11` `scene_add_to_group` and `scene_remove_from_group` no longer target the edited scene root when `target_node` is missing. [#396](https://github.com/saworbit/didi/issues/396) · [PR #409](https://github.com/saworbit/didi/pull/409)
- `2026-09-11` A wrong argument type no longer reads as a server fault. [#400](https://github.com/saworbit/didi/issues/400) · [PR #409](https://github.com/saworbit/didi/pull/409)
- `2026-09-11` Live Phase 7 tools no longer answer a bad argument with only a machine token. [#406](https://github.com/saworbit/didi/issues/406) · [PR #409](https://github.com/saworbit/didi/pull/409)
- `2026-09-11` `dry_run` no longer mints a confirmation token for arguments the tool would refuse. [#399](https://github.com/saworbit/didi/issues/399) · [PR #409](https://github.com/saworbit/didi/pull/409)
- `2026-09-11` `prompts/get` now requires the arguments `prompts/list` marks required. [#402](https://github.com/saworbit/didi/issues/402) · [PR #409](https://github.com/saworbit/didi/pull/409)
- `2026-09-10` `runtime_attach_session` no longer attaches a session belonging to a different project than the server's root. [#387](https://github.com/saworbit/didi/issues/387) · [PR #391](https://github.com/saworbit/didi/pull/391)
- `2026-09-10` `didi_control_room` no longer reports another project's session as this project's, and the Project light is a real preflight. [#388](https://github.com/saworbit/didi/issues/388) · [PR #391](https://github.com/saworbit/didi/pull/391)
- `2026-09-10` `scene_create` and `scene_pack_branch` no longer write a uid the engine never learns. [#379](https://github.com/saworbit/didi/issues/379) · [PR #390](https://github.com/saworbit/didi/pull/390)
- `2026-09-10` `script_check_syntax` no longer reports a false error for every script that names an autoload. [#383](https://github.com/saworbit/didi/issues/383) · [PR #386](https://github.com/saworbit/didi/pull/386)
- `2026-09-10` Didi can now enable its own addon in a project that does not have it. [#382](https://github.com/saworbit/didi/issues/382) · [PR #385](https://github.com/saworbit/didi/pull/385)
- `2026-09-10` The extension no longer leaks one ObjectDB instance on a clean engine exit. [#373](https://github.com/saworbit/didi/issues/373) · [PR #377](https://github.com/saworbit/didi/pull/377)
- `2026-09-10` `asset_reimport` no longer reports success for a path Godot has no importer for. [#374](https://github.com/saworbit/didi/issues/374) · [PR #376](https://github.com/saworbit/didi/pull/376)
- `2026-09-10` `scene_create` creates the parent directory of a nested scene path instead of failing with Error 19. [#372](https://github.com/saworbit/didi/issues/372) · [PR #376](https://github.com/saworbit/didi/pull/376)
- `2026-09-10` The offline `missing_colon` rule no longer reads `hits += 1` as an `else` missing its colon. [#371](https://github.com/saworbit/didi/issues/371) · [PR #375](https://github.com/saworbit/didi/pull/375)

---

## [1.8.0] - 2026-09-10

Full write-ups for 1.8.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#180---2026-09-10).

### Added

- `2026-09-09` A field trial can be run unattended. [PR #364](https://github.com/saworbit/didi/pull/364)
- `2026-09-09` `didi --version` prints the build id under the release version. [PR #364](https://github.com/saworbit/didi/pull/364)
- `2026-09-09` The live harness now proves the loop composed rather than in halves. [PR #360](https://github.com/saworbit/didi/pull/360)
- `2026-09-09` Fuzz targets for the three places Didi reads bytes it did not write: the IPC frame decoder, the JSON-RPC request parser, and base64. [PR #349](https://github.com/saworbit/didi/pull/349)

### Changed

- `2026-09-09` A viewport diff converts each image's pixels to luma once instead of twice. [#354](https://github.com/saworbit/didi/issues/354) · [PR #367](https://github.com/saworbit/didi/pull/367)
- `2026-09-09` `base64::decode` reserves its output. [#357](https://github.com/saworbit/didi/issues/357) · [PR #367](https://github.com/saworbit/didi/pull/367)
- `2026-09-09` `project_analyze_impact`, `project_rename_references` and `project_analyze_bloat` no longer crawl the whole project from scratch. [#355](https://github.com/saworbit/didi/issues/355) · [PR #366](https://github.com/saworbit/didi/pull/366)
- `2026-09-09` Node path impact analysis skips a file that cannot mention the target. [#356](https://github.com/saworbit/didi/issues/356) · [PR #366](https://github.com/saworbit/didi/pull/366)
- `2026-09-09` The three API-blocked names now say what to use instead. [PR #360](https://github.com/saworbit/didi/pull/360)
- `2026-09-09` CodeQL now runs on every pull request rather than on a path filter. [PR #349](https://github.com/saworbit/didi/pull/349)
- `2026-09-09` All ten of CodeQL's first-pass findings were triaged and dismissed with written reasons rather than left open. [PR #349](https://github.com/saworbit/didi/pull/349)

### Fixed

- `2026-09-10` Seeding a field trial no longer tries to execute a file that is not a program, and no longer waits forever when a probe does not come back. [PR #370](https://github.com/saworbit/didi/pull/370)
- `2026-09-10` `ctest` says where it stopped. [PR #370](https://github.com/saworbit/didi/pull/370)
- `2026-09-10` Every pull request now runs `ctest` against the same interpreter the release hands CMake. [PR #370](https://github.com/saworbit/didi/pull/370)
- `2026-09-10` Every pull request now runs `ctest`, which is what gates a tag. [PR #370](https://github.com/saworbit/didi/pull/370)
- `2026-09-10` `Tools.OfflineCapabilityIsDerived` sets up the tool registry it reads instead of inheriting whatever an earlier test left there. [PR #368](https://github.com/saworbit/didi/pull/368)
- `2026-09-09` `editor_reload_project` re-indexes the offline caches it says it re-indexed. [#358](https://github.com/saworbit/didi/issues/358) · [PR #366](https://github.com/saworbit/didi/pull/366)
- `2026-09-09` The shared resource index is keyed on the directory rather than on the spelling of it. [PR #366](https://github.com/saworbit/didi/pull/366)
- `2026-09-09` The native suite no longer leaves its checkpoint fixtures in the temporary directory when a run dies inside a test. [#363](https://github.com/saworbit/didi/issues/363) · [PR #365](https://github.com/saworbit/didi/pull/365)
- `2026-09-09` Offline tools no longer hand their child processes the server's standard input. [#350](https://github.com/saworbit/didi/issues/350) · [PR #362](https://github.com/saworbit/didi/pull/362)
- `2026-09-09` The test runner binds spawned processes so a timeout cannot orphan them. [#351](https://github.com/saworbit/didi/issues/351) · [PR #362](https://github.com/saworbit/didi/pull/362)
- `2026-09-09` `resource_create` validates its target with the shared project path rules instead of its own copy. [#352](https://github.com/saworbit/didi/issues/352) · [PR #362](https://github.com/saworbit/didi/pull/362)
- `2026-09-09` `scene_get_selection` is no longer listed as an offline capability. [#353](https://github.com/saworbit/didi/issues/353) · [PR #362](https://github.com/saworbit/didi/pull/362)
- `2026-09-09` The workflows that assert the pinned `jsonschema` version no longer read `jsonschema.__version__`. [PR #361](https://github.com/saworbit/didi/pull/361)

---

## [1.7.0] - 2026-09-09

Full write-ups for 1.7.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#170---2026-09-09).

### Added

- `2026-09-09` Signed releases: every archive carries SLSA build provenance signed through Sigstore, plus a `SHA256SUMS` file. [PR #341](https://github.com/saworbit/didi/pull/341)
- `2026-09-09` A rehearsal for the release pipeline. [PR #341](https://github.com/saworbit/didi/pull/341)
- `2026-09-09` Repository automation and supply-chain hardening. [PR #339](https://github.com/saworbit/didi/pull/339)
- `2026-09-08` Added `ui_list_controls`: every Control under a root, with its rectangle, class, visibility and text. [PR #309](https://github.com/saworbit/didi/pull/309)
- `2026-09-08` Added the Control Room, a dashboard of red, amber and green lights for clients that support MCP Apps. [PR #308](https://github.com/saworbit/didi/pull/308)
- `2026-09-08` Added opt-in managed editor recovery. [PR #307](https://github.com/saworbit/didi/pull/307)

### Changed

- `2026-09-09` The Linux release artifact is built *inside* Ubuntu 22.04 rather than *on* it. [PR #341](https://github.com/saworbit/didi/pull/341)
- `2026-09-09` Corrected the documented Linux minimum in [Administrator Guide](docs/ADMIN_GUIDE.md) from Ubuntu 20.04+ to Ubuntu 22.04+ / glibc 2.35+. [PR #341](https://github.com/saworbit/didi/pull/341)
- `2026-09-09` CI decides what to run instead of running everything. [PR #339](https://github.com/saworbit/didi/pull/339)
- `2026-09-09` The pinned `jsonschema` version is read out of `requirements-dev.txt` by the workflows that assert it, rather than typed into all three places. [PR #339](https://github.com/saworbit/didi/pull/339)
- `2026-09-09` The release job publishes with `gh` rather than a third-party action. [PR #339](https://github.com/saworbit/didi/pull/339)
- `2026-09-09` `project_audit_assets` now verifies both kinds of broken reference against the running editor, not just UID ones. [PR #324](https://github.com/saworbit/didi/pull/324)

### Fixed

- `2026-09-09` `didi::ipc::parseFramedMessage` accepted a frame whose length field was near `UINT32_MAX` and then read gigabytes past the end of the buffer it was given. [PR #344](https://github.com/saworbit/didi/pull/344)
- `2026-09-09` `project_get_uid_map` and `project_audit_assets` work again without a Godot session. [PR #324](https://github.com/saworbit/didi/pull/324)
- `2026-09-09` `project_audit_assets` checks its unresolved UID findings against the running editor instead of leaving them as guesses. [PR #323](https://github.com/saworbit/didi/pull/323)
- `2026-09-09` `project_get_uid_map` takes a `resolve` list and answers it from the engine. [PR #322](https://github.com/saworbit/didi/pull/322)
- `2026-09-08` `scene_close` no longer demands `discard_unsaved: true` for a scene the engine says is clean. [PR #321](https://github.com/saworbit/didi/pull/321)
- `2026-09-08` One Didi process can drive several Godot sessions at once. [PR #318](https://github.com/saworbit/didi/pull/318)
- `2026-09-08` A request no longer inherits a Godot session it never chose. [PR #318](https://github.com/saworbit/didi/pull/318)
- `2026-09-08` A modern request is validated before it is dispatched. [PR #318](https://github.com/saworbit/didi/pull/318)
- `2026-09-08` MCP Apps is negotiated per request again. [PR #318](https://github.com/saworbit/didi/pull/318)
- `2026-09-08` A live failure no longer publishes the session endpoint. [PR #310](https://github.com/saworbit/didi/pull/310)
- `2026-09-08` The managed editor does not outlive the host that owns it. [#305](https://github.com/saworbit/didi/issues/305) · [PR #306](https://github.com/saworbit/didi/pull/306)
- `2026-09-07` Ctrl+C stops the server. [PR #302](https://github.com/saworbit/didi/pull/302)
- `2026-09-07` The perceptual hash uses all 64 bits it reports. [PR #302](https://github.com/saworbit/didi/pull/302)
- `2026-09-07` `DIDI_BUILD_TESTS=OFF` builds no tests. [PR #302](https://github.com/saworbit/didi/pull/302)

---

## [1.6.0] - 2026-09-06

Full write-ups for 1.6.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#160---2026-09-06).

### Added

- `2026-09-06` `runtime_explore_scene` drives a running game and reports what happened. [PR #277](https://github.com/saworbit/didi/pull/277)
- `2026-09-06` `project_verify_changes` now takes `run_scene`, and a new `project_apply_changes` writes a proposal into the working tree once it has passed. [PR #277](https://github.com/saworbit/didi/pull/277)
- `2026-09-06` `viewport_capture_passes` takes a `segmentation` pass. [#141](https://github.com/saworbit/didi/issues/141) · [PR #266](https://github.com/saworbit/didi/pull/266)
- `2026-09-05` The Godot editor plugin now carries a console. [PR #262](https://github.com/saworbit/didi/pull/262)
- `2026-09-05` The console can close and reopen the live bridge from a switch. [PR #262](https://github.com/saworbit/didi/pull/262)
- `2026-09-05` A Log page, with two sources and no third invented one. [PR #262](https://github.com/saworbit/didi/pull/262)
- `2026-09-05` Connect writes the launch configuration for Claude Code, Cursor, Claude Desktop and VS Code, and Settings holds what it carries. [PR #262](https://github.com/saworbit/didi/pull/262)
- `2026-09-05` Automatic detection of the `didi` binary deliberately skips anything inside the project. [PR #262](https://github.com/saworbit/didi/pull/262)
- `2026-09-05` Editor preferences live in Godot's `EditorSettings`, outside the project, and the console never shows a session token. [PR #262](https://github.com/saworbit/didi/pull/262)
- `2026-09-05` Added `project_verify_changes`, which checks a set of proposed file contents together in an isolated copy of the project. [PR #257](https://github.com/saworbit/didi/pull/257)
- `2026-09-05` A transport failure on a route with a known session now reports `error.data.engine` as `alive`, `gone`, or `unknown`. [PR #256](https://github.com/saworbit/didi/pull/256)
- `2026-09-05` `didi --dump-tool-manifest` emits every tool's required fields, and the validator checks the docs name each one. [PR #254](https://github.com/saworbit/didi/pull/254)
- `2026-09-05` Added `editor_render_ghost_preview` and `editor_clear_ghost_previews`: wireframe boxes that show where a change would land. [#147](https://github.com/saworbit/didi/issues/147) · [PR #253](https://github.com/saworbit/didi/pull/253)
- `2026-09-05` Added `viewport_capture_passes`, which returns depth and normal images alongside the colour frame. [PR #252](https://github.com/saworbit/didi/pull/252)
- `2026-09-05` Added `spatial_query_frustum`, which lists the 3D nodes inside a camera frustum in the attached session, nearest first. [#142](https://github.com/saworbit/didi/issues/142) · [PR #249](https://github.com/saworbit/didi/pull/249)
- `2026-09-05` Added `shader_get_visual_graph`, which returns a VisualShader's nodes and connections per shader type as structured JSON. [#119](https://github.com/saworbit/didi/issues/119) · [PR #246](https://github.com/saworbit/didi/pull/246)
- `2026-09-05` Added `shader_set_uniform`, the write half of `shader_list_uniforms`. [PR #245](https://github.com/saworbit/didi/pull/245)
- `2026-09-05` Added `shader_list_uniforms`, which reads the shader uniforms of a `ShaderMaterial` held by a node in the edited scene. [PR #244](https://github.com/saworbit/didi/pull/244)
- `2026-09-05` Added `spatial_query_clearance`, which sweeps a box, sphere or capsule along a path and reports how far it gets. [PR #242](https://github.com/saworbit/didi/pull/242)
- `2026-09-05` Added `spatial_query_raycast_batch`, which casts up to 64 rays against the attached session's physics world in one dispatch. [PR #241](https://github.com/saworbit/didi/pull/241)
- `2026-09-04` Added `runtime_watch_invariants`, which watches declared conditions every frame of a running game and stops the game on the frame that breaks one. [PR #240](https://github.com/saworbit/didi/pull/240)
- `2026-09-04` Added `project_rename_references`, which renames a symbol in the places Godot serializes it. [#145](https://github.com/saworbit/didi/issues/145) · [PR #239](https://github.com/saworbit/didi/pull/239)
- `2026-09-04` `scene_set_property` and the `properties` argument of `scene_instantiate_node` take Vector2, Vector2i, Vector3, Vector3i, Color and Resource paths. [#210](https://github.com/saworbit/didi/issues/210) · [PR #236](https://github.com/saworbit/didi/pull/236)
- `2026-09-04` Added `script_create`, which writes a GDScript file under the project root. [PR #234](https://github.com/saworbit/didi/pull/234)

### Fixed

- `2026-09-06` `project_verify_changes` no longer reports every proposal as broken in a project that names a resource by `uid://`. [PR #278](https://github.com/saworbit/didi/pull/278)
- `2026-09-06` The build no longer races itself placing the class reference. [PR #276](https://github.com/saworbit/didi/pull/276)
- `2026-09-06` A blackboard operation waits out a lock somebody else is holding instead of reporting an error. [PR #275](https://github.com/saworbit/didi/pull/275)
- `2026-09-06` Every page under `docs/` is reachable again. [PR #274](https://github.com/saworbit/didi/pull/274)
- `2026-09-06` The `[Unreleased]` section had 18 `### Added` and 10 `### Fixed` headings; consolidated into one of each. [PR #274](https://github.com/saworbit/didi/pull/274)
- `2026-09-06` `docs/TOOL_REFERENCE.md` names every directory project search skips, including `build-` trees. [PR #274](https://github.com/saworbit/didi/pull/274)
- `2026-09-06` A blackboard save can no longer destroy the board. [PR #274](https://github.com/saworbit/didi/pull/274)
- `2026-09-06` Three tools no longer fail on a path the active Windows code page cannot hold. [PR #274](https://github.com/saworbit/didi/pull/274)
- `2026-09-06` `script_create` and `script_patch_method` report the Godot compiler's diagnostics again. [PR #274](https://github.com/saworbit/didi/pull/274)
- `2026-09-06` The resource index and project search no longer scan out-of-source build trees. [PR #274](https://github.com/saworbit/didi/pull/274)
- `2026-09-06` `script_patch_method` and `create_visual_test_lab` drop the shared resource index after they write. [PR #274](https://github.com/saworbit/didi/pull/274)
- `2026-09-06` A transport failure reporting `engine: unknown` now says why. [PR #267](https://github.com/saworbit/didi/pull/267)
- `2026-09-06` The live harness reports the editor's exit code when a request in the themed-Control block fails. [PR #267](https://github.com/saworbit/didi/pull/267)
- `2026-09-05` `docs/INTEGRATION_GUIDE.md` was reachable from no document in the repository. [PR #265](https://github.com/saworbit/didi/pull/265)
- `2026-09-05` `docs/REALIGNMENT_IMPLEMENTATION_PLAN.md` carries a status banner saying what shipped, instead of 37 unticked boxes. [PR #265](https://github.com/saworbit/didi/pull/265)
- `2026-09-05` Brought the documentation into line with the editor console rather than leaving eight pages describing a plugin that only printed a line at startup. [PR #264](https://github.com/saworbit/didi/pull/264)
- `2026-09-05` A Godot that Didi starts to answer a question no longer publishes a runtime session. [PR #263](https://github.com/saworbit/didi/pull/263)
- `2026-09-05` `project_verify_changes` reported a script with a plain syntax error as fine. [PR #263](https://github.com/saworbit/didi/pull/263)
- `2026-09-05` A live call that changes nothing is now sent once more, on a new connection to the same session, when the transport fails. [PR #261](https://github.com/saworbit/didi/pull/261)
- `2026-09-05` Removed a message prefix that nothing can emit any more. [PR #256](https://github.com/saworbit/didi/pull/256)
- `2026-09-05` A transport failure could not say why it failed. [PR #255](https://github.com/saworbit/didi/pull/255)
- `2026-09-05` `docs/TOOL_REFERENCE.md` told readers that `project_export` takes a preset `name`. [PR #254](https://github.com/saworbit/didi/pull/254)
- `2026-09-05` `shader_list_uniforms` reports a shader's declared default instead of `null` for uniforms a material does not override. [#119](https://github.com/saworbit/didi/issues/119) · [PR #251](https://github.com/saworbit/didi/pull/251)
- `2026-09-04` A live tool call no longer fails at the moment a connection is recycled. [PR #237](https://github.com/saworbit/didi/pull/237)
- `2026-09-04` A client no longer gives up connecting when the endpoint momentarily has no instances. [PR #237](https://github.com/saworbit/didi/pull/237)
- `2026-09-04` A server no longer applies a frame-arrival deadline to writing a response. [PR #237](https://github.com/saworbit/didi/pull/237)
- `2026-09-04` `viewport_capture_frame` works on a game session. [PR #235](https://github.com/saworbit/didi/pull/235)
- `2026-09-04` `viewport_capture_frame` refuses a viewport that has no size instead of returning it as a successful live frame. [PR #235](https://github.com/saworbit/didi/pull/235)
- `2026-09-04` `resource_create` refuses a `save_path` that is not `.tres` or `.res`. [PR #234](https://github.com/saworbit/didi/pull/234)

---

## [1.5.0] - 2026-09-03

Full write-ups for 1.5.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#150---2026-09-03).

### Added

- `2026-09-03` Added `scene_get_selection`, which reports the nodes selected in the Godot editor. [PR #201](https://github.com/saworbit/didi/pull/201)
- `2026-09-03` Exposed boards as subscribable MCP resources. [PR #201](https://github.com/saworbit/didi/pull/201)
- `2026-09-03` Added task allocation on the blackboard. [PR #201](https://github.com/saworbit/didi/pull/201)
- `2026-09-03` Added a shared blackboard. [PR #201](https://github.com/saworbit/didi/pull/201)
- `2026-09-03` Extended `project_audit_assets` with bounded, read-only Godot `.import` health evidence. [PR #188](https://github.com/saworbit/didi/pull/188)
- `2026-09-03` Started Phase 8 with exact static node-path blast-radius analysis in `project_analyze_impact`. [PR #187](https://github.com/saworbit/didi/pull/187)
- `2026-09-03` Hardened the Phase 7 delivery after independent red/purple review. [PR #186](https://github.com/saworbit/didi/pull/186)
- `2026-09-03` Delivered `tilemap_set_cells`, `tilemap_get_used_rect` and `gridmap_set_cells`: 80/83 tools implemented. [PR #185](https://github.com/saworbit/didi/pull/185)
- `2026-09-03` Delivered `viewport_set_camera_transform` and `viewport_toggle_debug_draw` (Phase 7A viewport half): 77/83 implemented. [PR #184](https://github.com/saworbit/didi/pull/184)
- `2026-09-02` Delivered `anim_list_tracks` and `anim_play_track` as the second half of Phase 7B, taking the surface to 75/83 implemented with 8 names still reserved. [PR #183](https://github.com/saworbit/didi/pull/183)
- `2026-09-02` Delivered `physics_raycast_query` and `nav_query_path` as Phase 7B partial delivery, taking the surface to 73/83 implemented with 10 names still reserved. [PR #182](https://github.com/saworbit/didi/pull/182)
- `2026-09-02` Delivered `runtime_inject_input`, so an agent can press keys and buttons in a running game: 71/83 implemented. [PR #181](https://github.com/saworbit/didi/pull/181)
- `2026-09-02` Delivered `runtime_read_profiler` as Phase 7C partial delivery, taking the surface to 70/83 implemented with 13 names still reserved. [#116](https://github.com/saworbit/didi/issues/116) · [PR #180](https://github.com/saworbit/didi/pull/180)
- `2026-08-31` Added `audio_configure_bus`, which sets a bus volume, mute or solo on the running engine. [PR #179](https://github.com/saworbit/didi/pull/179)
- `2026-08-31` Delivered the four signal tools (`signal_list_connections`, `signal_connect`, `signal_disconnect`, `signal_emit`): 69/83 implemented. [PR #179](https://github.com/saworbit/didi/pull/179)
- `2026-08-31` Added `audio_list_buses`. [PR #178](https://github.com/saworbit/didi/pull/178)
- `2026-08-31` Added `project_analyze_impact`, which answers what else changes if this changes. [PR #175](https://github.com/saworbit/didi/pull/175)
- `2026-08-31` Added `project_audit_assets`, which reports unreferenced assets, broken references and signals nothing uses. [#146](https://github.com/saworbit/didi/issues/146) · [PR #171](https://github.com/saworbit/didi/pull/171)
- `2026-08-31` Added specification tool `annotations` to every registered tool. [PR #163](https://github.com/saworbit/didi/pull/163)
- `2026-08-30` The documentation validator now requires every `tests/test_*.py` module to be named by some workflow. [PR #110](https://github.com/saworbit/didi/pull/110)
- `2026-08-30` Added YOLO mode: `--yolo`, or `DIDI_YOLO=1`, skips confirmation on destructive tools for unattended runs. [PR #109](https://github.com/saworbit/didi/pull/109)
- `2026-08-30` Confirmation for destructive tools can now reach a human. [PR #108](https://github.com/saworbit/didi/pull/108)
- `2026-08-30` A client that cannot elicit is not silently downgraded. [PR #108](https://github.com/saworbit/didi/pull/108)
- `2026-08-30` Didi now serves MCP revision `2026-07-28` alongside `2024-11-05`. [PR #107](https://github.com/saworbit/didi/pull/107)
- `2026-08-30` Discovery advertises only revisions Didi actually serves, and that is enforced rather than asserted. [PR #107](https://github.com/saworbit/didi/pull/107)
- `2026-08-30` Added `server/discover`, making Didi dual-era. [PR #106](https://github.com/saworbit/didi/pull/106)
- `2026-08-30` The signal test seams stay compiled out of shipping builds, and a test asserts it. [PR #103](https://github.com/saworbit/didi/pull/103)
- `2026-08-30` Added `runtime_read_output`, which reads what the engine printed rather than what Didi recorded. [PR #101](https://github.com/saworbit/didi/pull/101)
- `2026-08-30` Added `outputSchema` to every tool whose result shape has been observed, checked against real payloads. [PR #98](https://github.com/saworbit/didi/pull/98)
- `2026-08-30` Added `structuredContent` to successful JSON tool results, carrying the same payload as the text block after execution-mode and session attribution. [PR #96](https://github.com/saworbit/didi/pull/96)
- `2026-08-30` Added `didi --dump-tool-manifest`, which emits the registered tool surface as sorted, byte-stable JSON with counts and names. [PR #83](https://github.com/saworbit/didi/pull/83)
- `2026-08-30` Added `kLegacyToolNames` as the single declaration of which registrations are legacy. [PR #83](https://github.com/saworbit/didi/pull/83)
- `2026-08-30` Added `--list` and `--filter=<substring>` to the native test runner, so a single case can be run in isolation. [PR #83](https://github.com/saworbit/didi/pull/83)
- `2026-08-30` Added [docs/SURFACE_AMENDMENTS.md](docs/SURFACE_AMENDMENTS.md), the record through which the canonical tool surface may grow. [PR #83](https://github.com/saworbit/didi/pull/83)
- `2026-08-29` Completed the 2026-08-29 Phase 7 feasibility gate on Godot 4.5.1 and 4.7.2. [PR #64](https://github.com/saworbit/didi/pull/64)
- `2026-08-29` Added the approved Phase 7-12 roadmap, including canonical-surface completion and governance requirements for all future phases. [PR #63](https://github.com/saworbit/didi/pull/63)
- `2026-08-29` Closed Phase 6 without expanding the protocol surface. [PR #52](https://github.com/saworbit/didi/pull/52)
- `2026-08-28` Closed Phase 5 with six canonical tools. [PR #45](https://github.com/saworbit/didi/pull/45)
- `2026-08-28` Added a cross-platform argv-only process runner with deadlines, child-group termination and a 1 MiB output cap. [PR #45](https://github.com/saworbit/didi/pull/45)

### Changed

- `2026-09-03` Took the version out of the C++ sources. [#38](https://github.com/saworbit/didi/issues/38) · [PR #197](https://github.com/saworbit/didi/pull/197)
- `2026-09-03` Discovery now exposes 83 canonical tools plus 10 legacy registrations (93 total). [PR #185](https://github.com/saworbit/didi/pull/185)
- `2026-09-03` Phase 7 status is `PARTIAL_DELIVERY`. [PR #185](https://github.com/saworbit/didi/pull/185)
- `2026-08-30` The validator derives the Phase 7 status ratio and spelled-out counts from the tool manifest, not literals. [PR #101](https://github.com/saworbit/didi/pull/101)
- `2026-08-30` Corrected the published read-only registration count. [PR #101](https://github.com/saworbit/didi/pull/101)
- `2026-08-30` The live integration harness runs on Windows PowerShell 5.1. [PR #96](https://github.com/saworbit/didi/pull/96)
- `2026-08-30` The documentation validator derives every published tool count from the tool manifest instead of matching hard-coded numbers in prose. [PR #83](https://github.com/saworbit/didi/pull/83)
- `2026-08-30` The CI MCP smoke checks `tools/list` against the same build's manifest and asserts every `implemented` flag. [PR #83](https://github.com/saworbit/didi/pull/83)
- `2026-08-30` Split the fused surface rule: no success stubs stays absolute, and new tool names need a surface amendment. [PR #83](https://github.com/saworbit/didi/pull/83)
- `2026-08-30` Documented that Godot 4.5 and 4.6 expose no scene dirty state and that 4.7 adds `get_unsaved_scenes()`. [PR #83](https://github.com/saworbit/didi/pull/83)
- `2026-08-29` Mutating tool schemas now advertise `dry_run`. [PR #52](https://github.com/saworbit/didi/pull/52)
- `2026-08-28` The Godot 4.5.1 harness covers shader compilation, pack export, MeshLibrary generation and UI hit-testing; 162 native tests. [PR #45](https://github.com/saworbit/didi/pull/45)

### Fixed

- `2026-09-03` Packaged the addon from the build directory instead of the source tree. [PR #195](https://github.com/saworbit/didi/pull/195)
- `2026-09-03` Refused unknown and malformed command-line options at startup instead of ignoring them. [#189](https://github.com/saworbit/didi/issues/189) · [PR #190](https://github.com/saworbit/didi/pull/190)
- `2026-09-03` Corrected documentation that had drifted from the build. [96b436b](https://github.com/saworbit/didi/commit/96b436baea70256a205052be8a040ad979372a84)
- `2026-08-30` Reconciled all current operating documentation with Phase 6. [PR #101](https://github.com/saworbit/didi/pull/101)
- `2026-08-30` Updated the Linux, macOS, and Windows fast MCP smoke to lock the 79-canonical/89-total Phase 5 surface and all six new execution-mode/schema contracts. [PR #101](https://github.com/saworbit/didi/pull/101)
- `2026-08-30` Quarantined the runtime route only on transport failure. [PR #96](https://github.com/saworbit/didi/pull/96)
- `2026-08-30` Gave four order-dependent native tests their own setup. [PR #96](https://github.com/saworbit/didi/pull/96)
- `2026-08-30` Reaped orphaned session descriptor tombstones. [PR #84](https://github.com/saworbit/didi/pull/84)
- `2026-08-28` Godot 4.5 dummy-renderer shader diagnostics parse, and editor routing is restored after long offline work. [PR #45](https://github.com/saworbit/didi/pull/45)
- `2026-08-28` GDScript diagnostics and symbol APIs are string and comment aware, with safe project-confined file handling. [PR #44](https://github.com/saworbit/didi/pull/44)
- `2026-08-28` Declared explicit x86_64, arm64, and universal macOS GDExtension keys. [PR #43](https://github.com/saworbit/didi/pull/43)
- `2026-08-28` Malformed JSON-RPC params, ID-less requests, numeric overflow and bad `Content-Length` framing are rejected safely. [PR #42](https://github.com/saworbit/didi/pull/42)
- `2026-08-28` One reconnect-and-I/O IPC deadline on POSIX, exact response-ID matching, and distinct handler-exception responses. [PR #42](https://github.com/saworbit/didi/pull/42)
- `2026-08-28` `runtime_launch` is bounded to 1 to 120 seconds, with broader Godot discovery and Windows exit code 259 handled. [PR #42](https://github.com/saworbit/didi/pull/42)
- `2026-08-28` Failed closed before creating a Windows session pipe when the owner-and-Administrators security descriptor cannot be built. [PR #41](https://github.com/saworbit/didi/pull/41)
- `2026-08-28` Protected `resource_create` and visual test-lab files from replacement unless callers pass `overwrite: true`. [PR #41](https://github.com/saworbit/didi/pull/41)
- `2026-08-28` Preserved ordinary comments when replacing GDScript symbols. [PR #39](https://github.com/saworbit/didi/pull/39)
- `2026-08-28` Preserved explicit `null` JSON-RPC success results. [PR #39](https://github.com/saworbit/didi/pull/39)

---

## [1.4.0] - 2026-08-28

Full write-ups for 1.4.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#140---2026-08-28).

### Added

- `2026-08-28` Closed Phase 4 with four canonical tools. [PR #6](https://github.com/saworbit/didi/pull/6)
- `2026-08-28` Added 32-lowercase-hex live capture IDs backed by an 8-entry/64 MiB process-local RGBA LRU cache with a 2,048 × 2,048 per-image limit. [PR #6](https://github.com/saworbit/didi/pull/6)
- `2026-08-28` Added reversible `node_isolation_path` capture with optional transparent background. [PR #6](https://github.com/saworbit/didi/pull/6)
- `2026-08-28` Added exact-dimension RGBA diff metrics with transparent PNG output. [PR #6](https://github.com/saworbit/didi/pull/6)

### Changed

- `2026-08-28` Version is now `1.4.0`; discovery exposes 72 canonical tools plus 10 legacy registrations (82 total). [PR #6](https://github.com/saworbit/didi/pull/6)
- `2026-08-28` Project search enforces containment, an extension allowlist, UTF-8 validation, deterministic order and size limits. [PR #6](https://github.com/saworbit/didi/pull/6)
- `2026-08-28` Asset reimport validates the whole batch first, allows one request at a time, and waits for two idle callbacks. [PR #6](https://github.com/saworbit/didi/pull/6)
- `2026-08-28` Carried forward automated version, release-fact, support-policy, and Markdown-link drift validation from the Phase 3 documentation reconciliation. [PR #6](https://github.com/saworbit/didi/pull/6)
- `2026-08-28` Removed agent workflow reports and plans from the tree, with validation to keep them out. [PR #6](https://github.com/saworbit/didi/pull/6)
- `2026-08-28` The Godot 4.5.1 harness covers search, SVG reimport, node isolation and visual diffs. [PR #6](https://github.com/saworbit/didi/pull/6)

### Fixed

- `2026-08-28` Prevented synchronous `EditorFileSystem.reimport_files` callbacks from deadlocking the pending-reimport lifecycle lock. [PR #6](https://github.com/saworbit/didi/pull/6)

---

## [1.3.0] - 2026-08-27

Full write-ups for 1.3.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#130---2026-08-27).

### Added

- `2026-08-27` Closed Phase 3 with ten canonical tools. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` Added atomic schema-1 descriptors and process-unique same-user IPC endpoints for concurrent Godot editor and game sessions. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` Added a 2,000-record cursor log ring with deterministic gap/filter behavior, 16 KiB messages, 64 KiB details, and token/expression-source redaction. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` Added exact paused game stepping with runtime-tree bounds and process identity checks on all three platforms. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` Added strict read-only expression evaluation with an allowlist, deadlines and size limits. [PR #3](https://github.com/saworbit/didi/pull/3)

### Changed

- `2026-08-27` Version is now `1.3.0`; discovery exposes 68 canonical tools plus 10 legacy registrations (78 total). [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` Deterministic same-project auto-attach selects an unambiguous sole session or unique editor; ambiguity remains detached. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` Capability metadata is session-kind-aware. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` Live main-thread work now has a 15-second extension deadline with explicit `not_started` versus `unknown_outcome` results. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` POSIX session discovery now uses `$XDG_RUNTIME_DIR/didi-sessions` when XDG provides an absolute path, otherwise the effective-UID-qualified temporary fallback. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` CI smoke locks the 78-registration surface, Phase 3 metadata, cursor schema and evaluator limits. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` Runtime logging is explicitly scoped to structured Didi events. [PR #3](https://github.com/saworbit/didi/pull/3)
- `2026-08-27` The v1.3.0 release matrix runs the complete native suite plus concurrent editor/game integration coverage on Godot 4.5.1 and 4.7.2. [PR #3](https://github.com/saworbit/didi/pull/3)

---

## [1.2.0] - 2026-08-27

Full write-ups for 1.2.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#120---2026-08-27).

### Added

- `2026-08-27` Phase 2 project wiring: 18 new live tools for scripts, autoloads, input actions, settings, groups and scenes. [PR #2](https://github.com/saworbit/didi/pull/2)
- `2026-08-27` Atomic project persistence through `ProjectSettings.save()` with snapshot rollback and live `InputMap` reload. [PR #2](https://github.com/saworbit/didi/pull/2)
- `2026-08-27` A disposable 119-request Godot integration fixture covering Phase 1 and Phase 2. [PR #2](https://github.com/saworbit/didi/pull/2)
- `2026-08-27` Phase 1 live engine substrate for Godot 4.5+. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Honest capability discovery: every tool and resource says whether it is implemented, and why not. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Cross-version integration harness covering Godot 4.5.1, 4.6.2, and 4.7.2. [PR #1](https://github.com/saworbit/didi/pull/1)

### Fixed

- `2026-08-27` Made timeout cancellation state-aware for queued commands. [PR #2](https://github.com/saworbit/didi/pull/2)
- `2026-08-27` Removed the original outer timeout race; Phase 3 subsequently made the public live-call deadline finite and generation-safe. [PR #2](https://github.com/saworbit/didi/pull/2)
- `2026-08-27` Updated pull-request CI assertions to cover the complete 68-registration surface, dynamic execution modes, resources, and canonical scene hierarchy output. [PR #2](https://github.com/saworbit/didi/pull/2)
- `2026-08-27` Made `scene_close` conservative on Godot 4.5: explicit `discard_unsaved: true` is required because that API cannot expose active-scene dirty state. [PR #2](https://github.com/saworbit/didi/pull/2)
- `2026-08-27` Made explicit scene overwrite replace the ResourceLoader cache and reload existing editor tabs before verification. [PR #2](https://github.com/saworbit/didi/pull/2)
- `2026-08-27` Removed the non-functional GDScript singleton pump and all live-success stubs. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Prevented timed-out queued commands from mutating the editor later and bounded main-thread work to 64 commands per frame. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Made cross-thread bridge readiness atomic and resolved pending IPC promises during editor shutdown. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Kept scene mutations in the edited scene's UndoRedo history, used undo-side references for removed nodes, and preserved node lifetimes across history pruning. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Rejected unknown or type-incompatible scalar properties and restored exact sibling order after remove and reparent undo. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Node edits are confined to the edited scene, its root is protected, and cyclic reparenting is rejected. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Preserved live viewport provenance and dimensions at the public MCP boundary; only real GPU-backed captures report `is_live_frame: true`. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Centralized result-level execution provenance and kept offline-only filesystem/parser work out of Godot's main-thread command queue. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Raised the minimum supported Godot version to 4.5, where the required native main-loop callback API is available. [PR #1](https://github.com/saworbit/didi/pull/1)
- `2026-08-27` Reconciled every user and developer document with the verified implementation. [PR #1](https://github.com/saworbit/didi/pull/1)

---

## [1.1.0] - 2026-08-26

Full write-ups for 1.1.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#110---2026-08-26).

### Added

- `2026-08-26` A 40-tool canonical surface across nine domains, from the scene tree to the editor lifecycle. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)
- `2026-08-26` Added `docs/ROADMAP.md` with the nine-domain matrix and the architectural vision. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)
- `2026-08-26` The ten v1.0 tool names stay registered as a compatibility surface. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)
- `2026-08-26` Structured capture of GDScript compiler errors, runtime crashes and engine logs. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)

### Fixed

- `2026-08-26` Restrict named pipe DACL strictly to Owner and Local Administrators (`D:(A;;GA;;;BA)(A;;GA;;;OW)`), removing `WD`. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)
- `2026-08-26` Enforce `0600` permissions on POSIX Unix domain sockets. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)
- `2026-08-26` Fix recursive mutex deadlock in `PosixIpcClient`. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)
- `2026-08-26` Implement non-blocking I/O cancellation (`CancelIoEx` on Win32, `shutdown()` on POSIX) for graceful server shutdown. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)
- `2026-08-26` Windows binary stdio mode (`_setmode(_O_BINARY)`) and `cin.gcount()` framing checks. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)
- `2026-08-26` Project root path traversal boundary confinement on file modifications. [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)
- `2026-08-26` Atomic log verbosity level management (`std::atomic<LogLevel>`). [3e23174](https://github.com/saworbit/didi/commit/3e23174732d8d66868e8229b703fccfee4ce18e0)

---

## [1.0.0] - 2026-08-26

Full write-ups for 1.0.0: [as first written](https://github.com/saworbit/didi/blob/fd9d0e5e8517143c0e892166c225485a840b2d87/CHANGELOG.md#100---2026-08-26).

### Added

- `2026-08-26` One C++20 CMake build produces the standalone `didi` server and the in-engine GDExtension. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)
- `2026-08-26` MCP 2024-11-05 over JSON-RPC 2.0 on stdio, newline-delimited or `Content-Length` framed. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)
- `2026-08-26` Named-pipe and Unix-socket IPC to the editor, later replaced by per-session endpoints. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)
- `2026-08-26` Ten tools across vision, scene tree, scripting, runtime and the asset pipeline. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)
- `2026-08-26` Resources `godot://project/tree`, `godot://editor/state` and `godot://runtime/logs`. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)
- `2026-08-26` Prompt templates `godot_debug_visual_anomaly` and `godot_generate_gameplay_slice`. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)
- `2026-08-26` Offline fallback: diagnostics, asset indexing, scene parsing and headless test runs without an open editor. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)
- `2026-08-26` Security hardening: restricted pipe access, editor-only IPC, frame size limits and no shell in process launches. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)
- `2026-08-26` 16 unit and integration tests. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)
- `2026-08-26` Documentation: architecture, tool reference, integration, developer, API, admin and LLM guides. [f062719](https://github.com/saworbit/didi/commit/f062719d7960e31733898c8df80cbd3623b46167)

---

## Stability

Didi follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html), and a
major number is a statement about compatibility rather than about maturity. The
two are worth separating, because this project is further along on the first
than on the second.

**What a version number promises.** The tool names, their arguments, and the
shape of a successful answer are the public surface. A change that breaks one of
them bumps the major, which is what 2.0.0 is: the release corrects error codes,
handshake validation and schema strictness across the surface, and a client
written against 1.8.0 can break on any of them.

**What it does not promise.** Didi is not finished. Its own status block says
`PARTIAL_DELIVERY`, three canonical tools are registered and unimplemented, and
[the roadmap](docs/ROADMAP.md) has Phase 12, the phase that owns reproducible
artifacts, supported platform matrices and compatibility guarantees across
versions, barely started: `didi setup` and `didi doctor` are the whole of it so
far. Until it lands there is no upgrade or rollback guarantee beyond the
changelog, and no commitment to a support window for an older minor line.

Read the version for what changed. Read this section and the roadmap for how
much of the thing exists.
