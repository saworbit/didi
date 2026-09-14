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
| `bind_census.py` | Not a probe against the server: every `(class, method, hash)` bind in `src/gdextension` checked against `--dump-extension-api` output from each installed engine, `hash_compatibility` included. A miss is a null bind that prints at startup or answers 501 at call time. |
| `fixtures/` | What the ownership and game probes need beyond the sandbox: a sub-scene, a scene inheriting `main.tscn`, a game scene with a ticking script, a `Node3D` script. `sandbox.py --fixtures` copies them in. |
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

Add a row per session. The table is the reason this directory exists: a finding
that keeps coming back in a new place is a design problem, and only the log
makes that visible.
