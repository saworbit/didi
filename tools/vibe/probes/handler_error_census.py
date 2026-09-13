"""Which failures are still a bare string, once the argument check is satisfied?

`path_errors.py` asks whether a path failure is shaped like an error and reports
zero. `error_data_census.py` opens the envelope and counts what is inside it.
Neither reaches a handler that fails *after* its own parsing -- the persistence
layer, the writer, the offline capability check -- where
`CallToolResult::error(std::string)` produces prose with no code, no `data` and
no `structuredContent`. Four failures live there (#548).

The sweep builds arguments from each tool's own schema: every required property
filled with a well-formed value of the declared type that names something the
project does not have. That gets past the argument validator and into the
handler, which is the whole point.

Three of the four are not reachable that way, because they need arguments that
are not merely well formed but *meaningful and wrong* -- `remove` of a setting
that is not there, a value nested past the cap, an offline-only capability
refusal. Those are listed explicitly, and each one sits beside a sibling call on
the same tool that answers correctly, so a run prints the difference rather than
just the defect.

Usage::

    python tools/vibe/probes/handler_error_census.py SANDBOX
    python tools/vibe/probes/handler_error_census.py SANDBOX --verbose
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

MISSING_PATH = "res://vibe_absent_directory/vibe_absent_file"

# Severs the bridge, or spawns a process, which is not a thing a census does.
SKIP = {"runtime_detach_session", "runtime_launch", "runtime_stop", "project_export"}

DEEP_VALUE: object = 1
for _ in range(17):
    DEEP_VALUE = {"n": DEEP_VALUE}

# Each pair is (label, tool, arguments). The first of each pair answers in the
# envelope and the second does not, so a fix is visible as the two rows agreeing.
PAIRS: list[tuple[str, str, dict]] = [
    ("project_set_setting, no value", "project_set_setting",
     {"setting": "application/config/name"}),
    ("project_set_setting, remove an absent setting", "project_set_setting",
     {"setting": "application/config/vibe_absent", "remove": True}),
    ("project_set_setting, value nested past the cap", "project_set_setting",
     {"setting": "vibe/deep", "value": DEEP_VALUE, "create": True}),
    ("script_reflect_class, a class that is not one", "script_reflect_class",
     {"class_name": "VibeAbsentClass"}),
    ("script_reflect_class, an empty class_name", "script_reflect_class",
     {"class_name": ""}),
    ("viewport_capture_frame offline", "viewport_capture_frame", {}),
    ("viewport_capture_passes offline", "viewport_capture_passes", {"passes": ["color"]}),
]


def value_for(name: str, schema: dict) -> object:
    if "enum" in schema:
        return schema["enum"][0]
    if "const" in schema:
        return schema["const"]
    kind = schema.get("type")
    if isinstance(kind, list):
        kind = kind[0]
    if kind == "string":
        lowered = name.lower()
        if any(word in lowered for word in ("path", "file", "scene", "script")):
            suffix = ".gd" if "script" in lowered else (".tscn" if "scene" in lowered else "")
            return MISSING_PATH + suffix
        if "node" in lowered:
            return "/root/VibeAbsentNode"
        return "vibe_absent_" + name
    if kind == "integer":
        return max(1, schema.get("minimum", 1))
    if kind == "number":
        return float(max(1, schema.get("minimum", 1)))
    if kind == "boolean":
        return False
    if kind == "array":
        return [value_for(name, schema.get("items", {"type": "string"}))]
    if kind == "object":
        return {}
    return "vibe_absent"


def shape(result: dict) -> tuple[str, str]:
    """`(kind, text)` for one `tools/call` result."""
    if not result.get("isError"):
        return "ok", ""
    text = (result.get("content") or [{}])[0].get("text", "")
    try:
        json.loads(text)
    except ValueError:
        return "BARE", text
    return "envelope", text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("-b", "--binary")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    rows: list[tuple[str, str, str]] = []
    with Session(project=args.project, binary=args.binary) as session:
        print("== every tool, required arguments naming something absent")
        for tool in sorted(session.tools(), key=lambda entry: entry["name"]):
            name = tool["name"]
            if name in SKIP:
                continue
            schema = tool.get("inputSchema", {})
            properties = schema.get("properties", {})
            arguments = {
                required: value_for(required, properties.get(required, {}))
                for required in schema.get("required", [])
            }
            envelope = session.request("tools/call", {"name": name, "arguments": arguments})
            result = envelope.get("result")
            if result is None:
                rows.append((name, "rpc-error", json.dumps(envelope.get("error"))))
                continue
            kind, text = shape(result)
            rows.append((name, kind, text))
            if args.verbose or kind == "BARE":
                print(f"  {name:32} {kind:9} {text[:150]}")

        bare = [row for row in rows if row[1] == "BARE"]
        print(f"\n  {len(rows)} tools asked; {len(bare)} answered with a bare string")
        print(f"  {Counter(row[1] for row in rows)}")

        print("\n== the failures that need meaningful-and-wrong arguments")
        for label, tool, arguments in PAIRS:
            envelope = session.request("tools/call", {"name": tool, "arguments": arguments})
            result = envelope.get("result") or {}
            kind, text = shape(result)
            print(f"  {label:48} {kind:9} {text[:110]}")

    print(
        "\nEvery row above should read 'envelope' or 'ok'. A 'BARE' row is a caller "
        "with no code to branch on, which is the contract #420 set and #460, #492 "
        "and #526 each extended."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
