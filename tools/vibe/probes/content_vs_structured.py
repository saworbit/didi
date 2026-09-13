"""Every tools/call result carries its payload twice. Do the two agree?

`content[0].text` is what a host without structured-output support hands the
model; `structuredContent` is what a host with it parses. They are built in the
same place today, but nothing compares them, and #509 and #510 showed the
published side of that pair drifting from the handler already.

**This census is green, and it is kept for that reason.** All 126 tools agree,
errors included, and the two tools that return an image return it as a second
content block without disturbing the first. A probe that starts failing is a
regression nobody wrote a test for.

It also carries the lesson the README states twice. Run with no arguments, 108
of 126 tools answer an argument error, and an error sets no `structuredContent`
by design -- so the census sees nothing but failures and proves nothing. The
`--valid` pass sends arguments that succeed, which is the only pass that asks
the question. Keep both; the first is how you notice the second is needed.

Usage::

    python tools/vibe/probes/content_vs_structured.py SANDBOX
    python tools/vibe/probes/content_vs_structured.py SANDBOX --valid
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# Arguments that succeed against `sandbox.py`'s project. The point of the list
# is that every row returns a real payload, so both halves exist to compare.
VALID_CALLS = [
    ("scene_get_hierarchy", {}),
    ("scene_get_group_members", {"group": "x"}),
    ("script_check_syntax", {"file_path": "res://player.gd"}),
    ("script_get_symbols", {"file_path": "res://player.gd"}),
    ("project_get_setting", {"setting": "application/config/name"}),
    ("project_search_text", {"query": "extends"}),
    ("project_search_symbols", {"query": "Player"}),
    ("project_list_resources", {}),
    ("project_get_uid_map", {}),
    ("project_list_input_actions", {}),
    ("resource_inspect", {"resource_path": "res://main.tscn"}),
    ("blackboard_read", {}),
    ("blackboard_write", {"path": "probe/a", "value": 1, "reason": "census"}),
    ("blackboard_task_create", {"title": "t", "description": "d"}),
    ("runtime_get_session", {}),
    ("didi_control_room", {}),
]


def inspect(result: dict) -> list[str]:
    """What is odd about one `tools/call` result."""
    notes: list[str] = []
    content = result.get("content") or []
    structured = result.get("structuredContent")
    if structured is None:
        notes.append("no-structuredContent")
    if not content:
        notes.append("no-content")
    parsed = None
    if content and content[0].get("type") == "text":
        try:
            parsed = json.loads(content[0]["text"])
        except (ValueError, TypeError):
            notes.append("text-not-json")
    if parsed is not None and structured is not None and parsed != structured:
        notes.append("TEXT!=STRUCTURED")
    if len(content) > 1:
        notes.append(f"content x{len(content)}:{[block.get('type') for block in content]}")
    return notes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project", nargs="?", help="A project the server can open.")
    parser.add_argument("--valid", action="store_true",
                        help="Send arguments that succeed instead of {} to every tool.")
    args = parser.parse_args()

    session = Session(project=args.project)
    calls = VALID_CALLS if args.valid else [(tool["name"], {}) for tool in session.tools()]

    rows: list[tuple[str, str, str]] = []
    for name, arguments in calls:
        response = session.request("tools/call", {"name": name, "arguments": arguments})
        result = response.get("result")
        if result is None:
            message = response.get("error", {}).get("message", "")[:60]
            rows.append((name, "rpc-error", message))
            continue
        notes = inspect(result)
        rows.append((name, f"isError={result.get('isError')}", ",".join(notes) or "agree"))
    session.close()

    for name, state, note in rows:
        if note != "agree":
            print(f"{name:38} {state:14} {note}")

    print(f"\n--- {len(rows)} calls")
    for note, count in Counter(note for _, _, note in rows).most_common():
        print(f"{count:4}  {note}")
    print("\nA `TEXT!=STRUCTURED` row is the regression this probe exists for.")
    print("A run with nothing but `no-structuredContent` is a run that only saw")
    print("errors -- pass --valid.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
