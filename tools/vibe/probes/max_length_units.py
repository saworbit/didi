"""Every published `maxLength`, filled to the bound with characters that are not ASCII.

JSON Schema counts a string's length in characters. #663 found
`project_search_text` counting bytes against a `maxLength` of 256, so a query of
100 Japanese characters passed any client that validated against the schema and
was then refused by the server, in a second unit. Vibe session nineteen found the
same bound in bytes three more times, each written after #663: `audio_add_bus`'s
`name` and `send`, `project_add_export_preset`'s `name`, and
`project_analyze_impact`'s `target`. A fix scoped to one tool keeps coming back
in the next one, so this asks every tool at once.

For every string parameter with a `maxLength`, the probe builds the smallest
call the schema allows (each other required parameter from its example, default,
first enum value or a plain value of its type), fills the parameter with
`maxLength` copies of U+97F3 (three bytes each), and calls the tool. The schema
says that argument is valid, so a refusal that talks about its length is the
finding. Refusals for anything else are printed and ignored: most calls built
this way fail for a reason unrelated to length, which is fine, because a length
check that sits behind another refusal was not reached and is not claimed.

Offline only. A live-only tool answers 503 before its handler runs, so its bound
is not tested here, and the probe says how many were skipped that way.

    python tools/vibe/probes/max_length_units.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

WIDE = chr(0x97F3)  # three bytes in UTF-8, one character
LENGTH_WORDS = re.compile(r"\b(bytes?|characters? long|at most|too long|longer than|exceeds?|length)\b", re.I)
# Never called: these start processes, write outside the sandbox's own files,
# or wait on a person.
SKIP = {"runtime_launch", "project_export", "csharp_check_build", "runtime_recover_editor",
        "runtime_restore_checkpoint", "gridmap_export_mesh_library"}


def ascii(text: str) -> str:
    """A refusal quotes the argument back, and a Windows console cannot print it."""
    return text.encode("ascii", "backslashreplace").decode()


def plain_value(schema: dict) -> object:
    for key in ("examples",):
        if isinstance(schema.get(key), list) and schema[key]:
            return schema[key][0]
    if "default" in schema:
        return schema["default"]
    if isinstance(schema.get("enum"), list) and schema["enum"]:
        return schema["enum"][0]
    kind = schema.get("type")
    if isinstance(kind, list):
        kind = next((k for k in kind if k != "null"), kind[0])
    if kind == "string":
        return "a" * max(1, schema.get("minLength", 1))
    if kind == "integer":
        return schema.get("minimum", 1)
    if kind == "number":
        return schema.get("minimum", 0)
    if kind == "boolean":
        return False
    if kind == "array":
        return []
    if kind == "object":
        return {}
    return "a"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    findings, unreached, live_only = [], 0, 0
    with Session(args.project, binary=args.binary, editor_log=False) as session:
        tools = session.tools()
        for tool in sorted(tools, key=lambda t: t["name"]):
            name = tool["name"]
            modes = ((tool.get("_meta") or {}).get("didi") or {}).get("executionModes") or []
            properties = (tool.get("inputSchema") or {}).get("properties") or {}
            required = (tool.get("inputSchema") or {}).get("required") or []
            bounded = [(key, spec) for key, spec in properties.items()
                       if isinstance(spec, dict) and spec.get("type") == "string"
                       and isinstance(spec.get("maxLength"), int)]
            if not bounded or name in SKIP:
                continue
            if modes == ["live"]:
                live_only += len(bounded)
                continue
            for key, spec in bounded:
                arguments = {other: plain_value(properties.get(other, {}))
                             for other in required if other != key}
                arguments[key] = WIDE * spec["maxLength"]
                if "dry_run" in properties:
                    arguments["dry_run"] = True
                payload, errored = session.call(name, arguments)
                if not errored:
                    print(f"  ok       {name}.{key} ({spec['maxLength']} characters, "
                          f"{3 * spec['maxLength']} bytes) accepted")
                    continue
                error = payload.get("error", payload) if isinstance(payload, dict) else payload
                message = error.get("message", "") if isinstance(error, dict) else str(error)
                if LENGTH_WORDS.search(message):
                    findings.append((name, key, spec["maxLength"], message))
                    print(f"  FINDING  {name}.{key}: {ascii(message)[:300]}")
                else:
                    unreached += 1
                    print(f"  other    {name}.{key}: {ascii(message)[:140]}")
    print(f"\n{len(findings)} bound(s) refused in a unit the schema does not use; "
          f"{unreached} refused for another reason first, so their length check was not reached; "
          f"{live_only} live-only parameter(s) not asked.")
    return 1 if findings else 0


if __name__ == "__main__":
    raise SystemExit(main())
