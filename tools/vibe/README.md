# Vibe testing

Exploratory testing of the running server: sit down with a throwaway project,
call tools by hand, and follow whatever smells wrong. No assertions, no pass or
fail — the output of a session is a set of findings, filed as issues.

This is not a replacement for anything. `didi_tests` asserts unit behaviour,
`tests/run_godot_integration.ps1` asserts a scripted live scenario, and
`tools/field-trial` scores what an agent achieves unattended. All three only
ever send arguments their author already believed in. The findings here are
almost all in the space those suites cannot reach: what happens when a caller is
*wrong* — a mistyped property, a path that resolves to nothing, a token spent
twice.

## What is here

| File | What it does |
| :--- | :--- |
| `mcp_client.py` | stdio JSON-RPC client. `Session` for a live process (tokens, attachments survive), `batch()` for one-shot probing. |
| `sandbox.py` | Writes a throwaway Godot project — root, one child, one script — and optionally installs the built addon and opens the editor. |
| `probe.py` | Command line: list tools, print a schema, run one call or a file of calls. |
| `probes/*.json` | Saved probe sequences. Each one is the reproduction for findings already filed. |
| `probes/confirmation_token.py` | The confirmation gate walked end to end; a token cannot be carried between processes, so this one is a script. |
| `probes/write_then_read_back.py` | Three mutations that report success, read back against the file they wrote. |
| `probes/protocol_edges.py` | The wire below `tools/call`: stale cursors, a null id, an unknown tool, `arguments` that is not an object. |
| `probes/path_errors.py` | The same nonexistent path asked of every tool that takes one; finds the failures that live *behind* valid arguments. |
| `probes/error_data_census.py` | What each error's `data` lets a caller branch on. `path_errors.py` asks whether a failure is an envelope; this one opens it. |
| `probes/schema_descriptions.py` | How many parameters a caller has to guess at: counts `description` across the whole surface. Zero since #462, and a contract test keeps it there. |
| `probes/advertised_vs_reported_mode.py` | What `tools/list` says a tool's mode is, against what the tool then says. The two are built by different code and nothing compared them. |
| `probes/rpc_methods.py` | The methods a host sends *around* `tools/call`: `initialize` and its version negotiation, `ping`, subscriptions, a batch array, a cancellation, a progress token. |
| `probes/content_vs_structured.py` | The two copies of every payload -- `content[0].text` against `structuredContent` -- compared. Green; kept as the regression probe. |
| `probes/path_confinement.py` | Whether a write can leave the project, and whether the path the validator accepted is the path that got written. |
| `probes/bridge_exclusivity.py` | Two servers, one editor. What the second one is told, against what a server with no editor is told. |
| `probes/blackboard_rules.py` | The blackboard's own parameter descriptions, tested against the handler behind them. |
| `probes/engine_crash.py` | The editor killed mid-session: what the first call after it says, and what every call after that says instead. |
| `probes/surface_census.py` | The same question asked of all 126 tools: who rejects an unknown argument, what `execution_mode` each reports, who answers with a bare string. |
| `probes/handler_error_census.py` | The failures *behind* the argument check: arguments built from each tool's own schema, naming something absent, plus the calls that must be meaningful-and-wrong to reach a handler at all. |
| `probes/empty_string_census.py` | `""` sent to every required string parameter, sorted by whether the schema carries `minLength` -- the string half of the question #484 asked about numbers. |
| `probes/windows_path_forms.py` | What a case-insensitive filesystem, a device name and a dot segment do to a `res://` path the validator already accepted. Destroys files; throwaway sandbox only. |
| `probes/line_endings.py` | A CRLF file, a BOM, a missing final newline -- patched one method at a time, byte-counted before and after. |
| `probes/position_offsets.py` | The line and column every reader reports, asked of a file with multi-byte characters and four different framings. |
| `probes/session_lifecycle.py` | The handshake once it is past: a call before `initialize`, a second `initialize`, a missing or doubled `initialized`, a reused id, and a confirmation token carried across a client change. |
| `probes/viewport_diff_blindness.py` | The visual regression tool asked about a frame that changed. Reads both PNGs itself so it can say the frame moved without asking the tool under test. Order matters: it diffs *before* it captures. |
| `probes/stale_confirmation.py` | The world moved between the preview and the confirm -- the file grown, deleted, or its method removed. The token binds to the call; this asks whether it binds to anything else. |
| `probes/preview_vs_real.py` | Each mutating call twice, `dry_run` then for real, printing the pairs that disagree. A preview is a promise; this counts the ones the server will not keep. |
| `probes/oversized_arguments.py` | The largest accepted string rather than the smallest: a `maxLength` census beside session eight's `minLength` one, a megabyte sent to required parameters, and a 10 MB file read back. |
| `probes/node_path_forms.py` | The *other* path namespace. `..`, `.`, `%Unique`, `Node:property`, doubled separators -- asked of a reader, a second reader and a writer. |
| `probes/pipelined_requests.py` | Requests that overlap, because a host is not obliged to wait. Green: answers come back complete, in order and correctly addressed. Kept as the regression probe. |
| `probes/capture_baselines.py` | A capture id from a dead process, and ids nobody minted. Green: made-up ids are refused by name, and a real id outlives its process on purpose. |
| `probes/engine_versions.py` | The same questions asked of whichever editor is attached, printed as a table meant to be diffed between two engines. |
| `probes/game_session.py` | The second kind of session. Starts the game as its own process, attaches to it, and checks what only a game can answer: stepping verified to the frame through a native property, input injected while paused, a click with no position, and what the server says after a stop it asked for itself. Prints expected against observed. |
| `probes/game_session_census.py` | The wide pass over the same game: every tool asked once with the game attached, the control room's per-kind modes, invariants, explore, checkpoints, and a fresh server started while two sessions are alive. |
| `probes/scene_ownership.py` | Godot's own rules for a scene, asked of the tools that edit one: an instanced sub-scene's internals, an inherited scene, a script whose `extends` the node cannot take, signal connections, and a scene instanced into itself. Reads the saved file after every save. |
| `probes/instance_overrides.py` | A property, a visibility flag and a group set on a node inside an instanced sub-scene, saved, read back from the file and after a reload, beside the same edit on a node the scene owns. |
| `probes/editor_log_delta.py` | The editor's own log (`--log-file`) read after every call, so each ERROR or WARNING line Godot prints is attributed to the tool call that caused it. |
| `probes/domain_mutation_honesty.py` | The domain tools -- tilemap, gridmap, shader uniform, audio bus -- asked whether their mutations survive, read back from the `.tscn` bytes rather than from the tool that wrote. Needs `--fixtures`. |
| `probes/project_root_encoding.py` | The project path before the server has read it: what MSVC's ANSI-codepage `argv` does to a non-ASCII directory name. Needs no editor and no addon. |
| `probes/file_encodings.py` | A `.gd` that is not valid UTF-8 -- Latin-1, UTF-16, a truncated sequence, an embedded NUL -- asked of the script tools, with a plain-broken UTF-8 file as the control. |
| `probes/preview_value_rules.py` | `signal_emit`'s `dry_run` against its confirm, for argument *values* rather than names. The third layer of the seam #399 and #571 opened. |
| `probes/offline_refusals.py` | Not what a live-only tool answers offline -- what it *refuses* with. One message, fourteen tools. Run with no editor on the project. |
| `probes/posix_platform.py` | The states only a POSIX host can create: a file the process may not read against one that is not there, a directory it may not write into, a symlink out of the project shown to the read path and the write path, a name that is not valid UTF-8, a name containing a newline, a FIFO where a `.gd` is expected, and whether the host folds case. Skips a row by name when the host cannot set it up; run it as a user who is not root, or the permission rows cannot exist. |
| `probes/project_state_census.py` | The other half of `handler_error_census.py`. That one builds arguments from each tool's schema and so can only reach failures an argument can cause; this walks a *project file* through every state it can be in -- absent, unreadable bytes, structurally valid and empty, valid and incomplete, correct -- and asks the readers. Reports the bare strings and, separately, which states answer identically. |
| `probes/headless_editor.py` | The live surface with nothing to draw on: an editor started `--headless`, asked the tools whose answer is a picture. Decodes every image it is handed rather than believing the response about it. First caller anywhere of `viewport_capture_passes`, `viewport_create_test_lab`, `viewport_set_camera_transform`, and of `asset_reimport` against a real imported asset. |
| `probes/blackboard_two_writers.py` | Two servers writing one board, interleaved by hand rather than by luck: the same key, two different keys, an interleaved patch, and a clear while the other holds a task. |
| `probes/empty_versus_absent.py` | "Nothing there" beside "no such thing", for nine readers at once, printing whether the two answers differ at all. The oldest lesson in this file, asked as a census instead of one tool at a time. |
| `probes/yolo_mode.py` | `--yolo`, the mode where the confirmation gate is not there. Diffs what each mode publishes about itself before any call, then walks the three gate kinds in both. |
| `probes/list_changed_promise.py` | `tools/list` hashed either side of an attach and a detach, against the `listChanged` the handshake publishes, with the `notifications/tools/list_changed` that arrived over the same span counted beside it. The one claim on the surface that is about the surface moving. |
| `probes/csharp_build.py` | `csharp_check_build` given a real `.csproj` to build, and given a broken toolchain. Prints MSBuild's own `N Warning(s)` summary beside the tool's `diagnostics_count`, because the payload carries both and nothing compares them. |
| `probes/long_temp_endpoint.py` | The one platform constant the session endpoint must fit inside. Launches its own editors under temporary directories of chosen lengths, and does a raw `AF_UNIX` bind at the same length as its control so a row that could not be set up is visible as such. POSIX only. |
| `probes/description_vs_schema.py` | Every parameter's prose read against the schema keys beside it: a stated default against `default`, a listed value against `enum`, a stated range against `minimum`/`maximum`. #576 said this was "worth grepping for as a class"; this is the grep. |
| `probes/legacy_alias_parity.py` | Each legacy alias's published entry diffed against its canonical tool's. An alias is supposed to be the same tool under an older name, which makes every difference a claim a host reads differently depending on which name it picked. |
| `probes/output_schema_honesty.py` | Every published `outputSchema` against the payload it describes: required properties present, declared types matching, and how much of the answer the schema covers at all. Green; kept as the regression probe. |
| `probes/resource_subscriptions.py` | `resources/subscribe` driven by a second process that actually writes, with the write's own result printed as the control. Green: notifications arrive in about half a second and stop on unsubscribe. |
| `probes/blackboard_patch_parity.py` | `blackboard_patch`'s path handling against the rules `blackboard_write` enforces on the same board. |
| `probes/node_name_sanitising.py` | A node name the caller chose against the node name Godot allowed. The `#525` seam where the conversion is the engine. |
| `probes/eval_containment.py` | Nineteen things a "read-only expression" might reach -- the filesystem, the environment, the process, the network -- each printed with its refusal. Green, and the refusals are the interesting output. |
| `probes/call_method_reach.py` | What `scene_call_method` will call, against the script-declared-only rule it publishes. Walks the confirmation gate properly, because the outer gate refuses every row before the method rule is reached. Green. |
| `probes/signal_lifecycle.py` | Connect, list, disconnect, and every way each can be wrong, with the saved `.tscn` as the witness rather than `signal_list_connections`. Counts *its own* connection line, because a sandbox accumulates state and an earlier probe's leftover read as a failed teardown. |
| `probes/ghost_preview_survival.py` | What a refused `editor_render_ghost_preview` leaves on the user's screen. A refusal the argument check made is the control, and a refusal the engine made is the row that mattered: the teardown used to run before the engine was asked whether the new shapes could be drawn, so a `409` spent the proposal on its way out. |
| `probes/rename_disclosure.py` | `project_rename_references`'s preview against its confirm, field by field, over call sites the probe wrote itself so the arithmetic is known before either call. Needs no editor. |
| `probes/ui_app_modes.py` | The three `--ui-app` modes diffed before any tool is called: the handshake's `extensions`, the `ui://` resource in the listing and on a read, and `didi_control_room`'s own `_meta`. Needs no editor. |
| `fixtures/write_csharp_fixture.py` | A `.csproj` with one error and one warning in it. Kept out of `sandbox.py` because a C# project changes what Godot does with the directory. |
| `editor_exit_status.py` | Not a probe against the server: the same editor invocation with and without the built addon installed, exit statuses side by side. The control for a crash on shutdown. |
| `wait_for_session.py` | Polls `runtime_list_sessions` through the same binary the probes use, so a slow first import reads as a slow import rather than as an absent session. |
| `bind_census.py` | Not a probe against the server: every `(class, method, hash)` bind in `src/gdextension` checked against `--dump-extension-api` output from each installed engine, `hash_compatibility` included. A miss is a null bind that prints at startup or answers 501 at call time. |
| `fixtures/` | What the ownership, game and domain probes need beyond the sandbox: a sub-scene, a scene inheriting `main.tscn`, a game scene with a ticking script and an AnimationPlayer, a `Node3D` script, and since session eleven a `domain.tscn` (TileMapLayer with a real TileSet over a generated atlas, a ShaderMaterial over a four-uniform shader, an AnimationPlayer) plus `domain3d.tscn` (a GridMap with a MeshLibrary). Since session fourteen a `toolsubject.gd`, which is `@tool` and so runs in the editor -- the precondition for `scene_call_method` and for a signal connected to a method -- carrying one signal that takes an argument and one method that takes none, so a signature-compatible pair and an incompatible one are both available. `sandbox.py --fixtures` copies them in; a `.py` in there is a fixture writer rather than a fixture and is skipped. |
| `report.py` | Files a directory of finding bodies as issues in one pass. |

