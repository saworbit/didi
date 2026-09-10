# Changelog

All notable changes to **Didi** (`godot-mcp-native`) will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical entries describe the surface advertised by those releases. For the executable status of each current registration, use [docs/CAPABILITIES.md](docs/CAPABILITIES.md) or runtime `tools/list` metadata.

---

## [Unreleased]

The status block below states the current surface rather than anything this
release changed, which is why it lives here and not in a version section.

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `112/115`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Discovery now exposes 115 canonical tools plus 10 legacy registrations (125 total). 112 canonical tools are implemented and 3 remain unimplemented.
The three Phase 7 blockers are unchanged; the new name is `didi_control_room`, recorded in [Surface Amendments](docs/SURFACE_AMENDMENTS.md).

### Fixed

- `scene_create` and `scene_pack_branch` no longer write a uid the engine never
  learns (#379). `ResourceSaver.save` puts the uid in the file, but only Godot's
  own save callback registers it with `ResourceUID`, and that callback does
  nothing while `EditorFileSystem` is scanning, which is exactly the window an
  agent writes in after attaching to a freshly started editor. The scan's
  directory snapshot predates the file, so the scan did not pick it up either.
  Every load of a referencing scene then warned and fell back to the text path.
  Both writers now call `EditorFileSystem.update_file`, report `uid` and
  `uid_registered`, and when a scan deferred the work they say so and Didi
  re-indexes the path once the filesystem settles.

- `script_check_syntax` no longer reports a false error for every script that
  names an autoload (#383). Godot's `--headless --check-only` runs in a process
  with no `SceneTree`, which is where autoloads are registered, so it reported
  `Identifier not found` for a script the engine compiles and runs. It did so
  permanently, not until the next editor restart. Didi now reads the
  `[autoload]` section of `project.godot` and demotes those diagnostics to
  warnings carrying a `note`, along with the `Compilation failed` line that
  followed only from them, so `has_errors` is a verdict about the script again.
  An `Identifier not found` naming anything else stays an error, and a real
  parse error beside an autoload one keeps `has_errors: true`. `script_create`
  and `script_patch_method` surface the same check and get the same treatment.

- The extension no longer leaks one ObjectDB instance on a clean engine exit
  (#373). Its engine output logger is a `RefCounted`, and shutdown dropped the
  reference taken at install without freeing what that drop released. Godot
  documents `unreference()` as returning true when the object should be freed
  after the decrement, and freeing is the caller's job. The instance stayed in
  ObjectDB with its class already unregistered, which is why the verbose
  report named no class. A game process now exits with no leak warning, and
  the live harness asserts that.

- `asset_reimport` no longer reports success for a path Godot has no importer
  for (#374). Godot's import system owns only the files carrying a `.import`
  sidecar, and `EditorFileSystem.reimport_files` printed
  `importer for type '' not found` to the editor output for each of the others
  while returning nothing the tool could see. The batch is now split by what
  each path needs: sidecar files go to `reimport_files`, and the rest go to
  `EditorFileSystem.update_file`, which is the call Godot documents for a file
  changed outside the editor. The result carries both lists as `reimported`
  and `refreshed`.

- `scene_create` now creates the project-contained parent directory of a
  nested scene path instead of returning a bare `ResourceSaver.save failed
  with Error 19` (#372). `script_create` and `resource_create` already created
  theirs, so the writers disagreed with each other. `scene_pack_branch` shares
  the same save path and gets the same behaviour.

- The offline `missing_colon` rule no longer reads a four-character name
  followed by a space as an `else` header, so a plain `hits += 1` stops being
  reported as an `else` statement missing its colon (#371). Unlike the other
  block keywords, `else` carries no trailing space, and the check for what
  follows it never confirmed the line started with `else` at all.

---

## [1.8.0] - 2026-09-10

### Fixed

- Seeding a field trial no longer tries to execute a file that is not a
  program, and no longer waits forever when a probe does not come back. The
  seed asks the server it was handed for its build id and its tool manifest,
  and the unit tests hand it a fixture six bytes long with an `.exe` name. On
  Windows that launch reaches the antivirus filter driver before it fails, and
  behind the native suite's thirty thousand freshly written files it stopped
  coming back: a 1.8.0 release attempt sat in that one call for thirty-four
  minutes with a flat processor and one line of log.

  The module refuses those launches itself now, raising what the operating
  system would have raised, so every outcome is unchanged and nothing is
  spawned. The rule is the path rather than the name: anything under the system
  temporary directory was put there by a fixture in that file and is not a
  program, while `git` never is, so the tests that shell out to a real one are
  untouched. It is one guard for the whole module because there are three such
  fixtures and fixing the first one only moved the stall to the third.

  Both probes also carry a timeout, which is a different fault. `subprocess`
  bounds the wait for a child that started; a block inside `_execute_child` is
  the child never starting, and no argument reaches it. The timeout is there for
  a server that answers slowly, not for this.

- `ctest` says where it stopped. Both suites carry a timeout below the release
  job's own, and the Python suite runs unbuffered and verbose, so a run that
  stalls fails at a known bound and names the test it was in. Thirty-four
  minutes of the release attempt above produced one line, the one saying the
  suite had started.

- Every pull request now runs `ctest` against the same interpreter the release
  hands CMake. `release.yml` passes its virtual environment's Python through
  `-DPython3_EXECUTABLE` and `ci.yml` did not, so ctest's Python suite ran on
  the runner's own interpreter, which has no `jsonschema` and cannot import two
  of the modules. That difference is the only reason the release job's ctest
  could load them while a pull request's could not.

- Every pull request now runs `ctest`, which is what gates a tag. Nothing else
  ran it: the other steps run the native binary directly and then name Python
  modules one at a time, in separate processes and separate jobs, while `ctest`
  runs one process over all of them through unittest discovery. That is a
  different composition, and cutting 1.8.0 was the first thing to execute it. A
  release should not be the first run of a command.

- `Tools.OfflineCapabilityIsDerived` sets up the tool registry it reads instead
  of inheriting whatever an earlier test left there. It passed only in a full
  run and failed on its own, which is the opposite of what running a single test
  is for, and on its own it never reached the property it exists to check.

- `editor_reload_project` re-indexes the offline caches it says it re-indexed.
  With no editor connected it reported `Offline caches re-indexed.` and dropped
  nothing, so the cached resource index kept answering with what it read before.
  Callers reach for this tool after changing files outside Didi, which is
  exactly when that answer is wrong. (#358)

- The shared resource index is keyed on the directory rather than on the
  spelling of it. Almost every tool asks for `.`, so the cache filed two
  different projects under one key and could serve each the other's index.
  Resolving the path first is also what lets two tools that name the same
  project differently share one crawl, which is the whole point of the cache.

- The native suite no longer leaves its checkpoint fixtures in the temporary
  directory when a run dies inside a test. The destructor that removes them
  cannot run if the process never returns, and the file count boundary test
  creates ten thousand files, so repeated deaths piled up more than a hundred
  thousand of them. Nothing ever cleared those, and nothing said they were
  there.

  Each fixture now carries the pid that created it, and a run removes the
  fixtures whose owner is gone before it creates its own. A pid that still
  answers is left alone, so a suite running at the same time in another process
  keeps its files; the safe mistake is to keep a stale directory, not to delete
  a live one. The destructor now reports a removal it could not complete instead
  of discarding the error code, which is what made the pile up silent. Two
  suites run side by side stay green and clear the temporary directory between
  them. (#363)

- Offline tools no longer hand their child processes the server's standard
  input (#350). Didi speaks JSON-RPC on stdio, so a `dotnet build`, a `git`
  call or a headless Godot helper inherited the same handle the server reads
  requests from. Two readers on one stream race whether or not the child ever
  wants input, and a child that does want input waits for bytes that were meant
  for the server and will never arrive.

  `NUL` on Windows and `/dev/null` on POSIX, which is what
  `runtime/managed_process.cpp` already did. Reproduced before fixing: with the
  parent's stdin held open by a pipe, a child that reads to end of input blocks
  until the timeout; with the fix it returns immediately.

- The test runner binds spawned processes so a timeout cannot orphan them
  (#351). On Windows it now launches suspended, assigns the process to a job
  with `KILL_ON_JOB_CLOSE`, and then resumes, so there is no window in which the
  child runs outside the job. `TerminateProcess` alone only killed what
  `pi.hProcess` pointed at, and when Godot resolves to a `godot.cmd` wrapper
  that is the interpreter -- leaving the engine running detached, holding file
  locks and interfering with the next session. On POSIX the child calls
  `setpgid` and the timeout signals the process group rather than the single
  process.

  Covered by a test that reproduces the orphan: `GODOT_BIN` points at a wrapper
  script that starts a background grandchild publishing its own pid, the
  session is given three seconds, and the grandchild must be gone afterwards.
  Against the unfixed runner it fails on `!processAlive(grandchild)` and leaves
  three live processes behind, which is the defect as a user meets it.

- `resource_create` validates its target through
  `paths::resolveProjectFileForWrite` instead of its own copy of the rules
  (#352), and writes through the resolved path rather than the raw relative
  one. The private copy caught an escaping path but accepted shapes every other
  writing tool refuses -- an absolute path landing inside the project root, for
  one -- which is the disagreement `project_path.hpp` says that function exists
  to prevent. It also called `projectPathFromUtf8` outside its own `try`, so a
  `save_path` that is not valid UTF-8 threw out of the handler instead of
  returning an error.

- `scene_get_selection` is no longer listed as an offline capability (#353). A
  selection exists only in a running editor and there is no offline
  implementation. The live set is tested first, so the wire answer was already
  correct, which is exactly why the dead entry survived: it changed nothing
  until the order changed.

- The workflows that assert the pinned `jsonschema` version no longer read
  `jsonschema.__version__`. That attribute is deprecated as of 4.26.0 and its
  own warning says it will be removed, at which point the check would have
  raised `AttributeError` and taken the release gate down with it -- on a
  routine dependency bump, in the job that packages a release.

  `importlib.metadata.version` instead, which is what the deprecation warning
  points at. Verified under 4.26.0 with deprecation warnings promoted to
  errors, and verified to still reject a mismatch.

  Found by installing the version Dependabot proposed and running the check
  against it, rather than by reading the diff. The diff is one line.

### Added

- A field trial can be run unattended. `tools/field-trial/trial.py` seeds the
  working directory, briefs a fresh tester in its own client session, and scores
  what that tester did from the transcript rather than from its own account of
  itself. `--dry-run` exercises everything except the spending.

  The reason to automate it is not the agent's hour, it is the maintainer's.
  Three trials have now produced defects no suite found, and each cost a morning
  of seeding by hand and remembering which manifest to score against. The
  manifest is now dumped from the binary under test rather than copied from
  wherever one is lying: gating trial 01 scored a binary emitting 94 canonical
  tools against an on-disk manifest claiming 83, so the uncalled set, which is
  the interesting half of a coverage report, was wrong about eleven of them.

  It also closes the finding trial 03 paid for. A trial is seeded against a
  server binary and scored against that binary's manifest, while the live half
  of every call is answered by a GDExtension the tester installs by hand and no
  artifact recorded. That run spent about an hour concluding a shipped
  capability did not exist, because the bridge answering it was six days older
  than the server. The seed now records the server's build id and the hash of
  the addon the tester is meant to install, alongside the one lying in the
  repository's own gitignored `addons/didi`, and `bridge.py` reads back the
  pairing the server reported on every live session call. Its third verdict is
  the one worth having: a run where no call ever reported a pairing is
  `not_observed`, not clean, because nothing in it says which build served it.

- `didi --version` prints the build id under the release version.

  The version cannot tell two builds apart, and the server and the GDExtension
  are separate files a user copies around separately. A session that has
  attached reports both halves and says whether they match; before one is
  attached, this is the only way to record which build a run was handed.

- Fuzz targets for the three places Didi reads bytes it did not write: the IPC
  frame decoder, the JSON-RPC request parser, and base64. libFuzzer, built with
  ASan and UBSan, running on every code pull request and for longer nightly.

  This started as a way to raise an OpenSSF Scorecard number and stopped being
  that almost immediately. Reading `parseFramedMessage` closely enough to write
  a target for it found a buffer over-read -- fixed in #344, before a single
  fuzzer had been compiled. The only test that function had round-tripped a
  frame the same code had just written, which is the one input shape guaranteed
  not to find it.

  Worth stating what was declined. Scorecard detects C++ fuzzing by looking for
  the string `LLVMFuzzerTestOneInput` in a `.cc` file, so the point was
  available for the price of committing a file that nothing compiles or runs.
  That is the failure this repository's own documentation validator already
  polices for Python tests: a test that does not execute is worse than none,
  because it looks like coverage. These targets are built against `didi_core`
  and executed in CI, and the corpus persists between runs so findings compound
  instead of restarting from empty.

  The eight bytes that used to segfault the frame decoder are a committed seed,
  re-executed on every fuzz job for as long as the target exists.

- The live harness now proves the loop composed rather than in halves: pause a
  running game, inject one action, advance exactly one frame, and read back the
  fixture's own `_input` and `_process` counters.

  Both halves were already covered, and neither covered this. Stepping was
  proven on a paused game; injected delivery was proven on a running one, read
  back after a profiler window had let real time pass. An agent does neither.
  It pauses, presses, advances a known number of frames and looks, and nothing
  in the suite said the press is there when it looks. Six assertions now say
  it, with no sleep and no profiler wait between the press and the read, so
  what passes is determinism and not the wall clock being generous.

  What they claim is bounded on purpose: the press is observed no later than
  the completion of the step that follows it, and injecting does not resume the
  game. Which side of the step the engine flushes the event on is left unpinned,
  because that is the engine's business and an assertion about it would break
  on a change nobody using Didi would care about.


### Changed

- A viewport diff converts each image's pixels to luma once instead of twice.
  `structuralSimilarity` and `perceptualHash` each built their own pair of
  planes, so one diff held four of them; at the 2048 capture limit a plane is
  32 MB. Two frames that are the same bytes now skip the block pass and the
  second transform as well, because SSIM is 1.0 by definition there and the
  second plane would be a copy of the first. The hash reported for those frames
  is still the real hash rather than a zero: that would be a different answer,
  not a cheaper one, and a test now says so. (#354)

- `base64::decode` reserves its output. `encode` always did, while `decode`
  grew a byte at a time and reallocated its way through payloads that run to
  megabytes on every captured frame. (#357)

- `project_analyze_impact`, `project_rename_references` and
  `project_analyze_bloat` no longer crawl the whole project from scratch. They
  built their own indexer while every other read tool shared one, so inspecting
  a resource and then asking for its impact walked the tree and parsed every
  `.uid` file twice. They use the shared index now. `project_rename_references`
  deliberately does not: it rewrites files, and a resource created outside Didi
  while the cached list was alive would be missing from it, which is the
  half-applied rename its truncation check exists to refuse. (#355)

- Node path impact analysis skips a file that cannot mention the target. It used
  to duplicate the file text to mask comments and strings, then allocate a heap
  string per line, for every scene and script in the project, and throw the lot
  away one line later. Most files in a project never name a given node path.
  (#356)

- CodeQL now runs on every pull request rather than on a path filter. A change
  that "only touches documentation" is a claim worth checking rather than
  trusting. The cost is controlled by splitting the analyses: Python and
  Actions are about a minute each and always run, while the C++ analysis takes
  closer to twenty and runs only when something can reach the compiler.

- All ten of CodeQL's first-pass findings were triaged and dismissed with
  written reasons rather than left open. Alerts that can never be actioned are
  how a Security tab stops being read, and the next real finding then arrives
  looking exactly like the ones already learned to be ignored.

  Five of them are integer-multiplication overflows inside
  `stb_image_write.h`, vendored code this project is told not to modify. The
  first attempt excluded the vendored files through a CodeQL configuration
  file; it was loaded and had no effect. `paths-ignore` applies to interpreted
  languages and to compiled languages analysed without a build, and this
  analysis builds, because CodeQL for C++ observes the real compiler. The
  configuration file was removed rather than left in the tree describing a
  control that was not in force. None was a new defect: three are operator-nominated
  process launches that advertise `openWorldHint: true`, one is the documented
  `DIDI_SESSION_DIR` override, and one is a test probe reading its own
  argument. [SECURITY.md](SECURITY.md) records each disposition.

- The three API-blocked names now say what to use instead. `physics_simulate_step`,
  `nav_bake_mesh` and `runtime_get_call_stack` stay registered and unimplemented,
  and the documentation stopped ending the sentence there.

  Silence reads as absence. An agent told only that a name is not callable
  concludes the capability does not exist, and `LLM_INSTRUCTIONS.md` then sent
  it to hand-edit project files -- which is where every field trial's damage
  happened. Each blocked name now names its stand-in and, in the same breath,
  what the stand-in is not: `runtime_step` advances whole frames and not exact
  physics ticks at a caller's delta, a region baked in the editor is a bake
  Didi did not perform and cannot verify, and an error's originating file,
  function and line are one frame and not a stack. A stand-in offered without
  its limits is the same false success the trials named as the worst defect
  class, arriving in prose instead of in a response.

## [1.7.0] - 2026-09-09

### Added

- Signed releases. Every release archive now carries SLSA build provenance,
  signed through Sigstore by the release workflow, plus a `SHA256SUMS` file and
  the provenance bundle as `didi-<tag>.intoto.jsonl`.

  Didi ships prebuilt binaries, so "did this archive come from that source" has
  to be answerable by someone holding only the download. Until now it was not
  answerable at all. The check that matters names the workflow, not just the
  repository, because an attacker can sign something of their own but cannot
  produce a signature attributed to this repository's release workflow:

  ```bash
  gh attestation verify didi-linux-x64.tar.gz \
    --repo saworbit/didi \
    --signer-workflow saworbit/didi/.github/workflows/release.yml
  ```

  `--source-ref` is part of the documented check for a reason: the release
  workflow can also be run manually as a rehearsal, and those runs sign too, so
  their provenance carries the same repository and the same signer workflow.
  Only the ref separates a published release from a dry run, so a verification
  that omits it would accept a rehearsal artifact as something the project
  published.

  There is no signing key. The certificate is issued to the workflow run's own
  OIDC identity and expires in minutes, so there is nothing for a maintainer to
  leak, rotate or lose. The bundle ships as a release asset rather than living
  only in GitHub's attestation store, so verification works offline for someone
  who would rather not call the GitHub API -- and because that is the form
  OpenSSF Scorecard's Signed-Releases check reads.

  `SHA256SUMS` is covered by the same attestation, so it cannot be swapped
  independently of the archives it describes. [SECURITY.md](SECURITY.md) has
  the verification commands.

- A rehearsal for the release pipeline. Running the release workflow manually
  now builds, checksums and signs exactly what a tag would, then leaves the
  result as a workflow artifact instead of publishing it. The signing path used
  to be reachable only by tagging a real release, which meant the only way to
  find out whether it worked was to do the thing that cannot be undone.

- Repository automation and supply-chain hardening. Someone assessing this
  project from the outside can now see what is enforced rather than what is
  claimed.

  **Code scanning.** CodeQL runs the `security-extended` query set over the
  C++, the Python tooling, and the workflows themselves, on every code pull
  request and weekly. Before this the only automated security signal was
  Dependabot, which watches dependencies and says nothing about the C++ that
  parses JSON-RPC off a pipe. OpenSSF Scorecard publishes a supply-chain score
  behind a README badge, and dependency review blocks a pull request that
  introduces a known vulnerability or a copyleft licence.

  **Every action pinned to a commit SHA**, with the release named in a trailing
  comment. A tag is a pointer its owner can move at any time, which is how one
  compromised action leaked credentials from thousands of repositories at once.
  `tools/validate_documentation.py` now rejects a workflow that pins any other
  way, or that pins a SHA without saying which release it is. Every workflow
  declares least-privilege `permissions:`, and `actionlint` and `zizmor` run
  over them on every pull request.

  **A generated test inventory.** `tools/test_inventory.py` derives the counts
  from the suites themselves -- the native registry through `didi_tests
  --list`, the Python suites through `ast`, the live harness through its
  assertion sites -- and writes [docs/TEST_INVENTORY.md](docs/TEST_INVENTORY.md)
  and the README badge. CI runs `--check` after the build, so a stale number is
  a red run. The count this replaces went wrong often enough that the
  documentation validator carries a rule forbidding one specific out-of-date
  sentence about it.

  The page publishes the Windows figures and says so. The native suite is
  platform-conditional -- crash capture is Windows-only, and the IPC cases
  differ between a named pipe and a Unix socket -- so a single native total is
  false on two platforms out of three. The tool refuses to regenerate off the
  reference platform rather than quietly replacing them.

  **Branch protection on `main`**, a `CODEOWNERS` file, path-based pull request
  labelling, categorised release notes, stale-thread handling, and an
  `.editorconfig` that describes the indentation already in the tree.

- Added `ui_list_controls`: the Control nodes under a root, with the
  viewport-space rectangle each one occupies, its class, visibility, mouse
  filter, and its text where it has any. Editor or game.

  This is what makes a control addressable. `ui_hit_test` answers what sits
  under a point, which is only useful once you already have a point, and it is
  editor-only; `runtime_get_tree` gives the running tree with no rectangles and
  no text. An agent that had just written a menu and needed to press Start was
  left doing Godot's layout arithmetic on a `.tscn` itself, or guessing
  coordinates.

  The rectangle is `Control.get_global_rect`, the same one `ui_hit_test` reports
  for a hit, so listing a control and hit-testing the centre of its rectangle
  returns that control. Text is read as a property rather than through a
  `get_text` bind per widget class, so `Button`, `Label`, `LineEdit` and a custom
  Control exporting `text` are all covered by one path.

  Read-only, bounded to 10,000 traversed nodes and 256 results, and it injects
  nothing. Live only: a `.tscn` holds anchors and offsets, not the rectangle they
  resolve to. Verified against real editors on Godot 4.5.1, 4.6.2 and 4.7.2;
  every binding it uses was already shipped and carries an identical hash on all
  three.

- Added the Control Room: an interactive dashboard Didi serves to the host over
  the existing stdio connection, rendered in the conversation by clients that
  support MCP Apps. Red/amber/green lights for the bridge, project, safety
  posture and coordination board, each carrying the pid, path or session behind
  it; every registration with the execution mode it is in right now; and a tail
  of Didi's own log, which until now went only to a standard error stream that a
  client launching the server over stdio discards.

  One read-only canonical tool, `didi_control_room`, and one resource,
  `ui://didi/control-room`. The tool works with no host UI support at all and
  returns the same payload as text, so nothing depends on the extension.

  The extension is bilateral, so the UI surface is advertised only to a client
  that declared `io.modelcontextprotocol/ui` -- an unaware host is not handed a
  page of markup to read into a model's context. `--ui-app auto|always|off`
  overrides that, and `off` withdraws the resource rather than merely hiding it
  from the listing.

  The page loads nothing from anywhere, so no content security policy domain is
  declared and the host's default `default-src 'none'` applies unweakened. It
  builds every value with `textContent`, because the strings it renders -- paths,
  node names, log lines -- originate in files a project can contain, and a
  project is not a trust boundary. It ignores any message whose sender is not the
  host. The session token is outside the allowlist the payload is built from, and
  the build fails if that field name appears in either the payload or the page.

  This completes the recommended order in
  [Human Interaction Design](docs/HUMAN_INTERACTION_DESIGN.md), whose third step
  was deliberately left conditional on host support being broad enough to be
  worth it. See [Control Room Design](docs/CONTROL_ROOM_DESIGN.md).

- Added opt-in managed editor recovery: isolated project copies, saved-file checkpoints, one owned-editor restart, explicit reconciliation and preserved-workspace restoration. Four recovery tools expose state and actions without replaying uncertain edits. Ordinary attachment and runtime_launch remain unchanged. See [Managed Recovery](docs/MANAGED_RECOVERY.md) for coverage and limitations.

### Changed

- The Linux release artifact is built *inside* Ubuntu 22.04 rather than *on*
  it. The `ubuntu-22.04` runner image is being retired -- deprecation from
  2026-09-17, unsupported from 2027-04-17 -- and GitHub brownouts already kill
  jobs using the label. One killed a release rehearsal mid-compile, which is
  how this was found rather than by a failed release.

  The label was chosen for glibc in the first place: a binary built against
  2.35 starts on Ubuntu 22.04 and Debian 12, and one built on a newer host does
  not. Moving to `ubuntu-24.04` would have raised the floor to glibc 2.39 and
  silently dropped every Ubuntu 22.04 LTS and Debian 12 user -- a decision about
  who can run Didi, not a CI fix. Building in a pinned `ubuntu:22.04` container
  on a supported runner keeps the floor exactly where it was.

  The image is pinned by digest, because this build feeds the provenance
  attestation: what built the binary should be a fact rather than whatever the
  tag pointed at that day.

- Corrected the documented Linux minimum in
  [Administrator Guide](docs/ADMIN_GUIDE.md) from Ubuntu 20.04+ to Ubuntu
  22.04+ / glibc 2.35+. It had not been true of a published archive for some
  time: the build host sets the floor, and it had been 22.04. Nothing about
  what ships changed here -- only the claim made about it.

- CI decides what to run instead of running everything. A single cheap job
  classifies the changed files, and the two 30-minute Windows Godot integration
  matrices and the sanitizer build now start only when the change can reach
  code, fixtures, or the workflow itself. A documentation change used to start
  all four.

  The workflow-level `paths:` filters that used to gate this are gone, and that
  is the load-bearing part: a workflow skipped by a path filter reports no
  result at all rather than reporting success, so a required status check named
  on it leaves the pull request permanently unmergeable. A `CI Gate` job now
  runs unconditionally and fails only when something that did run came back
  red, which is what makes required checks usable here at all.

- The pinned `jsonschema` version is read out of `requirements-dev.txt` by the
  workflows that assert it, rather than typed into all three places. Dependabot
  version updates for Python were switched off precisely because a bump could
  only ever open a pull request that failed until someone edited two more
  lines; they are on now, and a bump either passes the schema contract suites
  or it does not.

- The release job publishes with `gh` rather than a third-party action. It is
  the one job holding `contents: write`, and `gh` is already on the runner, so
  publishing costs no additional trusted code.

- `project_audit_assets` now verifies both kinds of broken reference against the
  running editor, not just UID ones. A `missing_file` finding says nothing the
  scan indexed provides that path; `ResourceLoader.exists` says whether Godot
  can load it, which is a different question. A remap, a resource type the index
  does not cover, or an index that hit its own cap all read as absent on disk
  and load perfectly well, and those were false findings.

  A path the engine can load is cleared into `engine_only_references` with
  `kind: "missing_file"`; one it cannot keeps its finding and gains
  `confirmed_by_engine`. The two questions stay independent: a registered UID
  does not make a missing path loadable, and a loadable path does not register
  a UID, so a verdict on one never moves the other. UID clearing now also
  requires the resolved path to load, because a UID can stay registered for a
  file that is gone.

  `uid_verification` is renamed `reference_verification` and its counters are
  now `cleared` and `confirmed`, since it covers both kinds. UID findings are
  sent before path findings so a run truncated at the 256 bound is still
  deterministic. Neither name has appeared in a release.

### Fixed

- `didi::ipc::parseFramedMessage` accepted a frame whose length field was near
  `UINT32_MAX` and then read gigabytes past the end of the buffer it was given.
  An eight-byte input segfaulted.

  The bounds check was written `size < 4 + len`. `len` is `uint32_t` and `4` is
  `int`, so the usual arithmetic conversions evaluate the sum in 32-bit
  unsigned arithmetic: `0xFFFFFFFC + 4` is `0`, the guard passed for any buffer
  at all, and the `std::string` built from the payload was constructed with a
  four-gigabyte length. Both operands are now widened before the addition.

  Nothing in the shipping server called this function -- the live IPC paths
  read frames through their own bounded implementation, which checks the length
  against a maximum -- so this was reachable only by a caller of the header. It
  is fixed rather than deleted because `include/didi/common/protocol.hpp` ships
  in the addon include tree and this is the obvious function to reach for.

  Found while choosing fuzz targets, which is the argument for the exercise:
  the only existing test round-tripped a frame the same code had just written,
  and a decoder is defined by what it does with input it did not write. The new
  case covers wrapping lengths, truncated payloads, short headers, non-JSON
  payloads, exact fits, and trailing bytes, and it segfaults against the old
  decoder rather than merely failing.

- `project_get_uid_map` and `project_audit_assets` work again without a Godot
  session. Both were moved into the live-only capability set when they gained
  their editor-backed paths, and the standalone process refuses a live-only
  tool with `503 No atomic runtime route is available for live dispatch` when
  nothing is attached. Both have complete offline paths, so both now declare
  `live` and `offline_fallback`.

  The in-process tests could not see this. They drive a client that holds no
  route lease, so the refusal never fires there and the wrong advertisement
  looks like a pass. Two tests close that gap: a native one that requires every
  tool with an offline path to advertise it, and one in
  `tests/test_tool_output_schema_contract.py` that drives the real binary with
  no session and requires an answer rather than a refusal.

- `project_audit_assets` checks its unresolved UID findings against the running
  editor instead of leaving them as guesses. An `unresolved_uid` finding means
  no scanned project file records that UID, which offline is the only reading
  available; with an editor attached, `ResourceUID` can say whether it is true.

  A UID the engine resolves is not broken, so it leaves `broken_references` and
  is reported under `engine_only_references` with the path the engine gave. The
  file it points at leaves `orphans` too, and its bytes come off `orphan_bytes`:
  a file the engine proved is referenced cannot also be unreferenced, and a
  report that said both would be arguing with itself. A UID the engine does not
  know keeps its finding and gains `confirmed_by_engine`.

  Corrected rather than annotated, because a finding left standing with a
  footnote saying it is wrong is how a tool teaches people to skim past
  findings. The trade is disclosed rather than silent: `engine_only_references`
  adds a `limitations` line saying the editor's table is not in the repository,
  so a fresh checkout would report those references broken.

  `reference_verification` reports on every call whether the pass ran --
  `live`, `unavailable`, or `not_needed` -- how many findings were sent, and
  whether more than the 256-query bound existed. `execution_mode` follows it, because it
  describes whether an engine contributed to the findings. What was scanned
  does not change with it: `scan_source` is `project_files` on every call.

- `project_get_uid_map` takes a `resolve` list and answers it from the engine.
  Pass up to 256 `uid://` or `res://` values; with an editor attached they are
  resolved by the `ResourceUID` singleton, which is the table the engine itself
  resolves against, and each result reports `index_state` — whether the project
  files agree, contradict it, or hold nothing.

  This closes the roadmap's UID-to-path synchronization item, and it closes it
  without parsing `.godot/uid_cache.bin`. That file is an undocumented binary
  cache with no format guarantee: absent on a fresh clone, written on the
  editor's schedule, and readable mid-write. Building reconciliation on it would
  mean maintaining a format the engine does not support and re-proving it every
  release, which is the shape Phase 7 option C was rejected for. `ResourceUID`
  is public, and its binds carry identical hashes on Godot 4.5.1, 4.6.2 and
  4.7.2, so there is no version gate.

  What did not change is the map. `ResourceUID` exposes no enumeration through
  GDExtension, so `uid_map` is a scan of the project files in both modes and
  every response carries `uid_map_source` saying so. A call without `resolve`
  reports `offline_fallback` even with an editor connected, because claiming
  live for a file scan would be a lie. A miss says which kind it is:
  `unknown_to_engine`, `malformed_uid`, `unsupported_query`, or offline
  `not_in_project_files` — which is not the same claim as the resource not
  existing.

- `scene_close` no longer demands `discard_unsaved: true` for a scene the engine
  says is clean. Godot 4.7 added the read side of editor dirty state,
  `EditorInterface.get_unsaved_scenes()`; Didi now probes for that bind and, when
  the engine omits the active scene from its unsaved list, closes on a call with
  no arguments and reports `dirty_state: "clean"`.

  The flag was the project's marker for destructive intent, and requiring it on
  every close taught an agent to assert destructive intent it did not have. The
  guard is not weakened where the engine cannot answer: Godot 4.5 and 4.6 bind
  only the write-side `mark_scene_as_unsaved`, a scene that has never been saved
  has no path for the engine to name, and a scene the engine reports as unsaved
  is refused everywhere. Those three cases still return `409`, and the message
  says which one it is. `discard_unsaved: true` is unchanged and still skips the
  check.

  Results carry `dirty_state_readable` and `dirty_state`, so a client reads which
  case it is in from the response rather than inferring it from a version number,
  and an engine without the bind degrades to the refusal rather than to a silent
  discard.

- One Didi process can drive several Godot sessions at once. Routes are held per
  session, each with its own connection and ownership lock, so two tasks
  interleaving requests on one stdio process each drive their own editor and
  neither sees the other's. Opening a route for a named request does not move
  the process selection, so a legacy client sharing the process keeps what it
  attached, and detaching releases only the selected route.

  Eight routes at once is the ceiling. Each holds an ownership lock, and a lock
  held here is a session refused to every other Didi process, so the count is
  bounded rather than left to grow; routes whose engine has gone are dropped
  before the limit is consulted, and a genuine limit returns `429`. Every held
  route is released on shutdown, not only the selected one.

- A request no longer inherits a Godot session it never chose. Attaching set one
  route for the whole process and every later request acquired it, so on a
  process serving more than one task an agent could read from, or mutate, an
  editor a different task had attached.

  A request declaring protocol `2026-07-28` now names its session in
  `_meta.didi.runtime_session_id` and is served on that one only. Naming nothing
  gets no live route: a tool that can answer offline does and says so, and a
  live-only tool is refused with `400` naming the field to set. `tools/list`,
  `resources/list` and `resources/read` follow the same rule, so nothing is
  reported live, or read, because an unrelated task opened a route. The named
  session is checked for project identity, and a confirmation token now binds to
  the session the call will actually run on rather than whichever route happened
  to be current when the preview ran.

  Legacy clients are unchanged. `runtime_attach_session` still selects a session
  for the process and later legacy requests still inherit it. See
  [Naming the runtime session](docs/INTEGRATION_GUIDE.md#naming-the-runtime-session).

- A modern request is validated before it is dispatched. Advertising
  `2026-07-28` while checking only the protocol version meant a request missing
  its client capabilities, or carrying a null or fractional id, was executed and
  answered rather than refused. All of those now return `-32602` before the
  method runs, naming the field at fault. `server/discover` stays exempt,
  because refusing the probe a client uses to learn what a server speaks would
  make a dual-era server look like a legacy one.

- MCP Apps is negotiated per request again. A legacy client declaring the UI
  extension turned the Control Room on for every later request on that process,
  including modern ones that declared nothing and could not render it.

- A live failure no longer publishes the session endpoint. An error from a live
  route carried the full public descriptor, so the named pipe or socket path
  appeared in the tool's error text, in `error.data.session` as the engine
  attached it, and in resource errors. The endpoint is not a credential -- the
  descriptor directory is access controlled and the token is separate -- but an
  error string is the payload most likely to be quoted onward into a model's
  context, and identifying which session failed is a different act from handing
  out the address to reach it. Failures now carry `session_id`, `kind`, `pid`,
  `project_path`, `protocol_version`, `started_at_ms` and `schema_version`.

  Successful results are unchanged and still carry `endpoint`, because that is a
  client's own record of the route it used, and `runtime_list_sessions` still
  reports it because choosing a session is what that tool is for.

  A failure also names its session once now rather than twice. The extension
  states the route on its way out and the standalone states it again when it
  wraps the error, so a bridged failure carried the same session at the top
  level and again in `error.data`. The top-level copy is the one a success also
  uses, so it is the one kept; `error.data` retains everything else the engine
  said about the failure, including `outcome`, `route_quarantine`, `transport`
  and `engine`. The internal IPC error is unchanged, because at that layer the
  extension's copy is the only attribution there is.

- The managed editor does not outlive the host that owns it. It was reaped only
  by a destructor, so it survived every exit that does not run one: a `SIGKILL`,
  a supervisor or container stopping the host, a second Ctrl+C taking the C
  runtime default on Windows, or the host crashing. What was left was a headless
  Godot holding a workspace open, with nothing running that knew about it, in a
  mode built for unattended runs where nobody is watching a process list.

  Windows now creates the child inside a job object marked kill on close and
  attaches it at creation, so there is no window where the child runs outside
  the job, and the kernel ends it when the last handle to that job goes with the
  process. Linux sets `PR_SET_PDEATHSIG` in the child before the exec, and
  checks the parent again immediately afterwards because the kernel sends
  nothing if the parent had already gone. A host that cannot create or nest a
  job still starts its editor, and says in the log that the second line is
  missing rather than leaving somebody to find out from an orphan. macOS has
  neither mechanism and is recorded as the gap it is.

- Ctrl+C stops the server. The `SIGINT` and `SIGTERM` handler called `stop()`,
  which joins the blackboard watcher thread, detaches the runtime session over
  IPC, and logs through a mutex. None of that is safe from a signal handler,
  which the C runtime is explicit about: no heap, no stdio, nothing that makes a
  system call. The flag the handler also set was read by nobody, and the stdio
  loop stayed blocked inside `std::getline`, so a single Ctrl+C left the process
  running and still holding the session. It took closing the client, or another
  line on stdin, to get out.

  The handler now stores two atomics and returns, and that is all it does.
  Lines come off a reader thread, so the loop can wait on a flag rather than on
  a descriptor and leave when one is set. A blocking read cannot be cancelled
  portably: libstdc++ retries a read a signal interrupted, and on Windows the
  handler runs on a thread the operating system made for the interrupt, so the
  read is never interrupted at all. Teardown runs where it always belonged, on
  the normal path when the loop returns.

  Reading on a thread took away the backpressure the pipe used to provide, so
  the pending queue is capped and the reader waits once it is full. Three
  thousand pipelined requests come back in order and the process still exits 0.

- The perceptual hash uses all 64 bits it reports. `perceptualHash` dropped the
  DC term out of an 8x8 DCT block and wrote the 63 that were left to bits 0
  through 62. Bit 63 was clear for every possible input, so the Hamming distance
  of 64 that the header, `docs/TOOL_REFERENCE.md` and the `max_hamming_distance`
  schema all offer could not be produced by the function producing the hashes. A
  caller tuning a visual regression threshold was tuning against a range the
  implementation could not reach. The median was also the average of the pair
  either side of the middle of an odd count, which is not the median.

  The block is 9x9 now and the hash takes the 64 lowest frequency AC
  coefficients out of it, ordered by `u+v` and then by `u`. The DC term stays
  out, so a uniform exposure change still moves no bits. The count is even, so
  the median formula is the right one and exactly half the coefficients sit
  above it, which is what makes 64 reachable between two real hashes. Documenting
  63 instead would not have worked: a correct odd median split puts 31 bits in
  every hash, so the most two of them can differ by is 62, and the contract would
  have been wrong again by one. Hashes are computed per diff and never stored, so
  nothing on disk went stale.

- `DIDI_BUILD_TESTS=OFF` builds no tests. `didi_extension_signal_tests` sat
  outside the guard, so a production configure recompiled every GDExtension
  source a second time with test seams to produce a library nothing ships. It
  had come out from under that guard once before, so there is now a configure
  time check that refuses when either test-only target exists with the option
  off, rather than a comment asking the next person not to do it again. A clean
  test-off build produces exactly the server binary and the extension.

---

## [1.6.0] - 2026-09-06

### Added

- `runtime_explore_scene` drives a running game and reports what happened. `runtime_inject_input` presses a button and returns; `runtime_watch_invariants` samples every frame and presses nothing. Neither pair makes a playtest, because each injected event is its own IPC round trip: an agent driving from outside presses at whatever rate the transport allows and looks between presses, so a character that walks into a wall and stops responding is invisible to it. There is a position before the press and a position after it, and never the second in between where nothing happened. This runs the loop in the engine. It holds one InputMap action at a time on a schedule drawn from `seed` and from nothing else, samples the probes the caller named every frame through the same bounded sandbox the invariant watch uses, and reports the intervals in which nothing it pressed moved anything, each one naming the action that was down for it. It pauses on the first interval by default, so the state that stopped responding is still there to look at, or surveys the whole window when asked.

  Input actions, not movement, because nothing outside a project's own controller knows how that project moves its player. Setting a position directly would move the sprite without running any of the code that decides whether it can move, which proves nothing about whether the game can be played. `nav_query_path` and the `spatial_query_*` family stay how an agent decides where to go.

  A probe that cannot be read is not a probe that stayed still. An expression that fails every frame comes back with zero readings and its error, and never contributes a stuck interval, because a typo reported as a frozen game is the one answer this must never give. An action the project does not define fails the run rather than being skipped, for the same reason.

  It reports and does not judge. A cutscene, an open menu and a real soft lock are the same thing from here, and the response says so by carrying `verdict: "none"`. That is the rule `docs/GOGO_DESIGN.md` set for anything built on input dispatch. Recorded as an accepted amendment in `docs/SURFACE_AMENDMENTS.md`, which is what this repository requires before a name is registered. The surface is now 109 canonical tools, 106 implemented, 119 registrations.

- `viewport_capture_passes` takes a `segmentation` pass. Each `GeometryInstance3D` is painted a flat colour of its own and the response carries a legend: node path, class, the colour it was given, the colour that came back, how many pixels it claimed, and the 2D box those pixels fall in. The box is read out of the picture rather than projected onto it, which is the bounding-box overlay the report asked for, as data rather than as ink nobody has a font to draw. This did not ship with the other two passes because the viewport post-processes after the pass shader writes: a colour written as `(1,73,151)` came back as `(1,92,186)` on a 4.5.1 editor and unchanged on 4.7.2, so a legend naming the colour it asked for would have described pixels that were not in the picture. It no longer asks. The frame is read back, every pixel is matched to the entry it is nearest, and each entry reports the commonest colour among the pixels it claimed, which is in the image by construction on whichever engine drew it. A pixel further from every entry than the match radius belongs to nobody, so an antialiased edge is unclaimed rather than filed under whichever node it fell closer to, and `segmentation_unclaimed_pixels` counts them. The palette uses three levels a channel, because four puts two of them close enough after the shift that the answer would depend on the engine, and no neutral colours, because an entry a grey background could sit on is an entry that would claim pixels no node painted. A node past the palette is not painted at all rather than sharing a colour, and is named in `segmentation_unpainted`. The live harness reads the returned PNG and checks every legend colour against the pixels, on both engines.

- `project_verify_changes` now takes `run_scene`, and a new `project_apply_changes` writes a proposal into the working tree once it has passed. Checking a proposal was only ever a parse, and nothing could act on the answer: a caller who liked the result had to apply the same writes itself through the ordinary writers, one file at a time, with no relationship between what was proved and what was written. `run_scene` opens a scene in the isolated copy, headless and bounded by `run_frames`, so a scene that fails to load, an `@onready` path that resolves to nothing, or a `_ready()` that divides by zero is caught by the thing that catches it, which is running. The exit code is not the whole answer there, because Godot leaves a runtime script error on its error stream and still exits 0, so the error lines are read as well. A proposal whose scripts did not parse is not run at all, and `ran` says so, because paying for an engine start to be told what the parse already said produces a load failure that reads as a runtime fault. `project_apply_changes` runs that same check and writes only if it passes, staging every file before replacing any, so the change cannot stop half applied. It runs the verification itself rather than trusting an earlier call, because a caller who verified a minute ago is describing a project that may have moved since. A proposal that does not pass writes nothing and comes back as the report, marked as an error so it cannot be read as a success with a footnote. It always requires a confirmation token. The surface is now 109 canonical tools, 106 implemented, 119 registrations.

- The Godot editor plugin now carries a console. Enabling the addon adds a **Didi** main screen, marked with Didi's own mark, because the main screen is the one surface Godot draws a plugin's icon on -- an icon set on a bottom-panel button is not rendered, including one taken from the editor's own theme. Its Dashboard is six cards, each a red, amber or green light with the fact behind it and a button for the next step: the live bridge and how long it has been up, the extension and the library path it wants, the server binary and whether it has been verified by running it, the client configuration and whether the project has one, this editor's session, and the other sessions published on the machine. Cards are read from the session descriptors Didi publishes, so what the console reports is what a client would find.

- The console can close and reopen the live bridge from a switch. The endpoint belongs to the GDExtension rather than to the plugin, so the switch unloads and reloads the extension and reports the status Godot returns -- including the one that means "not without a restart". It never claims the bridge closed because it asked. Verified on Godot 4.5.1, 4.6.2 and 4.7.2: the endpoint stops, the session is retired, the editor stays up, and loading again publishes a new session.

- A Log page, with two sources and no third invented one. The console's own record is every state change it watched and every action taken through it, timestamped, kept across the panel being rebuilt and never written to disk. The other is the log Godot writes for the last *run* of the project, read from its tail, with the indented location lines under an error inheriting that error's level so filtering to errors does not hide the file and line. Both filter by level and by substring. Didi's own server logs to its process's standard error, which an editor started from a desktop shortcut has nowhere to show, and the page says that rather than presenting an empty view.

- Connect generates the launch configuration for Claude Code, Cursor, Claude Desktop and VS Code with the binary located and the project path filled in, and writes it into the project for the two clients that read one from there -- after showing what it will write and warning when it would replace a file. Settings holds what that configuration carries: log level, endpoint name, and whether it passes `--yolo`.

- Automatic detection of the `didi` binary deliberately skips anything inside the project. A path under `res://` is a path Didi's own file tools can write to, and Diagnostics runs the located binary to ask its version; keeping the project out of detection means an in-project binary is only ever run because a person browsed to it. Choosing one anyway is allowed and the page says so.

- Editor preferences live in Godot's `EditorSettings` under `didi/`, which is stored with the editor rather than in the project, so a preference governing an assistant is not a file that assistant can rewrite. The console never displays, copies or reports a session token: the descriptor reader copies the fields it names and the shared secret is not one of them, and a test fails the build if that changes.

- Added `project_verify_changes`, which checks a set of proposed file contents together in an isolated copy of the project. `script_check_syntax` already answers whether one file parses, from source text, without writing anything. What it cannot answer is whether a set of files is consistent with each other, because a script that preloads a sibling is only correct when that sibling is the proposed one rather than the one still on disk. So the whole proposal is written into a git worktree built from HEAD, checked there, and the worktree is taken away again, on the failing paths as well. Nothing it does can be seen from the working tree. Uncommitted work is carried across, because a check that ignored it would answer a question about a project nobody has open, and the commit it was checked against is reported. Untracked files cannot be carried and are named rather than counted, so a proposal that depends on one is not silently checked against a project missing it. It refuses rather than falling back to copying a project directory, which for a Godot project means its imported assets too.

- A transport failure on a route with a known session now reports `error.data.engine` as `alive`, `gone`, or `unknown`. A failure saying the peer closed the pipe does not say why it went, and that is the difference between an engine that crashed and one that is alive and simply not answering. `unknown` stays distinct from `gone`, because a process that could not be queried is not a process that has ended, and filing one as the other would invent the fact a caller most wants. The check compares the recorded start time as well as the pid, so a recycled pid reads as `gone` rather than as the session that used to own it. All four routes that classify a transport failure share one implementation, so they cannot answer the question differently; the tool route, which every live tool takes, previously had no session in scope to answer it with at all.

- `didi --dump-tool-manifest` now emits the required request fields of every implemented tool, and the documentation validator checks that the section documenting a tool names each of them. The counts and the name tables both passed while the `project_export` page described a call that cannot succeed, because nothing compared the documented request against the schema the binary enforces. A field documented by its components, such as `point.x` and `point.y`, counts as named. A manifest from an older binary carries no such map and the check simply has nothing to read.

- Added `editor_render_ghost_preview` and `editor_clear_ghost_previews`, which draw wireframe boxes in the open editor viewport to show where a proposed mutation would land before anything on disk changes. A developer asked to approve `Vector3(12.4, 0.0, -8.2)` in chat can now look at it instead. The shapes are handed to the rendering server directly rather than added to the scene, so the scene tree, the scene dock and the saved file are all untouched and the editor never becomes dirty; there is nothing to undo because nothing was done, and the responses say `scene_modified: false`. Cyan marks an addition, yellow a translation and red a deletion, and a caller that wants a different colour can give one. Both 2D rectangles and 3D boxes are drawn, each into its own world, and one call draws into one world rather than splitting across both. Previews stay up until they are cleared, which is what makes them useful to look at, so `editor_clear_ghost_previews` with no argument clears everything whatever left it behind, and no more than 256 shapes can be on screen at once.

- Added `viewport_capture_passes`, which draws the live 3D scene again with every geometry node's material replaced and returns a depth or world-space normal image next to the ordinary colour frame. A flat colour picture cannot say whether one thing is nearer than another or which way a surface faces, and these can. Each pass comes back as its own image, in the order it was asked for, rather than as one stacked picture that would need labels drawn on it to be read. Depth divides by the rendering camera's own far plane unless one is given, and the value used is reported. The passes are orderings rather than measurements, and say so: the shaders undo the sRGB curve the framebuffer applies, but the viewport post-processes afterwards by an amount that depends on the engine, with a 4.7.2 editor returning the written values unchanged and a 4.5.1 editor returning them scaled by about a quarter. A semantic segmentation pass was left out for the same reason, since a legend mapping colours to node paths would not match its own pixels on every engine. Every `material_override` is put back before the call returns, including on the paths that fail, and a restore that does not succeed is the error the caller gets, because a scene left wearing a debug material matters more than a frame that did not arrive.

- Added `spatial_query_frustum`, which lists the 3D nodes inside a camera frustum in the attached session, nearest first. The frustum comes either from a Camera3D already in the scene, whose transform, projection, near and far planes are read, or from parameters written out by hand; both build the same six planes, so a node one form calls visible is never a node the other calls hidden. A node with geometry is tested by its own bounding box and reported as `inside` or `intersecting`; a node without geometry is tested at its origin, and which test ran is reported. Aspect ratio does not live on a camera, so its source is reported too: a running game is measured through the camera's own viewport, while an edited scene uses the project's configured viewport size, because the shape of an editor pane is a fact about the window rather than about the game. With `sightline` set, rays are cast from the camera to each node's corners and centre, and the count that arrived is reported alongside `clear`, `partial` or `blocked`. Rays see physics colliders only, so geometry without one does not block, and a node whose sightline was not sampled carries no sightline field rather than a clear one.

- Added `shader_get_visual_graph`, which returns a VisualShader's nodes and connections per shader type as structured JSON. A project that builds its materials as graphs was opaque: the uniform tools read what a graph exposes, and nothing said what the graph itself was made of. Each node carries its id, its Godot class, and its position in the editor graph; each connection carries the node and port it runs from and to, because a graph is its links as much as its nodes. A shader written as code is refused with a message saying so, rather than reported as a graph with no nodes in it. Node and connection counts are reported separately from the returned lists, so a bounded response says how much it left out.

- Added `shader_set_uniform`, the write half of `shader_list_uniforms`. It takes the same JSON spellings `scene_set_property` takes for each Godot type, so a caller learns one contract rather than two, and a `res://` path fills a texture or other resource uniform after being loaded and checked against the class the uniform declares. A name the shader does not declare is refused rather than written: `set_shader_parameter` accepts any name and silently does nothing with an unknown one, so a typo would otherwise come back as a write that worked. The change goes through the editor UndoRedo stack on the material's own `shader_parameter/<name>` property, which is the one the scene file writes and the inspector edits, so undoing it is the undo a person expects. The result reports what the uniform holds afterwards rather than the value it was handed, with `applied` saying whether they match.

- Added `shader_list_uniforms`, which reads the shader uniforms of a `ShaderMaterial` held by a node in the edited scene. Nothing in the surface could see them: `resource_inspect` reports file metadata and `scene_get_property` reports the material slot, and neither reaches inside to the parameters an agent wants to read or tweak. Each uniform comes back with its declared Godot type and its effective value, which is the material's override where it has one and the shader's own default otherwise. There is deliberately no flag separating those two: `get_shader_parameter` returns the default for a uniform the material never set, so a flag built on the returned value would have claimed every uniform was overridden, and the live harness caught exactly that before it shipped. `settable` comes from the same decision `scene_set_property` makes about which JSON spellings exist, so the two cannot disagree about what a caller may write. A uniform of a type with no JSON spelling is still reported by name and type with a null value, rather than failing the whole read. A material slot holding something that is not a ShaderMaterial is refused and names what it found, because an empty uniform list would read as a shader with nothing to set.

- Added `spatial_query_clearance`, which sweeps a box, sphere or capsule along a path and reports how far it gets. A raycast answers whether a line is clear, which is not the same question as whether a body is: a corridor a ray passes down cleanly can still be too narrow for the character that has to walk it, and that is the question a doorway or a spawn point actually asks. It returns the safe and unsafe fractions the engine gave, plus the position the shape reached, and nothing in between interprets what a particular pair of fractions means. A zero-length sweep is accepted on purpose, unlike a ray of no length, because asking whether a shape fits where it stands is a real question. Bodies block; areas do not, because a trigger volume is not geometry and a corridor reported blocked by a checkpoint is answering a different question. Both dimensions, with `sphere` naming a circle in 2D so one request shape works either way.

- Added `spatial_query_raycast_batch`, which casts up to 64 rays against the attached session's physics world in one dispatch. Deciding whether a spawn point is in the player's line of sight, or whether a corridor has clearance, meant one round trip per ray or a viewport capture and a guess, and neither is affordable in a loop that pays per turn. The direct space state and the method binds are resolved once for the batch rather than once per ray, so every ray is also answered against the same physics state instead of against successive ones. Each entry goes through the `physics_raycast_query` contract unchanged and comes back with the same hit record, so a batch entry and a single call cannot describe the same hit differently; a rejection names the index of the ray that failed. One batch is one dimension, because a 2D and a 3D ray are answered by different space states and a batch that split across both would be answering from two worlds. A ray that cannot be answered fails the whole batch rather than leaving a gap, since a partial batch read as complete is a clear sightline that was never checked.

- Added `runtime_watch_invariants`, which watches declared conditions every frame of a running game and stops the game on the frame that breaks one. An agent could already drive a game and read values back out of it, one round trip at a time, but it could not notice a condition that is false for two frames, and by the time a poll returned the frame was gone. Three kinds, each one that can be answered honestly: `performance_between` on a Performance monitor, which is what a minimum frame rate is; `expression_between` on a bounded sandbox expression, which is what a bounded property is, and which covers a world-boundary check through a vector component; and `no_engine_errors`, which is what an unhandled script error looks like from outside the script. A violation reports the condition, the value that broke it, the bound it broke, and the elapsed time, and by default pauses the game on that frame so the state that failed is still there to read. The outcome has three values rather than two: an invariant that never produced a reading makes the run `inconclusive`, never `held`, because a condition nobody could measure is not a condition that stayed true. The request is refused if an invariant has no bound at all, since a condition that cannot be violated would report as held on nothing.

- Added `project_rename_references`, which renames a symbol in the places Godot serializes it: the `signal` and `method` attributes of a `[connection]`, and the property segment of a `NodePath` in an animation track. Those are exactly the references a text search finds but cannot explain, and exactly the ones an agent renaming a variable in a script forgets, so a project that looked clean broke at runtime. Every file is staged before any is replaced, so the change cannot stop half applied because the last file was the one that could not be written. GDScript and C# references are reported with their file and line and never rewritten: the language is dynamically typed, a whole-word match may be this symbol or an unrelated local that shares the name, and rewriting on that evidence would be a second source of the breakage this exists to prevent. It refuses a name a connection or track already uses, because that would merge two symbols with no way back, and it refuses to run at all on a truncated project scan, because renaming the files that were read and leaving the rest is the half-applied change itself. The rewrite and `project_analyze_impact` share one set of matchers and the plan is checked against the report before anything is written, so a disagreement between what was reported and what would be edited stops the whole change.

- `scene_set_property` and the `properties` argument of `scene_instantiate_node` take Vector2, Vector2i, Vector3, Vector3i, Color and Resource paths. The scalar contract meant a 2D node could not be placed at all, a CollisionShape2D could not be given a shape, a TileMapLayer could not be given a TileSet and a Sprite2D could not be given a texture, so scene data ended up in `_ready()` and the scene a person opens was not the scene Didi built. Vectors come from `{x,y}` and `{x,y,z}` objects, which is the encoding `resource_create` already takes, so the live and offline sides agree. Colour comes from `{r,g,b}` with an optional `a` or from a `#rrggbb`/`#rrggbbaa` string. A resource slot takes a `res://` path, loaded through ResourceLoader and refused if the loaded type is not the class the property holds, the same check `script_attach_to_node` makes for a Script; `null` clears one. An object carrying a member the target type does not have is refused rather than having it dropped. An empty resource slot reads back as nil, so the property's declared type now comes from the class rather than from whatever the slot happens to hold, and `scene_get_property` can describe a position, a colour and a filled slot instead of refusing to encode them.

- Added `script_create`, which writes a GDScript file under the project root. Every other `script_*` tool assumed the `.gd` file already existed, so the first step of the documented workflow was the one step that had to happen outside Didi. It takes `script_path`, `source_text` and `overwrite`, preserves an existing script unless the overwrite is explicit and confirmed, and runs the same diagnostics `script_patch_method` runs after its write, so a bad script is visible at creation rather than at attach time.

### Fixed

- `project_verify_changes` no longer reports every proposal as broken in a project that names a resource by `uid://`. The isolated copy is a git worktree, and a project gitignores `.godot`, which is where Godot keeps the UID cache, so nothing in the copy can resolve a uid. A project that names one, which `project.godot` does for its audio bus layout as soon as anything writes one, makes the engine print `ERROR: Unrecognized UID` when it loads. That line arrives in the output of every check the sandbox runs, and the verdict is taken from the error stream because Godot exits 0 for a plain syntax error, so it was attributed to each proposed script in turn and to the scene run. Every proposal came back failing, with a reason that had nothing to do with the files being checked. Godot 4.7.2 does not print it, so the tool worked there and failed on 4.5.1 and 4.6.2, which is most of the supported floor.

  The copy is now asked what it says on its own, once, before anything of the caller's is checked, and those lines are not counted against anybody. A measured baseline rather than a list of known engine messages: a pattern list would need extending for every engine version, and it would also hide a genuinely unresolved uid in a proposed resource, while subtracting what the copy already said keeps anything the proposal actually added. Failing to measure a baseline is not a reason to fail a verification, so a probe that cannot run leaves every line attributed, which is the behaviour this had before.

  Checked on Godot 4.5.1, 4.6.2 and 4.7.2: a proposal that parses passes on all three, a proposal missing the sibling it preloads still fails and still names it, and a script with a plain syntax error still fails even though Godot exits 0 for it. The fixture CI builds is clean and has never had a uid reference in it, which is why nothing here caught this.

- Every page under `docs/` is reachable again. Nine design and plan documents had nothing in the repository pointing at them, which is the same defect `docs/INTEGRATION_GUIDE.md` was fixed for and which comes back every time a page is added without a home. A new `docs/README.md` indexes all of them, grouped by what a reader came to do, and repeats the status banner each design record carries so a superseded proposal is not mistaken for current behaviour. The repository README links it and keeps its own short list for a first read. Checked with a link and anchor sweep over all 40 Markdown files: no broken relative link, no broken heading anchor.

- The `[Unreleased]` section of this file had eighteen `### Added` headings and ten `### Fixed` ones, because every change appended its own rather than joining the existing one. Keep a Changelog expects one section per category per release, which is what every released version here already does, and twenty-eight headings is not a section anyone can read. Consolidated into one of each, entry text and order untouched.

- `docs/TOOL_REFERENCE.md` said the project search skips "`.git`, `.godot`, `.worktrees`, and build outputs". It now names them, including `.gemini` and the `build-` prefix, because "build outputs" was the phrase that let `build-ninja` go unnoticed.

- The build no longer races itself placing the class reference. `didi` and `didi_tests` each copy `resources/didi_class_reference.json` next to their own binary, and on a single-config generator that is the same directory, so both wrote one path while ninja ran the two post-build steps at the same time. A macOS CI run failed on it with "No such file or directory" for a file that is committed and was never missing. Each target stages under a name of its own and renames, which replaces atomically, so whichever finishes last wins and neither can read a half-written file. The generator expression stays, because a multi-config generator does put the two binaries in different directories.

- A blackboard operation waits out a lock somebody else is holding instead of reporting an error. The wait was fifty attempts twenty milliseconds apart, so a holder that kept the board much over a second turned every other agent's call into an internal error. A board is a coordination surface between agents, so several of them wanting it at once is the ordinary case, and eight agents each holding it across a read, a modify, a write and the lock file's own fsync passes that budget on a loaded machine. That is what failed the sanitizer job on a4a5de5: one of eight concurrent claimers was told its claim errored rather than that it lost. The wait is now a deadline of five seconds rather than a count of attempts, because an attempt that itself costs twenty milliseconds quietly halved a budget expressed in attempts, and the retry sleep is spread so eight waiters do not wake together and collide again. Nothing is waiting on a lock no live process holds: both implementations tie the lock to an open file description, so the operating system releases it when the holder exits however it exits, and a longer wait cannot turn a dead holder into a hang. The message said "another process is holding the blackboard lock" when the holder is as often another thread in the same one; it now says the lock stayed held for the whole wait, and says how long that was. The two concurrency tests counted failures and threw the message away, which is what left the CI failure to be inferred from timestamps, so they report the first error they saw.

- A blackboard save can no longer destroy the board. `saveBoard` wrote the serialised board to a temporary and renamed it over the file, and when that rename failed it deleted the board and tried the rename again. A rename over an existing file already replaces it, so the delete was never buying anything, and the one case where it did work is the case that loses everything: on Windows a delete behind an open handle goes pending, the name stays taken until that handle closes, the second rename is refused, and the temporary is cleaned up on the way out. Every key and every task on that board was gone, with an internal error as the only trace. The save now goes through the same staged write `script_create`, `resource_create` and `asset_reimport` already use: a failure reports itself and the board on disk is untouched. The test makes the board read only, which is the one replace failure that can be forced on demand, because `MOVEFILE_REPLACE_EXISTING` refuses a read-only destination while `remove` clears the attribute and succeeds. On the old code the write returned success and the board had been overwritten.

- Three tools no longer fail on a path the active Windows code page cannot hold. `path::string()` and `path::generic_string()` convert through that code page, so a project directory named in Chinese, Japanese, Korean, Greek, Cyrillic or anything else outside it threw `std::system_error` and the call came back as "No mapping for the Unicode character exists in the target multi-byte code page". `project_search_text` and `project_search_symbols` hit it in four places, on the containment check, the skipped-directory name, the extension and the reported project root; `project_verify_changes` hit it building the project-relative path of a proposed file; `script_create` and `script_patch_method` hit it running diagnostics after the write, which means the file was already on disk when the call reported an internal error. The existing Unicode tests never reached this because `ç` and `ã` do have a mapping in code page 1252. The new ones use characters that do not.

- `script_create` and `script_patch_method` report the Godot compiler's diagnostics again. Both handed the analyser an absolute path, and Godot names `res://` paths in the errors it prints, so the location patterns matched nothing and every compiler diagnostic was dropped along with its line number. The lexical rules still fired, which is why the loss was quiet: a response with `has_errors: false` looked the same as a script with nothing wrong with it. They pass the `res://` path now, which is the form the analyser and the engine both already speak.

- The resource index and project search no longer scan out-of-source build trees. Both skipped `build`, `build-clean` and `build-vs` by name, so `build-ninja`, the tree this repository is actually built in, was walked in full. Every artifact in it counts against the index's cap, so a large enough build tree pushes real project files out of the index entirely, and a text search returned hits inside generated files as project matches. Both now also skip any directory whose name starts with `build-`, which is the prefix `import_health` already used, and the search skips `.gemini` so the two agree about the same tree. A prefix rather than the `build*` glob `.gitignore` uses, because `buildings/` is a plausible project directory and should stay searchable.

- `script_patch_method` and `create_visual_test_lab` drop the shared resource index after they write. `resource_indexer.hpp` states it as the contract for a tool that changes the tree, and these two were the ones not keeping it, so for as long as the cache stayed warm `project_audit_assets`, `project_find_referencing_scenes`, `project_search_symbols` and `resource_inspect` all answered from the tree as it was before the write. The per-file memo goes with it, which matters most for exactly these writers: a patch that lands on the same byte count inside the filesystem's timestamp resolution is the one change the memo cannot see for itself.

- A transport failure reporting `engine: unknown` now says why. That word covers two different problems, an engine that crashed and a query that was refused, and reporting them with one word leaves a reader exactly where they started; #227 spent several rounds inferring from log timestamps what the payload could have stated. `error.data.engine_reason` is `open_denied`, with the operating system's own number in `engine_os_error`, or `running_but_unidentified` when something with that pid is running and could not be confirmed as the process the session opened. `alive` and `gone` carry no reason, because they need none.

- The live harness reports the editor's exit code when a request in the themed-Control block fails. Every tool error there is the transport reporting a process that has gone, and the exit code is the only thing that names how it went; the payload reads threw first and nothing had looked at the process by the time anything else could.

- A Godot that Didi starts to answer a question no longer publishes a runtime session. The extension starts an authenticated IPC session and writes a descriptor whenever it loads, which is right for an editor or a game someone is running and wrong for an engine started to check a proposal: it leaves another session for the next discovery to find, and a run killed at its timeout leaves the descriptor behind for the tombstone reaper. The isolation the C# and shader helpers already used now covers the verification sandbox as well, and the guard that sets it lives in one place instead of two.

- `project_verify_changes` reported a script with a plain syntax error as fine. It judged each proposed file by the exit code of `godot --headless --check-only`, and Godot exits 0 for a parse error while printing it, reserving a non-zero exit for cases like a `preload` that resolves to nothing. So a proposal whose scripts do not parse came back `all_ok: true`, which is the one answer this tool must never give wrongly. It now reads the engine's error stream as well, which is what `script_check_syntax` has always done, and the failing script's `detail` carries those lines. Found by red-teaming the run half of this change: the same mistake would have let a scene run pass on an engine that had refused to load its script.

- A live call that changes nothing is now sent once more, on a new connection to the same session, when the transport fails. A broken pipe leaves a caller unable to say whether the engine ran the request, and for a mutation that has to be reported, because applying it twice is worse than not knowing. For a call that changes nothing, repeating it is the same as making it, so asking again is what settles it and the failure no longer has to be reported as an unknown outcome. This is what #227 costs a CI run for: an `eval_gdscript` that lost its connection failed a harness run for a reason that had nothing to do with what the run was testing. Repeatability comes from the same mutation table that decides dry runs and confirmation tokens, so a tool cannot be repeatable in one place and a mutation in another, and `editor_render_ghost_preview` asked to accumulate rather than replace is excluded because a second attempt would draw the same proposal twice. One repeat, before the quarantine that would otherwise retire the route, never in a loop. A result that took two attempts carries `transport.repeats`; a failure asked twice and answered neither time carries `transport.repeated`. Mutations are untouched.

- `docs/INTEGRATION_GUIDE.md` was reachable from no document in the repository. It is a current, user-facing page -- how to install the addon into an existing project and wire each supported assistant to it -- and the README's own navigation table, which is where readers are told to start, did not list it. Added.

- `docs/REALIGNMENT_IMPLEMENTATION_PLAN.md` showed thirty-seven unticked checkboxes and nothing ticked, while `docs/HUMAN_INTERACTION_DESIGN.md` recorded the first item of that same programme as done. Both cannot be right. The plan now carries a status banner saying what verifiably shipped -- the dual-era front end serving `2026-07-28` alongside `2024-11-05`, `server/discover`, and `resultType` on every result -- and what did not: `_meta.didi` is still what clients are told to read rather than a namespaced extension, and the four-tier documentation restructure with its `docs/history/` directory never happened. The boxes are left as the record of intent they are, rather than ticked retroactively by someone guessing.

- Brought the documentation into line with the editor console rather than leaving eight pages describing a plugin that only printed a line at startup. `docs/HUMAN_INTERACTION_DESIGN.md` is the substantial one: it recorded an editor status line as step 4, conditional and unstarted, and listed "any settings UI" and a log view in the editor among the things not to build. The console shipped all three, so the document now records what was built, where it exceeded the recommendation, and why each objection either still holds or was consciously overridden -- the log page shows the two sources `runtime_read_logs` and `runtime_read_output` do not serve and that are readable exactly when the connection is down, and the settings page composes launch arguments rather than becoming a second source of configuration. The reasoning is amended in place rather than deleted, as the rest of that document already does. The addon file tree in `docs/INTEGRATION_GUIDE.md` listed three of thirteen files, which is a copy instruction that produces an addon whose console cannot open. `ARCHITECTURE.md` gains the console in its topology, off the IPC path, and an off-by-one in that diagram's box art is fixed. `ADMIN_GUIDE.md`, `CAPABILITIES.md` and `LLM_INSTRUCTIONS.md` now point at the tab that answers "why is this unavailable"; `SECURITY.md` records that the console's copyable report cannot contain a token; `BRAND.md` records that three of its sources ship inside the addon and are held byte-identical; and `CONTRIBUTING.md` and `DEVELOPER_GUIDE.md` say what adding a file to the addon requires.

- Removed a message prefix that nothing can emit any more. `Failed or timed out reading response` was a fallback for transport failures carrying no structured state, and the messages it matched were replaced when failures started naming their cause.

- A transport failure could not say why it failed. A peer that hung up and a deadline that expired shared one message, "Failed or timed out reading response length", alongside `timed_out: false` denying the timeout that message offered, which is the payload in #227 and left nobody able to say which had happened. The failure now carries `reason` in `error.data.transport`, one of `peer_closed`, `deadline`, `io_error` or `stopped`, and `waited_ms` saying how long that operation actually waited. An operation ended at five seconds under a ten second deadline is being ended by something other than its own deadline, and the number is what shows that. Messages name the cause instead of offering two. Both the Windows pipe and the Unix socket report it, and the three existing flags are unchanged so nothing reading them has to move.

- `docs/TOOL_REFERENCE.md` told readers that `project_export` takes a preset `name`. The schema has never accepted that field and never will, so the call it documented fails with a 400 for anyone who follows the page; the required field is `preset`. Documented the request shapes of `tilemap_set_cells`, `tilemap_get_used_rect` and `gridmap_set_cells`, which were named in a bullet list and never described, and of `project_search_text` and `project_search_symbols`, whose section explained every limit but never said what to send. Added `scene_get_selection` to the capability matrix, which was the one implemented tool it never mentioned.

- `shader_list_uniforms` reported `value: null` for every uniform a material does not override, where the value actually in effect is the shader's own declared default. `ShaderMaterial.get_shader_parameter` answers nil for such a uniform on 4.5.1, 4.6.2 and 4.7.2, so a shader's declared defaults were reported as no value at all. The default lives in the rendering server, and it is now read from there when the material has none. A null value now means neither source could supply one, which is what a session with no renderer looks like, and the documentation says so. `shader_set_uniform` reports the same effective value in `old_value`, while its undo entry still restores nil so that undoing a write takes the override off rather than pinning the default in its place. There is still no flag separating an override from a default: the same call that answers nil in a running game answers with the default in a 4.7.2 editor, so it cannot establish that difference in every session.

- A live tool call no longer fails at the moment a connection is recycled. A server holds one endpoint instance and recycles it after a second of quiet so another client can have it, and recycling discards whatever the client has already written. A client that came back at that instant either had its request thrown away and was told the outcome was unknown for something the engine never ran, or had the write itself refused; both failed the call, and on a loaded CI runner that cost whole harness runs. The client now decides for itself, on elapsed time rather than on a liveness probe that can go stale between the check and the write: a connection quiet for longer than a fraction of the recycle window is replaced instead of trusted. Both numbers come from one place with the margin asserted, because two independently chosen timeouts in two processes is how this comes back. A server also serves, rather than discards, a request that arrives while it is tearing a connection down, which is what protects a client built before this change.
- A client no longer gives up connecting when the endpoint momentarily has no instances. A server destroys its only instance before creating the next, and in that gap the wait fails with "not found" rather than "busy", which was treated as final with seconds of the deadline unspent. Found by the test written for the recycle boundary above, which failed on this instead; the two ship together because making the client reconnect deliberately turns a rare path into the common one.
- A server no longer applies a frame-arrival deadline to writing a response. The response is the answer to a request the handler has already run, and one larger than the endpoint buffer needs the client to drain it, so giving up after a second threw away completed work and left the caller unable to tell what happened.

- `viewport_capture_frame` works on a game session. A running game could be paused, stepped, driven with injected input and read through its tree and both log streams, and never seen; the only way to look at it was an OS screenshot from outside Didi, which is useless headless and in CI. Attached to a game it captures the root viewport and reports `session_kind: "game"` and `camera_identifier: "root_viewport"`; the editor camera selectors stay editor-only and are refused there. `viewport_diff_capture` follows, which is how a stepped frame gets asserted. Node isolation stays editor-only, because it hides and restores nodes in the edited scene.
- `viewport_capture_frame` refuses a viewport that has no size instead of returning it as a successful live frame. A 2D editor viewport that is not the selected main screen is a collapsed control, and Godot hands back its 2x2 minimum; the tool reported that as `is_live_frame: true` with nothing to distinguish it from a scene that happens to be empty. The refusal names the size and says the requested main screen is not the one on screen.
- `resource_create` refuses a `save_path` that is not `.tres` or `.res`. It wrote Godot text-resource markup into whatever path it was handed, including a `.gd` file, and reported `created_offline` for a file `script_check_syntax` immediately called unparseable in the same session.

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
