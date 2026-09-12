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
| `probes/surface_census.py` | The same question asked of all 126 tools: who rejects an unknown argument, what `execution_mode` each reports, who answers with a bare string. |
| `report.py` | Files a directory of finding bodies as issues in one pass. |

The probe files are kept after their findings are fixed, and are worth re-running
for exactly that reason. Re-run against `abd8911`, every one of them now shows
the fixed behaviour: `schema_enforcement.json` was a list of twelve things the
server got wrong and is now twelve argument errors that name the property;
`offline_paths.json` gets a sentence explaining why a node path cannot be read
without an editor, plus `requested_root_path` on the fallback and
`target_exists` on an impact query. A probe that starts failing again is a
regression nobody wrote a test for.

## A session

```powershell
# 1. a project you are free to break, with the current addon in it
python tools/vibe/sandbox.py $env:TEMP\vibe\proj --launch C:\Godot\Godot_v4.5.1-stable_win64_console.exe

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

**Offline and live are different products.** The same argument can mean
different things depending on whether an editor is attached, and the offline
answer is the one nobody checks. Run the interesting probes twice.

**One process or two changes the answer.** Confirmation tokens live in the
server's memory. A dry run in one `probe.py` invocation and a confirm in the
next is not a test of the gate, it is a test of process lifetime.

## Sessions so far

| Date | Scope | Server | Findings |
| :--- | :--- | :--- | :--- |
| 2026-09-11 | Offline surface, then a live 4.5.1 editor: protocol edges, argument validation, the confirmation gate, offline path resolution, honesty of empty results. | `1.8.0+11aa42d92371` | #396–#408, thirteen findings. All thirteen were fixed the same day, in PRs #409–#414. |
| 2026-09-11 | Whole-surface censuses (unknown arguments, execution modes, error envelopes), then narrowing: non-ASCII identifiers, `dry_run` semantics, project.godot references, search coverage. | `1.8.0+6164335c8868` | #416–#427, twelve findings. |

| 2026-09-11 | Mutation honesty (write, then read back from the file and the engine), scene identity across a scene switch, semantic failures behind valid arguments, and the JSON-RPC layer below `tools/call`. | `1.8.0+5c34220b7d42` | #437-#450, fourteen findings. |

| 2026-09-12 | Inverse pairs and duplicates, path forms, property coercion, the confirmation preview, resource honesty, and two new whole-surface censuses. | `1.8.0+8fd6409268ac` | #460-#472, thirteen findings. |

| 2026-09-12 | Limit and truncation disclosure, schema flags against what the handler does, project settings and input actions written without checking, the error envelope's contents, and the legacy aliases. | `1.8.0+941db00ef6c6` | #482-#493, twelve findings. |

| 2026-09-12 | The discovery surface rather than the answers: advertised versus reported execution mode, tool annotations against what the tool does, published schemas against what the handler enforces, and the `resources/*` and `prompts/*` methods, swept for the first time. | `1.8.0+95ff4b9c9ebc` | #502-#515, fourteen findings. |

Add a row per session. The table is the reason this directory exists: a finding
that keeps coming back in a new place is a design problem, and only the log
makes that visible.
