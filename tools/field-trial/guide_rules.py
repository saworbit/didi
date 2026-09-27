"""Score a field trial against the rules the handshake guide gives a host.

Coverage says which tools a tester reached for. This says how it reached for
them: how many calls failed and why, whether a call that failed was sent again
unchanged, and whether the habits the guide asks for show up in the run: a
dry_run preview before a mutation, a bounded hierarchy read, a read-back after a
write. The guide is `kServerInstructions` in `src/mcp/mcp_server.cpp`, returned
from `initialize`; trial 06 is the first run that had it.

A failure's cause is read from the answer text, so it is a classification and
not a verdict. `other` is whatever no rule matched, and a reworded refusal can
move a call from one cause to another. Compare runs by the totals, then read the
calls.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
from pathlib import Path
from typing import Iterable

_TRANSCRIPTS = Path(__file__).resolve().parent / "transcripts.py"
_SPEC = importlib.util.spec_from_file_location("field_trial_transcripts", _TRANSCRIPTS)
if _SPEC is None or _SPEC.loader is None:
    raise ImportError(f"Cannot load the transcript reader from {_TRANSCRIPTS}")
transcripts = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(transcripts)

UNIMPLEMENTED = re.compile(r"\bunimplemented\b", re.I)
UNKNOWN_TOOL = re.compile(r"Unknown tool|Tool not found|No such tool", re.I)
NO_PREVIEW = re.compile(r"requires an? (exact )?dry-run preview", re.I)
NO_ROUTE = re.compile(
    r"not_connected|No atomic runtime route|unavailable for the selected session kind|"
    r"Cannot connect to Godot|most runtime sessions|requires a paused|requires a live",
    re.I,
)
MISSING_TARGET = re.compile(
    r"Scene node not found|Runtime node not found|No node at |No node was found|"
    r"Node not found|target_not_found|Property not found on target",
    re.I,
)
BAD_ARGUMENTS = re.compile(
    r"invalid_arguments|-32602|Invalid params|additionalProperties|Missing required|"
    r"is required|must be an? |must not exceed|unknown property|Invalid [a-z ]+ request",
    re.I,
)
UNSUPPORTED = re.compile(
    r"outside the Phase|not JSON-coercible|limited to native|forbidden identifier|"
    r"not supported|unsupported",
    re.I,
)

# Causes the guide speaks to. A missing live route is left out: the guide says to
# check sessions first, but an editor that has closed is not a calling mistake.
GUIDE_CAUSES = (
    "unimplemented_tool", "unknown_tool", "non_canonical_name", "mutation_without_preview",
    "missing_target", "bad_arguments", "unsupported_operation",
)

# A write, and the read that shows what it did.
READ_BACK = {
    "scene_set_property": "scene_get_property",
    "project_set_setting": "project_get_setting",
    "project_set_input_action": "project_list_input_actions",
    "project_set_autoload": "project_list_autoloads",
    "scene_add_node": "scene_get_hierarchy",
    "scene_remove_node": "scene_get_hierarchy",
    "scene_create": "scene_get_hierarchy",
    "script_patch_method": "script_get_symbols",
}
READ_BACK_WINDOW = 15
SESSION_CHECKS = ("runtime_list_sessions", "runtime_attach_session", "runtime_get_session")


def classify(invocation, implemented: set[str], unimplemented: set[str]) -> str:
    """Why a failed call failed, the cause the guide addresses most directly first."""
    text = invocation.result or ""
    if invocation.tool in unimplemented or UNIMPLEMENTED.search(text):
        return "unimplemented_tool"
    if UNKNOWN_TOOL.search(text):
        return "unknown_tool"
    if invocation.tool not in implemented:
        return "non_canonical_name"
    for cause, pattern in (
        ("mutation_without_preview", NO_PREVIEW),
        ("no_live_route", NO_ROUTE),
        ("missing_target", MISSING_TARGET),
        ("bad_arguments", BAD_ARGUMENTS),
        ("unsupported_operation", UNSUPPORTED),
    ):
        if pattern.search(text):
            return cause
    return "other"


def build_report(invocations: list, implemented: Iterable[str],
                 unimplemented: Iterable[str]) -> dict:
    implemented = set(implemented)
    unimplemented = set(unimplemented)
    failures: dict[str, int] = {}
    seen_failing: set[str] = set()
    repeated = 0
    for invocation in invocations:
        if not invocation.is_error:
            continue
        cause = classify(invocation, implemented, unimplemented)
        failures[cause] = failures.get(cause, 0) + 1
        key = invocation.tool + json.dumps(invocation.arguments or {}, sort_keys=True)
        if key in seen_failing:
            repeated += 1
        seen_failing.add(key)

    writes = read_back = 0
    for index, invocation in enumerate(invocations):
        read = READ_BACK.get(invocation.tool)
        arguments = invocation.arguments or {}
        if read is None or invocation.is_error or arguments.get("dry_run") is True:
            continue
        writes += 1
        window = invocations[index + 1:index + 1 + READ_BACK_WINDOW]
        if any(later.tool == read for later in window):
            read_back += 1

    hierarchy = [i for i in invocations if i.tool == "scene_get_hierarchy"]
    errors = sum(1 for i in invocations if i.is_error)
    return {
        "invocations": len(invocations),
        "errors": errors,
        "error_percent": round(100.0 * errors / len(invocations), 1) if invocations else 0.0,
        "failures_by_cause": dict(sorted(failures.items())),
        "guide_addressable_failures": sum(failures.get(c, 0) for c in GUIDE_CAUSES),
        "repeated_identical_failures": repeated,
        "non_canonical_calls": sum(
            1 for i in invocations if i.tool not in implemented | unimplemented
        ),
        "dry_run_previews": sum(1 for i in invocations if (i.arguments or {}).get("dry_run") is True),
        "confirmation_tokens_spent": sum(
            1 for i in invocations if "confirmation_token" in (i.arguments or {})
        ),
        "hierarchy_reads": len(hierarchy),
        "hierarchy_reads_bounded": sum(1 for i in hierarchy if any(
            k in (i.arguments or {}) for k in ("max_depth", "max_nodes", "summary"))),
        "session_checks": sum(1 for i in invocations if i.tool in SESSION_CHECKS),
        "writes_with_a_read_pair": writes,
        "writes_read_back": read_back,
        "blackboard_calls": sum(1 for i in invocations if i.tool.startswith("blackboard_")),
        "undo_redo_calls": sum(1 for i in invocations if i.tool in ("editor_undo", "editor_redo")),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transcript", required=True, type=Path, help="Session transcript .jsonl")
    parser.add_argument("--manifest", required=True, type=Path,
                        help="The trial's tool-manifest.baseline.json")
    parser.add_argument("--server", default="didi", help="MCP server alias in the client config")
    parser.add_argument("--output", type=Path, help="Write the report here instead of stdout")
    args = parser.parse_args(argv)

    with args.transcript.open(encoding="utf-8", errors="replace") as handle:
        invocations = transcripts.iter_invocations(handle, server=args.server)
    names = json.loads(args.manifest.read_text(encoding="utf-8"))["names"]
    report = build_report(invocations, names["implemented"], names.get("unimplemented", []))
    rendered = json.dumps(report, indent=2, sort_keys=True)
    if args.output is None:
        print(rendered)
    else:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
