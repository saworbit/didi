# Changelog

All notable changes to **Didi** (`godot-mcp-native`) will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Historical entries describe the surface advertised by those releases. For the executable status of each current registration, use [docs/CAPABILITIES.md](docs/CAPABILITIES.md) or runtime `tools/list` metadata.

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
versions, still unstarted. Until it lands there is no upgrade or rollback
guarantee beyond the changelog, and no commitment to a support window for an
older minor line.

Read the version for what changed. Read this section and the roadmap for how
much of the thing exists.

---

## [Unreleased]


The status block below states the current surface rather than anything this
release changed, which is why it lives here and not in a version section.

<!-- phase7-current-status:start -->
**Status:** `PARTIAL_DELIVERY`
**Canonical implementation:** `113/116`
**Phase 7 registrations:** `3/18` unimplemented
**Feasibility:** `15/18` implementation-feasible; `3/18` API-blocked
<!-- phase7-current-status:end -->

Discovery now exposes 116 canonical tools plus 10 legacy registrations (126 total). 113 canonical tools are implemented and 3 remain unimplemented.
The three Phase 7 blockers are unchanged; the new name is `didi_control_room`, recorded in [Surface Amendments](docs/SURFACE_AMENDMENTS.md).

### Added

- **The vendored headers have a watcher.** `THIRD_PARTY.md` ended by admitting
  that the three copied single-header libraries in `include/` were "reviewed by
  hand or not at all". They are files, not package manager entries, so
  Dependabot cannot see them and neither can a lockfile scanner -- and the
  review that was supposed to happen by hand is exactly the kind that does not.
  `tools/check_vendored_versions.py` asks two questions and keeps them apart,
  because they fail for different reasons. Whether this page still describes the
  files on disk is answered by reading each header's own version banner and
  comparing it with the table, needs no network, and runs in the documentation
  suite on every pull request, so replacing a header and forgetting the table is
  caught rather than inherited. Whether the file is still current is answered
  against upstream, runs weekly in `supply-chain.yml`, and opens a tracking
  issue rather than failing a check -- upstream shipping a release is news about
  the world, not a defect in whichever branch happens to be open. The first run
  found one: `json.hpp` is on 3.11.3 and nlohmann has published 3.12.0.
  `nothings/stb` publishes no releases and tags nothing, so asking its releases
  API returns an empty answer that would read as "up to date" forever; its
  version is read out of the upstream header's banner instead.
  `gdextension_interface.h` is deliberately untracked and the tool says so out
  loud rather than omitting it, because it is a compatibility contract and
  "is there a newer Godot?" is the wrong question to answer weekly.
- **Dependabot watches the base image, and groups security fixes.** The Ubuntu
  image `tools/localci/Dockerfile` builds on was the one moving part nothing
  watched: the local CI lanes and the dev container both run on it, and an
  unpatched base could sit there indefinitely while the tag beside it still read
  the same. It is now pinned by digest, on the same reasoning the workflows pin
  action SHAs -- `ubuntu:24.04` is rebuilt in place every few weeks, so the tag
  alone makes a lane repeatable but not reproducible -- and Dependabot offers
  the rebuilt image the way it offers a new action SHA, which is what keeps a
  pin from becoming a way of staying unpatched. The release number stays a
  manual decision, because that tag tracks what `ubuntu-latest` resolves to on
  the GitHub runners; semver bumps are ignored and digest updates are not. Every
  ecosystem also gained a second group, `applies-to: security-updates`, so a
  disclosure affecting several packages at once arrives as one pull request
  instead of six landing in the same minute. The security groups carry no
  `update-types` filter deliberately: a fix is worth taking whether upstream
  shipped it as a patch or as a major.

### Added

