# Field Trial Results

Results of field trial runs. Method and apparatus are in [Field Trial Design](FIELD_TRIAL_DESIGN.md); the seed, briefing and scoring script are in `tools/field-trial/`.

One section per run. Keep the numbers, because the point of a repeatable seed is that two runs can be compared.

Every section records which client hosted the tester. Trials 01 through 03 and trial 05 were Claude and trial 04 was Codex, and the comparison that buys the most is between engines rather than between consecutive runs of one.

---

## Trial 01, 2026-09-03

**Seed:** commit `355f818`, Didi 1.5.0, Godot 4.7.2 stable, Windows. Bare project, no addon, no enabled plugin, no editor. Tester given the repository path and no reading order.

**Outcome:** all six required features delivered. A playable single-screen 2D arena survival game, driven to a win through injected input.

### Coverage

Measured from the client transcript, not the server log. See [Measurement](#measurement) below.

| Metric | Value |
| :--- | :--- |
| Distinct implemented tools called | 36 of 91 |
| Coverage | 39.6% |
| Total invocations | 156 |
| Tools called that the manifest does not know | 0 |

Most used: `runtime_step` (24), `scene_instantiate_node` (22), `eval_gdscript` (20), `runtime_inject_input` (15), `project_set_input_action` (6).

### What the uncalled set showed

More useful than the called set, and not visible in the ledger, because nothing went wrong in these cases. The tester simply never reached for them.

- **All four `signal_*` tools: zero calls**, in a game whose central requirement was signal-driven scoring. Every connection was written in GDScript with `.connect()`. This is not a discoverability failure. `signal_connect` edits the scene's serialised connections; gameplay signals belong in `_ready()`. The tools and the task were aimed at different things.
- **`editor_undo` and `editor_redo`: zero calls.** UndoRedo safety is Didi's headline differentiator and the run never exercised it.
- **Written but never read back.** `project_set_input_action` x6 against `project_list_input_actions` x0; `project_set_autoload` x1 against `project_list_autoloads` x0; `scene_set_property` x2 against `scene_get_property` x0. `LLM_INSTRUCTIONS.md` asks for a read-back after each write. It did not happen once.
- **No structural editing.** `scene_remove_node`, `scene_reparent_node` and `scene_duplicate_node` were never called; the tester only ever built additively.

### Issues filed

Nine, all labelled `field-trial`, against a cap of twenty: #203 through #211. Four further findings were recorded in the ledger with the reason for not filing, which is the behaviour the briefing asks for.

The highest-cost finding was #203: `scene_instantiate_node` killed the editor for themed Controls. It cost four editor deaths and one full rebuild of the arena tree.

**Root cause, confirmed against the engine.** `GodotApi::classdb_construct_object` is bound to the interface entry point `classdb_construct_object2`, which Godot implements as `ClassDB::instantiate_without_postinitialization`. Objects came back before `NOTIFICATION_POSTINITIALIZE` had been sent, and a themed Control that resolves theme items during post-initialization was then used half-built. Eight call sites had to send the notification; the ninth construction, inside Didi's own `create_instance_func`, must not, because the engine sends it once that callback returns. Verified on a live 4.7.2 editor: `Label` and `Button` now construct and the editor survives.

**A finding that did not survive checking.** Every input event Didi wrote carried `device: 16`, and mouse events `device: 32`. The ledger recorded this and declined to file it. That judgement was right: `InputEvent.DEVICE_ID_KEYBOARD` is 16 and `DEVICE_ID_MOUSE` is 32. The values are correct. Recorded here because the reviewer initially read them as corruption sharing a root cause with #203, and an experiment after the fix showed them unchanged.

### Fallbacks

Three implementation routes were abandoned to direct file authoring, each recorded in the ledger:

- Placing HUD Labels, because construction segfaulted the editor (#203).
- Authoring the enemy animation into the scene, because no tool writes an AnimationLibrary (#210).
- Building the TileSet, because `resource_create` cannot emit `ext_resource`, `sub_resource` or `Vector2i`.

---

## Trial 02, 2026-09-04

**Seed:** commit `2292bae`, Godot 4.7.2 stable, Windows. Identical briefing and identical bare seed to trial 01. The briefing was deliberately left unchanged so the two runs stay comparable.

**Outcome:** all six required features delivered again. Both endings reached live and driven entirely through `runtime_inject_input`. Logs clean.

### Coverage against trial 01

| Metric | Trial 01 | Trial 02 |
| :--- | ---: | ---: |
| Distinct implemented tools called | 36 | 37 |
| Coverage | 39.6% | 40.7% |
| Total invocations | 156 | 313 |
| Ledger entries | 18 | 23 |
| Entries verdicted `failed` | 6 | 9 |
| Issues filed | 9 | 6 |

Newly reached: `scene_add_to_group`, `scene_get_property`, `scene_remove_node`. No longer reached: `project_analyze_impact`, `scene_close`.

### What the comparison says

**Breadth held, depth doubled.** One net tool, and twice the calls. The surface an agent naturally reaches for is stable across runs and is about 40% of what is implemented. That number moving would be more surprising than it holding.

**Removing the crash did not reduce friction, it revealed more of it.** The ledger halved in length, because trial 01's was dominated by four editor deaths and a full arena rebuild. Friction *events* rose from 18 to 23 and failures from 6 to 9. With the crash gone the run got further and met more walls, which is the expected and desirable shape.

**52 implemented tools were called in neither run.** Across two independent runs of the same task, that set is now evidence rather than an accident. It includes `editor_undo` and `editor_redo`, never called once, though UndoRedo safety is the headline differentiator; all four `signal_*` tools; every `blackboard_*` tool except `blackboard_write`; and every project-level list/get tool.

**The read-back instruction still did not land.** `LLM_INSTRUCTIONS.md` asks for a read-back after each write. `project_list_input_actions`, `project_list_autoloads` and `project_get_setting` were called zero times in both runs. This recurred on a clean run with the briefing untouched, so it is a property of the guidance rather than of one tester.

### Issues filed

Six new, #213 through #218, all labelled `field-trial`. Eight further findings were recognised as duplicates of #204 to #211 and recorded in the ledger instead of refiled, which is the behaviour the protocol asks for.

### The dominant wall moved

Trial 01's biggest cost was a crash. Trial 02's is the **Phase 1 scalar property contract**: `position`, `shape`, `tile_set`, `color`, `polygon` and `libraries` all had to be patched into `.tscn` text directly. Almost every fallback in the run traces to it.

This corrects a prioritisation made after trial 01, which named `script_create` (#208) as the next most valuable addition. Two runs of evidence say the property contract (#210) is the larger lever. Authoring a script by hand is one fallback at the start; the scalar contract forces a fallback at every point where a node needs a position, a shape or a resource reference.

### Mutations that report success without succeeding

Three of the six new issues are one shape: a tool returns success while the project did not change as described. #213 (`scene_set_property` on a discarded `anchors_preset` write), #215 (`script_patch_method` returning clean diagnostics for a file it had just broken), and #217 (`project_set_autoload` persisting without the editor registering the singleton).

This is the most serious class of defect the trials have produced, and worse than a crash. A crash is loud, in-band and recoverable. A false success silently corrupts the agent's model of the project, and every later decision is made against a world that does not exist. The tester put it well: an agent has no eyes, so the response is the entire world model.

The pattern to fix it already exists in-house rather than needing invention. `viewport_set_camera_transform` uses UndoRedo and verified post-state; `anim_play_track` rereads state after dispatching and is explicit that `dispatched` is not completion; project settings writes roll back in memory when persistence fails. Newer tools verify themselves and Phase 1 tools do not.

The cheapest correct form is for a mutation to **return the observed post-state rather than assert equality**. Returning what was read back is less work than a type-aware comparison across the scalar contract, it cannot itself be wrong in the way an equality check can, and it gives the caller the evidence to decide. For batch mutations such as `tilemap_set_cells`, verify the aggregate rather than each record, or the verification costs more than the write.

---

## Trial 03, 2026-09-08

**Seed:** commit `3b58517`, Didi 1.6.0, Godot 4.7.2 stable, Windows. Identical briefing and identical bare seed to trials 01 and 02.

**Outcome:** all six required features delivered again. Both endings reached live and captured from the running game. Logs clean: zero engine errors across the 832-frame play run, and an empty `errors` array from the headless launch.

### Coverage against trials 01 and 02

| Metric | Trial 01 | Trial 02 | Trial 03 |
| :--- | ---: | ---: | ---: |
| Distinct implemented tools called | 36 | 37 | 36 |
| Coverage | 39.6% | 40.7% | 32.1% |
| Total invocations | 156 | 313 | 174 |
| Ledger entries | 18 | 23 | 19 |
| Entries verdicted `failed` | 6 | 9 | 9 |
| Issues filed | 9 | 6 | 7 |

The coverage drop is not a regression signal. The implemented surface grew from 91 to 112 between trial 01 and trial 03, so the denominator moved; and a large share of this run went on diagnosing a single setup fault rather than ranging across the surface.

### The dominant wall from trial 02 is gone

Trial 02 named the **Phase 1 scalar property contract** as its biggest cost: `position`, `shape`, `tile_set`, `color`, `polygon` and `libraries` all had to be patched into `.tscn` text by hand, and almost every fallback traced to it.

That contract is gone. `scene_set_property` and the `properties` argument of `scene_instantiate_node` now accept Vector2, Vector2i, Vector3, Vector3i, Color and `res://` resource paths, and the writer produces correct `ext_resource` wiring. Placing a node, sizing a `ColorRect`, and filling a `CollisionShape2D.shape` slot are ordinary single calls in this run. Issue #210 is genuinely fixed, and features 1, 4 and 5 were completed without leaving the tool surface at all.

`didi_control_room` is the other clear improvement. The first call of the run reported the bridge as down and named the cause and the remedy, which is the question trials 01 and 02 both had to work out for themselves.

### The most expensive finding was not a tool defect

This run spent about an hour concluding that a shipped capability did not exist.

`README.md:174` says to copy the repository's `addons/didi` into your project, four lines after telling you to build into `build/`. `docs/QUICKSTART.md:21-31` says the opposite, correctly, and explains why: the addon is assembled at `build/addons/didi`, and "The `addons/didi` folder in the repository is the manifest that goes into that assembly, not a build output." That directory's extension is gitignored and, since #38, is written by no build step, so it holds whatever was last left there by hand.

Following the README installed an extension six days older than the server under test, predating the #210 fix. Every Vector2 write then failed with `Property type 5 is outside the Phase 1 scalar property contract`, which reads exactly like a documented limitation rather than a stale binary. `didi_control_room` reported the bridge light green throughout and reported `Server: didi 1.6.0`, which is the standalone server's version; nothing anywhere reports the version of the extension actually serving the calls.

This is the finding to carry into Loop B. A trial can be pointed at the right server binary, dump a fresh manifest from it, and still be scored against a different bridge. `seed_trial.py` records `didi_executable` and its manifest; it does not record, or check, the GDExtension that serves the live half of every call. Filed as #325 (the README line) and #326 (the missing version handshake).

### False success remains the highest-severity class

Trial 02 named mutations that report success without succeeding as the most serious defect class the trials have produced. This run found two more, both of a new kind: the response is not merely optimistic, it is *attributed to work that did not happen*.

`resource_create` (#327) accepts structured property values, reports `created_offline`, and writes a `.tres` that Godot misreads. It emits properties in alphabetical order, so `tracks/0/interp` is applied before `tracks/0/type` has created the track; and it writes JSON where Godot needs literals, so `tracks/0/keys` never becomes a Dictionary of `PackedFloat32Array` and `tracks/0/path` never becomes a `NodePath`. The resulting Animation loads, reports one track, and has discarded every field on it, with four `Index (uint32_t)track = 0 is out of bounds` errors going to a console nobody reads. The same limitation makes a `TileSet` unreachable, because there is no representation for a `SubResource` block or a collision polygon.

`viewport_capture_frame` (#330) is the second. #209 fixed the case where `editor_2d` returned a 2x2 image as a successful capture, and `editor_2d` now fails correctly with a message that names the remedy. The `2d` and `canvas_item` aliases for the same viewport did not get the fix: with a 2D scene open and the editor on the 3D main screen, both return a full-size PNG of the **3D** viewport, described as a capture of `'2d'`.

The cheapest correct form proposed after trial 02 still applies, and extends: a mutation should return the observed post-state, and a read should not describe a result as something it is not.

### A tool that cannot observe what it exists to observe

`runtime_explore_scene` (#329) drives a game by holding the project's own input actions and sampling `probes` every frame. Its schema documents the probe expression as "A sandbox expression evaluating to a number or a boolean, such as `position.x`."

`position.x` is the first form the sandbox refuses. So is every other read of project state: `self.position.x` and a bare `visible` and `get_position().x` all fail, the last two because `context_node` is never bound as the evaluation instance. Only source-local numeric literals evaluate, and a literal never changes, so the tool cannot detect motion in any project.

It reports this as a clean run. A twelve-second window held all five actions across 721 frames and returned `engine_errors: 0`, `stuck_intervals: []` and `verdict: "none"`, with every probe at `readings: 0`. Nothing in that response distinguishes "explored and found nothing wrong" from "measured nothing at all".

### Composition moved out of the scene file

`scene_pack_branch` produced a correct `res://enemy.tscn`, and then nothing could place one. `scene_instantiate_node` carries a `scene_path` argument and returns `501 PackedScene instantiation is outside the Phase 1 built-in-node bridge`; `instantiate_asset`, whose stated contract is exactly this, is reserved and rejects every call.

The fallback was to preload the scene in `arena.gd` and instance three copies in `_ready()`. The game is correct and the enemies do come from a packed scene, but `main.tscn` no longer contains them, so the scene file stops describing the scene. That is the difference between authoring a project and generating one, and it is why #328 is filed as the highest-leverage missing capability of this run.

### Issues filed

Seven, #325 through #331, all labelled `field-trial`: one `documentation`, two `enhancement`, four `bug`. Fourteen gaps were catalogued; seven were promoted and seven were recorded without filing, which is the behaviour the protocol asks for.

### The instructions that still do not land

Trial 02 recorded that the read-back instruction in `LLM_INSTRUCTIONS.md` did not survive contact with the task. This run read back after writes throughout, so that one landed, but two others did not:

- **The blackboard.** The briefing asks the tester to record architectural decisions and node paths on it. Trials 01, 02 and 03 have now all failed to do so. At three for three, the honest reading is that the instruction does not survive the task, not that three testers were careless.
- **`editor_undo` and `editor_redo`.** Trial 02 recorded these as never called despite UndoRedo safety being the headline differentiator. They did not occur to this tester either, at any point, even though every mutation response carries `undo_redo_registered: true`.

### Caveat on comparability

This tester read the briefing and the three field-trial design documents before starting, because the session running the trial also owned the harness. It had read no tool reference, no capability document and no installation instructions before its first call, which is why it walked into the README trap at full speed. Treat the friction counts as comparable and the discovery ordering as warmer than trial 01.

---

## Trial 04, 2026-09-10

**Seed:** commit `fcfa676`, Didi 1.8.0, build `1.8.0+fcfa676d1729.20260910T014252`, Godot 4.7.2 stable, Windows. Identical briefing and identical bare seed to trials 01 through 03.

**Tester: Codex, `gpt-5.6-sol` at high reasoning effort.** The first three runs were all the same client, and this one exists to find out which of their findings were about agents and which were about that client. Run through `tools/field-trial/trial.py --engine codex`.

**Outcome:** all six required features delivered again. Both endings reached live and captured from the running game. The final headless launch returned `success true`, `exit_code 0` and an empty `errors` array, and an attached game session reported zero engine warning or error records while running.

**Bridge: matched**, on 21 observations. This is the first run where the pairing was confirmed end to end rather than reconstructed afterwards. The seed recorded that the repository's own `addons/didi` was present and differed from the built one, so the tester had the same two indistinguishable addons in front of it that cost trial 03 an hour, and it installed the right one. The #325 documentation fix held.

### Coverage against every previous run

| Metric | Trial 01 | Trial 02 | Trial 03 | Trial 04 |
| :--- | ---: | ---: | ---: | ---: |
| Tester | claude | claude | claude | codex |
| Distinct implemented tools called | 36 | 37 | 36 | 36 |
| Coverage | 39.6% | 40.7% | 32.1% | 32.1% |
| Total invocations | 156 | 313 | 174 | 244 |
| Ledger entries | 18 | 23 | 19 | 23 |
| Entries verdicted `failed` | 6 | 9 | 9 | 7 |
| Issues filed | 9 | 6 | 7 | 4 |

Newly reached: `blackboard_read`, `blackboard_write`, `project_apply_changes`, `project_audit_assets`, `project_list_resources`, `project_set_setting`, `runtime_detach_session`, `runtime_stop`, `script_check_syntax`, `script_patch_method`. Four of those had never been reached by any run: `blackboard_read`, `project_apply_changes`, `project_list_resources` and `runtime_detach_session`.

No longer reached: `resource_inspect`, `runtime_explore_scene`, `runtime_step`, `runtime_watch_invariants`, `scene_close`, `scene_get_property`, `scene_remove_node`, `scene_set_property`, `tilemap_set_cells`, `ui_list_controls`.

### The plateau is a property of the task, not of one client

**36, 37, 36, 36.** Four runs of the same seed, two clients from different vendors, and the count of distinct tools an agent reaches for did not move. Ten tools changed hands in this run, in each direction, and the total came out where it always does.

Three runs of one client could not distinguish a fact about agents from a habit of that client, and the honest reading after trial 03 was that we did not know which we had. We know now. The number is about the task and the surface, and moving it means changing one of those, not waiting for a better tester.

**60 of 112 implemented tools have now been called by nobody**, across four independent runs and two engines. Two entries in that set have been named in every previous write-up and survive this one:

- **`editor_undo` and `editor_redo`: still zero calls.** UndoRedo safety is Didi's headline differentiator. Four testers, two vendors, and it has never once occurred to any of them, though every mutation response carries `undo_redo_registered: true`. This is no longer a run-specific observation. Either the capability needs to be surfaced where an agent is already looking, or it is a guarantee that agents want honoured rather than driven.
- **All four `signal_*` tools: still zero calls**, in a task whose central requirement is signal-driven scoring. Trial 01 concluded the tools and the task were aimed at different things, because `signal_connect` edits serialised connections while gameplay signals belong in `_ready()`. A second engine reaching the same conclusion independently confirms that reading. This one is working as designed and should stop being counted as a discoverability failure.

### The blackboard instruction landed for the first time

Trials 01, 02 and 03 all failed to record anything on the blackboard, though the briefing asks for it directly. The write-up after trial 03 said that at three for three the honest reading was that the instruction does not survive the task rather than that three testers were careless.

That reading was wrong, and this run is the correction. `.didi/blackboard/field-trial.json` holds the architecture overview, the combat and input decisions and the canonical node paths, written under `blackboard_write` with a stated reason on each entry, and read back with `blackboard_read`. The instruction survives the task. It did not survive three runs of one client.

### The dominant wall was `project_apply_changes` on a project with no git

The tester's own answer to what would have helped most, and it is the second entry in a 23 entry ledger:

```
project_apply_changes {"changes":[{"path":"res://TRIAL_LOG.md", ...}]}
The project is not inside a git work tree, so there is no cheap way to build
an isolated copy of it to check against
```

The seed creates a bare directory with a `project.godot` and nothing else, deliberately, because that is where real users start. A Godot project is not a git repository unless someone makes it one, and the atomic multi-file write refuses every project in that state. Every multi-file edit and every Markdown update in this run went to a direct-file fallback because of it.

This did not appear in trials 01 through 03 because none of those testers reached for the tool at all. It is the clearest single instance of what a second engine buys: a wall that three runs walked past without touching.

### Issues filed

Four, #371 through #374, all labelled `field-trial` and all `bug`. Fewer than any previous run, against a cap of twenty, with the rest of the friction recorded in the ledger and declined with a reason, which is the behaviour the protocol asks for.

- **#371** `missing_colon` treats four-character assignments as `else` statements.
- **#372** `scene_create` fails on a missing project-contained parent directory.
- **#373** the enabled extension leaks one ObjectDB instance on clean game exit.
- **#374** `asset_reimport` reports success while Godot rejects GDScript paths.

#374 is the false-success class again, now found by a third independent tester. Trial 02 named mutations that report success without succeeding as the most serious defect the trials produce, trial 03 found two more, and the pattern has now survived a change of engine. Nothing about it was an artifact of one client's reading of a response.

### Composition still moves out of the scene file

Trial 03 filed #328 as its highest-leverage missing capability: `scene_pack_branch` produces a correct `enemy.tscn` and then nothing can place one, because `scene_instantiate_node` returns 501 for a `scene_path` and `instantiate_asset` is reserved.

`scenes/main.tscn` in this run contains zero `instance=ExtResource` nodes, and `scripts/main.gd` preloads `res://scenes/enemy.tscn` and instantiates three copies in `_ready()`. A different engine, given the same task, produced the same fallback and the same shape of project: correct game, and a scene file that has stopped describing the scene. #328 is confirmed rather than merely repeated.

### Caveat on comparability

Two differences from trials 01 through 03 belong on the record rather than in a footnote.

**The tester had skills loaded.** `--ignore-user-config` suppresses `config.toml`, which is how this engine's MCP servers and plugins are configured, but it does not suppress `$CODEX_HOME/skills`. The tester therefore had a set of user-level workflow skills available and used them: it wrote a spec and a plan under `docs/superpowers/` before building anything. Trials 01 through 03 had whatever their own client loads by default. Neither run is skill-free, and the two sets are not the same set.

**There was no cost ceiling.** Codex has no equivalent of `--max-budget-usd`, so the three hour timeout was the only bound on this run. It finished in about fifty minutes and the point never arose, but a run on this engine is bounded by the clock rather than by spend.

Two leftovers from earlier work were running on the machine throughout: trial 03's Godot editor, open since the previous day on `D:\didi-trials\trial-20260908-2309`, and unrelated Codex sessions. Auto-attach is scoped to the project root, so neither could capture this run, and the bridge verdict confirms every observation came from the seeded build.

---

## Trial 05, 2026-09-10

**Seed:** commit `8e11c31`, Didi 1.8.0, build `1.8.0+8e11c31e41e1.20260910T044028`, Godot 4.7.2 stable, Windows. Identical briefing and identical bare seed to trials 01 through 04.

**Tester: Claude, `claude-opus-5`.** Run through `tools/field-trial/trial.py` with no engine flag. The build was made at HEAD before the run rather than reused, so this trial is the first to exercise #376 and #377.

**Outcome:** all six required features delivered again. Both endings reached live and captured from the running game. `runtime_read_output` returned two records, both the tester's own prints, nothing at warning or error level, and the final headless launch was clean.

**Bridge: matched**, on 8 observations. The seed again recorded the repository's own `addons/didi` as present and different from the built one, and the tester again installed the right one. Two runs now on the #325 fix.

**Cost and clock:** $18.64 and 238 turns in 22 minutes wall clock, against a $40 ceiling. The fastest run of the five by a wide margin, and the first where the ceiling was recorded rather than absent.

### Coverage against every previous run

| Metric | Trial 01 | Trial 02 | Trial 03 | Trial 04 | Trial 05 |
| :--- | ---: | ---: | ---: | ---: | ---: |
| Tester | claude | claude | claude | codex | claude |
| Distinct implemented tools called | 36 | 37 | 36 | 36 | 38 |
| Coverage | 39.6% | 40.7% | 32.1% | 32.1% | 33.9% |
| Total invocations | 156 | 313 | 174 | 244 | 160 |
| Ledger entries | 18 | 23 | 19 | 23 | 16 |
| Entries verdicted `failed` | 6 | 9 | 9 | 7 | 5 |
| Issues filed | 9 | 6 | 7 | 4 | 5 |

Newly reached against trial 04: `editor_reload_project`, `project_get_uid_map`, `resource_inspect`, `runtime_explore_scene`, `runtime_set_paused`, `runtime_step`, `scene_add_to_group`, `scene_get_property`, `scene_remove_node`, `scene_set_property`, `tilemap_set_cells`, `ui_list_controls`. No longer reached: `asset_reimport`, `blackboard_read`, `project_apply_changes`, `project_list_resources`, `runtime_detach_session`, `runtime_get_tree`, `runtime_read_logs`, `scene_open`, `script_patch_method`, `tilemap_get_used_rect`.

Almost all of that movement is the engine changing back. Ten of the twelve newly reached were reached by a Claude run before, and seven of the ten no longer reached were reached only by trial 04.

### The reached surface has stopped growing

**Not one tool in this run had gone uncalled by every previous run.** Twelve names changed hands against trial 04 and every one of them had been reached by trial 01, 02 or 03. The union of everything five testers on two engines have ever reached is unchanged, and **60 of 112 implemented tools have still been called by nobody**.

That is a stronger statement than the plateau recorded after trial 04. That one said the count of tools any single agent reaches is stable at about 36. This one says the *set* has closed: a fifth run, on the client that produced three of the previous four, added nothing to it. The 52 tools outside the union are not waiting for a tester that thinks to use them. They are outside what this task asks for, and nothing about the run will change that. Moving the number means changing the task or the surface.

Two entries in the uncalled set have now survived five runs and both were called out in every previous write-up:

- **`editor_undo` and `editor_redo`: still zero calls**, five runs, two vendors, though every mutation response in this run carried `undo_redo_registered: true` and the tester quoted that field approvingly in its ledger. It reads the guarantee and never drives it. That is the answer to the question trial 04 left open: agents want UndoRedo honoured, not exposed.
- **All four `signal_*` tools: still zero calls.** Trial 01 concluded the tools and the task were aimed at different things and trial 04 confirmed it on a second engine. Three engines-worth of evidence now. This should stop being counted.

### Three fixes from trial 03 held, and one of them reversed a standing finding

`resource_create` (#327) wrote a multi-track keyframed `Animation` correctly: ordered-array `properties`, `PackedFloat32Array` times, `Vector2(1, 1)` values, a file indistinguishable from what Godot's own animation editor writes. Trial 03's version of this call produced an Animation that loaded, reported one track and had discarded every field on it.

`runtime_explore_scene` (#329) reads project state now, and it is what rescued the run. See below.

**`scene_instantiate_node` with a `scene_path` writes a real instance (#328).** `scenes/main.tscn` contains three `instance=ExtResource("5_choun")` nodes and a `PackedScene` `ext_resource` header. Trials 03 and 04 both ended with a `main.tscn` holding zero instances and a `_ready()` that preloaded and instanced by hand, and both write-ups named that as the difference between authoring a project and generating one. That finding is retired. The scene file describes the scene again, on the first run after the fix, without the tester being told anything about it.

### The dominant wall is a bootstrap deadlock

The first entry in the ledger, at four minutes in:

```
project_set_setting {"setting":"editor_plugins/enabled",
                     "value":["res://addons/didi/plugin.cfg"]}
{"error":{"code":503,"message":"No atomic runtime route is available for live dispatch"}}
```

Every `project_*` writer is live-only. A live session requires the addon to be enabled. Enabling the addon is a `project_set_setting` write. **Didi cannot install itself into a project**, and the seeded bare project is exactly the state every real user starts in. The tester copied the addon with the shell and hand-wrote the `[editor_plugins]` section, which is what the quickstart tells a human to do; an agent handed a project and a server has no route at all. Filed as #382.

This is the same shape as trial 04's `project_apply_changes` wall, and for the same underlying reason: a tool's precondition is something the seed deliberately does not provide. Trial 04 needed a git work tree that a Godot project does not have. This one needs a live session that the project cannot have until the tool that needs it has already run.

### `resource_create` still cannot say "this points at that"

The tester's own answer to what would have helped most, and the more valuable half of #327's story. The ordered-array form and Vector2 inference are right; what is missing is any way to express a reference to another resource. There is no representation for an `ext_resource` or a `sub_resource` in the properties contract, so `sources/0` was written as the quoted string it was passed and the TileSet would not load.

The class that excludes is not marginal: TileSet, AnimationLibrary, SpriteFrames, Theme, ShaderMaterial, StyleBox. Two of the run's three hand-written files exist only because of it, and requirement 2 asks for a TileSet by name. It also forced a design choice worth recording, because it looks like preference and is not: the `bob` animation is a correct `Animation` resource that `enemy.gd` has to install into the `AnimationPlayer` at `_ready`, because an `AnimationLibrary` is a resource holding a reference to another resource and nothing can write one. Filed as #380.

Trial 03's `resource_create` finding was that it reported success for a file Godot misread. That half is fixed. The half that remains is the one it could not have found, because it never got past the first.

### A false failure, where every previous run found false successes

`script_check_syntax` returns `has_errors: true` and `Compile Error: Identifier not found: GameState` for four of the run's five scripts. All five are correct: the game runs, the HUD updates, the engine logs nothing. The check runs in a separate `godot --headless --check-only` process that never registers the project's autoloads, so it is wrong for any script in any project that uses a singleton, permanently, and not in the way `project_set_autoload`'s documented restart limitation is wrong. The tester re-tested after an editor restart and after the game had demonstrably run through the singleton. Filed as #383.

Trials 02, 03 and 04 all named mutations that report success without succeeding as the most serious class the trials produce. This is the mirror image and it costs the same thing: the response is the agent's entire world model, and a tool that cries wolf on every autoload user teaches an agent to stop reading it. The tester ignored the diagnostics and trusted `runtime_launch`, which is the right call and also the end of that tool's usefulness.

`scene_create` and `scene_pack_branch` writing a UID the engine never learns (#379) is a quieter member of the same family. The header is correct, `project_get_uid_map` resolves the path to that very UID in the reverse direction, and the engine warns on every load because nobody told its index. `editor_reload_project` repairs it by rescanning the whole project.

### The run's best moment was a bug that was not one

Four rounds convinced `runtime_inject_input` was broken: the player would not move for an action event, a key event, or with the game window focused. It was about to be filed.

`runtime_explore_scene` is what stopped it. Driving the same build for four seconds, it reported `moved: true` with x from 480 to 711 and y from 272 to 499. The player had been wedged between three `CharacterBody2D` enemies that had converged and stopped dead on top of it. Injection was never broken, and a fresh session moved the player 230 px/s for as long as it was held.

This is the tool trial 03 filed #329 against for being unable to read `position.x`, the example in its own schema, while reporting a clean run over 721 frames of measuring nothing. Fixed, it is the tool that answered the question the tester actually had, and the ledger says so directly: a tool that presses the project's own actions and reports whether anything moved is worth more than one that presses a button.

### The blackboard instruction landed halfway

`blackboard_write` four times, with architecture, node paths and the trial outcome, and `blackboard_read` not once. Trial 04, on the other engine, did both. So the correction recorded after trial 04 holds, and the instruction does survive the task, but the read half did not survive this run, and no Claude tester has yet read back what it wrote.

### Issues filed

Five, #379 through #383, all labelled `field-trial`: two `bug`, three `enhancement`. Against a cap of twenty. Three further findings were recorded in the ledger and deliberately not filed: a `runtime_launch` that returned an empty `logs` array for a run that printed and could not be reproduced, the pause-and-step stand-in advancing a `move_and_slide` body 0.063 pixels across 30 stepped frames, and `project_list_input_actions` returning roughly nine thousand tokens of built-in `ui_*` actions to show five project-defined ones.

The unfiled `runtime_launch` finding deserves the same eye as #383. `{"exit_code":124,"logs":[],"errors":[],"warnings":[]}` is indistinguishable from a clean run in a payload a caller is expected to check for emptiness. One occurrence and no reproduction is the right reason not to file it; it is the wrong reason to forget it.

### Caveat on comparability

The tester recorded, in the ledger before its first entry, that the session harness asks for a brainstorming skill before creative work, that the skill is a dialogue with the user, that this run is unattended and forbids questions, and that it therefore recorded the design on the blackboard and proceeded. Trial 04 recorded the same class of difference from the other direction: its skills were loaded and it used them to write a spec and a plan first. No run in this series has been skill-free and no two have had the same set.

---

## When to use Didi, and when not

Drawn from what the run actually did rather than from the tool list. This belongs in agent-facing guidance.

**Reach for Didi when the truth lives inside a running process.** Live scene tree with unsaved edits, real viewport pixels, a running game frame by frame, the InputMap after a reload, profiler samples. No file can answer these. The usage curve above is entirely this shape, and the tester named the pause, inject, step loop the most capable thing in the toolset.

**Reach for it when the editor's guarantees matter.** UndoRedo-backed mutation, atomic settings writes with in-memory rollback, batch preflight, path normalisation. Writing the file yourself gets the bytes right and the invariants wrong.

**Do not use it to author text.** GDScript source and resource files were where every fallback happened. A tool that emits text is a worse text editor than a text editor, and `resource_create` writing `.tres` markup into a `.gd` path (#204) is that mismatch made concrete. Author the file directly, then use Didi to attach, wire and verify it.

**Do not use it for search that grep does faster**, with one exception. `project_analyze_impact` sees scene connection endpoints, serialised `NodePath` values and animation tracks that a text search structurally cannot.

---

## Measurement

Coverage cannot come from the server log. `handleRequest` logs `Method: tools/call` and the tool name appears only in the `TOOL_EXEC` line written when a call throws, so log-based scoring silently under-reports every tool that worked. It comes from the client transcript, which `tools/field-trial/transcripts.py` reads for whichever client hosted the run: a `tool_use` block named `mcp__didi__<tool>` correlated to a later `tool_result` for the Claude client, and a single `item.completed` event carrying the server, the tool and the result together for `codex exec --json`. Either way the reader is the same call:

```
python tools/field-trial/coverage.py \
  --transcript <session>.jsonl \
  --manifest <trial>/tool-manifest.baseline.json \
  --output <trial>/coverage.json
```

The manifest is captured at seed time from the binary under test, so a run is always scored against exactly what it was handed. A stale manifest reports a wrong uncalled set; this was hit for real while gating trial 01, where the on-disk manifest claimed 83 canonical and 80 implemented against a binary emitting 94 and 91.

---

## Toward a repeatable loop

Trial 01 was run by hand. The pieces that make it repeatable already exist: a deterministic seed, a fixed briefing, and a scoring script that emits comparable JSON.

The loop is: seed, run, score, fix, re-seed at the same commit, re-run, diff the two coverage reports and the two ledgers. What should move between runs is the uncalled set shrinking and the ledger getting shorter. What should not move is the feature checklist.

Two things to settle before automating it:

1. **A tester session cannot be started from inside another session.** It needs its own MCP connection, which is the point. Any automation has to launch a client process, not call a tool.
2. **A run needs a fresh editor.** Trial 01's editor died four times from #203; with that fixed, a run should survive on one, but the harness should still assume it can be relaunched.

[Gogo Design](GOGO_DESIGN.md) is the parallel version of this and stays design-only.
