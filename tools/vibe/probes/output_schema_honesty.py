"""Every published `outputSchema` checked against the payload it describes.

`structuredContent` comes back from all 126 tools; a handful publish an
`outputSchema` for it, and a host that reads one is entitled to validate against
it. #509 counted how many there were and #510 found the one published naming a
field the handler does not return -- by reading, not by calling. Nothing has ever
called each of these tools and checked the answer against its own schema.

Three questions per tool, and the third is the one the earlier pass could not
ask:

* is every `required` property present in the answer?
* does every property the answer does carry have the declared `type`?
* does the answer carry properties the schema never mentions?

The third is not a defect on its own -- MCP output schemas are open -- but a
schema that describes four of a tool's fourteen fields is not much of a contract,
and the ratio is the interesting number.

Read-only tools only, and every call is given arguments from the tool's own
schema where required ones exist, so a row that errors is a row whose arguments
were wrong rather than a finding.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

JSON_TYPES = {
    "string": str, "integer": int, "number": (int, float),
    "boolean": bool, "object": dict, "array": list,
}

# Arguments for the tools whose schema has required parameters. Everything else
# is called with `{}`.
ARGUMENTS = {
    "viewport_capture_frame": {},
    "capture_viewport": {},
    "script_check_syntax": {"file_path": "res://player.gd"},
    "analyze_script_diagnostics": {"file_path": "res://player.gd"},
    "scene_get_hierarchy": {},
    "get_scene_hierarchy": {},
    "project_list_resources": {},
    "query_project_resources": {},
    "blackboard_read": {"board": "outschema", "path": "a"},
    "blackboard_list_keys": {"board": "outschema"},
    "shader_check_compile": {"shader_path": "res://swatch.gdshader"},
}


def type_of(value: object) -> str:
    """The JSON type name for a Python value.

    `bool` is a subclass of `int`, so the obvious loop reports every `true` as a
    number and prints nineteen type mismatches against a server that has none.
    Booleans are answered first for that reason.
    """
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    for name in ("string", "object", "array"):
        if isinstance(value, JSON_TYPES[name]):
            return name
    return "?"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    with Session(project=args.project) as session:
        tools = session.tools()
        published = [t for t in tools if t.get("outputSchema")]
        print(f"{len(published)} of {len(tools)} published entries carry an outputSchema\n")
        session.call("blackboard_write",
                     {"board": "outschema", "path": "a", "value": 1, "author": "vibe"})

        total_missing = 0
        total_mistyped = 0
        for tool in sorted(published, key=lambda t: t["name"]):
            name = tool["name"]
            schema = tool["outputSchema"]
            declared = schema.get("properties") or {}
            required = schema.get("required") or []
            payload, is_error = session.call(name, ARGUMENTS.get(name, {}))
            if is_error or not isinstance(payload, dict):
                text = json.dumps(payload) if isinstance(payload, dict) else repr(payload)
                print(f"{name}: the call did not succeed, so its answer proves nothing\n"
                      f"    {text[:200]}")
                continue

            missing = [p for p in required if p not in payload]
            mistyped = []
            for prop, spec in declared.items():
                if prop not in payload or not isinstance(spec, dict) or "type" not in spec:
                    continue
                want = spec["type"]
                wanted = want if isinstance(want, list) else [want]
                if type_of(payload[prop]) not in wanted and not (
                        "number" in wanted and type_of(payload[prop]) == "integer"):
                    mistyped.append(f"{prop}: declared {want}, answered {type_of(payload[prop])}")
            undescribed = sorted(set(payload) - set(declared))

            total_missing += len(missing)
            total_mistyped += len(mistyped)
            flag = "  <-- " if (missing or mistyped) else "      "
            print(f"{flag}{name}: {len(declared)} properties declared of "
                  f"{len(payload)} answered, {len(required)} required")
            if missing:
                print(f"        required but absent from the answer: {missing}")
            for row in mistyped:
                print(f"        type mismatch: {row}")
            if undescribed:
                print(f"        answered but never declared ({len(undescribed)}): "
                      f"{undescribed[:8]}{' ...' if len(undescribed) > 8 else ''}")

        print(f"\n{total_missing} required properties absent, {total_mistyped} type mismatches")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