The probe files are kept after their findings are fixed, and are worth re-running
for exactly that reason. Re-run against `abd8911`, every one of them now shows
the fixed behaviour: `schema_enforcement.json` was a list of twelve things the
server got wrong and is now twelve argument errors that name the property;
`offline_paths.json` gets a sentence explaining why a node path cannot be read
without an editor, plus `requested_root_path` on the fallback and
`target_exists` on an impact query. A probe that starts failing again is a
regression nobody wrote a test for.

The seventh session's six went the same way against 2.0.0, and re-running them
is how two defects in the probes themselves came out. `blackboard_rules.py`
wrote to a board called `leases`; a board is a file that outlives the process,
so the second run met the first run's completed task and reported its own
leftovers as conflicts. It takes a board per run now and reads the task id from
the create call rather than assuming `TASK-1`. And both live probes filtered the
control room down to a handful of labels, neither of which was the one their own
finding asked for -- so `bridge_exclusivity.py` and `engine_crash.py` would have
gone on printing `Route: detached` for a held bridge and a dead engine long
after #527 and #536 closed. A probe that cannot show the fix is not a regression
probe.

**A probe is only a regression probe if it can print the difference.** Writing
one while a finding is open makes it easy to show only the broken half: the
version rows in `rpc_methods.py` were four unserved revisions and two malformed
values, all answering `2024-11-05`, which reads as "they are all the same" and
cannot tell you when that stops being true. It sends a served revision now, so
the row that must echo itself sits beside the rows that must not. When the fix
lands, re-run the probe and ask what it would print if the fix were reverted. If
the answer is "the same thing", the probe is a record, not a guard.

## A session

