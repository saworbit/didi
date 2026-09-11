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
`probe.py --schema` is still faster than a round trip.

**Count it before you call it a bug.** Three of this session's findings only
became findings once every tool was asked the same question: 50 of 126 rejected
an unknown argument, 26 report `offline_fallback` with an editor attached, 18
answer with a bare string. Each looked like one tool misbehaving until the
census showed a missing default. The first is now 121 of 126, and the five that
are left are the unimplemented registrations, which refuse the whole call
before any argument is read. `probes/surface_census.py` runs all three in
about a minute each; the numbers are in the session log and are worth diffing.

**Narrow before you file, especially when two things changed.** This session
nearly filed "`editor_undo` reports success without undoing anything" — the
group list really was unchanged after an undo. The undo had reverted a node
*rename* from an earlier probe, so the later reads were looking at a path that
no longer existed. A tighter repro (set a property, undo, read it back) showed
undo working correctly and reporting `409 Nothing to undo` on an exhausted
stack. A sandbox accumulates state; a finding that depends on three earlier
mutations is a finding about your probe order until proven otherwise.

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

Add a row per session. The table is the reason this directory exists: a finding
that keeps coming back in a new place is a design problem, and only the log
makes that visible.
