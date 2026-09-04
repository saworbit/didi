# Field Trial Results

Results of field trial runs. Method and apparatus are in [Field Trial Design](FIELD_TRIAL_DESIGN.md); the seed, briefing and scoring script are in `tools/field-trial/`.

One section per run. Keep the numbers, because the point of a repeatable seed is that two runs can be compared.

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

## When to use Didi, and when not

Drawn from what the run actually did rather than from the tool list. This belongs in agent-facing guidance.

**Reach for Didi when the truth lives inside a running process.** Live scene tree with unsaved edits, real viewport pixels, a running game frame by frame, the InputMap after a reload, profiler samples. No file can answer these. The usage curve above is entirely this shape, and the tester named the pause, inject, step loop the most capable thing in the toolset.

**Reach for it when the editor's guarantees matter.** UndoRedo-backed mutation, atomic settings writes with in-memory rollback, batch preflight, path normalisation. Writing the file yourself gets the bytes right and the invariants wrong.

**Do not use it to author text.** GDScript source and resource files were where every fallback happened. A tool that emits text is a worse text editor than a text editor, and `resource_create` writing `.tres` markup into a `.gd` path (#204) is that mismatch made concrete. Author the file directly, then use Didi to attach, wire and verify it.

**Do not use it for search that grep does faster**, with one exception. `project_analyze_impact` sees scene connection endpoints, serialised `NodePath` values and animation tracks that a text search structurally cannot.

---

## Measurement

Coverage cannot come from the server log. `handleRequest` logs `Method: tools/call` and the tool name appears only in the `TOOL_EXEC` line written when a call throws, so log-based scoring silently under-reports every tool that worked. It comes from the client transcript, where each invocation is a `tool_use` block named `mcp__didi__<tool>`:

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