```powershell
# 1. a project you are free to break, with the current addon in it
#    (--fixtures adds the sub-scene, inherited scene and game scene the
#    ownership and game probes expect)
python tools/vibe/sandbox.py $env:TEMP\vibe\proj --fixtures --launch C:\Godot\Godot_v4.5.1-stable_win64_console.exe

# 2. what does the surface look like from here?
python tools/vibe/probe.py -p $env:TEMP\vibe\proj --list --dump-tools $env:TEMP\vibe\tools.json

# 3. sweep, then follow what looks odd
python tools/vibe/probe.py -p $env:TEMP\vibe\proj --calls tools/vibe/probes/readonly_sweep.json
python tools/vibe/probe.py -p $env:TEMP\vibe\proj --calls tools/vibe/probes/schema_enforcement.json

# 4. narrow a single case until the repro is two lines
#    (--schema still needs -p: the server refuses to start without a project root,
#     so a schema lookup with no sandbox exits before it answers `initialize`)
python tools/vibe/probe.py -p $env:TEMP\vibe\proj --schema scene_add_to_group
python tools/vibe/probe.py -p $env:TEMP\vibe\proj --call scene_add_to_group '{"group":"x"}'

# 5. file what survived narrowing
python tools/vibe/report.py $env:TEMP\vibe\findings --dry-run
python tools/vibe/report.py $env:TEMP\vibe\findings
```

Pass `--project` a path the server can open. On Windows that means a native
path: a Git Bash style `/c/Users/...` is refused at startup with "The explicit
project root is not an accessible directory", which reads like a permissions
problem and is not one.

Stop the editor at the end. An editor left running keeps publishing a session
descriptor, and the next server started anywhere on the machine can attach to
it — that was #387, and it will quietly poison the next session's results.

## What this pass keeps turning up

Written down because each one cost time before it was understood, and the next
session should start from here rather than rediscover it.