- **A project website.** [saworbit.github.io/didi](https://saworbit.github.io/didi/)
  is rendered from `site/` by a Pages workflow on every push to `main` and
  checked on every pull request: the landing page and the brand assets it
  draws on. The marks are inlined from `docs/brand/svg` at build
  time, and the version and surface counts are read from `CMakeLists.txt` and
  the README status block, so the site cannot publish a number the README does
  not. The CI change classifier treats `site/` and the Pages workflow as
  repository furniture, since nothing in either can reach the compiler.
- **Platform support is stated where people look.** The README, the website,
  the repository card and the contributing guide now say that Didi builds,
  tests and ships on Windows, macOS and Linux, which of those the live editor
  harness covers in CI, and that testers on macOS and Linux are wanted.
- **The live editor matrix covers the whole supported range.** The extension
  declares `compatibility_minimum = "4.5"` and 4.7 is the newest line, so the
  supported range is three minor versions and CI loaded the addon into two of
  them. Nothing anywhere opened a 4.6 editor (#759). That is the arrangement
  that hides a regression rather than catching one, because GDExtension method
  binds are pinned per version and the middle of a range is not something you
  can interpolate: a fault that starts at 4.6 and is gone again by 4.7 is
  invisible from both ends. #600 was a 4.6-affecting bridge fault and it was
  only caught because it happened to show on 4.5 as well. The harness now runs
  on 4.6.2 alongside 4.5.1 and 4.7.2, on every event rather than only on
  `main`, because the leg is free: matrix legs get their own runners, all three
  start in the same second, and the new one finishes inside the shadow of the
  sanitizer job, which is what the run already waits for. A pull request waits
  no longer than it did, and a 4.6 regression is found by the change that
  caused it instead of after the merge.

### Added

- **Every tool publishes a title.** Of the 126 entries `tools/list` returns,
  the number carrying a human-readable title was zero, on either protocol
  revision, so a host that displays one fell back to the identifier and
  somebody approving a destructive mutation was shown
  `gridmap_export_mesh_library` (#686). Every other field on the entry is
  filled in with care: the four annotation hints take seven distinct
  combinations across the surface, and a contract test keeps a description on
  every parameter. The one field that exists solely for what a person reads was
  the one nobody filled in, and it matters here because Didi's confirmation
  story ends in a person. Titles come from one table, so a legacy alias cannot
  be titled differently from the tool it resolves to.
- **A blackboard writer can say "only if this has not changed".** The board
  exists because more than one client is expected, and the lease was the only
  concurrency guard on it: it covers tasks. Keys had no version and nothing a
  second writer could pin a write to, so two agents that both read 0, both
  incremented and both wrote 1 left the board holding 1, with neither call an
  error and nothing in either response saying a concurrent change had happened
  (#682). `blackboard_write` takes `expected_updated_at_ms`, which a read now
  returns for the path it was given, and `0` means the path must not exist yet.
  `blackboard_patch` takes `expected_revision`, the board's, because a patch
  spans paths. A mismatch is refused `409` with `reason_code` `stale_write` or
  `stale_patch`, naming what the board holds and who last wrote it, which is
  the shape a refused task claim already uses. A caller that passes neither
  keeps last-writer-wins.

### Added

- **`runtime_launch` can leave the game running.** The tool is a batch runner:
  it blocks, captures, classifies and terminates the child at the timeout. So a
  game that runs -- the normal outcome when you launch one to play it -- was
  reported `success: false`, `exit_code: 124`, "timed out", and was gone before
  a caller could do anything with it. There was no other way to start one: no
  `editor_play`, no `runtime_start`, and `runtime_stop` with no counterpart that
  starts what it stops. That left the interactive half of the runtime surface --
  inject input, step, pause, read output, get tree, explore, watch invariants,
  checkpoint -- reachable only for a game somebody else had started, and this
  repository's own harness starts one by hand for exactly that reason (#733).
  `detach: true` starts the game, waits for it to publish a session, and answers
  with that session; `runtime_attach_session` takes it from there and
  `runtime_stop` ends it. An author can write code, run it, look at it and fix
  it without asking a person to press F5. Nothing is captured -- the game's
  output goes to the null device, because nobody is left to drain a pipe and a
  full one would block the game -- so `limitation` says so and points at
  `runtime_read_output`, and `session_published` is the field to branch on. The
  pid reported is the game's own, which on Windows is often not the process this
  tool started: Godot's console build launches the engine and waits on it. Two
  things a game that outlives its launcher makes load-bearing, both of which a
  blocking run hid: the child inherits nothing but its own null output, because
  `bInheritHandles` would otherwise hand it a copy of the server's MCP stdout
  and a client would wait for an end of input that never came while the game
  ran; and the launch selects the session it just waited for, because a caller
  that starts a game means to drive that game and the process may well still be
  pointed at the editor it was launched from.

- **`project_audit_assets` reports what is wrong with `project.godot` itself.**
  Every other finding the audit makes is about a reference from one file to
  another, so a manifest that registers a setting nobody can name read as a
  project with nothing wrong with it. A `# disabled for now` above
  `Good="*res://good.gd"` is the case: the engine registers
  `autoload/#disabledfornowGood`, the script enters the tree under a name
  nothing references, and the only place that surfaces is the compiler refusing
  `Good` in a file three directories away, with nothing connecting the two.
  `project_settings_issues` now carries that finding as `unusable_setting_name`,
  with both names and the line the join started on, because the remedy is to
  move or delete one line and the caller has to be told which. It is reported
  for any section, not only `[autoload]`: a note above `config/name` registers
  `application/#noteconfig/name` and the project runs under the engine's default
  name instead of the one in the file (#818). The second finding is
  `unparseable_project_settings` (#817). This is not a full parse, and
  `limitations` says so: an empty list is not a promise that Godot will load the
  file.

### Changed

- **Every security alert now has a disposition written down, including the ones
  Scorecard raises.** `SECURITY.md` explained the ten CodeQL findings it
  dismissed and said why a Security tab full of permanent alerts is one nobody
  opens, but five Scorecard alerts had been sitting open underneath that
  sentence since 2026-09-09 with nothing recorded about them anywhere. Four
  measure controls a single maintainer cannot operate -- an approving reviewer,
  CODEOWNERS review, last-push approval, a repository older than 90 days -- and
  the fifth asks the local CI image to hash-pin a `pip install` whose version is
  already pinned exactly and watched by Dependabot. Each one now carries its
  reasoning in the same table as the CodeQL dismissals, so the next person to
  open the tab reads a decision rather than re-deriving one. The section also
  records something that cost an investigation to learn: a CodeQL dismissal is
  bound to the code it was made against, not to the finding, so an edit near an
  alert closes it and raises the same finding again under a new number with the
  dismissal gone. That is what happened to the `test_runner` and
  `process_runner` process operations when the process-tree kill and the stdin
  change moved the `execvp` call, and it will happen again.

### Fixed

- **A `.import` and an `export_presets.cfg` the engine refuses were both read as
  though they had loaded.** #817 and #820 taught `project.godot` that a file can
  end inside a value and that a balanced file is not a loadable one. Both
  sentences are about a ConfigFile rather than about that one file, and the two
  readers one file along consulted neither: `project_audit_assets` walked a
  `.import` Godot answers `ERR_PARSE_ERROR` for, counted it as scanned and
  reported `import_issue_count: 0`, and `project_list_export_presets` published a
  preset out of a file with `export_path=)` in it, runnable, with `)` as the path
  an export would write to (#823). Both now ask the scan what it already knows.
  A sidecar is reported as `unparseable_import_metadata`, a finding of its own
  rather than another `invalid_import_metadata`, and it carries the line and what
  the parser could not start: the remedy is to repair one line, and the cost of
  not repairing it is particular. The engine recovers from a broken sidecar
  destructively -- on the next reimport it prints the parse error, imports the
  asset with the importer's defaults and writes a new uid -- so what is lost is
  every import setting in the file and every `uid://` reference to that asset,
  while the asset itself comes back looking fine. An `export_presets.cfg` is
  malformed as a whole instead, because that is what Godot does with it: asked to
  export from a file with one refused value, 4.5.1, 4.6.2 and 4.7.2 all answer
  `Invalid export preset name` and list no presets at all, even though the keys
  ahead of that value parse. `project_export` reads through the same code, so it
  refuses the same file rather than running against a preset the editor has never
  had.
- **A balanced `project.godot` is not a loadable one, and a line can hold two
  settings.** `Scan::complete` counts brackets, which is what the walk it falls
  out of can count. Godot parses a value, so `config/broken=)`,
  `config/name=Pair` without the quotes, `a={1 2}` and `a=[1,,2]` are every one
  of them `ERR_PARSE_ERROR` with every bracket closed, the project does not open
  at all, and `project_set_setting` wrote into such a file, reported
  `persisted: true`, and left it exactly as unloadable (#820). The write and the
  dry run now refuse it with a 409 that names the line and says what the parser
  cannot start, and `project_audit_assets` reports it as
  `unloadable_setting_value` rather than answering `project_settings_issue_count:
  0` for a project nothing will open. The check is deliberately not a
  `VariantParser`: a constructor with the wrong arity, one the engine does not
  know, and a `Resource()` whose file is missing are all `ERR_PARSE_ERROR` and
  none of them is reported, because deciding those needs the engine's own tables
  and a reader that guesses at them refuses files that load. An empty answer is
  not a promise that Godot will load the file; a finding is a promise that it
  will not.
- Two settings on one line are two settings. `a=1 b=2` is two keys the engine
  reads and registers, and `project_set_setting` works in whole lines -- which
  is right for a value spread over four of them and wrong here, because it
  rewrote the line with only the key it was asked about and deleted the other,
  with every field in the response true about the one that survived and nothing
  anywhere about the one that went (#821). The write and the removal now refuse
  with a 409 naming the sibling, and splitting the line is the remedy. Refusing
  rather than rewriting part of a line is the smaller change for a shape Godot's
  own writer never produces. `project_analyze_impact` had the quieter half of
  the same bug: its walk took the first key on a line, so a file named only by
  the second one came back as `impact_count: 0`. It now reads every key on the
  line.
- A key the engine built by joining the line above into this one is refused for
  the same reason: that name lives on more lines than a rewrite of the
  assignment replaces, so the text above would join forward again and the
  setting written would not be the setting asked for.
- An identifier does not end a value. `Vector2 (1, 2)`, the same value with
  `(1, 2)` on the line below, and an `Array[int]` with its `([1, 2])` after it
  all load on 4.5.1, 4.6.2
  and 4.7.2, and the reader ended the value at the space or at the `]` and built
  a key out of what followed -- reporting a setting the engine does not have and,
  once the check above existed, refusing a file that loads. The seven names that
  are a value on their own are matched exactly, so `flag=true` followed by a
  section header is still the boolean and then the section. This is not a
  hypothetical shape: Godot's own editor writes `$selected_nodes` as an
  `Array[NodePath]` with an empty `([])` after it, in the editor state files
  under `.godot/editor/`, and the reader was building the key `([])2D` out of
  the tail of it. Running the old walk and the
  new one over 2036 ConfigFiles from three real games, the only lines that
  differ are those, read correctly now.

- **A value is what the engine reads, not the rest of one line.** #815 made the
  writer replace a whole multi-line value. The reader was still handing back the
  text between the `=` and the end of that one line, and Godot means neither
  boundary. Its own writer spreads a dictionary over four lines, and the banner
  it puts at the top of every `project.godot` documents `param=value ; comment`.
  So `project_set_setting`'s dry run offered `{` as the value it was about to
  replace, which is #417's failure exactly, and `previous_value` in the
  confirmed report said the same thing. A `; note` after a value was carried
  into it: `project_list_export_presets` published a preset named
  `"Trailing" ; a note`, quotes and all, and `project_export` could not be given
  that preset at all, while the same note in a `.import` made
  `project_audit_assets` report `invalid_import_metadata` against a file the
  engine loads without complaint. The value is now every line it spans, with the
  comment on each line dropped, and a `;` is a comment at any bracket depth --
  all asked of 4.5.1, 4.6.2 and 4.7.2, all three identical (#816). Asking
  settled one more thing while it was being asked: a value ending does not end
  the line. The engine carries on reading from where the value stopped, so
  `a=1 b=2` is two settings, `name="a" [t]` opens a section, and
  `name="a" # note` joins `#note` forward into the key below exactly as a
  `# note` line of its own would. That last one is the habitual way to write a
  comment, and it was the one spelling of #813's trap the scan still missed.

- **A `project.godot` Godot refuses to parse is no longer read, previewed and
  written as though it loaded.** A file that ends part-way through a value is
  `ERR_PARSE_ERROR` for the engine and the project does not open at all.
  `ConfigFile.load` returns 43 for the same text and still hands back the
  sections it managed to read, which is the trap: a partial parse looks like a
  parse. `project_set_setting` offered to replace a value in a file nothing can
  load, and the confirmed write rewrote one line and reported success, leaving
  it exactly as unloadable. The scan already computed the fact and no reader
  consulted it. Both the write and its dry run now refuse with a 409 naming the
  engine's own verdict, and `project_audit_assets` reports it (#817). The
  opposite case is not this: a file that ends part-way through a *key* is
  dropped by the engine with `OK`, which is an ordinary trailing `# note`, and
  it is still read and written.

- **A key is the tokens joined, not the text before the first `=`.** Godot does
  not read a ConfigFile key as the text on one line. It reads a run of tokens,
  joined, with every space between them dropped, and a line that has no `=` does
  not end the key: it joins forward into the next line that does, swallowing any
  `[section]` header on the way. Asked on 4.5.1, 4.6.2 and 4.7.2, all three
  identical. The consequence people actually hit is writing `# disabled for now`
  above an `[autoload]` entry, which is the natural way to turn a singleton off
  by hand and is worse than leaving it on: the script loads under the name
  `#disabledfornowGood`, so every `Good.` reference in the project fails at run
  time and `autoload/Good` does not exist. `project_analyze_impact` reported an
  autoload named `Good`, `project_rename_references` would have rewritten for a
  name nothing registers, and the user's real fault was the one thing nothing
  said (#813). The same rule made `config / name` a setting `project_get_setting`
  could not find and `project_set_setting` appended a duplicate key for, and made
  the GDScript diagnostics suppress an undefined identifier for a singleton that
  is not there. Every reader of a ConfigFile now computes the key the way the
  engine computes it, in one place. That includes the part a line-at-a-time
  reader gets wrong in the other direction: a value can span lines -- every
  `[input]` action Godot writes is a dictionary across four or more of them --
  and those lines are value text, not keys. `project_set_setting` replaces the
  whole of such a value rather than the first line of it, which used to leave a
  dangling `}` that would join forward into the setting below.

- **`export_presets.cfg` is read by the same rules as every other ConfigFile.**
  Its reader still treated `#` as a comment, which is the mistake #810 fixed
  elsewhere, and it could not take the one-token fix because of what sits under
  it: a `#` line with no `=` is not a comment and not a broken line either, it is
  a key that eats what follows. So a trailing `# note` had to stay harmless while
  a note above a key had to stop the presets being reported as though they were
  intact (#812). They now read as the engine reads them: a trailing note leaves
  both presets, a note that swallows `platform` leaves a preset with no platform
  and the file is refused as unparseable, and a file with content but no section
  the engine honours is refused rather than reported as a project with no
  presets. An ini whose sections are all something else is still a project with
  no export presets, not a broken file.

- **A spaced header hides nothing from the last two readers.** #809 fixed the
  section rule in the four readers of `project.godot` and left two out on
  purpose, because their fix was not the same one-token change. `[ preset.0 ]`
  matched neither anchored pattern in `readExportPresets`, so every key under it
  was skipped and a project with a working export preset reported as a project
  with none, with nothing attached to say why -- the half that matters, because
  `export_presets.cfg` is committed, hand-edited and generated for CI exports.
  `[ remap ]` and `[ deps ]` were compared as whole lines in the `.import`
  reader, so `project_audit_assets` reported `invalid_import_metadata` against a
  file the engine loads without complaint (#814). Both now name their sections
  through the same rule as the rest.

- **A `#` line in `project.godot` is a setting, not a comment.** `;` starts a
  comment in a Godot ConfigFile. `#` does not: asked on 4.5.1, 4.6.2 and 4.7.2,
  `# Hash="*res://a.gd"` under `[autoload]` registers the setting
  `autoload/#Hash`, and the script it names enters the tree on every run. The
  impact scan skipped both characters, borrowing GDScript's comment rule for a
  file that is not GDScript, so a user who disabled a singleton the habitual way
  had a singleton that still loads and `project_analyze_impact` answered
  `impact_count: 0` for the script it loads (#810) -- the answer that tool uses
  to mean safe, beside `target_exists: true`. `project_rename_references`
  collects from the same place, so it dropped the entry too. Such a line is now
  read as the setting Godot reads, and reported with the text of the line, which
  is the evidence that says why the autoload is still there. `;` is unchanged.
  The same rule was wrong in the `.import` reader behind
  `project_audit_assets`'s import findings, and is corrected with it.

- **A spaced section header is the same section.** `[ application ]` is the
  application section, and Godot merges a second spelling of a header into the
  first -- asked on 4.5.1, 4.6.2 and 4.7.2, with a tab-padded header too. Three
  readers compared the header as a whole line instead of the text inside the
  brackets. `project_set_setting`'s dry run reported a setting that is in the
  file as absent, with an empty `previous_literal`, so a caller could not see
  what they were replacing, and the write appended a second `[application]`
  section rather than updating the line above it -- a `project.godot` Godot's own
  writer would never produce, from the one tool on the surface that edits that
  file offline (#809). The GDScript diagnostics read no singletons out of such a
  file, so every autoload a script named came back as an undefined identifier,
  which is the whole reason that list is read. And `project_remove_input_action`
  refused to remove an action that is there, because the file it checks did not
  appear to define one. A header is now the bracket text, trimmed, everywhere it
  is read. A different section is still a different section.

- **A spaced `[autoload]` key is the same key.**
  `GameState = "*res://scripts/game_state.gd"` is a working autoload. Godot
  registers it exactly as it registers the spaceless form, and a tabbed one too
  -- asked on 4.5.1, 4.6.2 and 4.7.2. `project_analyze_impact` matched the key
  by the prefix `Name=`, so it read the spaced line as a line that names
  nothing and answered `impact_count: 0` (#802), which is the answer this tool
  uses to mean safe. `project_rename_references` collects from the same place
  since #792, so it said nothing either, and a caller renaming or deleting on
  an empty report broke every script that named the global. The key is now read
  as the text before the first `=`, trimmed and compared whole, through the
  same matcher `project_set_setting` already used on this file. The section
  header was read the same strict way two lines up, so `[ autoload ]` was not
  the autoload section either; it is now the text inside the brackets, trimmed.
  A longer name that starts with the target is still a different autoload, a
  key under another section is still under another section, and the spaceless
  form reports exactly what it reported before.

- **A second merge no longer cancels the first one's CodeQL run on `main`.**
  The workflow cancelled any in-progress run for the same ref, which is right
  for a pull request -- pushing a fixup should not leave the superseded run
  burning twenty minutes -- and wrong for `main`, where the two runs are two
  different commits rather than two attempts at one. Four of the last thirty
  runs on `main` died that way, each time because a merge landed inside the
  previous merge's C++ analysis. Two costs, neither obvious from the red X:
  the cancelled commit was never analysed, and this workflow's own header says
  it exists partly because Scorecard's SAST check scores on whether a tool ran
  on the commits, which a cancelled run does not. The other is the X itself,
  on a run that had nothing wrong with it, on the branch where a red mark is
  supposed to mean something. `cancel-in-progress` is now true only for
  `pull_request`, so pushes to `main` queue and each merge gets its own
  analysis.

- **`project_rename_references` reports the `[autoload]` line that defines the
  name.** The rename and the impact analysis are meant to agree about what a
  rename touches, and for a singleton they did not. The analysis reads
  `project.godot` and reports the `[autoload]` key; the rename never read the
  file, so the one line that defines the global was in `updated_files` because
  it was not rewritten, in `code_references_not_updated` because it was not
  listed, and in no other field (#792). A caller worked through the list by
  hand, finished it, and still had a global that no longer existed. Both tools
  collect from the same place now, and the key arrives in
  `code_references_not_updated` with kind `autoload` and its line. It leads the
  list rather than trailing it, so `max_impacts` cuts a use of the name before
  it cuts the definition. It is reported and never rewritten, because an
  autoload key and a symbol that happens to share its spelling are different
  things and rewriting the definition of a global on a whole-word match is the
  breakage this tool exists to prevent. When there is one, a `limitations`
  sentence says to edit `project.godot` too; when there is not, nothing is said.

- **`project_audit_assets` follows the resources `project.godot` names.** The
  audit built its reference list from the project's resources, and
  `project.godot` is not one, so nothing it names was ever counted as used.
  Every Godot project ships an icon, so every project got at least one false
  orphan on the one question this tool answers, and acting on the answer
  deletes the icon (#774). The manifest is read through the same bounded scan
  as everything else and counted in `scanned_text_files`, so `config/icon`,
  `boot_splash/image`, `run/main_scene`, the `[autoload]` entries and the
  `res://` values under `[internationalization]` all count as use. A quoted
  `res://` value is a reference form now wherever it appears, which also covers
  a path in an exported string property. It counts as use and is not checked
  for existence: in a script the same form can be `"res://levels/"` with the
  rest built at runtime, and a broken reference that is not broken is worse
  than one that is not reported. The manifest stays out of the shared source
  list, so `project_analyze_impact`, which reads that file itself and names the
  section a line belongs to, still reports each setting once.

- **`runtime_launch` names the process and the engine the rest of its answer
  means.** Two fields in one response pointed somewhere else. The `summary`
  sentence carried the pid of the process Didi spawned while `pid` and
  `game_session.pid` carried the game's, and on Windows those are two live
  processes: Godot's console build starts the engine as a child and waits on
  it, so an agent quoting the sentence named a launcher that `runtime_stop`,
  `runtime_attach_session` and Task Manager all disagreed with (#773). The
  sentence is built from the game's pid now. When no session is published
  there is no game pid to give, and the sentence says the number it has is the
  process Didi spawned and may be a launcher.
  And `attached_engine_version` was read after a detached launch had selected
  the game it just started, so the game answered about itself: a 4.5 editor and
  a 4.7 game read as agreement, `engine_version` was null because a detached
  run captures no banner to read it from, and `matches_attached_engine` was
  null in every configuration there is (#772). The attached session is read
  before the launch selects the new one, the launched engine names itself
  through the session the game published, and the comparison is made. This is
  the press-play loop: an author edits in one engine and the game can run on
  another, and the field whose name promises to say so said the opposite.

- **A resource slot takes a list of classes, so materials can be assigned
  again.** Godot spells the classes a property accepts as one comma-separated
  string and repeats it under `class_name`, and `scene_set_property` compared
  the whole string as a single class name. No class is called
  `BaseMaterial3D,ShaderMaterial`, so every write to such a slot was refused,
  including one naming a type in the list verbatim (#783). That is 34
  properties in the pinned class reference, among them every material on every
  mesh, `CanvasItem.material` which every `Node2D` and every `Control`
  inherits, the particle process materials, the sky materials, the decal
  textures and the camera attributes. A game is meshes with materials on them,
  and none could be given one in 2D or 3D. The declared type is parsed as the
  list it is now: a resource is accepted when it is, or inherits from, any
  entry, and an entry written with a leading `-` names a class the slot
  excludes even though it inherits from another one, which is how
  `Decal.texture_albedo` takes a `Texture2D` and not an `AtlasTexture`. That is
  the rule the editor's own resource picker applies. The refusal for a genuine
  mismatch now names every type the slot takes rather than the raw string, and
  an excluded type is told that it was excluded. `scene_instantiate_node`'s
  `properties` and `shader_set_uniform` share the check and are fixed with it.

- **`resource_create` stops writing files Godot cannot load or silently empties.**
  Two faults in the same writer, both reporting success. The composite packed
  arrays came out with a constructor per element --
  `PackedVector2Array(Vector2(0, 0), Vector2(512, 0))` -- and Godot's text
  parser answers `Expected float in constructor` and fails the whole resource,
  not just the property, so a `NavigationPolygon`, a `Polygon2D`, a `Line2D`'s
  points, an `OccluderPolygon2D` or a `Gradient`'s colours could not be
  authored through the surface at all (#765). Godot's own saver writes one flat
  run of components and that is what these write now; `"values"` still takes an
  element per entry, and also takes the components already flattened, which is
  what a caller copying one out of a `.tres` will send. A flat run that is not
  a whole number of elements is refused, because Godot drops the trailing
  part-element and says nothing. `PackedVector4Array` is written too, where
  before it was refused as a type the writer did not know.
  The second is that the declared-type guard only ever inspected objects, so
  any string, number, boolean or array went into any typed slot verbatim with
  `property_check` reporting `checked: true` (#764). Godot keeps the property's
  default for a value it cannot convert and prints nothing a caller sees:
  `radius = "big"` loads as `0.0`, `corner_detail = "many"` as `1`, and
  `size = 7` on a `RectangleShape2D` as `(0, 0)`, which is a collision shape
  with no extent that every tool in the chain called written. The guard now
  reads the whole declared type rather than only the vector and colour ones, so
  a slot declared `int`, `float`, `bool`, `String`, `StringName`, `NodePath`,
  `Array` or a packed array refuses a value it cannot hold, naming the property,
  what it is declared as and what to send. The conversions Godot does anyway are
  kept: an integer into a `float`, a `"#rrggbbaa"` string into a `Color`, and
  `4.0` where `4` was meant, because JSON does not separate the two. A declared
  type with no rule -- a `Transform3D`, a `Dictionary`, a resource slot -- is
  left exactly as it was.

- **The Unix socket server stops without pulling a descriptor out from under
  its own thread.** `PosixIpcServer::stop()` closed the listening socket and
  then joined the thread that was still polling and accepting on it, so for up
  to one 50 ms poll slice the accept loop worked a descriptor number the
  process had already given back, and any `open` in that window could be handed
  it (#757). POSIX names that reuse in the rationale for `close`, and this runs
  inside a Godot editor, which opens files constantly. The `shutdown` that came
  before the close was not buying a wakeup either: `shutdown` on a socket that
  is only listening is `ENOTCONN`, so the Linux behaviour that made it look
  deliberate does not hold on macOS, and the loop already left on its own
  within a poll slice because the socket is non-blocking and it rereads the
  running flag every pass. `stop` now leaves the listening descriptor alone,
  joins, and closes it with no other thread left holding the number. The
  connected client keeps its `shutdown` before the join, because that one is
  connected, so the call applies, and it is what ends the long read an idle
  client is sitting in. Taking it there now also stops the accept loop closing
  it, so each descriptor has one owner and one close. The cost is up to 50 ms
  on a teardown that already joins a thread. The POSIX branch had no test that
  started and stopped a server at all; it now has one that holds a client open
  across the stop, takes every descriptor number the teardown frees, and
  fails on a stop that waits out the idle window, on a descriptor closed twice,
  and on one closed by nobody.
- **A timeout that could not finish the kill says so.** `runtime_launch`
  terminates the job its child was spawned into and waits for the job to
  empty, so the tool does not answer while its own game is still dying. The
  wait is bounded at five seconds, because a process that will not die must
  not hang the tool, and the bound left no trace: the job emptying, the
  query failing and the bound expiring all reached the same exit and
  produced the same answer, so a caller could not tell a kill that finished
  from one that was abandoned (#755). A loaded machine reaches that bound
  where an idle one does not, which is how it red-lighted a pull request
  about tilemap coordinates that touches none of this. The answer now
  carries `kill_wait`: `tree_exited`, `wait_expired`, `query_failed`, or
  null for a run that never waited on a kill. `wait_expired` and
  `query_failed` say so in the summary as well, because a tree that may
  still be running changes what the next call can assume. The bound is
  unchanged and the test asserts the strong form on the outcome that
  earns it.
- **The offline process tools take the whole tree down on timeout.** The
  README states it as fact and the code only sometimes did it.
  `process_runner.cpp` backs `csharp_check_build`, `shader_check_compile`,
  `project_export`, `gridmap_export_mesh_library` and the git and engine runs
  behind speculative verification, and it had neither guard the sibling spawner
  grew in #351: on Windows it
  started the child running and assigned the job afterwards, so anything
  spawned in between was outside the job and survived `TerminateJobObject`,
  and `dotnet build` starts MSBuild worker nodes almost immediately. On
  POSIX only the child called `setpgid`, so a timeout that fired before the
  child reached it signalled a process group that did not exist yet and
  nothing was delivered at all (#758). The child is now created suspended,
  put in the job, and then resumed; the parent calls `setpgid` too, the way
  POSIX specifies for exactly this race; and the group signal falls back to
  the single process rather than giving up. Didi is a long lived server, so
  a leaked build accumulated over a session instead of dying with the call.

- **One InputEvent vocabulary, spelled the engine's way, and published.**
  `project_set_input_action` and `runtime_inject_input` both describe their
  `events` as objects "in Godot's InputEvent shape" and did not agree on what
  that shape is: the first read `shift`, the second `shift_pressed`, and Godot's
  own property -- the one `project_set_input_action` itself writes into
  `project.godot` -- is `shift_pressed`. So the tool accepted `shift`, stored it
  as `shift_pressed`, and refused `shift_pressed` (#737). The engine spelling is
  the answer now on both tools, the short form stays as an alias, and
  `project_list_input_actions` reports both so a descriptor read from it can be
  written straight back.
- **The InputEvent vocabulary is published, not just enforced.**
  `project_set_input_action`'s handler is closed -- it refuses an unknown
  property, an unsupported type, a missing type and a non-integer keycode, every
  time -- and its schema said `items: {"type": "object"}`, which is any object at
  all. A host validating against the published schema sent whatever the model
  invented and learned the vocabulary one round trip at a time (#736). It now
  publishes a `oneOf` over the four shapes with `additionalProperties: false`,
  per-field bounds and `required` on each branch, the way `runtime_inject_input`
  already did.
- **A refusal about an event says which entry and which property.** "Key event
  contains an unknown property" named neither, on a tool taking up to 64 events,
  where every other argument refusal on this surface names the property and
  lists what the tool takes (#737). And a `oneOf` where every branch pins one
  property to a `const` is a tagged union, so the validator reads the tag
  instead of guessing from which required properties happen to be present: an
  event with an unsupported `type` used to match the branch whose only required
  property is `type` and be refused for a property of the wrong shape. That is
  the shared validator, so `runtime_inject_input` and `tilemap_set_cells` get it
  too. `keycode` now says in the schema that it is a `Key` enum value, which
  nothing said before.
- **A coordinate is an object anywhere a vector is.** Vectors are objects on
  this surface -- `scene_set_property`, `scene_instantiate_node`,
  `resource_create`, `physics_raycast_query`, `nav_query_path` -- and
  `LLM_INSTRUCTIONS` states that rule with no exception. The two cell writers
  took arrays, so an agent following its own instructions met two refusals in a
  row on the tool whose whole job is painting a level, and
  `tilemap_get_used_rect` answered with objects a caller then had to transform
  before feeding them back (#738). `tilemap_set_cells` and `gridmap_set_cells`
  take `{x, y}` and `{x, y, z}` now, the array forms still work, and each takes
  the other's field name, `coords` or `position`, for the same thing.
- **`LLM_INSTRUCTIONS` no longer forbids what the surface does.** One section
  said to send `{x, y}` for a Vector2, a `#rrggbb` string for a Color and a
  `res://` path for a Resource slot, and eleven lines later said not to send
  Vector, Color or Resource values "in Phase 1" (#739). All three work, the
  phase they name finished long ago, and an agent reading top to bottom met the
  prohibition last. The sentence is now the exception list it should always have
  been: arrays, objects that are not one of those shapes, and Transform, Basis
  and Object values are refused, and the refusal names the property, its Godot
  type and the shape it wants.
- **A property write that landed says so.** `scene_set_property` compared what
  the property holds against the raw JSON argument, so a Color sent as
  `{r, g, b}` -- the spelling the instructions document -- came back with four
  keys and was reported `applied: false`, and a `"#rrggbb"` string never matched
  an object at all. `applied: false` is documented to mean Godot discarded the
  write, so an agent following the instructions would undo or retry a write that
  was perfect. #638 fixed exactly this for `shader_set_uniform` and this call
  site kept the raw argument; it compares against the Variant actually sent now.
- **A `oneOf` refusal picks the branch by type.** A coordinate taken as either
  `[x, y]` or `{x, y}` has an array branch with no required properties, so every
  malformed object counted it as the one shape the caller meant and was answered
  "must be an array, not an object" -- about a form the tool accepts. The
  validator drops branches whose declared type cannot hold the value, and
  describes an array branch by its length rather than as "no required
  properties", which is #489's finding in the shape this change produces.
- **`scene_create` makes the scene you meant.** `root_type` was an enum of
  `Node2D`, `Node3D` and `Control`, and almost every scene in a real project has
  a root outside it: a player or an enemy is a `CharacterBody2D`, a pickup or a
  trigger an `Area2D`, terrain a `StaticBody2D`, a HUD a `CanvasLayer`. The
  route that worked was to create a throwaway `Node2D` scene, instantiate the
  type you wanted under it, build the subtree there and `scene_pack_branch` it
  to the real path -- four calls, nothing on the surface saying so, and a
  scratch scene left on disk the caller then has to remember to delete (#740).
  `root_type` is any Godot class that inherits `Node` now, which is the set
  `scene_instantiate_node` has always taken. A class the engine does not know
  and a class that is not a `Node` are refused separately, each naming the
  class, and neither writes a file.
||||||| parent of 3449745 (Import an asset the editor has never seen, and say whether it did)
- **An asset the editor has never seen gets imported, and the answer says
  whether it did.** Adding art is step one of building a game and there was no
  way to do it through the surface. `asset_reimport` sends a path with no
  `.import` sidecar to `EditorFileSystem.update_file`, which announces a file
  to the editor and does not import one, so no sidecar was written, nothing
  appeared under `.godot/imported`, and the asset stayed unusable -- loading as
  a null texture -- while the call answered `accepted_count: 1`,
  `refreshed: [path]`, `idle: true`, which reads as "done, nothing was stale"
  (#731). A path with no sidecar now also triggers `EditorFileSystem.scan`,
  the walk that finds new files and runs the importer over them;
  `editor_reload_project`'s `scan_sources` only re-examines files the editor
  already knows about, which a new file is not. The result reports the outcome
  rather than the call: `imported` for paths that carry a sidecar now and did
  not before, `announced` for the ones that still carry none, which is the
  ordinary answer for a script and a real problem for an image. The scanning
  flag clears before the importer has written the sidecars, so a scan-driven
  call asks the editor whether its work on each path is finished instead of
  watching the flag.

- **A syntax check says whether it asked a compiler.** `script_check_syntax`
  takes either a `file_path` or a `source_text`, and only the first runs
  `godot --headless --check-only`. Nothing in the result, the published schema
  or the agent instructions said so, and checking a draft before writing it to
  disk is what `source_text` is for. Six scripts with real GDScript compile
  errors -- a typed variable assigned the wrong type, a mistyped keyword, an
  undeclared identifier, an absent method, an unknown base class, a wrong
  constructor arity -- each came back `has_errors: false`, which is the answer
  a clean script gets, and the same bytes checked by `file_path` reported every
  one (#728). The engine fields made it worse: all four come back null for a
  check that asked no compiler, which is byte for byte what a `GODOT_BIN` that
  cannot be launched returns, so one response shape stood for three states.
  Every answer now carries `engine_checked`, and one that is `false` carries a
  `limitation` naming what the verdict covers and pointing at
  `project_verify_changes`, which compiles unsaved source in an isolated copy
  of the project. The parameter description, `TOOL_REFERENCE` and
  `LLM_INSTRUCTIONS` say it too.
- **`runtime_launch` finishes its own kill before it answers.** The timeout
  terminated the process it started and left the job object to clean up the
  rest on the way out, and `KILL_ON_JOB_CLOSE` terminates asynchronously when
  the last handle closes. Godot is often not the process that was started --
  a `godot.cmd` wrapper, or Godot's own Windows console build, launches the
  engine and waits on it -- so the game outlived the call that had just killed
  it. The documented discovery flow is `runtime_launch`, then
  `runtime_list_sessions`, then `runtime_attach_session`, and that sequence
  lands inside the window every time: the list reported the game alive and not
  stale, which was true, and the attach one call later could not connect to it
  (#732). The timeout now terminates the job and waits for it to empty, so a
  caller reading `alive` is reading a settled answer. The test that covers this
  waited twenty seconds for the tree to go, which is exactly what hid it; on
  Windows it now asserts with no wait at all.

- **A crash comes back with somewhere to go.** `runtime_launch` captured a
  script error correctly and then filed the only part a caller can act on under
  `INFO`. Godot prints an error across several lines -- the message, then
  `at: _ready (res://crasher.gd:6)`, then the GDScript backtrace -- and each
  line was classified on its own text, so every frame of a crash got the level
  `print()` gets and a caller filtering `logs` on `ERROR` kept the message and
  dropped the whole stack (#744). A continuation now carries the level of the
  error it belongs to and says so with `continuation: true`. `errors` stays a
  list of message lines, and a `diagnostics` list beside it carries
  `severity`, `message`, `file`, `line`, `function`, `rule` and `frames` --
  the shape `script_check_syntax` and `script_create` already return for the
  offline half.
- **A run that crashed says so in its summary.** A script error aborts the rest
  of the frame, so a game that throws in `_ready` never reaches its own exit
  path and always runs to the timeout. `summary` is the field a reader reads
  first and it named the timeout, while the cause sat in `errors` one key away
  (#744). A run that times out with errors captured now names the count, the
  timeout and the first message.
- **A handler on a script that did not compile is reported as that, not as a
  missing method.** Register an autoload, write the scripts that use it, wire
  the signals: that is the order every Godot project is built in, and
  `project_set_autoload` says `requires_editor_restart: true` for a reason. What
  it does not say is what the restart costs every other tool. Until it happens,
  a script naming the new singleton will not compile in that editor, no script
  instance stands behind it, and every method the file declares is absent as far
  as the engine is concerned. `signal_connect` answered "The target node has no
  method by that name" and named the method -- correct, and the one thing that
  was not wrong -- so the repair a caller reaches for is to rename a handler
  that is already right (#729). That is now `409` with
  `code: "target_script_not_compiled"`, carrying `script_path`,
  `unresolved_autoloads` naming any registered singleton the script mentions,
  and the same `note` `script_check_syntax` already carries for this condition.
  A method that is in no file is still `404 target_method_not_found`.

- **A raycast in the editor asks the edited scene's own world, and answers with
  a path the surface takes.** Every spatial query resolved its world from the
  root viewport. In a game that is where the scene lives, so it was right; in an
  editor the edited scene is parented into a SubViewport under the editor's own
  docks, and a Viewport carries its own World2D. A 2D ray was asked of a
  different, empty space and reported `hit: false` through a floor that was
  standing in its path, which is exactly what a ray through empty space returns
  (#743). The 3D case only looked healthy by accident, because a SubViewport
  inherits the root's World3D -- it hit, and then named the collider with a
  370-character absolute path through the editor's dock tree and its volatile
  instance ids, which every reader and writer on the surface refuses (#742).
  Both now go through `Viewport.find_world_2d`/`find_world_3d` from the edited
  scene's own viewport, and a collider is reported as `/root/<scene-root>/Child`
  like everything else. `spatial_query_clearance`, `nav_query_path` and the
  frustum query's sightline rays were reading the same wrong world and are
  fixed with them; the sightline self-hit check compared a logical path against
  an editor one and never matched, so it works now too.
- **`collision_mask` takes Godot's whole 32-bit range.** The ceiling was
  `2147483647`, so layer 32 and the natural "every layer" value `4294967295`
  were both refused by a surface whose engine defines the mask as unsigned
  32-bit (#743). `physics_raycast_query` also published no bounds at all where
  its three siblings published theirs; it does now.
- **A vector is written as the type the property is declared, not the type its
  JSON looks like.** JSON has one shape for a vector and Godot has two types for
  it, so `{"x": 16, "y": 16}` became `Vector2(16, 16)` wherever it appeared --
  including in every integer-vector slot on the surface. Godot drops a `Vector2`
  written into a `Vector2i`, so a TileSet built the obvious way was unloadable
  while `resource_create` reported `created_offline`, `property_check` reported
  `checked: true`, `scene_set_property` reported `applied: true` and
  `tilemap_set_cells` reported six changed cells. The only witness that nothing
  had been painted was the game's own stderr (#730). The pinned class reference
  already carried each property's declared type and the check was using it for
  names only; it now picks the literal too, so `tile_size` on a TileSet takes
  `{x, y}` and gets a `Vector2i` while `size` on a RectangleShape2D takes the
  same `{x, y}` and gets a `Vector2`. A component that will not fit -- a
  fraction in an integer vector -- is refused rather than truncated, a `"type"`
  that contradicts the declaration is refused naming both, and
  `property_check.written_as_declared_type` names each property whose literal
  came from the declaration, so the correction is visible rather than silent.
- **`resource_create` says which engine its property check was not run
  against.** The comment above the call site said the caller gets what
  `script_reflect_class` gives them, and the code called the same helper, and
  the documentation promised the two fields. They were never emitted: the tool
  was handed the lease dispatch wrapper rather than the session client, so the
  cast that reads the attached session's descriptor produced nothing and the
  annotation returned early (#735). `property_check` and every entry of
  `sub_resource_property_checks` now carry `attached_engine_version` and
  `api_version_matches_attached_engine`, which matters because the dump is
  pinned to one engine line and CI covers three.

- **The last uncached build in CI is cached, and every platform now configures
  the same way.** After the sanitizer job and the two live Godot jobs were given
  a compiler cache, `windows-latest (msvc)` was the only build left compiling
  from scratch, and on its own it was the entire critical path: a 12m28s run in
  which every other job finished inside six minutes. It was the exception
  because it used the Visual Studio generator, which ignores
  `CMAKE_<LANG>_COMPILER_LAUNCHER`, so a cache could not be attached to it
  without moving it to Ninja. It is on Ninja and sccache now, which collapses the
  two configure steps into one -- the launcher is the only thing that still
  differs across the three platforms, because ccache does not handle `cl.exe`
  and sccache does. `--config Release` goes with it, since a single-config
  generator has no configuration to select.

  The Windows binaries move from `build/Release/` to `build/` in the six places
  that named them, two of which hand the test runner `didi_tests` and four of
  which hand it the server `didi`; the distinction is preserved. `ctest -C
  Release` is left alone deliberately: CTest ignores `-C` on a single-config
  generator and still finds both tests. The staged addon is unchanged, verified
  by running the workflow's own file-list check against a real Ninja tree rather
  than trusting that it would be.

- **The live Godot jobs cache their compile instead of repeating it.** Both
  engine versions built the same Windows Release tree from scratch, and the
  `windows-latest (msvc)` job beside them built it a third time: about 11 minutes
  of each 17-minute job, and the longest thing in CI once the sanitizer build was
  cached. Sharing one build between the three jobs was the obvious move and the
  wrong one. The msvc build is 9m40s against the Godot jobs' 11m38s, so making
  them wait on it trades two parallel builds for one serial build plus an
  artifact download and leaves the critical path where it was; it saves runner
  minutes, which are free on a public repository, and spends latency, which is
  not. These jobs now compile through sccache instead. That needs Ninja, because
  the Visual Studio generator ignores `CMAKE_<LANG>_COMPILER_LAUNCHER`, and Ninja
  needs the MSVC environment on PATH. Both engine versions share one cache key,
  since they compile a byte-identical tree and differ only in the editor they
  then drive. Ninja is single-config, so the binary moves from
  `build/Release/didi.exe` to `build/didi.exe`; the build tree stays at `build/`,
  which is where the harness writes its engine logs and where the failure
  artifact upload looks for them, and the harness already resolved
  `build\didi.exe` among its candidates.

- **CI's critical path was a cold compile of a tree that was already cached.**
  The sanitizer job builds on `ubuntu-latest` and had no compiler cache, so every
  run compiled the whole tree from scratch: 21m45s of a 23m run. The
  `ubuntu-latest (gcc)` job beside it compiled the same sources on the same
  runner in 8 seconds, off a ccache the sanitizer job never got. Nothing else in
  the workflow ran past minute 17, so one missing cache step was the whole of
  CI's latency, and it had been paid on every pull request that touched the
  engine. The job now takes the same pinned `ccache-action` under a key of its
  own. The sanitizer flags are part of every compile command, so ccache hashes
  them and cannot hand this build an object compiled without them; a separate key
  also keeps two sets of objects from evicting each other out of one 500M cache.

- **A push to main no longer cancels the run that was checking the last merge.**
  `cancel-in-progress` applied to every ref, and main is the one ref where the
  older run is not obsolete. The branch rule does not require a pull request to be
  up to date before it merges, so two of them can both be green against an older
  base, both merge, and disagree only once they are both on main -- and the run on
  main is the only place that disagreement gets caught. The next merge cancelled
  that run 29% of the time, and a cancelled run reports nothing at all.
  Cancellation is now scoped to `pull_request`, where a newer head does genuinely
  obsolete the run before it.

- **`project_rename_references`'s preview names the sites it will leave
  behind.** The tool's behaviour and its post-hoc honesty are unusually good: it
  updates the scene connection, leaves GDScript alone, and names every site it
  skipped, the function's own declaration included. It just named them after the
  mutation. The `dry_run` preview -- the thing a caller reads to decide whether
  to confirm -- carried every other field the confirm did and withheld that one
  list, leaving the caller to notice that `code_reference_count: 4` beside
  `updated_files: [one .tscn]` does not reconcile, and to infer by subtraction
  that the declaration was among the sites being skipped (#716). The field was
  already computed by the same call for the same arguments. It is now in the
  preview, which also puts it in the confirmation fingerprint, so a token is
  spent against the sites the caller was shown rather than against a list that
  moved underneath them. The list is capped at `max_impacts` and the cap was
  computed and dropped; `code_references_truncated` now says when it bit.

- **Three semantic failures answer with the error envelope rather than a bare
  string.** #420 put a code on eighteen of these, #460 on eight more, #492 on the
  five unimplemented registrations and #548 on four behind valid arguments. Each
  pass found what its census could reach, and these three sat behind a different
  precondition again (#705): `project_export`'s overwrite refusal needs an output
  path that **exists**, so the wrongness is in the filesystem rather than in the
  arguments; `gridmap_export_mesh_library`'s extension check needs a valid
  `source_scene` first, so a census generating junk gets that tool's other error;
  and `csharp_check_build`'s launch failure needs a broken environment variable,
  which no census varies. The export one was the sharpest: its whole message is an
  instruction to retry with a different argument, and nothing said so in a way a
  caller could branch on. It now answers `409` with `data.code: "conflict"`, the
  `res://` output path and `retry_with: {"overwrite": true}`, the same shape
  `script_create` gives the same collision. The extension check answers `400`
  `invalid_arguments` naming the parameter and the extensions it takes. The
  `csharp_check_build` one was fixed with the rest of that tool.

- **Every unbound name in `eval_gdscript` is refused by its own name.** A typo in
  `node`, a singleton a caller reasonably expected, a name of the caller's own --
  each reached Godot and came back as "Expression execution failed: self can't be
  used because instance is null (not passed)", a sentence about a word the
  expression did not contain (#712). #488 diagnosed exactly this and fixed it for
  the literal string `self`; the common case is not `self`. A bare identifier that
  is not `node`, a literal, a letter-spelled operator or one of Expression's own
  numeric constants is now refused with its own name in the message, saying that
  `node` is what is bound and pointing at `node.get(...)` and
  `scene_get_property`. The nineteen containment refusals are unchanged, and a
  forbidden or reserved name keeps the sentence it already had.

- **Two parameter descriptions stop offering a value their own enum refuses.**
  `blackboard_task_list.status` read "such as pending, claimed or blocked", and
  `claimed` is not a near miss for one of the six the enum holds -- it is the
  word the sibling tool uses, and `blackboard_task_claim` puts a task in
  `in_progress`. `project_export.mode` read "whether to export a debug or a
  release build" and left out `pack`, which is not a variation on the other two:
  it writes a `.pck` and is the only mode that works on a machine with no export
  templates installed, which is every CI runner and most fresh checkouts (#708).
  Since #462 every parameter carries a description precisely so a caller does not
  have to make a call go wrong to learn the argument names; a description that
  offers a value the enum rejects spends that budget backwards. Nothing could
  catch these: the description tests count descriptions and the schema tests read
  keys, and `probes/description_vs_schema.py` is the check that compares one
  against the other.

- **The two legacy names with no canonical replacement say so.** Eight of the ten
  legacy aliases publish `_meta.didi.canonical` and a description naming the tool
  they stand for. `mutate_scene_tree` and `instantiate_asset` published neither,
  so a host routing `legacy: true` entries by their canonical name handled eight
  and fell through on two with nothing in the entry saying why (#709). The reason
  is not that their canonical tool is unimplemented: there is no canonical tool.
  Neither capability was ever re-registered under a canonical name, so the legacy
  name is the only name and `canonical_tool` in error data correctly reports it.
  Both entries now say that, and say that the absent `canonical` is the reason
  rather than an omission.

- **Two tools stop advertising a set the answer is not a member of.** With an
  editor attached, `project_audit_assets` and `project_get_uid_map` publish
  `currentMode: "live"` and can answer `execution_mode: "local"`, while declaring
  `executionModes: ["live", "offline_fallback"]` -- a claim that `local` is not
  one of the things they do (#713). The answer is right and deliberate: #504 gave
  both `local` for the call with no live work to do, because calling that an
  offline fallback told a caller to reattach an editor that would change nothing.
  The advertisement never learned the third word, and now does. The live path was
  not removed from either: a uid map with `resolve` queries and an audit whose
  scan produced findings both take it. `probes/advertised_vs_reported_mode.py`
  now separates the two questions -- a mode the entry never declared, which no
  argument can excuse, from a declared mode that is not `currentMode`, which is
  what a tool whose live work depends on its arguments looks like.

- **The listings say they move, and say when.** `initialize` published
  `capabilities.tools.listChanged: false`, which in MCP is the server telling a
  host that one `tools/list` at startup is enough. Every entry in that listing
  then changed when the bridge changed: 68 move `currentMode`, and the other 58
  move anyway because `editorConnected` and `sessionKind` are published per
  tool. Both cannot be true. A host that listed before the user opened Godot
  cached `currentMode: "unavailable"` for 59 live tools and never offered them
  again, for the whole session (#701). Both listings now declare
  `listChanged: true` and mean it: `notifications/tools/list_changed` and
  `notifications/resources/list_changed` are sent when the state those entries
  carry has moved -- a session attaching or detaching, a route obstruction
  appearing or clearing. The check is one string comparison after each request,
  taken from reads that attach nothing and scan nothing, so a quiet session
  sends nothing and ten listings in a row send nothing. The resource listing
  gets the same treatment because it carries the same state from the same
  computation; this is separate from `notifications/resources/updated`, which is
  per URI and already worked.

- **`--ui-app off` stops declaring the MCP Apps extension.** The flag turned the
  Control Room off everywhere except the handshake, which still told the client
  the server serves it -- byte-identical `extensions` in all three modes. The
  sequence a host then walked was: declare the UI extension, read the server
  declaring it back, look for the app resource, and get a `400` on the only
  resource the extension exists for (#717). Unconditional declaration is right
  for `auto`, where the surface is opt-in on both sides and the two declarations
  are the negotiation. `off` is not a negotiation: the operator has decided, and
  no client declaration can change the answer. `auto` and `always` are unchanged.

- **A refused ghost preview leaves the screen as it found it.** These are
  on-screen gizmos an agent draws to show a human what it is about to do.
  `editor_render_ghost_preview` replaces by default, and the teardown ran before
  the engine had been asked whether the new shapes could be drawn at all, so a
  2D preview asked for in a 3D scene reported `409` -- which reads as "nothing
  happened" -- having already freed the proposal that was on screen. Neither the
  agent nor the user could tell: the agent's bookkeeping still said the preview
  was up, and the next clear answered `cleared_previews: 0`, which is what
  "nothing was there" looks like (#707). The target world is now resolved before
  anything is replaced, and when a refusal does follow a replace -- the shape cap,
  or a rendering server call that fails mid-draw -- the error's `data` carries
  `previews_were_replaced`, `cleared_previews` and `cleared_shapes`.
  `tools/vibe/probes/ghost_preview_survival.py` holds the four rows, with the
  argument-check refusal as the control that was always safe.

- **`signal_disconnect` stops reporting a method signature problem.** Arity
  compatibility is a precondition for making a connection and cannot be one for
  removing it: a disconnect never calls the method. One validation helper served
  both handlers, so tearing down a connection that is not there was answered
  with "the target method cannot accept the arguments that signal carries" --
  telling an agent that had changed a method's arguments, and was now cleaning
  up the connections it made, to go and edit a signature to satisfy a check for
  a connection it is trying to delete (#714). The true answer is the one the
  same tool already gave for a compatible pair that was never connected: no such
  connection. The three refusals `signal_connect` owns are unchanged.

- **`scene_instantiate_node` says when the engine did not use the name it was
  given.** Godot forbids `.`, `:`, `@`, `/`, `%` and `"` in a node name and
  substitutes rather than refusing, and it uniquifies a name a sibling already
  has. Reporting the real path was already right; what was missing was any field
  saying a substitution had happened, so an agent that named a node after a
  `class_name`, a filename or a JSON key built its next `NodePath` from the name
  it chose, and the reason for the `404` that followed was four responses back
  (#710). A call that gave a name now gets `node_name` back, and when that is not
  the name asked for, `requested_name` and `name_substituted: true` beside it.
  Omitting the name asks the engine to name the node and is not a substitution.

- **A runtime endpoint too long for `sockaddr_un` says so.** On macOS the
  session endpoint is built under the temporary directory and `sun_path` holds
  104 bytes, so a stock `macos-latest` runner has five bytes of headroom.
  `TMPDIR` twelve bytes longer and both ends of the bridge answered the
  overflow with a bare `return false`: the plugin reported itself active, no
  descriptor was published, every live tool gave the ordinary "no editor is
  running" refusal, and nothing anywhere -- the editor log, the server log,
  `runtime_list_sessions`, the control room -- mentioned a path length. That is
  indistinguishable from the editor genuinely not running, which is the most
  common state in the world (#711). The session now refuses before it builds
  the socket, naming the byte length, the limit and the endpoint, and saying
  that the temporary directory is the part a user can change. The two bridge
  failures the extension already logged also go through Godot's own
  `print_error`, so they reach the editor's Output panel and its `--log-file`
  rather than only this process's stderr and a log ring that is read over the
  route that just failed to exist. That is where the addon's Diagnostics page
  has been telling people to look.

- **`csharp_check_build` now reports the build it actually ran.** Four things
  were wrong in the one payload. The MSBuild console logger prints every
  diagnostic twice, once as it happens and once in the summary block it appends
  by default, and nothing de-duplicated them, so a build MSBuild called `1
  Warning(s)` and `1 Error(s)` was reported as `diagnostics_count: 4` beside
  that summary in its own `raw_output`; the diagnostic code is optional in the
  format MSBuild documents, and requiring one dropped `warning : Unable to find
  a project to restore!` entirely, so a build that compiled nothing reported
  zero diagnostics (#702). `diagnostics[].path` carried an absolute host path
  beside a `project_file` the same object had already converted, so the one
  field naming the file to go and fix was the one field no path-taking tool
  accepts, and it carried the home directory of whoever ran the server; paths
  inside the project are now `res://` and paths outside it, which are SDK and
  NuGet targets, are left as they came (#703). Nothing said which `dotnet` ran,
  so a missing SDK arrived as `exit_code: 2` on Windows and `127` with no error
  at all on POSIX, wearing the costume of "your C# does not compile"; the
  answer now names `dotnet_executable` and `dotnet_version`, a `DOTNET_BIN`
  that does not exist or is a directory is passed over and reported in
  `dotnet_executable_configured_rejected` the way `GODOT_BIN` has been since
  #656, and an executable that is not a .NET SDK is a `503` naming what it
  printed rather than a build verdict (#704). And `project_file` accepted a
  `.sln` its description did not mention and preferred it over the `.csproj`
  beside it, which is the exact shape a Godot C# project has, while a solution
  that compiled nothing exited `0` and was reported as `success: true`; the
  description names both and states the preference, `projects_built` counts the
  projects MSBuild produced an assembly for, and a `dotnet` that exits `0`
  having built none is no longer a success (#706).

- **A session test reads its own directory, not the machine's.**
  `RuntimeSessions.ListingNamesTheDirectoryItRead` made an empty descriptor
  directory and then never pointed the client at it, so it listed whatever
  `DIDI_SESSION_DIR` happened to hold: an earlier test's value, or nothing, in
  which case the real one under the system temp directory. Any Godot running
  anywhere on the machine put a live descriptor in the answer and failed it.
  It sets the directory it created, compares the reported path exactly rather
  than for the substring `didi`, and no longer asserts that no other directory
  holds sessions -- the candidates come from the environment, so an editor
  running for any reason puts a correct entry there, which is the thing the
  field exists to say. The sibling test beside it goes through the same
  helpers rather than writing the environment by hand, so isolation is visible
  at a glance.

- **`viewport_create_test_lab`'s preview names the file it replaces.** It made
  the weak claim -- `argument_binding`, "this tool names no subject of its own
  beyond the arguments it was given" -- about a call whose subject is a
  constant, and showed the reader `target_resource_path`, a file the call reads
  and does not modify (#685). The confirmation gate had already resolved
  `res://didi_test_lab.tscn` and stat'd it, which is the only reason it fired
  at all. The preview now reports that path with its size and content digest,
  the way `script_create` does, `target_read` and `target_checked_on_confirm`
  are true, and a token approved against one lab scene is refused if that file
  changes inside the window.
- **`--yolo` shows up where a client reads before it calls.** The flag removes
  the confirmation gate, and the only difference it made to the published
  surface was one fact inside a `didi_control_room` call: `initialize`, the
  annotations and every tool's `_meta.didi` block were byte-identical in both
  modes, and the per-result `confirmation: skipped` arrives after the mutation
  (#684). `server/discover` did carry the flag, but that is a method a
  2024-11-05 client never calls. `initialize` reports it now, and so does every
  tool entry beside `currentMode` -- that block is already state rather than a
  static fact about the tool, and whether this server will stop and ask is the
  same kind of fact. The behaviour is unchanged; only its visibility was
  missing.
- **The blackboard records who removed a value, not only who wrote one.**
  `blackboard_clear` is the one destructive call on the board and was the only
  one with no identity argument at all: an agent that came back to find its
  keys gone could read `author` on every value still there and nothing about
  the call that took the rest (#681). It takes `author` and `reason` now, and a
  read of a cleared path answers `reason: "cleared"` naming them; a clear of
  the whole board, which leaves no path to attach that to, writes a line in the
  board's audit that the next unanswerable read reports.
  `blackboard_task_create` takes `author` for who asked for the task, which is
  not `assigned_to`. `author` and `agent_id` stay different names because they
  are different things -- one is provenance, the other an identity a lease is
  checked against -- and every one of those parameters now says so.
- **A blackboard key that expired does not read like one nobody wrote.** Once a
  `ttl_seconds` lapsed, the value and its metadata were swept and the only tool
  that can ask about the key answered as if it had never existed -- identical
  to a read of a path nobody has ever written, down to the field list, with
  `include_metadata: true` returning nothing on either (#680). A ttl is written
  by an agent that wants something to lapse, and the two explanations lead
  opposite ways: take the claim again, or go and find the work filed under a
  path with a typo in it. A read that finds nothing now says `reason:
  "expired"` with `expired_at_ms`, and the author and reason the write
  supplied, or `reason: "no_record"`. A board remembers its 256 most recent
  expiries.
- **`blackboard_patch` says which operation failed, in its own words.** Every
  semantic failure answered with nlohmann's exception text and its internal
  identifier: "[json.exception.parse_error.105] parse error" for a patch
  document that parsed perfectly well, and a byte offset into a JSON pointer
  rather than into anything the caller sent (#679). A three-operation batch was
  rolled back with "key 'nope' not found" and no way to tell which of the three
  did it. The shape of each operation is checked here now, so a refusal names
  the entry, the field and what was expected; the failures that can only be
  found by applying the patch are applied one at a time, so they name the entry
  too, and a failed `test` says what the board actually holds.
- **A headless editor is a state Didi can name.** `--headless` is the only way
  an editor runs on a build machine, in a container or over ssh, and Didi
  attached to one happily: the session published, the bridge green, 68 tools
  live. Then every tool whose answer is a picture failed with `404 not_found`
  and "Viewport image is unavailable", which names nothing and reads as "look
  again with a better argument" for a condition that will not change while that
  editor is running (#676). The same server with no editor at all answered
  better, because the live route existed so the offline fallback never ran. Both
  capture tools now refuse `409`, naming the display driver and saying nothing
  about the request can fix it, in the shape the far-plane refusal beside them
  already used. The editor reports `display_server` and `renders` with its
  state, so `didi_control_room` names the limitation beside the Godot 4.7
  dirty-state one. Reproduced identically on Windows, macOS and Ubuntu.
- **`editor_save_scene` reports what the engine printed while saving.** Against
  a headless editor every save made Godot print `ERROR: Parameter "t" is null`
  from its dummy rendering backend -- the save path asks for a scene thumbnail
  and there is no renderer to make one -- and the tool answered `saved` with no
  mention of it, leaving the error in a log nothing pointed at (#683). Seven
  accumulated in one probe run of a couple of dozen calls. The thumbnail step
  belongs to Godot's save and cannot be switched off from here, so the answer
  now carries `engine_diagnostics` and a note saying they are about the
  thumbnail rather than about the scene. The scene still saves.
- **`script_check_syntax` refuses when the compiler never ran.** It answered
  `has_errors: false` with no diagnostics when the Godot it was told to use
  could not be launched, so a caller asking "does this compile?" about a script
  with four compile errors was told yes (#677). `engine_version: null` was the
  only trace and nothing said the pass had not happened. The one misconfigured
  value that gets this far is a real file that is not the engine, which is what
  a version-manager shim or the wrong file out of a bundle looks like; a
  directory or a missing path was already discarded. When the executable that
  was tried is a file that exists and it prints no Godot banner, the call is
  now `503` with `code: "engine_unavailable"`, naming what was tried and what
  went wrong, which is what `shader_check_compile` has always done. A machine
  with no Godot installed is a different state and answers as it always has:
  the lexer verdict, with `engine_available: false` and
  `engine_unavailable_reason` saying the compiler did not run.
  `engine_exit_code` and `engine_duration_seconds` accompany a check that did.
  A `source_text` check spawns no engine by design and is unchanged.
- **The engine-mismatch check works without an explicit attach.**
  `attached_engine_version` and `matches_attached_engine` read the process
  selection, which only `runtime_attach_session` or an earlier live call sets,
  so on a server that had made neither they came back `null` -- "there is
  nothing to compare against" rather than "I did not look" -- beside a live
  editor on the same project (#687). They are now filled in whenever one live
  session on this project can be seen, which is the condition live routing
  already selects on; the read takes no route and no lock. `runtime_launch`,
  the tool whose whole answer is what happened when your project ran, gained
  the same four fields: which build ran it appeared only in the banner Godot
  prints into the captured logs.
- **`--managed-editor` starts against Godot's Windows console build.** It
  refused after thirty seconds with "Owned editor did not attach", and pointed
  the reader at an editor log that shows a healthy editor (#678).
  `Godot_v*_win64_console.exe` is a launcher: it starts the ordinary editor as
  a child, and the child is what loads the addon and publishes the session, so
  managed mode waited for the process ID it spawned next to a descriptor for
  its own workspace that had arrived in under four seconds. The owned editor is
  now matched by the workspace it published on, which this run created and
  nothing else has open, and `runtime_recovery_status` reports `editor_pid`
  beside `pid` so the two are not confused when they differ. A refusal now
  names the launched process ID, the workspace and every process that published
  a session for it. `stop()` ends the job rather than only the launched
  process, and waits for the whole tree rather than for that one process: it
  used to return while a launcher's editor was still exiting, so
  `runtime_restore_checkpoint` renamed a project directory another process
  still had open. A restore also retries that rename for a few seconds rather
  than failing on it: once no owned process holds the workspace, what is left
  is somebody else's handle on a file written moments ago -- a scanner, an
  indexer -- held for a fraction of a second, and a destructive operation
  should not stop for that. CI runs the managed recovery suites against the
  console build, which is the one the issue was about and the one that step
  had been filtering out; the suites' own fixture cleanup waits for the editor
  to release the workspace instead of reporting the wait as an error.
- **`--log-level DEBUG` answers a client that does not read stderr.** The
  startup log at `DEBUG` is one line per registered tool, which is past a pipe
  buffer, and it was written inline on the thread that would have answered
  `initialize`. A host that left stderr undrained saw no error, no exit and no
  output: the server simply never answered (#689). The MCP stdio transport says
  a server may write logs to stderr and a client may ignore them, so depending
  on the client reading was depending on something the server was not promised,
  and `DEBUG` is the level the field trial documents tell people to use. The
  console is now written on its own thread behind a bounded queue: a full pipe
  costs log lines, not the server, and the next line to reach a reader says how
  many were dropped. The extension is unchanged and still writes inline.
- **The editor exits cleanly on macOS and Linux.** Every editor exit on both
  platforms ended in `SIGABRT` with the addon installed, and exited 0 without
  it, whether the run was `--import` or `--editor --quit` (#688). macOS named
  it: an uncaught `std::system_error`, "mutex lock failed: Invalid argument".
  Linux reported `malloc_consolidate(): invalid chunk size` for the same thing.
  The runtime IPC singleton's destructor calls `stop()`, which asks
  `EditorHook` to cancel pending commands, and the hook was constructed inside
  `start()` -- after the IPC object. Statics are destroyed in reverse order of
  construction, so at exit the hook went first and `stop()` then locked a mutex
  and swapped a queue that no longer existed. The hook is now built by the IPC
  constructor, which puts the whole chain in an order whose reverse is safe.
  Windows survived the same code because a destroyed `std::mutex` there does
  not refuse a lock, which is why thirteen sessions of Windows testing never
  saw it.
- **The Linux release starts on a distro whose glibc meets the stated floor.**
  The archive is built in an Ubuntu 22.04 container so the release does not
  raise its glibc floor, and `release.yml` says so at length. glibc was not the
  constraint that bound: the binary linked libstdc++ dynamically, and Ubuntu
  22.04's libstdc++ is GCC 12's, which needs `GLIBCXX_3.4.30`. Rocky, RHEL and
  AlmaLinux 9 have glibc 2.34 -- exactly the floor the container was chosen to
  hold -- and libstdc++ from GCC 11, one release short, so neither `bin/didi`
  nor the addon's `.so` would start there (#647). The release leg links the C++
  runtime statically, which is what Godot's own Linux builds do and costs about
  a megabyte, and a new step reads the symbol versions off both binaries and
  fails the release if either needs a libstdc++ symbol version or a glibc above
  2.34. README's platform table names the distros rather than saying "older
  glibc".
- **The macOS archive's `.gdextension` declares only what the archive holds.**
  It is one file copied into every platform's archive and it declared
  `macos.*.x86_64` and `macos.*.universal` pointing at the one dylib the release
  builds, which is a thin arm64 Mach-O. Godot's loader requires every tag in a
  key to be a feature of the running engine, so on an Intel Mac
  `macos.release.x86_64` matched, the file was found, and the dynamic loader was
  handed a library for another architecture: the user saw a generic "can't open
  dynamic library" about a file that is plainly there, rather than the honest
  answer the release notes already knew (#648). Only arm64 is declared now, so
  Apple Silicon still matches and Intel is told there is no library for it.
  `OS::has_feature("universal")` is true on both macOS slices, which is why
  those entries were a second name for the same mistake.
- **The Diagnostics page checks the architecture, not the filename.** It
  reported `Extension binary: OK` for a library the engine cannot load, and that
  page is what README asks macOS testers to paste into an issue (#648). It reads
  the Mach-O header now and says which architecture the file holds and which one
  the editor is. README's platform table also states the minimum macOS version,
  which nothing did.

- **The addon folds path case where the filesystem does, not only on Windows.**
  Its two path comparisons each asked `OS.get_name() == "Windows"` and meant
  "does this filesystem fold case". The default macOS volume is APFS,
  case-insensitive and case-preserving, and folds exactly as NTFS does, so the
  branch was right for two platforms out of three and wrong for the one that
  shares the behaviour it was written for (#655). On a Mac a descriptor recorded
  through one spelling and an editor reporting another read as a different
  project in the Peers list, and the notice saying a chosen binary sits
  somewhere Didi own file tools can write was not shown. Both now go through one
  helper in `didi_client_config.gd`. The C++ side settled the same question in
  #546 by taking the on-disk spelling from `std::filesystem::canonical`.

- **A `GODOT_BIN` that cannot be used is reported, not discarded in silence.**
  Resolution dropped it and fell through to the known locations, and
  `script_check_syntax` reported that fallthrough as `engine_executable`, so
  the one field that could have shown a user their variable was ignored named
  something they never set (#656). On macOS the thing called Godot is
  `/Applications/Godot.app`, a directory, so the obvious value to set is exactly
  the one that got dropped, and `ADMIN_GUIDE` tells people to set this variable
  when installations use another name or layout, which is the population that
  will set it wrong. It now answers with `engine_executable_configured` and
  `engine_executable_configured_rejected` beside the executable that ran
  instead, and logs a WARN line; the rule is stated too, since "not a directory"
  meant a non-executable file was kept and a bundle was dropped. The POSIX
  `GODOT_PATH` directory search also looks for `Contents/MacOS/Godot`, so
  pointing it at a bundle works wherever the bundle lives, including on a
  case-sensitive volume where the bare `godot` candidate does not match `Godot`.
- **`runtime_list_sessions` says which directory it read.** The server resolves
  exactly one descriptor directory, and when the editor published somewhere
  else there is nothing to find. "Nothing to find" came back in the same words
  as "Godot is not running", down to the byte: the control room's Bridge light
  named one cause and instructed the reader to do the thing they had already
  done, and every input to that decision was in the facts list except the one
  that was wrong (#649). `descriptor_directory` is now in the answer whether or
  not anything was in it, and `descriptor_directories_with_sessions` names any
  other candidate directory on the machine that does hold descriptors, which is
  where to point `DIDI_SESSION_DIR`. The Bridge light's reason carries the same.
  This bites hardest on macOS, where `TMPDIR` is a per-user path launchd sets
  and a scrubbed environment does not have.

- **`maxLength` counts what it says it counts.** JSON Schema defines the length
  of a string as its number of characters, and `checkBounds` measured
  `std::string::size()`, which is the UTF-8 byte count. So the server enforced a
  bound a third as generous as the one it published for anything outside ASCII,
  and reported the refusal in the units it was not using: a hundred-character
  Japanese or Greek search term came back as "must be at most 256 characters
  long" (#663). 140 string parameters carry a `maxLength`, and the ones people
  put words in are the ones this bit. The schema check counts code points now,
  and so do the handlers that had their own byte cap behind the same published
  number -- the two search queries and the blackboard task title and
  description -- so a client that validates against the published schema before
  sending reaches the verdict the server reaches. Where a handler bounds bytes
  for its own reason, its parameter description says so, which is what
  `eval_gdscript.expression` and the node-path parameters already did.
- **A parameter pinned to one value says why.**
  `viewport_toggle_debug_draw.wireframe` is `const: false` and its description
  read "Draw geometry as wireframe", so an assistant reading discovery was told
  the parameter exists, told what it does, sent the value that does it, and was
  refused with a sentence that restated the constraint and gave no reason
  (#654). The reason was already written in `TOOL_REFERENCE`: Godot exposes no
  supported live wireframe control. It is the description now, and every
  refusal for a `const` or a single-value `enum` carries that parameter's
  description with it. Three of the four pinned parameters on the surface
  already said "this is pinned, and here is why", which is what made this an
  invariant rather than one tool's slip, and there is a test for it now: the
  description tests counted descriptions and the schema tests read keys, and
  nothing compared one against the other.

- **The export family answers with an envelope and previews what it will do.**
  Four failures in `project_list_export_presets` and `project_export` were bare
  prose strings with no code and nothing to branch on, which the error-envelope
  census could not reach because they need the project in a particular state
  rather than a particular argument (#651). With them, three things they were
  getting wrong. A project with no export presets is now the same answer
  whether or not `export_presets.cfg` is on disk, where an absent file was a
  success and a present empty one an error; a file that is there and cannot be
  parsed is its own `422`, carrying how many preset sections it declared, rather
  than one sentence with an "or" in it covering three states. `project_export`
  reports what `project_list_export_presets` already knows about the file
  instead of answering "Export preset not found" for an unparseable one and
  sending the reader off to add a preset the file already declares. And Godot's
  console output is carried as `engine_output` under `error.data` with the
  terminal escapes removed, rather than four kilobytes concatenated into a
  message with the actionable line sixty lines down and the colour escapes
  still in it.
- **`project_export`'s preview reads the presets file.** It came back clean in
  all five states `export_presets.cfg` can be in, and the real call failed in
  every one, including for a preset name the sibling tool in the same process
  could prove does not exist (#652). The check is one local file read, so the
  preview does it: a preset that is not declared is refused at the dry run with
  the names that are there under `available_presets`. The preview and the call
  go through one function, so they cannot answer differently about the same
  file.
- **`gridmap_export_mesh_library`'s preview describes the file it replaces.**
  It claimed `preview_kind: "target_state"`, which means the preview opened the
  target and this is what it found, and described `source_scene` -- a file the
  call reads and does not modify -- with its size and content digest under
  `kind: "planned_mutation"`, while the file `overwrite` was about to destroy
  appeared only in the echoed arguments (#657). The preview now reads
  `output_path` and keeps the source beside it as context, and still refuses a
  `source_scene` that is not there. Its conversion failure carried a message
  shaped to hold a reason and holding none, ending in a colon with nothing
  after it; it is an envelope with the engine's output now.

- **A script this process may not read is not reported as bad code.**
  `script_check_syntax` answered `isError: false`, `has_errors: true` and one
  diagnostic at line 1 column 1 of a file whose bytes were never read, under
  `rule: "file_not_found"` about a file that is found, so a reader acting on it
  went and edited a line that is fine. `script_get_symbols` answered `400
  invalid_arguments` about arguments that were fine. Both were the inverse of
  the absent case, which answers `404`, so the state a `chmod` fixes was the one
  that read like a code problem (#653). Both now refuse with `403`,
  `code: "forbidden"` and `reason: "unreadable"` -- `project_search_text`'s
  word for the same state -- naming the `res://` path rather than the absolute
  host path. The fabricated diagnostic no longer carries a line and column it
  did not derive from the file. The state is a mode with no read bit on Unix and
  a file another program is holding open on Windows, and the new test creates
  whichever of those the host supports.

- **A file whose name JSON cannot carry is named, not blamed on the caller.**
  A POSIX filename is a byte string, so a `.gd` copied off an old drive or
  unpacked from a Latin-1 zip is a legal file with a name that is not UTF-8.
  JSON is defined over Unicode, so serialising that path threw while building
  the response, and the throw was caught as a caller having read an argument at
  the wrong type: `project_list_resources` and `project_audit_assets` answered
  `400 invalid_arguments` to calls that carried no arguments at all, and
  `project_search_text`, `project_search_symbols` and `project_get_uid_map`
  each behaved differently depending on the file's extension (#650). The
  encoder failure is now its own code, `response_not_encodable` with a `500`,
  because a response the server cannot encode is the server's fault; the
  walkers skip such a file once, in the index and in the search walk, so they
  all give the same answer about it; and they name it, with the undecodable
  bytes rendered as U+FFFD, under `undecodable_paths` and
  `undecodable_path_count` or as a `diagnostics` entry reading
  `undecodable_name`. Windows names are UTF-16 and the default macOS volume
  refuses the name outright, which is why eleven sessions of probing never met
  this.

- **`project_rename_references` previews the plan it is about to carry out.**
  It is in the always-confirmed list with the reason written beside it: it
  rewrites several files at once, there is no editor undo stack behind a file
  on disk, and the preview is the only chance to see which files it will touch.
  It had no preview probe, so the dry run bound the two identifiers the caller
  had just typed to a token, reported `target_read: false`, and named no file
  (#662). The preview now runs the scan and the plan the execute path already
  computes: `before` carries `updated_files` with a per-file `changed_lines`
  count, `updated_file_count`, `changed_lines` and `code_reference_count`. The
  refusals moved with it, so a `new_name` that collides with an existing
  connection is refused at the preview with its conflict list instead of
  minting a token for a call that cannot run. The confirmation is bound to that
  plan, so a project that changes in between is refused rather than rewritten
  against a plan nobody saw.
- **A match in a scene or a resource is not called a source code reference.**
  The name-target fallback never looked at which file the line came from, so a
  `[node name="health"]` line in a `.tscn`, an `ext_resource` line and a
  property in a `.tres` all came back as `code_reference` (#665). They are
  `resource_reference` now, which is the distinction `collectNodePathImpacts`
  in the same file already made. It matters most in
  `project_rename_references`, where the same bucket is returned as
  `code_references_not_updated` under a limitation pointing at
  `script_patch_method`, which cannot touch a `.tscn`; that limitation now says
  which entries it applies to.
- **The whole-project readers answer on a project with a baked mesh in it.**
  A Godot resource writes a packed array on one line, and an `ArrayMesh` or a
  baked `Curve3D` puts hundreds of kilobytes there. `project_analyze_impact`
  and `project_audit_assets` handed every such line to the regex engine, at a
  cost quadratic in its length, so one 400 KB line cost ten seconds in each and
  a line past a megabyte never came back inside any client timeout (#661). Both
  now check for the literal they are looking for before running a pattern, which
  is the guard the sibling collectors in the same file already had. The same
  calls answer in hundredths of a second and return the same findings. The
  profile moved one of the two sites the report named: `referencesIn` was
  already cheap, and the cost was the dead-signal member-call pattern, which is
  asked a line at a time now.
- **Those two readers are bounded, and say when they read less than the whole
  project.** They held every file in memory at once with none of the limits
  `project_search_text` applies, so on a 71 MB project one call cost 86 MB of
  working set and 10.7 seconds, where the bounded sibling reading the same
  bytes cost 10 MB (#664). The shared scan now applies the same three bounds --
  4 MiB per file, 64 MiB in total, 10000 files -- and reports what they kept
  out: the same project now costs 8 MB and answers in 1.9 seconds, and says
  `truncated: true` rather than presenting a partial read as a whole one.
  `project_rename_references` already refused a truncated scan and now names
  which of the two reasons it was. `project_audit_assets` goes through that
  scan rather than its own copy of the same loop, so a file type added to one
  can no longer be invisible to the other.
- **A duplicated branch survives the save.** `scene_duplicate_node` set the
  owner of the copy's root and nothing else. Godot leaves every duplicated
  descendant unowned and `PackedScene::pack` keeps only what the edited root
  owns, so the editor showed the children, the tool reported success,
  `editor_save_scene` reported saved, and the file held a bare copy of the root
  (#659). The copy's descendants now take their owners from the branch they
  were copied from, which keeps a runtime-only child unowned and leaves a node
  inside an instanced sub-scene owned by that instance. The harness case only
  ever duplicated a leaf, so nothing could fail; the new one duplicates a
  branch two levels deep, saves, undoes, redoes, saves again, and reads the
  file.
- **Undoing `scene_remove_from_group` puts a persistent group back
  persistent.** Deciding that needs the node's `SceneState` entry, and the two
  sides of the comparison spelled the same node differently: `SceneState` says
  `./Subject` where `Node.get_path_to` says `Subject`. Nothing ever matched,
  every membership read as transient, and the undo restored it transient, so
  the next save dropped a group the editor was still showing (#660). Both
  comparisons now go through one helper. The harness covered only the transient
  direction, which passed whatever the code did.
- **`audio_configure_bus` says where the change ends up.** It writes the
  running engine's `AudioServer` and reports `undo_redo_registered: false`
  beside a `revert_with` block, which is the shape of "this lives in memory".
  In an attached editor it is not: the editor's own bus-layout autosave picks
  the change up and writes `res://default_bus_layout.tres` a moment later with
  no call from anybody, so a tracked project file appears in the working tree
  carrying whatever value was tried last (#622). The result now carries
  `persisted_by_editor`, `layout_path` and a `limitation` saying which case it
  is; a game session gets the opposite sentence, because nothing writes it
  down there.
- **`script_check_syntax` and `shader_check_compile` name the engine that
  answered.** Both spawn a Godot to answer "will the engine accept this?", and
  `resolveGodotExecutable` picks newest-first from `GODOT_BIN`, `GODOT_PATH` and
  a fixed list, so on a machine with 4.5, 4.6 and 4.7 installed both answered
  about a 4.5 project using 4.7. Neither said so, and `script_check_syntax` had
  no `raw_output` for it to hide in (#617). Both now carry `engine_version`,
  `engine_executable`, `attached_engine_version` and
  `matches_attached_engine`, the same shape #466 gave `script_reflect_class`.
- **The fix cycle spawns the interpreter it is running, not a command called
  `python`.** There is no such command on macOS -- the Xcode Command Line Tools
  provide `python3` and Apple removed the Python 2 shim in 12.3 -- nor in a
  plain `ubuntu:24.04` image, so two gates in `tools/field-trial/cycle.py` died
  on any machine GitHub did not prepare (#635). Both GitHub runners ship a
  `python` shim, which is why CI could not see it. `sys.executable` is correct
  by construction and needs no PATH lookup, which is what two existing test
  modules already do.
- **Both Python floors are written down.** The C++ build needs 3.9, which is
  what a stock macOS ships; the Python test suite needs 3.10, because
  `requirements-dev.txt` pins `jsonschema==4.26.0` and that release declares
  `requires-python >= 3.10`. Neither number appeared anywhere, so a 3.9 machine
  met the second one as a pip resolution error listing every jsonschema that
  ever existed and naming no Python version (#634). The pin stays exact,
  because CI reads it from that file and checks what it installed against it.
  The floor is now in `requirements-dev.txt`, in the developer guide, and in a
  configure-time note.
- **A reconfigure with no source change recompiles one file, not 57 targets.**
  The build identity carries a wall-clock stamp, so `version.hpp` was different
  on every configure -- and it sits behind `mcp_protocol.hpp`, which 28 files
  include, so a "nothing changed" reconfigure cost several minutes of compiling
  (#633). `kBuildId` moves into a generated translation unit with an `extern`
  declaration in the header, which keeps the identity exactly as it was and
  leaves one object file to rebuild. CI never saw this; every incremental
  workflow did.
- **The checkpoint file-count boundary is tested with eleven files, not ten
  thousand.** One test created, hashed, copied and then deleted ten thousand
  files to reach the limit: 80 to 90 seconds on NTFS, which dominated the whole
  native suite and left a temp directory full of them behind on an unclean exit
  (#627). The limit is a value with a test seam now, production keeps its
  10000, and a second test asserts that it does. The suite's Checkpoints block
  went from 101 seconds to 5.
- **The tilemap and gridmap rules a schema cannot state answer with a
  sentence.** Uniqueness across array entries and a constraint between two
  fields of one entry are hand-written, run either side of the schema check, and
  are the rules a caller is most likely to trip because no schema-aware client
  can pre-check them. All three answered with a raw identifier naming neither
  the offending cell, nor the rule, nor what to change, and the third did not
  even say which of several rules it stood for (#619). They now name the entry
  indices, the coordinate or position, and the values.
- **A live-only tool with no engine attached says what to do about it.** All
  fourteen answered "No atomic runtime route is available for live dispatch",
  which names neither Godot, nor the editor, nor anything a caller can act on,
  in the single most common state a caller meets (#615). The refusal now names
  the tool, says it needs a live Godot engine, and says how to get one, with
  `blocked_on`, `needs_live_engine`, `offline_fallback`, `discover_with` and
  `attach_with` beside it, plus `offline_alternative` where a sibling answers
  offline. `audio_configure_bus` had that sentence written for this case and it
  sat behind the route check where it could never ship; its content is in the
  shared refusal now.
- **A script the engine cannot read is not a script with nothing in it.**
  `script_get_symbols` returned empty lists for a `.gd` saved as UTF-16 or in a
  single-byte encoding, with `truncated: false` confirming nothing was dropped,
  which is byte for byte the answer a correct empty script gets; an agent asking
  where a method lives was told there is no such method (#614). It now refuses
  with `415` and `binary_or_invalid_utf8`, the classification
  `project_search_text` already reported for the same files.
- **`script_check_syntax` fails a script Godot refuses to load.** It reported
  `has_errors: false` for the same files. The engine does refuse them, but its
  refusal points at engine source rather than at a `res://` line, so the parser
  dropped the message and the answer came back clean (#613). The encoding is
  checked before Godot is spawned, and an engine load failure with no `res://`
  line is now kept as a diagnostic rather than dropped.
- **Emitting a signal nothing is connected to is a no-op, and says so.**
  Godot keeps a signal in its object's signal map only once it has a
  connection, so a built-in signal with no listeners returns `ERR_UNAVAILABLE`
  and the bridge answered "The engine refused the emit" -- in exactly the state
  a caller is most likely to be in, driving a signal by hand before the
  connection exists (#624). It now reports `emitted: false`,
  `connection_count: 0` and a sentence saying nothing is listening. A delivered
  emit reports the count it reached.
- **An engine that answered is no longer reported as a session that is gone.**
  The guard that keeps an engine's own refusal from reading as a routing
  problem was scoped to 4xx, so a 5xx came back as `503` with
  `data.code: "not_connected"` on a session whose very next call succeeded, and
  an agent branching on that field detached and re-attached over a per-call
  refusal (#625). An engine failure now answers `502` with
  `code: "engine_refused"` and the engine's status under `upstream_code`;
  `503` and `not_connected` stay for a route that could not deliver. A game
  stopped on request still answers as the requested exit.
- **`signal_emit`'s dry run runs the argument-value rules the confirmed call
  runs.** #399 moved the argument *names* onto the preview path and the values
  stayed behind, so a preview signed nesting, array and object sizes the
  confirmed call then refused: two round trips and a spent token to learn
  something the first call could have said (#616). The rules run before the
  preview now, and the refusal names the entry, the rule and the limit instead
  of answering `unsupported_signal_emit_argument`.
- **A preview says what its probe actually read.** The probe reads a node's
  `name` to confirm the node is there, and that was reported as the before
  state of a `planned_mutation` of `name`. `signal_emit` changes no property of
  its target, and neither do the eight other tools that reach the same branch,
  so a caller diffing `before` against the result saw `name` unchanged and
  concluded the call had not happened (#621). Those previews now report
  `changes[].kind: "resolved_target"` with `before.resolved: true`.
- **A float shader uniform set to a whole number survives the save.**
  `ShaderMaterial.set_shader_parameter` stores the Variant it is handed without
  coercing it, and the bridge built an `int` Variant for a JSON integer, so a
  `float` uniform set to `0` or `1` held an int the material could not
  serialise and Godot dropped the parameter at save time (#612). The writing
  tool, the reading tool and the save all reported success on the way there.
  The Variant is now built for the type the shader declares.
- **A colour or vector write that landed says it landed.** `applied` compared
  composites exactly, so a Color channel that is not representable in the
  float32 it is made of -- `0.1`, which reads back as `0.10000000149011612` --
  and a colour sent as `{r,g,b}` with the alpha left off, which the tool's own
  schema documents, both reported a write that had worked as one that had not
  (#618). Members are compared one by one now, against the value as it was sent
  to the engine, with a tolerance sized to that float32 round trip.
- **`shader_list_uniforms` reports each uniform's declared hint, and
  `shader_set_uniform` honours it.** The engine has the hint in the same
  `PropertyInfo` the name and type are read from, and neither tool looked at
  it, so a caller could not learn a declared range short of reading the shader
  source and a value 2.5x a `hint_range` maximum persisted without comment
  (#620). The list now carries `hint`, and a value outside a declared range is
  refused by name, the way `audio_configure_bus` refuses a volume outside the
  bus editor's own range. `or_greater` and `or_less` are honoured.
- **`shader_set_uniform` says the change is not on disk yet.** It writes to the
  same place as `scene_set_property`, `tilemap_set_cells` and
  `gridmap_set_cells` and is discarded the same way if the editor closes
  without saving, and it was the only one of the four that carried neither
  `scene_saved` nor the `limitation` sentence, which read as "this one did not
  need saving" (#623).
- **The schema gate refuses arguments that are not an object.** The one place
  that reads a tool's published schema returned "no complaint" for an array, a
  number or a string, so the gate reported that a call had satisfied a contract
  it cannot satisfy (#629). It now names the type it was given. No handler was
  ever reached with one: `tools/call` and `MutationSafety::evaluate` both refuse
  a non-object first.
- **`viewport_diff_capture` refuses a malformed capture id with the error
  envelope.** Seven argument checks in the handler duplicated bounds the
  published schema already states, and `dispatchTool` checks that schema before
  any handler runs, so six of them could not be reached (#628). The seventh
  could: the schema states the capture id's `pattern` and the checker does not
  model `pattern`, so a 32-character id in the wrong case reached the handler
  and came back as a bare sentence with no code to branch on. The duplicates are
  gone and the surviving check answers the way #424 made its two neighbours
  answer.
- **The server starts under a project root with an accent in it.** On Windows
  the narrow `main` the CRT synthesises converts the command line and the
  environment through the system ANSI codepage, so `--project` and
  `DIDI_PROJECT_ROOT` arrived mangled. A root holding a character that codepage
  can represent, such as the one in `C:/Users/Jose`, killed the process with
  `0xC0000409` and no output at all; Cyrillic, CJK and emoji roots became
  question marks and were refused as an inaccessible directory (#611). The
  Windows entry point is now `wmain`, so both arrive as UTF-16 and are encoded
  to UTF-8 here, and the refusal written for an undecodable root is reachable
  instead of escaping `main`.
- **A game stopped on request is reported as the exit it is.** After
  `runtime_stop`, the first call to reach the game answered a `504` with
  `retryable: true` and a sentence about a pipe, the calls after it a `503`
  about a missing route, and nothing said the exit was requested by this
  caller with this code, so the sensible next move was to retry a process
  that would never answer (#595). The server now remembers the stop it asked
  for: the first call to reach the stopped game answers `incident:
  "game_stopped"` with `exit_code`, `requested_by: "runtime_stop"` and
  `retryable: false`, later calls carry the same fact under
  `route_obstruction`, and `didi_control_room` names it until another session
  is attached.
- **What discovery advertises is what a call gets, for hit-testing a game
  and for managed recovery.** `ui_hit_test` promised a game default in its
  own `root_path` description and was refused for a game session, while
  `ui_list_controls`, the other half of the same read, answered; it now
  answers for the running game's root the way the listing does, so what a
  game lists can be hit-tested before a click is injected at it (#592). The
  four managed-recovery tools advertised `currentMode: "local"` and answered
  `501 unimplemented` when the server was not started with
  `--managed-editor`, and `runtime_restore_checkpoint` issued a confirmation
  token before saying so; discovery and the control room now advertise them
  as `unavailable`, every call answers `409` with `data.code:
  "managed_mode_disabled"`, and the check runs before the confirmation gate,
  so no token is minted for a call that cannot succeed (#599).
- **Three places where the published contract and the handler disagreed.**
  `runtime_explore_scene` refused every `duration_ms` below 3000 unless
  `stuck_ms` was lowered too, because the default of one exceeded what the
  other allowed; the default is now the smaller of 3000 and the window, and a
  caller who sets both backwards is still refused (#596). `runtime_read_logs`
  and `runtime_read_output` carried `exhausted: false` on every page, a flag
  that meant the 64-bit sequence had wrapped and read as "there is more"; it
  is now `sequence_overflowed`, every page carries `has_more`, and the tools
  say to page until it is false (#598). `runtime_watch_invariants` documented
  `node.get('health')` as its example expression, which the sandbox refuses
  because a script's own variable can run a getter; the example, the probe
  descriptions and the refusal now say native properties only and show one
  that works (#593).
- **The extension no longer prints engine errors at startup and on every
  dashboard read, and its log stays out of a game's output.** On Godot 4.5
  and 4.6 every `didi_control_room` call printed `ERROR: Parameter "mb" is
  null` into the editor's log, because the bind for `get_unsaved_scenes`, a
  4.7 method, was looked up on every call, and every editor and game printed
  `non-existent interface function` at startup because a 4.7 class
  registration function was asked for on every engine (#600). The method is
  now checked once through `ClassDB.class_has_method`, and the interface
  function is asked for only from 4.7. The extension also wrote two coloured
  INFO lines per tool call into a game's own output (#601): its default level
  is now `WARN`, the `didi/native/log_level` project setting sets it for every
  process that loads the addon, `DIDI_LOG_LEVEL` still wins over both, and
  colour codes are written only when stderr is a terminal. The server's
  default stays `INFO`.
- **Injected input reaches a paused game's nodes, and a click lands where it
  is aimed.** A paused tree delivers `_input` only to nodes that process while
  paused, so a batch injected during a pause reported `completed` and was gone
  before `runtime_step` ran a frame, which made pause, press, step, look do
  nothing (#594). `runtime_inject_input` now holds such a batch and reports
  `outcome: "queued"` with `paused: true` and `delivery:
  "next_unpaused_frame"`; `runtime_set_paused` and `runtime_step` hand the held
  events to Input on the way to running, so they land in the first frame that
  processes, and both report `released_input_events`. The `mouse_button` shape
  had no position, so every injected click landed at the viewport origin and a
  caller who had found a control's rect could not click it (#597); it now takes
  `position` and `global_position`, and a `mouse_motion` shape with `position`
  and `relative` moves the pointer first. `target_context` is a constant whose
  description offered the editor as a choice the schema refused; the description
  now says it is always `game_input` (#602).
- **Scene edits the file cannot hold are refused before they happen, and the
  hierarchy says who owns what.** Godot's packer keeps only the nodes the
  edited scene owns, so a property, group or script change on a node inside an
  instanced sub-scene was applied to the live tree, reported as applied, and
  dropped by the save, which reported `saved` (#588). A `.tscn` has no marker
  for a removed inherited node, so removing, moving or duplicating one went the
  same way, and so did removing a node inside an instance (#589). Instancing
  the edited scene into itself, directly or through a scene that depends on
  it, built a recursive tree the editor then refused to save in a dialog the
  caller never sees (#590). All of these now answer `409` before anything is
  touched: `node_not_owned` names the owning scene and the instance root,
  `node_inherited` names the base scene, and `cyclic_instance` names the chain
  of scene files, which are the checks the editor makes in
  `SceneTreeDock` before the same operations. The property gate also covers
  `tilemap_set_cells`, `gridmap_set_cells` and `viewport_set_camera_transform`,
  which write node state the packer drops the same way. Undoing a
  `scene_remove_node` now restores the ownership of the branch it puts back:
  Godot clears the owner of a node whose owner is no longer an ancestor when
  the branch leaves the tree, so the restored node was one the next save would
  have dropped, which the new gate was the first thing to notice. The dry-run preview refuses the
  same calls with the same codes, because the probe now names the mutation it
  is previewing and the bridge runs that call's own preconditions before the
  read; the script base-type check runs on the preview the same way, so
  `script_attach_to_node` no longer previews an attach the real call refuses
  (#603). `scene_get_hierarchy` carries `owned_by_scene` on every node,
  `instance_of` and `editable_instance` on instance roots, `inherited` on
  nodes from the base scene and `inherits` at the top level, live and offline;
  the offline parse's `instance` field is now `instance_of`, the same name on
  both routes (#591). The `saved` answer from `editor_save_scene` is documented
  as Godot's acceptance of the request, since `EditorInterface.save_scene`
  returns OK for any open scene with a path.
- **A bounded reader publishes `max_response_bytes`, and both new ones use the
  same figure.** `script_get_symbols` and the dry-run preview each grew a count
  bound without one, which left the response size resting on a per-item
  declaration: a caller raising `max_symbols` could talk past it, and a tool
  taking a list of bounded strings could sum past it. Both now carry the 8 MiB
  `scene_get_hierarchy` publishes, reported whether or not it is reached, with
  `truncated` beside it. Symbols are charged per declaration the way hierarchy
  charges per node, so the one that crosses the limit is the one that stops. A
  preview replaces values over 4 KiB with the byte count that stood there, and
  the argument block whole if that is not enough; an elision always says it is
  one, because the preview is the artifact a person approves. The confirmation
  token is bound to the real arguments rather than the displayed copy, so
  nothing elided for reading can fail a later confirm (#574, #575).
- **The social preview banner named the wrong surface size.** The count in
  its text had not moved since the surface was 98 names. `docs/brand/build.py`
  now says 116 tools, and `social-preview.svg` and `.png` are regenerated
  from it.
- **Every required string parameter carries a declared length.** `minLength`
  landed in #553; the other end did not, so 50 required strings had no
  `maxLength` at all while 62 of 64 numbers carried a `minimum`. The split was
  not along any line a caller could guess: a blackboard key was capped at 512
  bytes and a Godot node path was not capped at all. Sized by what the value is
  rather than by one global number: an identifier is 256, a path is the 1024
  `search_path` already used, and a body (`source_text`, `new_definition`) is
  1,048,576, which is the one place a large number is right and the point is
  that it is declared. A schema stating its own narrower bound keeps it.
  Stamped where `minLength` is stamped, so a parameter added later is bounded
  without anyone remembering to say so, and a contract test keeps the count of
  unbounded required strings at zero (#573).
- **A refusal names both halves of the mistake.** The missing-argument check
  and the unknown-argument check ran in sequence and each returned on its first
  find, so the message that lists what a tool accepts only ever appeared when
  every required argument was already correct. Getting a *required* name wrong,
  which is the likelier mistake, reported a name the caller had not used as
  missing and said nothing about the ones they had: `{"node_path",
  "property"}` answered `Missing required argument 'target_node'.` and left
  them to guess whether `node_path` had been ignored or accepted. One message
  now carries the missing names, the unknown names and the whole parameter set
  (#577).
- **`case_sensitive` describes what it does.** Both `project_search_text` and
  `project_search_symbols` published `"default": true` beside "Off by
  default", and the handler is case-sensitive, so the declared default was
  right and the description was the opposite of the behaviour. A caller who
  read the prose and searched for `Player` in a codebase that spells it
  `player` got zero matches and no reason to doubt the tool. A contract test
  now compares every boolean parameter's prose against its declared default,
  which is a class of defect neither the description tests nor the schema tests
  could see: one counts descriptions and the other reads keys, and nothing
  compared them (#576).
- **`script_get_symbols` publishes a limit and says what it left out.** It was
  the one reader on the surface with neither: a 10 MB script with 120,000
  declarations came back as 15 MB of JSON, `isError: false`, with no
  `truncated`, `max_*`, `omitted_*` or count field anywhere, so a caller could
  not tell a complete answer from a clipped one and could not ask for less.
  `max_symbols` defaults to 2000, counted across all six kinds because the
  response is one thing, and every result carries `symbol_count_total`,
  `returned_count` and `truncated`. The scan still reads the whole file, so the
  total is the real total. The same disclosure `scene_get_hierarchy`,
  `runtime_get_tree`, `project_search_text`, `ui_list_controls` and
  `scene_get_selection` already publish (#575).
- **A confirmation token is bound to what the preview saw, not only to the
  call.** The preview read the target, computed `before`, signed it and never
  looked again, so a file rewritten between the preview and the confirm was
  overwritten with a change approved against bytes that were no longer there,
  and the answer was `status: "success"` either way. The confirm reads the
  target again and compares everything the probe reported, including a digest
  of a file's bytes so an edit that keeps the length is caught like any other.
  A mismatch is `409` with `data.target_changed` and nothing applied. The
  preview says up front whether that check will be possible with
  `target_checked_on_confirm`. A target that has *gone* is not a change of this
  kind: there is no other writer's work to discard, and #425's rule that a
  token minted while the target was there stays spendable once it is not is
  unchanged (#572).
- **A dry run runs the argument checks the real call runs.** The `..` node path
  rule lived in the bridge alone, so seven of nine cases previewed a mutation
  the identical arguments were then refused for: `scene_reparent_node` wrote
  `preview_kind: "target_state"` and `kind: "planned_mutation"` over a real
  `before` read off the live tree, for a call that cannot run. The rule is a
  property of the argument, so it needs no engine and no open scene, and it now
  lives in one place both binaries read. The same goes for
  `script_patch_method`'s replacement guard: a `new_definition` that declares
  the wrong kind of symbol is refused at the preview rather than at the write
  (#571).
- **A preview carries its arguments once.** `mutation_preview.arguments` and
  `mutation_preview.changes[0].target` held the same object, so every response
  was a little over twice the size of its request at every scale, with no cap
  anywhere along the way: an 8 MB argument returned 16.8 MB of JSON on one
  stdio frame with `isError: false`. `changes[].target` names what the change
  is about now, such as the path and the symbol, which is what the field was
  for; a tool with no subject of its own says so and points at the arguments
  one level up (#574).
- **`viewport_diff_capture` makes the viewport render before it compares.**
  It takes its own comparison capture and had no way to ask for the main
  screen, so it read whatever was last drawn in a viewport with no size and
  answered `bit_identical: true`, `ssim: 1.0`, `isError: false` for a frame
  where 55% of the pixels had changed. No threshold could help, because the
  image was stale. `select_main_screen` is on `viewport_diff_capture` and
  `viewport_capture_passes` now, the way it has been on
  `viewport_capture_frame`, and all three go through the same deferred path:
  select the screen, wait a frame for the layout, capture, put the previous
  screen back. A call that names no camera selects the screen its own default
  camera belongs to rather than asking for one (#568).
- **`script_patch_method` refuses a symbol the script does not declare.** It
  appended one instead and reported the same success as a replacement, so the
  typo `ready` for `_ready` left a method nobody calls beside the one the
  caller meant to edit, with `has_errors: false` because the file still
  parses. A name the script does not declare is a 404 now, naming the kind and
  the name; `create_if_missing: true` keeps the append for the callers who
  want it, and `created` in the result says which of the two happened. The dry
  run answers the same way, with `before.symbol_exists` (#569).
- **`symbol_type` publishes the six kinds it models, and refuses the rest.**
  It was a bare string with a default and no `enum`, and an unrecognised value
  did not just fall through to a looser match: it switched off the check that
  the replacement declares what it replaces. So `fucntion` for `function`
  replaced a function with a variable, deleted the body and reported success,
  while the schema said only "Defaults to function". The argument check now
  refuses anything outside `function`, `variable`, `constant`, `signal`,
  `enum` and `class` before the file is opened, and the patcher refuses it
  again behind that. `patch_script_symbols` is the same handler (#570).
- **Every required string parameter carries `minLength: 1`** unless the schema
  says otherwise, stamped where `additionalProperties` is stamped. 41 of 90
  accepted `""`, and the handlers behind them each answered differently: five
  said the argument was missing when it had been supplied, one answered with a
  bare string, and `viewport_create_test_lab` wrote a lab with no target in it
  and reported the same success as a lab with one. The argument check now
  refuses all of them in the envelope, naming the property. `script_create`'s
  `source_text` is the one exception, because an empty file is a file (#553,
  #554).
- **Four failures behind valid arguments answer with the error envelope**
  instead of prose: removing a project setting that is not there is `404
  not_found`, a value nested past 16 levels is `400 invalid_arguments` and the
  message no longer names an internal phase, an empty `class_name` is `400`,
  and `viewport_capture_passes` offline is `503 not_connected` with
  `retryable: true` like every other live-only tool (#548).
- **Writers report the path they resolved, not the argument.** `script_create`,
  `resource_create` and `script_patch_method` echoed the argument as the path
  they wrote, so `res://d1/../reported.gd` was reported through a directory
  that never existed while every reader named the file `res://reported.gd`.
  On Windows the same seam let `res://PLAYER.gd` replace `res://player.gd`
  while the preview's `before.path`, the result and the 409 conflict all named
  a file that was never on disk. All of them now carry the resolved `res://`
  path, in the on-disk case when a file is already there (#546, #551).
- **`script_patch_method` keeps the file's line endings.** The file was read in
  text mode, so on Windows a CRLF file came back LF on every line while the
  result reported a single-method change. CRLF stays CRLF, a BOM stays, a
  file with no trailing newline does not gain one, and a replacement spelled
  with CRLF joins the file in the file's convention. `patch_script_symbols` is
  the same handler (#550).
- **`project_search_symbols` reads a `.GD` script.** The collector folded the
  extension to admit the file and the symbol path then tested the suffix
  case-sensitively, so `res://Upper.GD` was collected, dropped as having no
  symbol extractor, and `.gd` was reported as an unsearchable extension for
  the whole run. Both tests fold the same way now (#549).
- **Search columns count code points.** `column` in `project_search_text` and
  `project_search_symbols` was the byte offset plus one, and nothing said so,
  which pointed past the match in any editor on a line with a non-ASCII
  character before it. It is the 1-based column in Unicode code points now,
  and the published `outputSchema` describes both `line` and `column` (#556).
- **`script_reflect_class` compares the pinned dump to the project when no
  session is selected.** It answered from the 4.7 dump and compared it to
  nothing unless an earlier call had happened to select a session, so a 4.5
  project asking first was told about 4.7 without a word. With no session the
  response now carries `project_features_version` from `config/features` and
  `api_version_matches_project_features`, and an unknown name is described as
  absent from the pinned reference rather than from Godot (#555).
- **A second `initialize` is refused.** The lifecycle makes initialization the
  first interaction, and the server accepted another at any point, from any
  `clientInfo`, answering it as though it were the first while resetting
  nothing, so the confirmation tokens minted for the previous client stayed
  spendable by whoever sent it. It now answers `-32600` with
  `error.data.initialized: true`, and the session goes on serving the client
  that opened it (#552).
- **Live scene mutations say the change is unsaved.** Every committed
  edited-scene mutation, from `scene_set_property` to `tilemap_set_cells`,
  reported plain success while the change lived only in the editor's open
  scene and its undo history, and `didi_control_room` read `Project: Ready`
  over it. Each reply now carries `scene_saved: false` and a `limitation`
  naming `editor_save_scene`. The dashboard asks an attached editor which
  open scenes hold unsaved changes: on Godot 4.7 and later the Project light
  goes amber and names them; on 4.5 and 4.6 the engine cannot say, and the
  `Unsaved scenes` fact reports that rather than reading as clean (#557).
- **`viewport_create_test_lab` checks the target first and writes the lab
  where the audit can see it.** The handler created `addons/didi` before it
  resolved the target, so a refused target left the project with a folder it
  did not have, and it wrote the lab inside the addon's own folder, which
  `project_audit_assets` excludes from orphan checks. The target is now
  resolved before anything is touched, the lab is written to
  `res://didi_test_lab.tscn`, and the result reports `target_resource_path`
  resolved. A client that read the old path from the result sees the new one
  there (#564).
- **`viewport_create_test_lab` says whether it instanced the target.** The
  description said it did not instance the target resource while every
  PackedScene target was instanced as `TargetInstance`. The description, the
  docs and a new `target_instanced` field now say which of the two scenes was
  written (#565).
- **`project_set_setting`'s descriptions say where the `create` guard runs.**
  The name check needs an attached editor; offline the name is written whether
  `create` is set or not, and the result's `limitation` already said so. The
  tool and parameter descriptions promised a refusal the offline path never
  gave (#547).

## [2.0.0] - 2026-09-13

A major because the surface changed, not because the project grew up. See
[Stability](#stability) for what the number does and does not promise.

Almost all of this release is correctness work on answers that were already
wrong. The reason it is a major rather than a patch is that a client written
against 1.8.0 branched on those wrong answers, and several of them are now
different. Read the list below before upgrading.

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

- `scene_call_method` runs a method the target node's own script declares, and
  returns what it returned (#389). Didi could read a project completely and
  could not press its main verb: `eval_gdscript` is read-only by contract, an
  outbound signal announces work that never happened, and the editor dock a
  human would click is not reachable. The allowlist is the node's own script,
  so every engine method is out of reach by construction rather than by a
  denylist, and leading-underscore names are refused. A coroutine is awaited
  and the result carries the value its `completed` signal delivered, because
  the call itself hands back a `GDScriptFunctionState` and answering with that
  would report work that has not happened. The script must be a `@tool` script,
  which is refused explicitly rather than returning the nothing Godot hands
  back. Always confirmed. Recorded in
  [Surface Amendments](docs/SURFACE_AMENDMENTS.md); the canonical surface is now
  116 names.

- `viewport_capture_frame` can select the editor main screen it needs (#381). An
  editor viewport has no size unless its main screen is the one showing, so the
  capture refused with a message telling the caller to switch to it in the
  editor, which is the one thing an unattended agent could not do and no tool
  could do for it. `select_main_screen: true` now selects the screen the
  `camera_identifier` belongs to, waits the frame the control layout needs,
  captures, and puts the previous screen back. The result reports
  `main_screen_selected`, `main_screen_restored` and `previous_main_screen`; a
  main screen an addon contributes cannot be named back, and that is stated
  rather than implied. Default behaviour is unchanged.

- `resource_create` can express a reference to another resource, so the
  composite resources are authorable at last (#380). A property value of
  `{"type": "ExtResource", "path": "res://..."}` becomes an `[ext_resource]`
  entry carrying the type and uid read from the project index, and a new
  `sub_resources` argument declares `[sub_resource]` blocks that
  `{"type": "SubResource", "id": "..."}` names. Sub-resource properties follow
  the same rules as top-level ones, so there is no second dialect, and
  `load_steps` is computed rather than guessed. A reference to a file that is
  not in the project, or to a sub-resource id not declared above the point that
  names it, is refused: Godot resolves those to null rather than failing, which
  is a resource reported as written and quietly wrong. TileSet,
  AnimationLibrary, SpriteFrames, Theme and ShaderMaterial no longer have to be
  written by hand outside the tool surface.


- Nine more tools publish an `outputSchema`, and the rule for which do is
  written down (#509). `blackboard_list_keys`, `blackboard_read`,
  `blackboard_task_list`, `didi_control_room`, `project_list_export_presets`,
  `resource_inspect`, `script_get_symbols` and `script_reflect_class` join the
  eleven that already did. The rule is that a tool publishes a schema when
  something checks it against a real answer, so a tool with no schema is
  unspecified rather than special. It was already enforced and never stated: the
  contract test fails a declared schema that nothing exercises. A live-only tool
  now fails the suite if it acquires one, because nothing offline could check
  it, and writing schemas for those 67 would be 67 unverified claims of exactly
  the kind that made `scene_get_hierarchy` wrong.

### Fixed

- `initialize` reads the `protocolVersion` it is sent (#531). Every value got
  the same answer: a revision this server serves, one it does not, an empty
  string, a bare JSON number and a missing key all came back `2024-11-05`, so a
  malformed `InitializeRequest` was accepted in silence and a client could not
  tell a version it had been granted from one it had been refused. #312
  validated the `tools/call` envelope; `initialize` sits in front of it and was
  not covered. The field is a required string, so a missing key or a non-string
  is refused with `-32602` carrying `supported` and `requested`, the shape the
  2024-11-05 lifecycle's own example uses. A revision this server serves is
  answered with itself, and any other string with `2024-11-05`, which is what
  that specification requires of both cases.

- `resources/subscribe` refuses with a reason that is true (#532). Subscribing
  to any of the three `godot://` resources was refused because "nothing else
  changes without a tool call from this client", which is false of all three:
  `godot://runtime/logs` is described in its own listing entry as incremental
  engine-side records, and editor state and the project tree change whenever the
  user edits. The message stated as a fact the very thing that makes a
  subscription worth having. The refusal stands, because accepting one would
  promise updates that never arrive, but it now says what is actually so: Didi
  does not yet publish change notifications for engine-side resources. And
  because `initialize` advertises `resources.subscribe` for the server as a
  whole, every `resources/list` entry now carries `_meta.didi.subscribable`, so
  a host can tell before it asks rather than by asking and being refused.

- Resources with no live path say what they are, not what they fell back from
  (#533). #419 removed `offline_fallback` from the tools that never had a live
  path; the `_meta.didi` block on resources and their own answers were not part
  of that sweep, which the code said outright was still owed. A board is a file
  in `.didi/blackboard/` and the project tree is a filesystem index, so
  `offline_fallback` named a fallback from a route neither ever had, and a host
  that dims or warns on that flag -- which is what the flag is for -- dimmed the
  resources that are always fully available, with an editor attached and
  healthy. `blackboard://<board>/state`, `blackboard://<board>/tasks` and
  `godot://project/tree` report `local` now, and `ui://didi/control-room`
  reports `local_status`, matching the tool that serves the same dashboard.
  `godot://editor/state` and `godot://runtime/logs` keep `offline_fallback`,
  because those really do fall back. The advertisement and the answer come from
  the registration, so they cannot drift apart.
- A second MCP server on a held editor is told so (#527). Only one server can
  hold the editor bridge, which is a reasonable design; what a second server was
  *told* was not. Its auto-attach was refused with `423`, the reason was dropped
  on the floor, and every call afterwards answered the `503 not_connected` that a
  server with no Godot running at all receives, byte for byte. The blackboard
  exists because more than one agent is expected to work on a project at once, so
  this is the multi-agent path rather than an edge case, and the second agent's
  sensible next move from "nothing is running" is offline file edits over the
  first agent's live work. The refusal is remembered now and reported as
  `bridge_held`, with `bridge_held_by_another_client: true` in the error's `data`
  and a control-room fact saying the editor is up and owned.

- An engine crash survives the call that discovered it (#536). The first live
  call after the editor died got a complete account: the incident, the cause, and
  what to do. Every call after that reverted to the generic `503`, offline
  answers resumed with nothing saying why, and `didi_control_room` -- the one
  tool whose job is to say what state the bridge is in -- reported `Route:
  detached`, which is also what it reports when no editor was ever started. The
  incident is kept until a route opens again: the control room names it and when
  it happened, and an `offline_fallback` answer carries `offline_reason` saying
  the fallback follows a crashed engine rather than a session that never
  attached. `runtime_detach_session` clears it, because letting go deliberately
  is not an obstruction.

- `runtime_detach_session` is idempotent (#537). Detaching with nothing attached
  answered `503 not_connected` with `retryable: true`, advising a retry that
  would never attach anything. Detach is a cleanup, and the session is torn down
  implicitly when the editor goes, so a caller doing the tidy-up it is told to do
  got an error for it. It answers `detached: false` now, the shape
  `resources/unsubscribe` already uses two layers down for the same question, and
  `detached: true` when it is the call that released something. Answering with
  nothing attached is also what makes its success payload producible offline, so
  it publishes an `outputSchema` now.


- `blackboard_task_update` says what it takes for `progress` (#528). The
  published description read "0 to 1" while the handler required an integer
  percentage, so the documented range was not even representable and `0.5` was
  refused as "must be an integer, not a number". A completed task has always
  reported `progress: 100`, which is the real scale. The description says that
  now, and the schema already did.

- `blackboard_task_claim` answers a conflict like its siblings (#529). Three
  tools on one board met the same state and answered in two shapes: a claim of a
  task somebody else holds came back with `isError` false, `claimed: false` and
  an English sentence, so a caller branching on `isError` read a lost race as a
  win. Contention is what the board is for, so this was the most travelled path
  on it. Naming a `task_id` that cannot be claimed now answers `404` when the
  task does not exist and `409` when its state is what stands in the way, with
  `data.reason_code` saying which: `already_leased`, `blocked_by_dependency`,
  `tag_mismatch` or `not_pending`. Asking for whatever is ready and being told
  nothing is stays a success, and carries a `reason_code` of `no_tasks`,
  `all_leased`, `all_blocked` or `no_ready_task`, so the three cases can be told
  apart without reading prose.

- Completing an already-completed task is `409 conflict` (#530). It answered
  `400 invalid_arguments`, which tells an agent to fix its arguments when there
  is nothing to fix: the task exists, the lease was held, the id is well formed,
  and the work is done. A retried completion after a dropped response is the
  ordinary way to reach this state. `blackboard_task_update` answers the same
  state the same way.


- A path holding a NUL is refused rather than written somewhere else (#525).
  `script_create` and `resource_create` checked the extension against the string
  they were handed, and a NUL truncates that string at the filesystem boundary,
  so `res://n1\0x.gd` passed the `.gd` check and thirteen bytes landed in a file
  called `n1` with no extension. The call reported `created_offline` and echoed
  back the path it had not written to. The shared write resolver now refuses any
  control character in a path, by the same rule `blackboard_write` already
  applies to a board key, so every writer in the server agrees about what a path
  may hold.

- `script_create` answers a bad path with a code (#526). Four failures behind
  well-formed arguments came back as a bare JSON string with nothing to branch
  on: a name too long for the filesystem, a `user://` target, a path not ending
  in `.gd`, and a directory component too long. `scene_create` and
  `resource_create` already returned the error envelope for the same shapes.
  All four now carry a code, `retryable` and the tool name. A failed write
  reports the reason the filesystem gave and the status that reason deserves,
  so a path the filesystem will not take is a 400 rather than a 500 that says
  the server broke. `resource_create` picked up the same envelope on the three
  answers it still gave in prose.

- `res://nested/../ok2.gd` is accepted, because it lands inside the project
  (#534). Containment was decided by looking for `..` in the string, which
  refused a normalised path inside the root while accepting `res://./ok.gd`
  through the same root, and the resolve-and-compare check behind it never ran.
  Composing a path from a directory and a relative name is the ordinary way to
  build one. The three copies of the substring rule are gone and the resolve is
  the check; what actually lands outside the root is still refused, with the
  message that already said so.

- A Godot `Error` reaches the caller with its name (#535). `scene_create` gave a
  300-character filename to `ResourceSaver` and answered `500 internal_error`
  with "failed with Error 19": the server had not broken, the argument was bad,
  and the number had no name attached. The engine's file-and-path errors now
  answer `400 invalid_arguments` and say what to do about it, and every place
  the bridge printed a raw enum value now prints `ERR_CANT_OPEN (19)`. The table
  is Godot's own `Error` enum from `extension_api.json`.


- `runtime_detach_session` reports `server_build_id` again. Naming the echoed
  descriptor `detached_session` left the bridge-build check looking for a
  `session` key that is no longer there, so the one answer that reports on a
  bridge the caller has just stopped talking to silently lost the field. It
  reads either name now.

- `godot://editor/state` names a scene root the scene tools accept (#502). It is
  the resource a client reads to find out which scene is being edited, and it
  answered with `Node.get_path()`: 364 characters of the editor's own viewport
  chain, down through `@EditorNode@`, a `@SubViewport@` and the rest of where
  Godot parents an edited scene. Every `scene_*` tool refuses that, so a caller
  that read `active_scene_root` and passed it on got a 404 blaming the node.
  `active_scene_root` is now built by the same function every scene answer
  builds its paths with, so the resource and the tools describe the same tree in
  the same vocabulary.

- Every blackboard board is served as `application/json` (#513). Only
  `blackboard://default/*` is a registered resource, so the mime type came from
  the registry and fell through to `text/plain` for every other board. The same
  JSON document was labelled two ways, and a client branching on mime parsed one
  board and rendered the next as a wall of text. The type now comes from the
  scheme and kind the read handler has already parsed; `text/plain` remains the
  answer for a scheme this server does not serve.

- A blackboard board says whether it exists, and the parameterised shape is
  discoverable (#514). A board nobody had written answered exactly like a board
  that exists and is empty, so an agent could not tell "empty, go ahead" from
  "you have the name wrong and are about to start a second, private board nobody
  is reading". Both payloads now carry `exists`, and reading a board still never
  creates one. `resources/templates/list` is implemented and publishes
  `blackboard://{board}/state` and `blackboard://{board}/tasks`, so a board other
  than `default` can be found by a client that was never told its name.

- A malformed blackboard URI names the part that was wrong (#515). Three
  different shapes came back with the kind error, which named the one segment
  that was fine in two of them: a query string on a correct kind, and a
  traversal in the board name, were both told to fix a kind that was already
  `state`. Each part is now checked in the order it appears and each names
  itself. The refusals themselves are unchanged.

- `prompts/get` refuses an argument the prompt does not declare (#511). #397 and
  #418 closed unknown arguments on `tools/call`; `prompts/get` kept the old
  behaviour on a different method, accepting the argument silently and dropping
  it, so a caller who misremembered a name was handed a prompt rendered from
  defaults and no signal that what they passed went nowhere. The refusal names
  the property and lists what the prompt accepts, and `data` carries `argument`,
  `prompt` and `accepted`.

- A prompt has one description (#512). Each handler wrote a second one, so
  `prompts/list` and `prompts/get` described the same prompt differently and a
  host that listed prompts and then fetched one showed a person two sentences
  for the same thing. `prompts/list` is served with an hour of cacheability, so
  the first one stayed on screen. The description now comes from the
  registration, which is the wording that says which tool families the workflow
  is built from; the handler's pinned a Godot version that is the floor rather
  than the target.

- Every published `inputSchema` carries the `additionalProperties: false` the
  server enforces (#508). #418 closed arguments by default and the schemas did
  not follow, so 73 of 126 accepted anything by JSON Schema while the server
  refused the same call. A client validating locally before sending passed the
  call and then lost a round trip to a 400. The flag is now stamped from the
  validator's own predicate, so the published schema and the check cannot
  disagree; a schema that deliberately opens its arguments still says so.

- `scene_get_hierarchy` declares the fields it returns (#510). Its
  `outputSchema` named `file_path` and three others and stayed silent about
  everything else, including `node_count` and `omitted_fields`, which are the
  two a caller has to read to know whether the tree it got back is complete.
  `file_path` was not a stale rename: the offline path parses a `.tscn` and
  names the file it read, while a live answer carries `scene_file_path` from the
  edited scene's own identity. Both are declared now, and neither is required,
  because which one arrives depends on `source`. `project_search_text` and
  `project_search_symbols` were missing `unsearchable_files` and
  `unsearchable_extensions` the same way. The envelope the registry and the live
  bridge stamp on the way out, `execution_mode`, `is_live_engine`, `session` and
  `session_kind`, is declared from the place that stamps it.

- `tools/list` says the mode a tool actually answers with (#503). #419 gave the
  answers an honest name for work that was never engine work, and never reached
  the discovery entry, which is the copy a host reads before it ever makes a
  call. Eleven tools advertised `offline_fallback` and answered `local`,
  `local_status` or `local_session_management`, so the entry said a tool was
  running in fallback when there was nothing to fall back from. The name a tool
  reports is now declared once, beside its execution modes, and both the entry
  and the answer are taken from it. A tool with a live path keeps
  `offline_fallback`, because for it the word is true.

- `project_get_uid_map` and `project_audit_assets` stop calling an authoritative
  answer a fallback (#504). Both have a live path and take it when there is live
  work: the uid map when `resolve` names something for `ResourceUID` to answer,
  the audit when the scan found references an engine can verify. With nothing
  for the engine to do they still reported `offline_fallback`, which tells a
  caller to attach an editor and ask again, including when one was already
  attached and could have added nothing. That case now reports `local`.
  `uid_map_source` and `reference_verification.mode` already said why.

- Tool annotations are decided per tool instead of being four names for one bit
  (#505, #507). `readOnlyHint`, `destructiveHint` and `idempotentHint` were all
  derived from the mutation classification, so across all 126 tools the four
  hints took exactly four shapes and the last two carried nothing a client could
  act on. `runtime_attach_session` and `runtime_detach_session` sat in the
  read-only bucket while picking and severing the attachment every later live
  call routes through, which is the one thing a host auto-approving read-only
  tools must not be told it can do unasked. Both are now `readOnlyHint: false`.
  `destructiveHint` is false for the writers that can only add, and stays true
  for anything taking an `overwrite` flag. `idempotentHint` is true for the
  writers that land in the same state when called twice, so a client can retry a
  call that timed out.

- `runtime_detach_session` says what it did (#506). A successful detach answered
  with the full descriptor of the session it had just disconnected, in the same
  `session` field a connected answer uses, and the only thing separating the two
  payloads was a missing `connected` key. The descriptor is now
  `detached_session` and the answer carries `connected: false`.

- The expression sandbox names the read that works (#488). A refused object read
  stated the rule and not the way through, so the natural next call after being
  refused was another refusal. `self` was worse: it parsed, reached Godot, and
  came back as "self can't be used because instance is null", which reads like a
  fault in the caller's expression rather than a fact about the sandbox. Both
  refusals now name `node.get("position")`, which is the supported read and has
  been all along, and `self` is refused by the sandbox rather than by the
  engine. `eval_gdscript.context_node` says in its own description that it is
  what `node` is bound to.

- `tools/list` says which names are legacy (#493). Ten of the 126 registrations
  are legacy names for a tool that is also listed under its own name, with an
  identical schema, an identical description and identical `_meta`, so nothing
  an MCP client reads said they were duplicates. Which of the two an agent
  picked was a coin flip, error data named a `canonical_tool` the caller had
  never heard of, and any inventory of the surface double-counted eight
  capabilities. `didi_control_room` has reported `legacy` for all ten the whole
  time; this carries the same fact one layer further out. `_meta.didi.legacy` is
  now on every tool, true or false, because "this is not an alias" is a fact a
  client should be able to read rather than infer from a missing key, and the
  eight aliases that resolve to a differently named tool also carry
  `_meta.didi.canonical` and name it in a closing sentence of their description.

- `project_apply_changes` stops issuing a token for a call it cannot apply
  (#491). The tool needs an isolated copy of the project, which needs a git work
  tree that holds it. `project_verify_changes` checks that before doing
  anything and refuses with a clear `409`. The apply tool's preview has no
  target to read, and it did not check the precondition either, so it bound the
  arguments to a confirmation token and spending that token returned the same
  `409` the sibling had returned before any of it started: two calls and a token
  to learn what the first could have said. The preview runs the same check now.
  It still reports `target_read: false`, because the files the call would
  overwrite have not been opened, and that is the honest half of what the old
  preview said.

- A `oneOf` branch behind a `$ref` says what it needs (#489). The required
  properties for a branch were read straight off the branch object, and a
  `$ref` object carries none of its own, so `physics_raycast_query.from` and
  `nav_query_path.start_point` both rendered as "One of these is needed: no
  required properties; or no required properties", which tells a caller nothing
  at all. The reference is resolved first now, and the message reads "x, y; or
  x, y, z". A branch that spells its own `required` out beside the `$ref` keeps
  it, because that is the narrower statement of the two. Three tools on the
  surface use `$ref` under `oneOf`; a contract test pins the rendered text for
  all three, the inline `tilemap_set_cells` case included.

- The project writers check what they are about to write (#485, #490).

  `project_set_setting` refuses a name the engine does not define and then
  wrote whatever value it was handed. `application/config/name` took `42` and
  the project name became an integer; `application/run/main_scene` took
  `res://nope.tscn` and the project stopped running. Both reported
  `persisted: true`. The lookup that answers `defined_by_engine` is holding the
  engine's own value, so its type was available at exactly the point where the
  check belongs. A type mismatch is now a `409` naming `expected_type` and
  `given_type`, and the conversions the engine does anyway are still allowed: an
  integer into a float setting, a whole number into an int setting, a string
  into a `StringName` or `NodePath`, a JSON array into any of the packed arrays.
  A `res://` value is checked against the filesystem the way
  `project_set_autoload` has always checked a script.

  `project_remove_input_action` would remove an engine default such as
  `ui_accept`, leave the running editor's InputMap without it, write nothing to
  `project.godot`, and report `persisted: true`. `has_setting` answers true for
  a built-in because the engine registers the built-in map as settings, which is
  the right answer to "does this exist" and the wrong answer to "did this
  project define it". The second question is answered by `project.godot` itself
  now, and an action the file does not contain is refused with a `409` carrying
  `engine_default: true`. A removal that does go ahead reports the deadzone and
  event count the action had, rather than the defaults it used to echo.

- Every error says what kind of failure it is, in the same place (#486, #487,
  #492). `error.data` is the part a caller can branch on without parsing prose,
  and on 35 well-formed, wrong calls only 14 carried a `data.code`. Twelve
  carried an empty object, four carried `retryable` and nothing else, one
  carried everything but the code, and the confirmation gate's `428` carried no
  `data` at all. Each was a call site that filled it by hand or did not.

  There is now a floor, filled once on the way out of the registry rather than
  at each site: `code`, `tool`, `canonical_tool` and `retryable`, and a site
  that knows more still says more with nothing it set overwritten. `code` is a
  stable string rather than the status number beside it. The confirmation gate
  carries its own copy, because it answers before the registry does, and its
  `428` now names the argument to set to get a token.

  The five unimplemented registrations answered with a bare string rather than
  an envelope, because they refuse the call before any handler runs and so sat
  in front of the sweep that fixed everything else. They answer `501` with
  `code: "unimplemented"` now, which is the thing the sentence buried: this
  failure is permanent.

- `scene_get_hierarchy` answers the question it was asked (#482, #483, #484).
  Three things it got wrong, all of them in the shape of an answer that reads
  as a fact and is not one.

  A branch stopped by `max_depth` was byte for byte a leaf: empty `children`,
  no flag, nothing on the response. `max_depth` defaults to 10, so that is the
  cut most callers actually hit, and the answer to "what is under this node"
  was "nothing" rather than "not reported". A depth cut now reports itself the
  way a `max_nodes` cut always has, with `children_omitted` and
  `children_summary` on the node that stopped and `truncated: true` on the
  response. The tally covers the whole subtree below the cut, which is what
  `children_omitted` means everywhere else. `max_depth` also gained
  `minimum: 0` and `maximum: 64`, so a negative value is refused rather than
  silently clamped; it was the only bounded limit on the surface with no
  bounds published.

  `root_path` is documented as a node path or a `.tscn` file path, and with an
  editor attached a `.tscn` path went to the live bridge, which resolves node
  paths only. The result was a 404 saying it searched for the file and did not
  find the file. A `.tscn` path is now read from the file in either mode.

  `include_signals` and `include_scripts` are removed. They were advertised
  with a default of `true` and did nothing on either route, and the live route
  built `omitted_fields` out of them, so asking for properties added
  `bulk_properties` to the list of fields omitted and declining them took it
  off while the properties stayed empty either way. `omitted_fields` is now
  the fixed list the live walk actually produces. `include_properties` stays,
  because the `.tscn` parser honours it, and its description now says that the
  live route never returns bulk properties.

- Every tool parameter says what it is (#462). 217 of 381 carried no
  `description`, and 52 tools documented none of theirs. Every tool had a
  top-level description; the parameters inside it mostly did not, and the names
  that cost a caller the most are the ones that are not the obvious guess:
  `target_node` not `node_path`, `setting` not `setting_path`, `scene_path` not
  `output_path`, `emitter_node` not `source_node`, `target_method` not
  `method_name`. Argument errors already do this work after the mistake; a line
  of prose does it before. `signal_connect.flags` is the case that shows what
  was lost: `enum: [2]`, `default: 2`, no prose, so that only `CONNECT_PERSIST`
  is accepted, and that a deferred or one-shot connection is not on offer, was
  recoverable only by reading the enum and knowing what 2 means.

  The prose lives in one table applied at registration rather than beside each
  schema, because a name that means the same thing in fifteen tools should read
  the same in all fifteen, and because an alias should document its parameters
  identically to the tool it resolves to. Where a name genuinely differs, the
  table says so: `signal_emit`'s `target_node` is the emitter and
  `signal_connect`'s is the receiver. A description written inline in a schema
  is left alone. A contract test asserts none are missing, rather than that few
  are, so the next tool added cannot quietly reintroduce the gap.

- A confirmation skipped by YOLO mode, or offered to a person, reports the
  preview's own refusal (#463). Both paths mint the token by running the dry
  run themselves, so a preview that refuses leaves no token, and both then fell
  through to the ordinary path, which answered with the generic "this mutation
  requires a dry-run preview". That is advice the caller had already taken, and
  it hid the actual reason. The comment beside it promised the opposite:
  skipping confirmation is not skipping validation. This was invisible until
  `scene_call_method`'s preview became able to refuse.

- `scene_call_method`'s dry run reads the call instead of a property (#463).
  The preview reported the node's `name`, a constant that came back for a
  method the script declares, for one it does not, and for every argument
  list, because the shared node probe reads `name` when the call names no
  property. It then minted a confirmation token for a call it already had the
  evidence to refuse, and spending that token returned a 422 the preview never
  mentioned: the script is not a `@tool` script, so the editor never made an
  instance of it and there was nothing to run. Two round trips and a consumed
  token to learn something the first one knew. The gate's own 428 promises the
  preview reads the target where it can, and this one claimed
  `preview_kind: "target_state"` while answering a different question.
  `scene_call_method` now has its own probe: the bridge runs every check that
  decides the outcome and stops before the method would run, so `before`
  carries `method_name`, `method_exists`, `script_is_tool` and the declared
  signature, and a call that cannot succeed is refused at preview with the
  code the real call would have returned, issuing no token.

- `signal_list_connections` marks the editor's own listeners (#461). A freshly
  created `Sprite2D` with no user connections at all reported five, all of them
  the scene dock's `SceneTreeEditor` callbacks. None exist in the saved
  `.tscn` and none exist at runtime; they are alive only while the editor has
  the scene open. An agent asking what is wired to a node got five false
  positives to one true one, at a ratio that gets worse the emptier the scene,
  and the only tells were a null `target_node` and a `Class::_method` spelling
  that a caller has to already know to look for. Every connection now carries
  `origin`, `scene` or `editor`, and the response carries `editor_connections`.
  Marked rather than filtered, because a caller debugging the editor itself has
  no other way to see them.

- `ui_list_controls` and `ui_hit_test` name the subtree they actually covered
  (#470). `root_path` is an input to both, and neither echoed back something
  that could be sent anywhere: one answered `"<edited scene root>"`, a
  placeholder no tool accepts, and the other echoed the literal default
  `"/root"` while traversing from the edited scene root. The two reported
  different subtrees for the same two nodes, and a caller comparing them had no
  way to tell which spelling was real. Both now resolve the path the same way
  every control and hit in their own answers is already named.

- `scene_get_group_members` returns the group names a scene actually uses
  (#472). A group nobody has ever used answered exactly as one that had just
  been emptied, field for field, and nothing enumerated a scene's groups:
  `scene_list_groups` requires a `target_node` and answers for that one node.
  An agent that asked for `enemys` instead of `enemies` got a successful empty
  answer and no second question available to ask. In Godot a group is only its
  members, so there is no emptied group to go and find, which is exactly what
  made the typo unrecoverable. The walk that collects members now also collects
  the names in use, so `known_groups` comes back beside the empty list, with
  `group_exists` for the name that was asked about. Capped at 128 names with
  `known_groups_truncated`, and Godot's own underscore-prefixed internal groups
  are left out.

- `project_set_setting` checks the setting name against the engine (#464). It
  accepted any name, `display/window/size/viewport_widht` included, wrote it
  into `project.godot` and reported `status: "success"` with
  `persisted: true`. The window was unchanged and nothing in the engine would
  ever read that key. `project_get_setting` then returned it happily, so
  reading back to double-check did not catch it either. Godot does support
  custom project settings, so an unknown name is a legitimate mode, but it was
  indistinguishable from the overwhelmingly more common case. With an editor
  attached, an undefined name is now a 404 naming `create: true`, using the
  same `ProjectSettings.has_setting` the getter already answers with, and
  every success carries `defined_by_engine`. Offline there is no engine to ask
  and the shipped class reference publishes no `ProjectSettings` property
  list, so that route reports `defined_by_engine: null`, says in `limitation`
  that the name was not checked, and still writes. The addon bootstrap, which
  is an offline write by necessity, is unaffected.

- `scene_instantiate_node` refuses a request that names nothing to
  instantiate (#471). It declared no required arguments and sits behind no
  confirmation gate, so an empty argument object added a bare `Node` named
  `Node` under the edited scene root, and repeat calls added `Node2`, `Node3`.
  `{}` is what a caller sends when it has not decided yet, when a schema
  lookup failed, or when an argument-building step produced nothing, and
  everywhere else on this surface that costs one 400 because every other
  mutation either declares required arguments or sits behind the dry-run gate.
  Nothing in the response said it had chosen both the parent and the type
  itself. One of `node_type` or `scene_path` is now required, and `node_type`
  no longer advertises a default a client would fill in. Defaulting
  `parent_path` to the edited root is unchanged; there is one obvious answer
  for that. The refusal is in the bridge as well as the tool, because
  `mutate_scene_tree` with `action: "instantiate"` forwards straight to the
  bridge and the mutation happens on that side.

- `res://.didi/` is not listed or searched as project content (#468). Didi
  keeps its blackboard and crash state there, and `query_project_resources`
  reported those files as project resources while `project_search_text`
  returned hits inside them. An agent auditing a project saw two files nobody
  created, and a search over a project that uses the blackboard came back with
  values the agent itself had written earlier in the session, as evidence about
  the project. Didi's own lock file also surfaced in
  `unsearchable_extensions`. Godot's resource filesystem ignores
  dot-directories; the index and both searches now skip this one, and
  `project_audit_assets` inherits it through the shared index.

- `project_search_symbols` counts a file it reached and could not read symbols
  from (#469). A `.tres` in the search path was neither scanned, nor skipped,
  nor unsearchable, and its bytes were counted anyway, so the response read
  `scanned_files: 0` with `scanned_bytes: 76`. Those counters are the only
  thing separating "I searched and there was nothing" from "I searched
  nothing", and a caller reading that one correctly concludes the path was
  empty. A file with no symbol extractor now lands in `unsearchable_files`
  with its extension, the way the text search already handles one it cannot
  read, and contributes no bytes.

- `resource_create` refuses a `resource_type` Godot does not know (#465). It
  wrote `[gd_resource type="NoSuchResourceType"]`, reported
  `status: "created_offline"`, and left a file the engine cannot load. The
  tool already refused the lesser version of the same mistake, one property the
  type does not declare, and the reason it skipped the larger one came back as
  `property_check.reason: "type_not_in_api_reference"` on a success nothing
  forces a caller to read. An unknown type is now a 400 naming the spelling
  check. The escape hatch stays open, because a `class_name` script or a
  GDExtension type is not in the shipped reference either: pass
  `allow_unknown_type: true` and the result reports `checked: false` with
  `allowed_by`. Sub-resource types are held to the same rule.

- `resource_create`'s `property_check` says which engine it checked against
  (#466). The check reads a shipped API dump pinned to one engine line, and CI
  covers three, so a gap between the dump and the attached engine is the normal
  case. `checked: true` read as "verified against your engine": a property
  added in 4.7 passed the check and was then dropped by the 4.5.1 engine that
  loaded the file, which is the failure the check exists to prevent. With an
  editor attached, `property_check` now carries `attached_engine_version` and
  `api_version_matches_attached_engine`, the two fields `script_reflect_class`
  already carried. Both read one helper now, so the two cannot drift.

- `resource_inspect` reads the type out of the file (#467). `type` is derived
  from the extension, so every `.tres` came back as `Resource`: a valid
  `CanvasItemMaterial` and a file Godot cannot load at all were reported
  identically, differing only in byte count, on the tool named inspect. The
  `[gd_resource]` header is the first line of a file the indexer already opens
  for its dependencies, so `resource_type` now carries what the file declares,
  or `null` where the header could not be read. `type` is unchanged, because
  `project_list_resources` filters on it. Anything that is not a text resource
  has no such field.

- `resource_create` answers a bad `save_path` with the error envelope, the same
  defect as #460 in a tool the path probe could not reach behind the
  confirmation gate.

- Eight tools answer a path-validation failure with the error envelope
  (#460). The argument checks already answered with `error.code`, so the census
  in `probes/surface_census.py` reported no bare-string errors on this build:
  it sends junk arguments, and the argument check fires first. Behind valid
  arguments naming a path that is not there,
  `script_check_syntax`, `analyze_script_diagnostics`, `script_get_symbols`,
  `script_create`, `viewport_create_test_lab`, `create_visual_test_lab`,
  `project_search_text` and `project_search_symbols` handed the validator's
  message back unwrapped, so a client switching on `error.code` got
  `undefined` and had to substring-match English. The validator already carried
  the right code; prefixing its message by hand was what threw it away. Each of
  the eight now carries it through: 404 for a path that is not there, 400 for
  parent traversal. The sentence naming which argument was read is unchanged.

- Live scene answers name the scene they describe, and `scene_create` says it
  changed which one that is (#448). `scene_create` opens the scene it writes,
  which it reported as `opened: true`. What it did not report was that every
  later `scene_*` call now answered about a different file, because no live
  scene result carried the edited scene's path: a hierarchy came back with one
  node and no field naming the scene it came from, and
  `scene_get_property` on a node in the previous scene answered
  `Scene node not found` without saying which scene it had searched. Both
  answers were true of the wrong question, and the nodes were not gone, they
  were in the other tab. `scene_get_hierarchy` and `scene_get_selection` now
  carry `scene_file_path`, with `scene_is_unsaved` for a scene that has never
  been saved; the scene node 404 names the scene it searched; and `scene_create`
  reports `edited_scene_changed` and `previous_scene_file_path`. This is the
  class #401 was about, one level up.

- Every semantic failure in the Phase 7 bridge says a sentence, and a node of
  the wrong type is told apart from a path that resolves to nothing (#441,
  #443). #406 and #424 fixed the argument rejections; the failures underneath
  them still answered with the identifier as the whole message, which is the
  string a client shows a person. `target_method_not_found` does not say which
  method was looked for or on which node, so the one thing the caller needed was
  the one thing missing, and the same identifier appeared again as
  `data.upstream_message`, so the envelope carried it twice and a sentence zero
  times. All 129 of those sites now answer with a sentence and keep the
  identifier as a stable code under `data.code`. `declared_signal_not_found` and
  `target_method_not_found` name what was looked for and where;
  `camera_path_does_not_resolve_to_camera3d` names the path and the type it
  found instead. And `tilemap_target_not_found` no longer covers two different
  problems: a path that resolves to nothing keeps that code and says so, while a
  path that resolves to a node of the wrong type answers
  `tilemap_target_wrong_type` naming the type it found, with
  `gridmap_target_wrong_type` beside it. Those are different problems with
  different fixes, and a caller that could not tell them apart retried the path
  when it should have been looking at the node. The editor hook's own
  `session_kind_rejected` is included: it rejects before the bridge is entered,
  so it is the identifier a caller most often sees, and it now says which method
  needs which session kind and which one is selected.

- A number no float property can hold is refused rather than written as `inf`
  (#437). A Godot `float` property is `real_t`, 32 bits in a standard build, and
  `Vector2`, `Vector3` and `Color` are made of the same, so a JSON number above
  about 3.4e38 became `inf` the moment it landed there. Three things then went
  wrong and none was loud: the saved scene held `Vector2(inf, 5)`, which
  propagates through the transform to every child on the next frame; the value
  reported back was JSON `null`, which is not a number and cannot be sent back;
  and `null` is also what a caller reads as unset, so an infinite value and an
  unreadable one looked identical. `applied: false` was the only signal, and it
  sits beside `status: "success"` where it also appears for a value the engine
  merely coerced. `scene_set_property` and `scene_instantiate_node` now refuse
  such a number, naming the property, the vector or colour component where there
  is one, and the bound. Separately, a non-finite number read out of the engine
  comes back as the string `"inf"`, `"-inf"` or `"nan"` instead of `null`,
  because JSON has no spelling for them and nlohmann serialises all three as
  `null`: nothing this tool accepts can produce one any more, but a scene
  written by hand still can.

- `script_patch_method` reads the replacement before it writes it, and keeps the
  declaration where it found it (#438, #439, #440). Three separate ways to lose
  a method silently. The replacement text was spliced over the target without
  ever being parsed, so `new_definition: "var x = 1"` deleted `hello` and
  reported `hello` patched, and a body under a mistyped name added that name
  while the target stayed gone. The splice also wrote at column zero whatever
  indentation the declaration was found at, so a method declared inside a nested
  `class` was moved out of the class and the script stopped parsing, with the
  write already done and nothing to roll back to. And a name declared both
  inside a nested class and at the top level took the first match with no word
  said about the other. Now: `new_definition` has to declare the symbol
  `method_name` names, of the kind `symbol_type` asks for, or the call is
  refused with a 400 and nothing is written; the replacement is reindented to
  the declaration it replaces; and a name that matches more than one member
  declaration is refused with the scopes and lines that matched. A local
  variable that shares a member's name is not a second declaration and does not
  trigger that. `new_definition`, `file_path` and `method_name` carry
  `minLength: 1` in the published schema, so an empty one is rejected at
  validation rather than previewing clean, minting a confirmation token and
  failing the execute path. The required-argument refusal names the arguments
  that are actually missing, uses the error envelope, and no longer offers
  `symbol_name`, which this tool does not publish and would reject as an unknown
  property.

- `resource_create` checks property names against the type before it writes
  anything (#444). Whatever names `properties` carried went into the
  `[resource]` block and came back in `properties_written`. Godot drops a
  property the type does not have when it loads the file, silently, so the call
  reported four properties written and the loaded resource had none of them, and
  nothing in the surface could show the loss: `resource_inspect` reports type,
  file size, uid and dependencies, and no properties. Names are now checked
  against what the pinned API dump declares for the type and its ancestors, and
  an undeclared one is refused naming it, before anything is rendered. Three
  cases are deliberately not refused: `script`, which is how a resource gets
  properties of its own; a name beginning with `_` or containing `/`, because
  the dump lists only the inspector-visible set while Godot stores more than
  that, and `_data` on a Curve or `sources/0` on a TileSet have to keep working;
  and any name on a type the reference does not carry, such as a script class,
  because there is nothing to check it against. The result carries
  `property_check` saying whether the check ran and which storage-only names
  went in unverified, and `sub_resource_property_checks` says the same per
  sub-resource.

- The verification sandbox says which repository it used, and refuses one that
  merely encloses the project (#450). `verifyChangesInSandbox` resolved the
  repository with `git rev-parse --show-toplevel` from the project root and used
  whatever came back, however far above the project it sat, reporting it only as
  "the repository". A machine with a stray `git init` in the user profile made
  that directory the repository for a sandbox project under `%TEMP%`: had it
  carried a commit, the next step would have been `git worktree add` against the
  home directory plus a copy of its uncommitted state, and the tool would have
  reported `all_ok`. `repository_root` now names the work tree in the result and
  in every error about it, and a repository that tracks nothing under the
  project is refused naming both, because a tree that encloses the project
  without holding it is far more likely to be an accident than an instruction. A
  project in a repository of its own, and a project committed into a larger
  repository, are both unaffected.

- `project_apply_changes` fails in the same shape `project_verify_changes` does
  (#449). The dry run answered with an envelope and the confirmed call did not:
  apply built one by hand and only when the error carried data, so every failure
  without data came back as a bare string with no code and no `retryable`. The
  same condition reached through verify was wrapped and through apply was not.
  This is the #420 family on a path the surface census does not reach, because
  it is behind the confirmation gate.

- The schema layer enforces the shapes it publishes, so `tilemap_set_cells`
  names the field the way `gridmap_set_cells` always has (#442). The validator
  resolved no `$ref`, read no `prefixItems`, no `oneOf` and no `const`. The
  Phase 7 schemas are generated with their shared shapes under `$defs`, so
  `tilemap_set_cells` publishing `coords` as a two-element array through
  `$defs/vector2i` meant that shape was enforced nowhere: sending the
  `{"x": .., "y": ..}` object form, which most of the rest of the surface takes,
  went straight past validation and came back from the handler as a 404 carrying
  `invalid_tilemap_set_cells_request`, the identifier #406 was closed for.
  `gridmap_set_cells` is the same shape of tool with the same shape of mistake
  and answered `Argument 'cells' entry 0.position must be an array, not an
  object.`, because its schema inlines what tilemap's references. Now both do.
  A wrong-shaped argument is a 400 from the schema, before the handler, with the
  field named. `oneOf` reports against the one branch whose required properties
  are all present, which is the shape the caller was reaching for; when no
  single branch stands out it lists what each shape demands. Only same-document
  `$ref`s are followed.

- Three request edges below `tools/call` answer the way the specification says
  (#445, #446, #447). A request with an explicit `id: null` was parsed as an
  ordinary request and answered with a result. MCP narrows JSON-RPC here: the id
  must be a string or a number and must not be null, because null is how a
  response marks a request whose id could not be read, so answering one puts a
  response on the wire no client can match to a request. It is now `-32600`,
  beside the two neighbouring checks that were already there. `tools/list`,
  `resources/list` and `prompts/list` took any `cursor` and answered with the
  whole first page, which reads as a successful page to a client that kept a
  cursor across a restart, and can loop; all three answer in one page and issue
  no cursor, so any cursor is one this server did not issue and is refused with
  `-32602`. And a `tools/call` naming a tool no registration carries answered
  with a bare string inside an `isError` result, the last failure in the surface
  shaped unlike every other; the specification calls an unknown tool a protocol
  error, so it is now `-32602` carrying the name.

- A dry run reads its target, so a preview of a mutation that cannot succeed is
  no longer shaped like a preview of one that will (#417). Every preview was an
  echo of the arguments: `scene_remove_node` previewed against a node that does
  not exist came back byte-identical in shape to one that does, differing only
  in the path and its hash, and the real call was a 404. An agent using
  `dry_run` as its safety check before a batch got a clean preview for a typo'd
  target and found out mid-batch. A preview now resolves a file target on disk,
  a node target through one read-only property read on the attached engine, and
  a setting against `project.godot`, and fails the way the real call would
  rather than minting a token for it. `changes[].before` holds what is actually
  there: the file's size, the property's current value, the setting's current
  literal. `target_read` and `preview_kind: "target_state"` say when that
  happened, and a preview that could not read its target reports
  `unverified_mutation` rather than calling itself a planned one.

- Work that was never engine work is no longer reported as a fallback (#419).
  26 tools reported `execution_mode: "offline_fallback"` with a healthy editor
  attached. That is the label this server uses to mean you did not get the good
  answer and should attach an editor and ask again, so a caller branching on it,
  or an agent reading it as a quality signal, concluded that reattaching would
  improve an answer that was already authoritative. It cannot: the blackboard is
  a file on disk, `project_search_text` walks the project tree,
  `script_patch_method` rewrites a `.gd` file. It also buried the genuine
  signal, because `viewport_capture_frame` and `capture_viewport` really do
  synthesize a preview when there is no live frame and meant something different
  by the same word. A tool with no `live` path now reports `local`, joining
  `local_status` and `local_session_management`, and `offline_fallback` is left
  to the seven tools for which it is true. The registration's `executionModes`
  vocabulary is unchanged; this is the payload's own field, which already
  differed for the session-management tools.

- `project_audit_assets` does not call third-party addon files orphans (#427).
  The tool's output is advice to delete files, and in a fresh project most of
  that advice was about Didi's own brand assets: three of four orphans and 96%
  of the reported orphan bytes. `res://addons/` is a conventional boundary in
  Godot, holding code a developer did not write and is not responsible for
  tidying, and the noise is worst in an empty project, which is when someone is
  most likely to run an audit for the first time. Files beneath it are excluded
  by default, `excluded_addon_orphans` says how many were left out so the number
  is explainable, and `include_addon_orphans` asks for them back.

- Every semantic failure carries a code (#420). Eighteen tools answered one with
  a bare JSON string: no code, no `data`, no `retryable`. The prose was the good
  part and is unchanged, but a client that switches on `error.code`, which is
  the documented way to tell retryable from not, got `undefined` from those
  eighteen and had to substring-match English instead. Most of them were losing
  a code that already existed, because 31 call sites answered with an `Error`'s
  message and dropped its `code`. `script_create` over an existing file is the
  409 this server uses elsewhere for a conflict, a blackboard task that is not
  there is a 404 rather than an invalid argument, and managed recovery being
  switched off is a 501 rather than a bad request.

- `viewport_toggle_debug_draw` and `viewport_set_camera_transform` say what was
  wrong instead of returning a C++ identifier (#424). Both answered every
  argument mistake with a single token such as
  `invalid_viewport_toggle_debug_draw_request`, which is neither prose a person
  can act on nor a code a client can branch on, and carried no
  `"code": "invalid_arguments"` the way the other argument errors do. Calling
  `viewport_toggle_debug_draw` with `{}` was the worst of it, because the
  requirement it broke is expressed with `anyOf`, so the identifier was the
  entire explanation of which of its flags it wanted. `shader_set_uniform`
  reports the node it could not find rather than `shader_target_not_found`.

- `resource_inspect` tells a directory from a path with nothing behind it
  (#426). Both were "Resource not found", and they lead to different next
  actions: fix the argument, or go find the file. A directory now says so and
  points at `project_list_resources`.

- `project_analyze_impact` reads every `project.godot` setting that holds a
  path, not only `[autoload]` (#421). `run/main_scene` is the most load-bearing
  path a Godot project has and is exactly what someone runs an impact analysis
  before moving, and it came back `impact_count: 0` with `target_exists: true`,
  which this tool uses to mean nothing depends on the target. The rule is any
  line in the file whose value names the target rather than a list of keys that
  goes stale as Godot adds settings, so `config/icon`,
  `application/boot_splash/image`, `default_environment` and the `res://`
  entries under `[editor_plugins]` are covered by the same change. The new kind
  is `project_setting`; `autoload` keeps its own, because a rename treats it
  differently.

- `project_search_text` reads the text formats a project keeps references in,
  and counts what it did not read (#422). It read four extensions, so a shader
  uniform, an input action, a setting key or an `.import` flag returned an empty
  result with `skipped_files: 0`, `truncated: false` and `diagnostics: []`, every
  honesty field saying nothing was left out, while `project_list_resources`,
  `project_audit_assets` and `project_analyze_impact` all read those same files.
  `.gdshader`, `.gdshaderinc`, `.godot`, `.cfg`, `.json` and `.import` are read
  now, and the result reports `unsearchable_files` and
  `unsearchable_extensions` for whatever was never a candidate, so an empty
  result can be told apart from a string the project does not contain.
  `project_search_symbols` keeps the narrower set, because a declaration does
  not live in a shader or a `.json`.

- A tool's arguments are closed by default, so a typo'd property name is
  refused rather than ignored (#418). Rejecting an unknown argument depended on
  the schema remembering to publish `additionalProperties: false`, which 50 of
  126 tools did. The sharp case is a plausible guess: `project_search_text`
  takes `search_path`, `path` is the obvious guess, and it was accepted,
  ignored, and the unscoped search it ran was reported as a success with
  nothing in the response to tell the two apart. Every implemented tool answers
  the same way now, and a new tool is covered on arrival rather than by
  remembering. Nested objects keep the old rule, because several of them are
  deliberately free-form maps. A tool that really does take arguments it does
  not publish can say `additionalProperties: true`.

- `pattern` and `uniqueItems` are enforced, having been published at 22 sites
  and checked at none (#423). A confirmation token or session id of the right
  length and the wrong alphabet passed validation and was looked up as if it
  were real, while `minLength` and `maxLength` beside it were enforced. A
  duplicate array entry passed while `minItems`, `maxItems` and `enum` on the
  same property were enforced. `viewport_capture_passes` now declares the
  uniqueness its own description promises, so a repeated pass is refused
  offline the way the engine refuses it live.

- The overwrite gate arms on the target, not on the flag (#425). `overwrite:
  true` demanded a dry-run preview and the confirmation token it returns even
  when nothing was behind the path, so a generator or a repeatable setup step
  paid two extra round trips for every file, including the files that were new.
  Writing a new file with the flag and writing one without it have identical
  effects on disk, and only one of them was gated. A path with a file behind it
  is still gated, because that call destroys something. A token minted while the
  target existed stays spendable if the target goes before it is spent, which
  arming on state would otherwise have turned into a refusal for offering the
  confirmation the caller was told to get. `viewport_create_test_lab` and
  `create_visual_test_lab` write one fixed path rather than the path their
  `target_resource_path` names, and are gated on that.

- Symbol scanning keeps a name that is not spelled in ASCII (#416). GDScript
  identifiers may hold Unicode letters, and the scanners classified every byte
  above ASCII as the end of a name, so `CaféMenu` came back as `Caf` and a name
  that began with one was dropped with nothing to say so. A truncated name is
  worse than a missing one because it looks real: `project_audit_assets`
  reported a dead signal `pr` that no project contains, and
  `project_search_symbols` disagreed with `project_search_text` about the same
  file. The identifier rule now lives in one place and is shared by
  `script_get_symbols`, `project_search_symbols`, `project_search_text`
  whole-word matching, the audit's signal scan, and the impact and rename
  scanners, which had also been refusing such a name as not an identifier. The
  audit's patterns are a bounded character class behind a left boundary: an
  alternation, an unbounded repeat and a missing boundary each turned a single
  packed line in a .tscn into a refusal or a scan that ran for minutes.

- Offline `scene_get_hierarchy` no longer answers a different question than the
  one asked (#401). Any `root_path` that did not end in `.tscn` was replaced by
  the project main scene and returned as an ordinary success, so a node path
  that does not exist, a `res://project.godot`, and a binary `.scn` all came
  back as the whole main scene with nothing saying the request had been
  substituted. A non-`.tscn` path is now refused, and the main-scene default for
  an omitted `root_path` reports `requested_root_path` and
  `substituted_main_scene`.

- `project_list_export_presets` reports no presets instead of a file error
  (#403). Godot writes `export_presets.cfg` the first time a preset is added, so
  a project that has never configured an export has no file, and the answer was
  a failure naming an absolute host path the user never created. A missing file
  is now an empty list with `presets_file_exists: false`. A file that exists and
  cannot be read or parsed is still an error.

- `project_analyze_impact` says whether the target exists (#404).
  `resolved_kind: "file"` describes the shape of the string, so a typo'd
  `res://` path returned a clean empty result byte-identical to a real file with
  no dependents: the answer to "is it safe to delete this" and the answer to
  "you typed it wrong" were the same response. A `res://` target now reports
  `target_exists`, and an absent one adds a limitation saying the empty list
  means not found. A `uid://` target reports `null`, because the engine resolves
  those from a table a file scan cannot read.

- `script_reflect_class` no longer gives advice it cannot honour, and says when
  the pinned API is not the engine you are running (#405). Its description told
  the caller to attach a live editor for the running engine's own state, and the
  tool has no live mode, so following it returned the same answer. It now says
  what the shipped dump covers and points a script class at
  `script_get_symbols`. Separately, the session descriptor gained an optional
  `engine_version`, which the extension fills from the engine itself, so with an
  editor attached the response carries `attached_engine_version` and
  `api_version_matches_attached_engine`. Major and minor are compared, so a
  patch difference is not a mismatch, and an extension older than the field
  reports `null` rather than a match it cannot vouch for.

- `viewport_create_test_lab` names `runtime_launch` in the message it returns
  (#408). It told the caller to run `execute_test_session`, which is one of the
  ten legacy aliases, so a client that lists tools by canonical name and follows
  the instruction was being steered onto the deprecated surface.

- `tools/call` now checks the `inputSchema` each tool publishes before anything
  dispatches (#397). The schemas were advisory: `additionalProperties: false`
  was not applied, so `blackboard_read` accepted an argument it does not have,
  and `required` was not applied, so `project_get_setting` with no `setting`
  failed downstream as a `503` transport error. Every handler re-derived its own
  checks by hand, so coverage was uneven. One check now reads the published
  schema and refuses with a message naming the property; handler checks stay as
  a second line of defence. Two of this project's own tests were calling tools
  with argument names the schemas do not have, and passing.

- `viewport_capture_passes` publishes the segmentation pass it has always drawn.
  Its schema listed three pass kinds and capped the array at three; the engine
  side takes four. Enforcing that schema turned the understatement into a
  refusal of a picture the tool takes, which is how it was found.

- `scene_add_to_group` and `scene_remove_from_group` no longer target the edited
  scene root when `target_node` is missing (#396). Both declare it required, and
  omitting it added the group to whatever the editor had open, reported success,
  and left a mutation on a node the caller never named. A missing required
  argument is now refused before the request reaches the editor.

- A wrong argument type no longer reads as a server fault (#400). `max_depth`
  given a string came back as `Internal error executing tool:
  [json.exception.type_error.302] ...` and logged at ERROR. Declared types are
  refused up front by name; anything the schema leaves open is reported as the
  caller mistake it is, without quoting a C++ library.

- Live Phase 7 tools no longer answer a bad argument with only a machine token
  (#406). `invalid_signal_list_connections_request` and its siblings said
  nothing about which property was wrong. The schema check runs first and names
  it; the token stays in `data` as a stable machine code.

- `dry_run` no longer mints a confirmation token for arguments the tool would
  refuse (#399). The preview path skipped argument validation, so a call naming
  `new_body` instead of `new_definition` was previewed, signed, and then
  rejected on execution. The preview runs through the same check, so a token
  exists only for a call that could have run.

- `prompts/get` now requires the arguments `prompts/list` marks required (#402).
  Omitting `target_resource_path` rendered the template with the placeholder
  collapsed to `res://`, handing an agent an instruction to diagnose the whole
  project. It is an invalid-params error naming the argument.

- A confirmation token is no longer consumed by an attempt that failed its own
  binding check (#398). The token was erased from the map before expiry and
  argument binding were checked, so one mistyped argument burned the token the
  caller had just previewed and the retry with the exact previewed arguments
  came back "unknown or already used". A token is now spent only on the mutation
  it authorises. An expired token is still dropped, and the mismatch message
  says the token is still good.

- The mutation gate no longer calls its preview exact (#407). The `428` demanded
  "an exact dry-run preview" and the preview it demanded reported
  `before: "not read or modified during dry-run"`, so a person approving a token
  had nothing to approve on. The preview now reports
  `preview_kind: "argument_binding"` and says plainly that it binds arguments
  and does not read the target. What it does has not changed; what it claims
  has.

- `runtime_attach_session` no longer attaches a session belonging to a different
  project than the server's root (#387). Automatic selection has always required
  the project paths to match; naming a session skipped the check, so a server
  started on one project would serve another project's scene tree, and route
  mutations into it, while still reporting its own root. It now refuses with
  `409` naming both paths, and `allow_foreign_project: true` is the explicit way
  to do it anyway, which reports `project_mismatch: true` and the limitation.

- `didi_control_room` no longer reports another project's session as this
  project's, and the Project light is a real preflight (#388). The Bridge reason
  is computed against descriptors for this project root, so a project with no
  addon is no longer told to "attach one" because some unrelated editor happens
  to be open, which is the instruction that produced the cross-project attach
  above. The Project light checks for `addons/didi/didi.gdextension` and for the
  plugin in `editor_plugins/enabled`, and names the fix for each; both are file
  stats, so they answer in the state where nothing live can.

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

- Didi can now enable its own addon in a project that does not have it (#382).
  Every `project_*` writer was live-only, a live session needs the addon, and
  enabling the addon is a `project_set_setting` write, so the first call an
  agent makes in a new project was the one call it could never make.
  `project_set_setting` now falls back to writing `project.godot` directly when
  no session is attached, reporting `execution_mode: "offline_fallback"` and
  the literal it wrote. The value reach and the name rules are the live ones,
  so a setting written offline is the Variant a live write would have stored.
  QUICKSTART and LLM_INSTRUCTIONS now carry the bootstrap as a tool sequence
  rather than only as a sequence of clicks.

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

- The schema-enforcement test brings its own project. It searched `res://addons`
  and asserted the search succeeded, with no fixture anywhere in it, so the only
  thing making that true was the binary happening to be started from the
  repository root. Run from anywhere else, the assertion that closing a schema
  must not close the tool failed for a missing directory. It now creates the
  project it stands in, and asserts the search found something rather than only
  that it did not error.

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
