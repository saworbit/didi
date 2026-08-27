# Phase 3 whole-branch fix wave B report

## Scope

Wave B addresses whole-branch red-team findings 3–7:

- finite, truthful end-to-end live-command deadlines and route quarantine after an unresolved started command;
- unambiguous project-local auto-attachment with editor preference and transactional rollback;
- protocol-gated, full authoritative handshakes and bounded fresh session reads;
- token-free live success/error envelopes that preserve engine code and data;
- selected-session-kind-aware tool and resource availability.

Wave A retained ownership of session registry/retirement and common transport until its commit.
Wave C retained ownership of runtime-tree bounds, public schemas/descriptions, documentation, and CI.

## TDD evidence

### RED: command routing, envelopes, handshake, and availability

The first new native target failed to link because `handleSessionHandshake` and
`awaitRuntimeCommand` did not exist. This isolated the missing authoritative handshake and
bounded Running-command completion behavior from fixtures and transport.

After the pure routing seam existed, the new assertions exposed the remaining text-only live
errors, missing session provenance, editor/game availability confusion, and game-route editor
resource call. Each became GREEN after the corresponding centralized routing change.

### RED: auto-attachment

With real descriptor discovery and a complete fake authenticated transport, the focused suite
reported three expected failures:

- `RuntimeRouting.AutoAttachFirstAvailability`: one valid matching session performed no handshake;
- `RuntimeRouting.AutoAttachEditorPreferenceAndAmbiguity`: an editor was not preferred over matching games;
- `RuntimeRouting.AutoAttachRollback`: a rejected authoritative identity performed zero handshake attempts.

These tests remained RED while Wave A owned `session_client.*`, as required by the shared-file
sequencing constraint.

### RED: fresh authoritative reads

The first `runtime_get_session` freshness test failed to compile because the session-client
interface had no refresh operation. After adding a bounded authenticated refresh, the test
verified that an identity change quarantines the route and that the returned handshake is fresh.

### Mutation check

The single-candidate auto-attachment branch was deliberately restricted to game sessions. The
focused suite then failed the single-editor case and its editor-dependent routing checks. Restoring
the specified one-session behavior returned those checks to GREEN, demonstrating that the tests
detect the editor auto-attach path rather than only game attachment.

### Final red-team iteration

A read-only implementation review found six boundary gaps: first-call auto-attach provenance,
dead-route refresh quarantine, finite/quarantined live-log resources, explicit-attach route races,
false offline advertising on a connected wrong-kind route, and plain-text local validation errors.
Focused regressions produced six expected Wave B failures; the overall shared RED result was
69 passed / 7 failed / 76 total because a concurrent Wave C prose assertion was also changing.
After correction, every focused case and the whole Release native suite passed.
A read-only re-review of the six fixes reported no remaining actionable issue.

## Implementation notes

- The extension owns a finite 15-second main-thread command budget. Pending commands return a
  definitive `not_started` timeout. A command already in `Running` state returns
  `unknown_outcome`, sets `route_quarantine: true`, and never blocks the IPC worker indefinitely.
- The standalone transport call uses a finite 17-second outer budget. Explicit or transport-level
  unknown outcomes disconnect the selected route before another command can be attributed to it.
- Handshake requests require protocol `1.3`. Successful responses carry the complete public
  descriptor and never the token.
- Live tool and resource results use `{execution_mode:"live", session:{...}}`. Errors additionally
  carry `{error:{code,message,data}}`; engine data is preserved rather than flattened to text.
- Availability treats legacy injected live routes as editor routes, but runtime session routes use
  their selected `editor`/`game` identity. Runtime logs/tree/eval allow both kinds; pause/step/stop
  allow games; editor APIs/resources allow editors; local session management remains local/offline.
- Auto-attachment considers only alive sessions whose canonical project path matches the server
  project. One candidate attaches; a unique editor is preferred over matching games; multiple
  editors or multiple game-only candidates remain detached. Authentication failure rolls back to
  the detached state. Explicit route changes supersede concurrent discovery through a generation
  check.
- `runtime_get_session` performs a fresh handshake with a three-second transport bound. Any
  protocol or identity mismatch quarantines the route. Success returns the token-free selected
  session and authoritative handshake under `execution_mode:"local_session_management"`.

## Verification

- Release build: GREEN.
- Native suite: GREEN, 76 passed / 0 failed / 76 total. Before Wave C's fixture repair the sole
  pre-review failure was its intentionally changed `Tools.CaptureViewportWithIpc` fake-resource
  fixture; after that repair and the final red-team iteration, the whole-branch suite passed.
- Godot 4.7.2 integration: GREEN — Phase 1/2 editor workflows and concurrent Phase 3 game tree and
  execution control passed.
- Godot 4.5.1 integration: GREEN — Phase 1/2 editor workflows and concurrent Phase 3 game tree and
  execution control passed.
- `git diff --check`: GREEN.

No technical pushback remains for findings 3–7. The POSIX transport execution limitation reported
by Wave A does not affect Wave B's platform-neutral routing tests or the two requested Windows Godot
integrations.