**The sharpest findings are `isError: false`.** A call that fails loudly is
usually fine. The ones worth chasing report success while having answered a
different question: the whole main scene returned for a subtree that does not
exist (#401), a group added to the scene root instead of the named node (#396),
an empty impact list for a path with no file behind it (#404).

**Ask what two different situations look like.** Most findings here are one
response shape standing in for two states a caller must be able to tell apart —
"no presets configured" and "the file is unreadable" (#403), "nothing depends on
this" and "you typo'd the path" (#404), "here is your subtree" and "there is no
such node" (#401). Neither state is a crash, so nothing else catches them.

**Read the schema before believing a result.** Several tools take a parameter
whose name is not the obvious guess: `target_node` not `node_path`, `setting`
not `setting_path`, `scene_path` not `output_path`, `source_text` not `content`,
`new_definition` not `new_body`. The server rejects an unknown property by name
and lists what the tool does take, which makes the mistake cheap. That covered
50 of 126 tools until #418; arguments are closed by default now, so every
implemented tool answers the same way and a new tool is covered on arrival.
`probe.py --schema` is still faster than a round trip, and since #462 it
answers in prose: every parameter carries a description, so the schema says
which name a tool wants before a call has to go wrong to find out.

**Count it before you call it a bug.** Three of this session's findings only
became findings once every tool was asked the same question: 50 of 126 rejected
an unknown argument, 26 reported `offline_fallback` with an editor attached, 18
answered with a bare string. Each looked like one tool misbehaving until the
census showed a missing default. The first is now 121 of 126, and the five that
are left are the unimplemented registrations, which refuse the whole call
before any argument is read. The second is now 7, all of them tools that
really do have a live path to fall back from (#419). The third is now zero:
every semantic failure carries a code (#420). `probes/surface_census.py` runs
these in about a minute each; the numbers are in the session log and are worth
diffing. A fourth, `--which identifier-messages`, asks who answers with a token
where a sentence belongs; it reports zero from junk arguments, because the
argument check fires first, and the tokens live behind *valid* arguments that
name the wrong thing (#441). A census that returns zero is a census asking the
wrong question. `probes/path_errors.py` is that lesson applied: the
error-envelope census reports zero bare strings on `8fd6409`, and eight tools
still returned one — the argument check answers junk first, so the path
validator is only reachable with arguments that are well-formed and wrong
(#460). Those eight carry the envelope now; the probe stays, because the next
validator to be wrapped by hand will land in the same blind spot.

**A census that passes has only proved its own question.** The error-envelope
census reports zero bare strings, and `path_errors.py` reports zero too. Both
ask whether a failure is shaped like an error. Neither opens it.
`probes/error_data_census.py` asks the next question -- what is in `data`, which
is the part a caller can branch on -- and 20 of 35 semantic failures answer with
an empty object, a lone `retryable`, or no `data` at all (#486, #487). The same
move found the five unimplemented registrations still answering with a bare
string (#492), because they refuse before the argument check and so sit in
*front* of the census rather than behind it. When a census goes green, ask what
it was actually counting.

**A schema is a claim about the handler, and nothing checks it.** Three of this
session's findings are a declared parameter the code does not honour:
`include_properties` on the live hierarchy only decides whether the field is
named in `omitted_fields`, so passing `false` claims properties were included
and returns none (#482); `context_node` on `eval_gdscript` is validated against
the live tree, echoed in every response, and unreachable from any expression the
sandbox permits (#488); `max_depth` has no `minimum` where every other limit on
the surface has one, so -5 is accepted and clamped (#484). Read the handler
beside the schema. `grep` for the parameter name is usually the whole
investigation.

**Compare a tool against its sibling, not only against itself.** Several
findings are one tool doing a check that a tool doing the same job does not.
`project_set_autoload` refuses a script that is not on disk; `project_set_setting`
writes `run/main_scene` pointing at nothing and reports success (#490).
`project_verify_changes` refuses at once when the project has no repository of
its own; `project_apply_changes` previews the same arguments and hands out a
confirmation token for a call that cannot succeed (#491). Neither pair is
inconsistent in a way either half can see on its own.

**Narrow before you file, especially when two things changed.** This session
nearly filed "`editor_undo` reports success without undoing anything" — the
group list really was unchanged after an undo. The undo had reverted a node
*rename* from an earlier probe, so the later reads were looking at a path that
no longer existed. A tighter repro (set a property, undo, read it back) showed
undo working correctly and reporting `409 Nothing to undo` on an exhausted
stack. A sandbox accumulates state; a finding that depends on three earlier
mutations is a finding about your probe order until proven otherwise.

**Diff the tree around a census, not just after it.** A `/root/Main/Node` nobody
had asked for appeared in this session's sandbox partway through a census, and
finding out which call had put it there cost a fresh sandbox, a fresh editor and
three replays. The answer was `scene_instantiate_node`, which has no required
arguments and instantiates a bare `Node` when sent `{}` (#471). A census that
sends a call per tool is a batch of mutations wearing a survey's clothes: read
the hierarchy between calls, keep the diff, and the bisect is already done.

**A mutation that reports success has still only told you it ran.** Four of
this session's findings are a tool doing something adjacent to the request and
saying it did the request: a method patched at the wrong indentation (#438), a
`new_definition` spliced in without being read (#439), resource properties the
type does not have (#444), a float the property cannot hold (#437). None of them
error, and in three of the four the tool's own response is consistent with
itself. The file, or the engine loading the file, is the only witness. Read back
with something other than the tool that wrote.

**Ask what the answer is about, not just what it says.** `scene_get_hierarchy`
returns a correct tree and never names the scene it came from, so after
`scene_create` opens a new scene every later call is a true answer to a
different question (#448). The same shape as #401. When a tool has an implicit
subject, ask whether the response identifies it.

**Discovery is a surface too, and it is published twice.** Every claim a tool
makes about itself exists in two places: `_meta.didi.currentMode` on the
`tools/list` entry, which a host reads to decide whether to offer the tool, and
the `execution_mode` in the answer, which only a caller sees. Five sessions of
censuses asked the second and never the first, so #419 could land on the
responses, leave the discovery metadata untouched, and look finished --
`probes/advertised_vs_reported_mode.py` puts them side by side and eleven tools
disagree (#503, #504). The same split runs through the schemas: arguments are
closed by the handler and open in 73 published `inputSchema`s (#508),
`structuredContent` comes back from all 126 tools and 11 publish an
`outputSchema` for it (#509), and the one that is published names a field the
handler does not return (#510). Ask what a tool says about itself before it is
called, not only what it says afterwards.

**Annotations are the part of the surface nobody is calling.** `readOnlyHint`
and friends are never exercised by a probe, because a probe just calls the
tool -- so they drifted until all four were a function of one read/write bit
(#507), and two of the tools sitting in the read-only bucket were not read-only.
`runtime_detach_session` was annotated read-only, non-destructive and
idempotent, and severs the bridge (#505). That one was not a paper cut: it
silently wrecked
the first run of this session's own census, which had filtered to read-only
tools and called it in alphabetical order ahead of every `scene_*` and
`viewport_*` tool. Four tools were reported as falling back to offline; every
row after the detach was an answer about a detached server. A census is only as
trustworthy as the metadata it filtered on.

**The layers below `tools/call` have their own conventions, and they are not
the tool surface's.** `prompts/get` still accepts and drops an unknown argument
(#511), which is the pre-#397 behaviour living on a method nobody re-checked;
`prompts/list` and `prompts/get` describe the same prompt differently (#512);
and `resources/read` serves a non-default blackboard board as `text/plain`
because only `default` is a registered URI (#513). `probes/protocol_edges.py`
covers the JSON-RPC frame. The methods between the frame and `tools/call` --
`resources/*`, `prompts/*` -- had never been swept.

**One client is not the product.** Every session until the seventh drove a
single server against a single editor, which is the one configuration where the
coordination story cannot go wrong. Start a second server on the same project
and it cannot reach the editor at all -- reasonable -- and it is told so with
the same 503, word for word, that a server gets when Godot is not running, while
`didi_control_room` says `Route: detached` in both cases (#527). The blackboard
exists *because* two agents are expected, so the second client is the supported
path, not an edge case. Anything with a notion of ownership deserves a probe
that runs two of the thing.

**A path the validator accepted is not always the path that gets written.**
`script_create` enforces a `.gd` extension, and `res://n1<NUL>x.gd` satisfies it,
because the string ends in `.gd`. The write then truncates at the NUL and lands
a file called `n1`, and the tool reports success with the path it did not use
(#525). The check and the write were looking at different strings. The same
project already refuses a control character in a blackboard path, by name -- so
the rule exists, it just is not applied to file paths. When a validator and an
effect are separated by a conversion, probe the conversion.

**Ask what happens when the thing you are attached to goes away.** Killing the
editor mid-session costs one `taskkill` and reaches code no scripted suite runs.
Didi handles it well -- the first call afterwards returns the best error payload
on the surface, with `incident: engine_crashed` and a recovery sentence -- and
then says it exactly once (#536). Two calls later the fallback is answering
happily from the `.tscn` file and nothing anywhere records that an engine died.
Probe the recovery *and* the second call; the difference between them is the
finding. This is also where narrowing earned its keep twice in one session: the
first run made it look like the offline fallback had been suppressed until an
explicit detach, and a loop of four calls showed it recovering on the second.

**Offline and live are different products.** The same argument can mean
different things depending on whether an editor is attached, and the offline
answer is the one nobody checks. Run the interesting probes twice.

**The host operating system is part of the surface.** Seven sessions treated a
`res://` path as a string the server validates, and it is also a string Windows
resolves. The filesystem is case-insensitive, so `res://PLAYER.gd` and
`res://player.gd` are one file to an `exists()` check and two paths to
everything that reports: `script_create` with `overwrite` replaces
`res://player.gd`, reports `res://PLAYER.gd`, and the dry-run preview says
`before.path: "res://PLAYER.gd"` about a file that does not exist (#546).
`resource_create` refuses the same collision and names the path that is not
there. That is #525's seam -- a validator and an effect separated by a
conversion -- except the conversion is the OS and no amount of string checking
finds it. `probes/windows_path_forms.py` walks the whole family. **Ask what the
platform does to the argument after the server is finished with it.**

**A file the tool did not author is a different file.** Every probe until now
created its fixtures with `script_create`, which writes LF and no BOM, so every
mutation tool has only ever been tested against its own output.
`script_patch_method` on a CRLF file converts the whole file to LF and reports a
one-method change (#550); the preview shows `before.size_bytes` and nothing that
would make a whole-file rewrite visible. A BOM survives, a missing final newline
does not. Write the fixture with something that is not the tool under test, and
read it back as bytes rather than through the tool that wrote it.

**Two censuses of the same property can both be green and both be wrong.**
`path_errors.py` and `error_data_census.py` both report zero bare strings, and
four semantic failures still answer with prose (#548). They are reachable only
with arguments that are *meaningful and wrong* rather than merely well formed --
`remove` of a setting that is not there, a value nested past the cap, an
offline-only capability refusal -- so they sit behind both censuses.
`handler_error_census.py` lists those calls explicitly beside a sibling call on
the same tool that answers correctly, because a census that can only generate
its own inputs can only find what its generator reaches.

**A guard that was added to close a finding may have been added to one mode.**
#464 closed "`project_set_setting` persists a misspelled setting name"; the
check it added runs on the live path only, and the exact call from #464 still
writes the typo offline and reports success (#547). Offline is the mode where
the guard is worth more, because there is no Project Settings dialog to notice
the stray key in. When a fix lands, re-run its own repro in the other mode. The
README has said "offline and live are different products" since the third
session; this is what it costs when a fix forgets it.

**A pinned snapshot is a version claim, and the server can check it.**
`script_reflect_class` answers from `resources/didi_class_reference.json`, pinned
at 4.7, while attached to a 4.5.1 editor whose session descriptor is sitting in
the same response (#555). `api_version` discloses which dump was read, which is
honest and is not the same as telling the caller the two disagree -- `build_id`
and `bridge_build_matches` (#326) exist because a mismatch a caller has to
notice is a mismatch nobody notices. When a tool answers from something that was
frozen at build time, ask what it would take to compare that against what is
running.

**One process or two changes the answer.** Confirmation tokens live in the
server's memory. A dry run in one `probe.py` invocation and a confirm in the
next is not a test of the gate, it is a test of process lifetime.

**A tool that takes its own input has to be able to get it.** Most tools here
answer about something the caller named. `viewport_diff_capture` is different:
it takes half of its own input, the comparison capture, and it has no
`select_main_screen` while the sibling that captures for the caller does -- and
that parameter exists because "an editor viewport has no size unless its main
screen is showing". So it compares against whatever was last rendered and
reports `bit_identical: true`, `ssim: 1.0`, for a frame where 55% of the pixels
changed (#568). The one tool whose whole job is answering "did this change?" is
the one that cannot make the thing render. **When a tool supplies part of its
own input, ask what it can do that a caller could not.**

**Order is part of the repro, and a probe can hide its own finding.** The first
version of `viewport_diff_blindness.py` captured a second frame before it
diffed, to prove the frame had changed. That capture carries
`select_main_screen`, which forces the render -- so the diff then worked, three
times in a row, and the probe printed a clean bill of health for a broken tool.
Moving one call above another turned `bit_identical=True` into
`changed_pixels=630000`. **If a probe does anything to the subject before it
asks its question, that setup is part of the experiment.**

**A guard can be switched off by an argument nobody validates.**
`script_patch_method` refuses a `new_definition` that declares the wrong kind of
symbol, and the code comment says why: without it "a body with a mistyped name,
or no declaration at all, deleted the target and reported the patch done".
`symbol_type` has no `enum`, and an unrecognised value takes an `else` branch
that skips that check by design. So `symbol_type: "fucntion"` -- one
transposition -- replaces a whole function with a variable and reports success
(#570). **Where the code branches on a string, check that the schema constrains
it; an unenumerated string is a switch a caller can flip by accident.**

**A preview is a promise, and the preview path is not the call path.** #399
closed "the dry run issued a token for arguments the real call refuses" by
checking argument *names* before previewing. One level down, a `..` in a node
path is refused by every reader and every writer, and previewed happily by seven
of nine mutating tools (#571) -- including one that reports
`preview_kind: "target_state"` and a real `before`, which is the preview saying
it looked. The control in the same run is a node that does not exist: `404` on
both paths, so the preview *does* resolve node paths. It runs one rule and not
the other. **Run every probe twice, `dry_run` then for real, and diff the pair.**

**A token can bind to the call and not to the world.** The confirmation gate
binds hard to tool, arguments, project and session, refuses reuse, and survives
a failed confirm without being spent -- all verified. What it does not carry is
any record of the target it previewed, so a file rewritten between the dry run
and the confirm is changed anyway, and `before.size_bytes` sits in the preview
unused (#572). The blackboard exists because two agents are expected, which
makes a concurrent edit the supported path. **Ask what a token is bound to, then
ask what it is not.**

**Census the other end of the range.** Session eight sent `""` to every required
string parameter. Sending a megabyte instead finds a different surface: 90 of
191 string parameters carry `maxLength` and 50 required ones carry nothing,
while 62 of 64 numeric parameters carry a `minimum` (#573). The project believes
in bounds; it applied them to 97% of its numbers and 47% of its strings. The
same unbounded space is what lets an 8 MB `method_name` become a 16.8 MB
response, because a dry run echoes its arguments twice (#574). **A census is a
direction as well as a question -- run it both ways.**

**Two claims in one object.** `case_sensitive` is published with
`"default": true` and a description reading "Off by default" (#576). Nothing can
catch that: the description tests count descriptions, the schema tests read
keys, and no test compares one against the other. A description that names a
default beside a `default` key is worth grepping for as a class.

**The engine matrix came back green, which is worth writing down.** Nine
sessions have driven 4.5.1. Run against 4.7.2, the whole live surface answers
the same way: the same 68 live tools, the same findings reproducing identically,
`script_reflect_class` correctly reporting
`api_version_matches_attached_engine`, and the control room switching
"Unsaved scenes: not reported before Godot 4.7" for an actual answer. The
capability difference is reported rather than hidden. `engine_versions.py` makes
the comparison a `diff` so the next session can re-check it cheaply.

**Pipelining and lease conflicts are also green.** Four requests written to
stdin before any answer is read come back complete, in order and correctly
addressed, including duplicate ids; a blackboard task claimed twice answers the
second agent `409` with `leased_by` and `reason_code: already_leased`. Both were
untested and both were fine. Recorded so the tenth session spends its budget
elsewhere.

**The editor already knows what a scene file can hold, and the tools do not
ask.** Three findings in one shape: a node removed from inside an instanced
sub-scene or from an inherited scene (#589), a property set on a node inside an
instance (#588), and the edited scene instanced into itself (#590). Each
reports success, `editor_save_scene` reports `saved`, and the file is
byte-identical, because `PackedScene.pack` only stores what the scene owns or
has marked editable, and Godot's own `SceneTreeDock` refuses all three before
the tree is touched. The hierarchy shows none of this (#591): an instance root,
an instance's internal node and an inherited node all read like an owned one.
**Read the file after every save, byte count first.** One trap inside the
trap: before Godot 4.7 `scene_close` refuses without `discard_unsaved`, and
`scene_open` on an open scene is a tab switch, so the first run of
`scene_ownership.py` believed a reload that never happened. A scene that
*instances* the one under test is the reload that cannot lie.

**The second kind of session refuses well and succeeds badly.** Nine sessions
attached to an editor. With a game attached, every editor-only tool answers
`409` with `allowed_session_kinds`, which is right; the findings were all
`isError: false`. Input injected while paused is `completed` and never
delivered, not even by a `runtime_step` (#594). A stop the server itself
requested is reported afterwards as a retryable connection timeout (#595). A
click has no position and lands at the origin (#597). The policy has one read
on each side of the line (#592). And script state, which is the only state a
game author writes an invariant about, cannot be read by the sandbox that the
invariant and explore tools document with `node.get('health')` (#593); the
fixture's ticker mirrors its frame counter into the parent's `position.x` so a
step can still be verified to the frame. **Pause, inject, step, read is the
workflow those tools exist for; run it end to end and read the output stream,
not the tool's own response.**

**Read the editor's own log after every call.** `didi_control_room` prints
`ERROR: Parameter "mb" is null` in the editor on 4.5 and 4.6, every time
(#600), and the game's stdout carries two ANSI-coloured INFO lines per command
(#601). Neither shows in any tool response. `editor_log_delta.py` attributes
each new log line to the call before it; `bind_census.py` names the bind
behind a null-bind error in seconds from the engine's own API dump, and found
exactly one across 4.5.1, 4.6.2 and 4.7.2: `get_unsaved_scenes`, absent before
4.7, which the control room already reports as a limitation and still asks
for on every call. A burst of `Inconsistent redo history` seen once during the
ownership probe reproduced under no attributed call and was left unfiled; a
log line that cannot be tied to a call is a lead, not a finding.

**The host operating system gets the argument before the server does.** Session
eight asked what Windows does to a `res://` path the server had already
accepted. One step earlier is the project path itself, and MSVC's narrow
`main(int argc, char** argv)` converts the wide command line through the ANSI
codepage before a single line of didi runs. So the answer splits on what cp1252
can encode: `ó` arrives as a lone high byte and the process fast-fails with
`0xC0000409` and *no output at all*, while `日` arrives as `?` and gets
"The explicit project root is not an accessible directory" about a directory
that is perfectly accessible (#611). The catch that was written for this case
names `std::filesystem::filesystem_error`; the throw is a `std::system_error`,
so the message "The project root must be valid UTF-8" has never once reached a
caller. **Probe the process before the protocol.** Nothing above `initialize`
can find this, and a Windows username with an accent in it is ordinary.

**A guard scoped to one status class leaves the others behind.** #547 was a fix
applied to the live path and not the offline one. The same shape, one layer
down: `phase7_live_forward.cpp` carries a careful comment about why an engine
answer must not be reported as a 503 routing problem, and the condition it
guards is `failure.code >= 400 && failure.code < 500`. An engine refusal with a
5xx therefore still arrives as `503 runtime_route_request_failed` with
`data.code: "not_connected"` on a session whose very next call succeeds (#625).
The condition that carried the meaning was `!transport.has_value()` -- the
engine answered -- and the status range was a detail that quietly narrowed it.
**When a fix keys on two conditions, ask which one was the finding.**

**Setting a property and storing a Variant are different things.**
`scene_set_property` goes through `Object::set`, which coerces, so `{"value": 1}`
into a float property becomes `1.0` and everything is fine.
`shader_set_uniform` ends at `ShaderMaterial.set_shader_parameter`, which stores
the Variant exactly as handed over -- so the same `1` lands as a Godot `int` in
a `float` slot, reads back as `1` through the tool that would catch it, and is
*discarded by the save*, reverting to the shader's default (#612). The tool, its
reader and `editor_save_scene` all report success on the way to losing the
value. **When two tools look like siblings, find the engine call at the bottom
of each; that is where they stop being siblings.**

**Census the refusals, not just the answers.** Every session has run the
interesting probes twice, offline and live, and every one of those asked what
the *answer* looked like. The refusal is what a caller meets first, because no
editor running is the ordinary state of a machine. Asked of fourteen live-only
tools it is one string, fourteen times: "No atomic runtime route is available
for live dispatch", naming no engine, no editor and nothing to do (#615) --
while `audio_configure_bus` has a hand-written "Godot Editor is offline ... so
launch Godot to change it" sitting in `handleAudioConfigureBus` that the route
check answers in front of, so it has never shipped. **A message nobody can reach
is the same as no message, and a `grep` will not tell you which you have.**

**Look again, later.** #622 was filed saying `audio_configure_bus` changes
nothing on disk and nothing can persist it. Both halves were wrong, and the
mistake was looking once, immediately: the editor's bus-layout autosave writes
`default_bus_layout.tres` a few seconds afterwards, unprompted and not on
`editor_save_scene`. The finding survived in the opposite direction -- the tool
reports an in-memory change, with `revert_with` and `undo_redo_registered:
false`, for something that reaches a tracked project file on its own. **Anything
the editor owns may be written on its own schedule; a single `ls` right after
the call is a measurement of latency, not of persistence.**

**A control that cannot pass proves nothing.** Two probes this session reported
a clean sweep of failures because their control was broken rather than because
the subject was. `preview_value_rules.py`'s control emitted a signal with no
listener, which `signal_emit` refuses outright (#624), so every row failed and
the probe read as "the gate refuses everything". `file_encodings.py` checked for
the substring `utf` anywhere in the payload and matched `file_path:
"res://utf16.gd"`, marking the worst case as passing. **Write the row that must
stay green first, and make sure it is green for the reason you think.**

**The platform was a variable and eleven sessions held it fixed.** The harness
lives on Windows, so that is where every session ran, while README has been
asking for macOS and Linux testers the whole time. Running the same probes
elsewhere is cheap -- `.github/workflows/vibe-platform.yml` builds on
`macos-latest` and `ubuntu-latest`, runs the probes and prints the output, and a
push to a `vibe/**` branch is the whole trigger -- and it moves a surprising
number of answers. A file the process may not read is a `chmod` on POSIX and an
ACL nobody sets on Windows; a filename is bytes on Linux and UTF-16 on Windows;
`temp_directory_path()` is stable on Windows, is `/tmp` on Linux and is a
per-user `/var/folders` path on macOS that launchd sets and a bare environment
does not have. **Ask which of your constants is a variable somewhere else.**

**"POSIX" is not one platform, and the probe has to prove which one it met.**
The strongest finding of this session -- four project walkers reporting `400
invalid_arguments` because a filename would not serialise -- looked like a POSIX
finding and is a Linux one: APFS refuses a non-UTF-8 name outright, with `[Errno
92] Illegal byte sequence`, so a Mac cannot even be put in that state. The probe
skips that row *by name* on macOS rather than passing it. A skipped row is not a
passing row, and a row that passes because the precondition failed is worse than
either.

**A census of arguments is not a census of states.**
`handler_error_census.py` reports zero bare strings on this build and two
survive, both reachable only when a file on disk is present and wrong:
`export_presets.cfg` that will not parse, and a preset name the file does not
hold. That is #460's lesson -- "the argument check answers junk first" -- one
layer further out: there, the key was an argument that was meaningful and wrong;
here it is a *project* that is. `project_state_census.py` varies the project and
asks two questions per row, and the second one is the one that keeps paying:
which states answer identically. `project_export`'s `dry_run` cannot tell five
states apart, and the real call fails in all five.

**The probe's own argument names are part of the experiment.** Twice in one
session a block of rows came back green because the call never reached the code
under test: `script_create` takes `script_path` and `source_text`, not
`file_path` and `content`, so every symlink-containment row was really the
argument validator answering, and `overwrite: true` is confirmation-gated, so
the case-folding row was a `428` printed as `observed=None`. Both read as
"working" and "finding" respectively, and neither was either. The README has
said "read the schema before believing a result" since the second session; this
is what it costs when the probe is the one that did not.

**Ask what a preview is about, not just whether it looked.**
`gridmap_export_mesh_library` reads `source_scene` and writes `output_path`. Its
preview reports `preview_kind: "target_state"` -- the strong claim -- with the
`before` of `source_scene`, a file the call does not modify, while the file it
is about to replace appears only in the echoed arguments. Its three siblings
that take a source and an output get this right or say plainly that they did not
look. #448 was this shape in a reader; this is the same shape inside the
confirmation gate, which is where it costs the most.

**Read the shipped archive, not the build tree.** Two findings came out of
`tar -xzf` and sixteen bytes of Mach-O header: the macOS archive's
`.gdextension` declares `x86_64` and `universal` for a thin arm64 dylib, and its
deployment target is macOS 14.0, inherited from the runner's SDK and stated
nowhere. A third came from running the published Linux binary in six distro
containers: it needs `GLIBCXX_3.4.30`, so RHEL 9 -- whose glibc is exactly the
floor `release.yml` chose its container to hold -- cannot start it. The release
job's reasoning is about glibc; libstdc++ is the constraint that binds. **What
the project builds and what it ships are different artefacts, and only one of
them is what a user gets.**

**Asked and green, so the thirteenth session can spend its budget elsewhere.**
A symlink out of the project is refused by the reader, the preview and the
writer, each naming the reason ("file path resolves outside the project root").
Two servers over one Unix socket behave exactly as two over a named pipe: the
second is told `bridge_held_by_another_client` with the holder's pid, and a
project with no editor still gets the plain 503, so #527 has not come back on
POSIX. Managed recovery runs on Linux -- workspace copy, `armed`, checkpoints,
a live bridge to the owned editor -- which is the `fork` plus `PR_SET_PDEATHSIG`
path no suite had executed. The unknown-argument, execution-mode and
error-envelope censuses return the same numbers on Ubuntu as on Windows. #546's
fix -- take the on-disk spelling from `std::filesystem::canonical` rather than
trusting the argument -- holds on macOS, which is the *other* case-folding
platform and the one it was never run on: the runner prints `host folds case:
True`, the preview names `res://casecheck.gd` with its real size and digest for
a call made as `res://CASECHECK.gd`, the write reports the same, and one file
exists afterwards. And a Godot editor runs headless on Linux with the addon
loaded and publishes a session, which makes live coverage on that platform a
thing a container can do.

**A headless editor is a configuration, not a degraded one.** Twelve sessions
drove an editor with a window, because that is what a desktop has. A build
machine, a container and a box reached over ssh have `--headless`, and so does
every CI runner, which is why no session had ever seen one. Didi attaches
happily -- session published, bridge green, `68 live now` -- and then every tool
whose answer is a picture returns `404 not_found` with the three words "Viewport
image is unavailable", while the strictly *worse* state, no editor at all, gets
a synthesised frame and a sentence naming the cause (#676). `editor_save_scene`
makes the engine print `ERROR: Parameter "t" is null` from the dummy renderer on
every save and reports `saved` (#683). Identical on Windows, macOS and Ubuntu.
**Ask what your subject looks like without the thing your machine always has.**

**Watch the process exit, not only the calls.** Every probe here calls a tool and
reads the answer; none had ever looked at what the editor does on the way out.
It aborts, on both POSIX platforms and on both invocations, immediately after
`[Didi] Didi Native MCP Editor Plugin deactivated.` -- `malloc_consolidate():
invalid chunk size` on Linux, an uncaught `std::system_error: mutex lock failed`
on macOS -- and exits 0 on Windows (#688). A crash on shutdown needs a control
more than most findings do, because "Godot does this" is entirely plausible and
the subject is a whole engine; `editor_exit_status.py` runs the same invocation
with and without the addon and prints the four statuses together. Without the
addon, all four are 0.

**A tool that shells out has a failure its caller cannot see.**
`script_check_syntax` runs Godot with `--check-only` and merges the compiler's
diagnostics into its own. Point `GODOT_BIN` at a real file that is not Godot and
the launch fails, the compiler diagnostics are simply absent, and a script with
four parse errors comes back `has_errors: false`, `diagnostics_count: 0`, with
`engine_version: null` as the only trace and no `exit_code` at all (#677). The
sibling that does the same job for shaders says "Failed to launch process" on
the same misconfiguration. A directory or a missing path is caught and falls
back to discovery -- the one value that gets through is the *plausible* wrong
one. **When an answer is assembled from a subprocess, ask what the tool says
when the subprocess never ran.**

**An honesty field can be switched off by the path the caller took.** Three
tools shell out to a discovered engine and publish whether it is the one the
caller is editing in. `matches_attached_engine` is `false` and correct after an
explicit `runtime_attach_session`, and `null` without one -- on the same server,
against the same editor, on the path every other live tool routes over
transparently (#687). `runtime_launch` has no such field at all: which engine
ran the project appears only in Godot's own banner inside the captured logs.
**A field that reports a mismatch has its own precondition; find out what it
is.**

**The layer below `tools/call` has a layer below it too.** `blackboard_patch`
hands back nlohmann's exception text -- `[json.exception.parse_error.105]`,
`[json.exception.out_of_range.403]`, `[json.exception.other_error.501]` -- for
every failure past the argument check, without saying which of up to a hundred
operations failed (#679). The one case the server checks itself reads like the
rest of the surface: "Argument 'operations' entry 0 must be an object, not an
integer." The `items` schema is `{"type": "object"}` for a closed RFC 6902
vocabulary, which is #570's lesson where the unconstrained string is the whole
operation object.

**Two agents is the supported path, and only tasks are protected.** The
blackboard exists because more than one client is expected, and the lease covers
tasks. Keys have no revision and no compare-and-set: both agents read 0, both
write 1, the board holds 1, neither call is an error (#682). Writes to
*different* keys compose correctly, which is the important half. A key whose
`ttl_seconds` has lapsed reads exactly like a path never written -- `found:
false` both times, with `include_metadata: true` set on both (#680) -- while the
lease one layer up answers `409` with `leased_by`. And `blackboard_clear`, the
only destructive call in the family and the only one gated unconditionally,
takes neither `author` nor `reason`, which every value write does (#681).
**Run two of the thing, then ask which half of it the guarantee covers.**

**A mode is a product, and there are three nobody had started the server in.**
Thirteen sessions probed the confirmation gate and every one ran a server in the
mode where the gate exists. With `--yolo`, `initialize`, the annotations and
every tool's `_meta.didi` are byte-identical to the default; the only difference
on the published surface is one fact inside `didi_control_room`, and the
per-result `_meta.didi.confirmation: "skipped"` arrives after the mutation
(#684). `--log-level DEBUG` is worse than invisible: the startup log is one line
per registered tool, the logger writes from the thread that would service the
request, and against a client that does not drain stderr the pipe fills and
`initialize` is never answered -- one row of six, and the one the field-trial
docs tell people to use (#689). **Read the process's own `--help`; each flag is
a product nobody has probed.**

**The launcher is not the program.** `--managed-editor` pointed at Godot's
Windows `*_console.exe` refuses to start -- "Owned editor did not attach within
30 seconds; inspect editor log" -- and the editor log shows a healthy editor
with the plugin active. That build is a launcher: it starts the ordinary editor
as a child, the child publishes the descriptor 3.7 seconds in under a pid that
is not the one didi spawned, and managed mode waits out its whole budget for a
pid that will never appear (#678). The same command with the non-console binary
answers `initialize` in six seconds. Session twelve ran managed recovery on
Linux, where the launcher does not exist, and found it green. **When a platform
ships two binaries for one program, the harness has been using one of them.**

**Asked and green this session, so the fourteenth can spend its budget
elsewhere.** A project reached over UNC is opened, read, written and searched
correctly, reports its own path in that form, and `path_confinement.py` holds on
it unchanged. `--recovery-workspace` pointed at a directory that already has
files in it refuses -- "Managed container must be new" -- and touches nothing.
`viewport_set_camera_transform` answers well: `old`, `new`,
`undo_redo_registered`, and the sentence about the change being in the editor
rather than on disk. `ui_hit_test` is correct on a headless editor -- the right
`local_point`, the invisible `Button` excluded, `hit_count_total: 0` outside
every rect -- because a Control's rect is layout, not rendering.
`asset_reimport` on a real imported `.png` works headless on all three
platforms, `csharp_check_build` on a project with no C# refuses by name, and
`runtime_launch` reports its own timeout as `success: false` with a sentence
beside `exit_code: 124`. `didi_control_room` carries the server's own log from
`INFO` down. The annotations are no longer a function of one read/write bit --
the four hints take seven distinct combinations and `openWorldHint` is set on
exactly the tools that start a subprocess -- so #507 has not come back, and
`_meta.didi`'s `currentMode` matches what each tool then answers, offline and
live. Of the 126 names, eleven had never appeared in any probe, README or
manifest here; five are legacy aliases, three of the rest are the registered
unimplemented set, and `spatial_query_raycast_batch`, `project_set_input_action`
and `shader_get_visual_graph` answered correctly. Eight of nine reader pairs in
`empty_versus_absent.py` are distinguishable; the ninth is #680.

**The handshake makes claims too, and one of them is about the surface holding
still.** Every session has read `tools/list` -- for modes, for annotations, for
schemas, for `outputSchema` -- and none had read the sentence in `initialize`
that says a host need only read it once. `capabilities.tools.listChanged: false`
is that sentence, and all 126 entries change when the bridge does: 68 change
`currentMode`, and the other 58 change anyway because `editorConnected` and
`sessionKind` are published per tool (#701). Nothing sends
`notifications/tools/list_changed`; `grep -rn "list_changed" src/` finds nothing.
#35 is the same shape resolved the other way, by withdrawing an over-claim.
**Read what the handshake promises before reading what the tools say.**

**The fourth tool in a family is the one nobody wrote.**
`script_check_syntax` and `shader_check_compile` have been probed for three
sessions and publish `engine_executable`, `engine_version`, `engine_available`
and `matches_attached_engine` between them. `csharp_check_build` sat unswept for
fourteen sessions for one reason -- no sandbox had a `.csproj` -- and the fixture
is five lines. It publishes none of those four (#704), counts every MSBuild
diagnostic twice while dropping the codeless ones (#702), reports absolute host
paths beside its own `res://` (#703), and prefers an undocumented `.sln` whose
empty build reports `success: true` (#706). Four findings from one fixture.
**A tool that has never been called is not a tool that works; find out what the
fixture costs before assuming it is expensive.**

**A refusal that reaches the engine is not the same as a refusal that does not.**
`editor_render_ghost_preview` validates arguments in the server and dimensions in
the editor. An argument-level refusal leaves the live preview alone; an
engine-level one tears it down and reports `409`, and the clear afterwards says
`cleared_previews: 0` (#707). The two refusals are indistinguishable to a caller
and the control is one row apart in the same probe. **When a tool validates in
two places, ask what each refusal costs, not only what each one says.**

**Run the old probes; a green number is a claim with a date on it.**
`advertised_vs_reported_mode.py` was written for #503 and recorded green at the
end of session thirteen. Re-run on `28d1c2193b37` it reports two tools
advertising `live` and answering `local` -- a value their own `executionModes`
array does not contain (#713). The probe cost a minute and the finding was not
reachable any other way, because both tools answer correctly in isolation.
**A regression probe only earns its keep if somebody runs it.**

**The fix that was scoped to one identifier.** #488 special-cased `self` in the
expression sandbox because Godot's `self can't be used because instance is null`
"reads like a fault in the caller's expression rather than a fact about the
sandbox". Every *other* unbound identifier still gets it: `Time`, `Performance`,
and a typo like `nodee` (#712). The comment in the source states the diagnosis
correctly and the guard matches one string. This is #547's shape -- a fix applied
to the case that was in front of it -- inside a message rather than across modes.

**Five bytes is not a margin.** The macOS AF_UNIX `sun_path` is 104 bytes and the
runner's temporary directory is 48 of them, which the platform workflow has been
printing since session twelve without anyone forcing the overflow. Forced, the
result is: plugin reports itself active, no descriptor published,
`runtime_list_sessions` empty, and nothing anywhere naming a length (#711). Both
ends of the bridge answer it with a bare `return false`. **A number a workflow
has been printing for three sessions is a measurement nobody has acted on.**

**A preview can know something and not say it.** #571 is a preview that
resolves less than the call, and #572 is a token that binds to the call and not
to the world. `project_rename_references` is the third: its preview and its
confirm agree on every shared field, and only the confirm carries
`code_references_not_updated` -- the list of call sites, the declaration
included, that the rename will leave saying the old name (#716). The count is in
the preview; the list is not, and the list is the fact the caller is deciding
about. **Diff the preview against the confirm key by key, not value by value:
the finding can be a key that is only on one side.**

**A flag every consumer honours except the one that speaks first.**
`--ui-app off` hides the `ui://` resource from the listing, refuses it on a read
with a message that names the flag, and shrinks `didi_control_room`'s `_meta`
from 277 bytes to 196. `initialize` declares the MCP Apps extension anyway, byte
for byte the same in all three modes (#717). The comment above the declaration
explains why it is unconditional -- the surface is opt-in on both sides, so the
client's declaration is the other half of the negotiation -- and that argument
holds for `auto` and does not for `off`, where the operator has already decided
and no client declaration can change the answer. **When a flag has three values,
check the consumers against all three: a guard written for the negotiated case
can be correct there and wrong at the ends.**

**Asked and green this session, so the fifteenth can spend its budget
elsewhere.** `resources/subscribe` works end to end: a board written by a
*separate process* produces `notifications/resources/updated` in about 0.6
seconds, on the registered `default` board and on one `resources/list` does not
carry, and the stream goes quiet on unsubscribe -- the refusals for
un-notifiable URIs name the reason. Every published `outputSchema` is honest:
zero required properties absent and zero type mismatches across twenty tools.
`eval_gdscript`'s sandbox refused all nineteen escape attempts -- the
filesystem, the environment, `OS.execute`, `shell_open`, a network client,
`queue_free`, every singleton not on the denylist, and assignment -- each by
name, because the gate for calls is an allowlist rather than the denylist the
error message suggests. `scene_call_method` is genuinely script-declared-only:
`queue_free`, `free`, `set_script`, `set_owner`, `emit_signal`, `call`,
`connect` and `set` are each refused as "not a method this node's script
declares", with a `@tool` script attached so the control passes.
`scene_instantiate_node` reports the engine's own spelling for a name Godot
sanitises, which is #546 handled correctly. Attaching to a session belonging to
another project is refused with both roots in `data`. `blackboard_patch` and
`blackboard_write` agree on path rules, and the board's `.`-or-`/` separator is
documented. The C# findings and the unknown-argument, execution-mode and
error-envelope censuses are identical on Windows, macOS and Ubuntu.

**A control that is missing is worse than a control that fails.** Two rows this
session read as findings until a control was added, and one of them was in a
probe from session twelve. `posix_platform.py` asked four project walkers about
a file whose name contains a newline; `project_audit_assets` names no script at
all and `project_get_uid_map` answers from the import cache, so their silence was
about `.gd` files rather than about the name. It writes a `plaincontrol.gd`
beside the odd ones now and says "cannot be asked" where it used to score a
miss. The other was `output_schema_honesty.py` reporting nineteen type
mismatches, all booleans "answered as number", because `bool` is a subclass of
`int` in Python. **Before filing, ask what the row would say if the subject were
perfect.**

## Sessions so far

| Date | Scope | Server | Findings |
| :--- | :--- | :--- | :--- |
| 2026-09-11 | Offline surface, then a live 4.5.1 editor: protocol edges, argument validation, the confirmation gate, offline path resolution, honesty of empty results. | `1.8.0+11aa42d92371` | #396–#408, thirteen findings. All thirteen were fixed the same day, in PRs #409–#414. |
| 2026-09-11 | Whole-surface censuses (unknown arguments, execution modes, error envelopes), then narrowing: non-ASCII identifiers, `dry_run` semantics, project.godot references, search coverage. | `1.8.0+6164335c8868` | #416–#427, twelve findings. |

| 2026-09-11 | Mutation honesty (write, then read back from the file and the engine), scene identity across a scene switch, semantic failures behind valid arguments, and the JSON-RPC layer below `tools/call`. | `1.8.0+5c34220b7d42` | #437-#450, fourteen findings. |

| 2026-09-12 | Inverse pairs and duplicates, path forms, property coercion, the confirmation preview, resource honesty, and two new whole-surface censuses. | `1.8.0+8fd6409268ac` | #460-#472, thirteen findings. |

| 2026-09-12 | Limit and truncation disclosure, schema flags against what the handler does, project settings and input actions written without checking, the error envelope's contents, and the legacy aliases. | `1.8.0+941db00ef6c6` | #482-#493, twelve findings. |

| 2026-09-12 | The discovery surface rather than the answers: advertised versus reported execution mode, tool annotations against what the tool does, published schemas against what the handler enforces, and the `resources/*` and `prompts/*` methods, swept for the first time. | `1.8.0+95ff4b9c9ebc` | #502-#515, fourteen findings. |

| 2026-09-12 | The methods around `tools/call` rather than through it, two servers against one editor, the blackboard's own stated rules, path confinement and what the write actually does with the path, and the editor killed mid-session. | `1.8.0+3a528c10387d` | #525-#537, thirteen findings. |

| 2026-09-14 | What the host operating system does to an argument the server already accepted: a case-insensitive filesystem, device names, dot segments. Then files this harness did not author -- CRLF, a BOM, a missing final newline -- the empty string sent to every required string parameter, and the handshake once it is past. | `2.0.0+39daaad58e9d` | #546-#557, twelve findings. |

| 2026-09-14 | The tools that supply part of their own input, and the preview path against the call path: the visual diff, the confirmation token against the world rather than the call, `dry_run` versus real, the largest accepted argument rather than the smallest, and the whole live surface re-asked on a second engine. | `2.0.0+74578cb657ee` | #568-#577, ten findings. |

| 2026-09-14 | The running game as a second kind of session (pause, step, injected input, invariants, explore, stop, and a fresh server beside two live sessions), Godot's own ownership rules asked of the scene tools (instanced and inherited nodes, a scene instanced into itself), the editor's own log read after every call, and every method bind checked against three engine API dumps. | `2.0.0+0ddfa3614b61` | #588-#603, sixteen findings. |

| 2026-09-15 | The domain tools given something to bite on for the first time (a real TileSet, MeshLibrary, ShaderMaterial and AnimationPlayer), the project path before the server parses it, files that are not valid UTF-8, the preview path against the call path for argument *values*, and the refusal every live-only tool gives when no editor is running. | `2.0.0+e8999e1bd52f` | #611-#625, fifteen findings. One of them, #622, was filed wrong and corrected in place. |

| 2026-09-15 | The other two supported platforms, for the first time: a Linux and a macOS runner beside a local Ubuntu container and a headless Godot editor on it. POSIX states Windows cannot make (unreadable files, symlinks, non-UTF-8 names, FIFOs), the shipped archives read as artefacts rather than as build output, a census of project *state* rather than of arguments, and the export and gridmap families, never swept. | `2.0.0+c3fcfb282883` and `2.0.0+nogit` (Linux) | #647-#657, eleven findings. |

| 2026-09-16 | A headless editor, the only kind a runner can have and the only kind no session had probed, on Windows and -- for the first time anywhere -- on live macOS and Linux runners. Then the modes and the processes around the surface: `--yolo` and `--log-level DEBUG`, `--managed-editor` on Windows, a project over UNC, two servers writing one blackboard, the engines the subprocess tools shell out to, and what the editor does on the way out. | `2.0.0+0aadad99005d`, and `2.0.0+408a953f8f37` on the runners | #676-#689, fourteen findings. |

| 2026-09-16 | The handshake's own claims rather than the tools': `listChanged: false` against a listing that moves with the bridge, and every parameter description against the schema keys beside it. Then `csharp_check_build` given a real `.csproj` for the first time in fourteen sessions, `project_export` and the ghost previews on Windows, the macOS `sun_path` overflow the twelfth session measured and never forced, the expression sandbox and the method channel walked for containment, and the old censuses re-run. | `2.0.0+28d1c2193b37`, and `2.0.0+5349d27e5f59` on the runners | #701-#717, sixteen findings. |

Add a row per session. The table is the reason this directory exists: a finding
that keeps coming back in a new place is a design problem, and only the log
makes that visible.
