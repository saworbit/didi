"""Call tools and read what comes back, without writing a script each time.

The loop this supports is: guess at a call, look at the answer, notice something
off, narrow it. Anything that adds friction to that loop -- writing a driver,
reformatting output, scrolling past a `session` block repeated on every line --
costs findings, because the interesting ones come from the fourth or fifth
variation of a call rather than the first.

So the output here is deliberately lossy by default. Every live result embeds
the same several-hundred-byte `session` object; it is folded to its `kind` and
`pid` unless `--full` is passed. What survives is the part that differs between
two calls, which is the part being compared.

Usage::

    # what does this tool actually accept?
    python tools/vibe/probe.py --schema scene_add_to_group

    # one call
    python tools/vibe/probe.py -p SANDBOX --call scene_get_property \\
        '{"target_node": "/root/Main/Child", "property_name": "position"}'

    # a sequence, in one server process so tokens and attachments survive
    python tools/vibe/probe.py -p SANDBOX --calls tools/vibe/probes/schema_enforcement.json

A `--calls` file is a JSON list of `[name, arguments]` pairs, or of
`{"name": ..., "arguments": ...}` objects. Keeping probes as files is what makes
a finding reproducible in the issue: the file is the repro.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_client import Session  # noqa: E402

SESSION_KEYS = ("session", "handshake")


def fold(payload: object, full: bool = False) -> object:
    """Shrink the parts of a result that are identical on every call."""
    if full or not isinstance(payload, dict):
        return payload
    folded = {}
    for key, value in payload.items():
        if key in SESSION_KEYS and isinstance(value, dict):
            folded[key] = {
                "kind": value.get("kind"),
                "pid": value.get("pid"),
                "folded": "pass --full for the whole session descriptor",
            }
        else:
            folded[key] = value
    return folded


def render(name: str, arguments: dict, payload: object, is_error: bool | None, width: int) -> str:
    head = f"--- {name} {json.dumps(arguments)}"
    body = json.dumps(payload, sort_keys=True)
    if width and len(body) > width:
        body = body[:width] + f" ... (+{len(body) - width} bytes, raise --width)"
    marker = "ERR " if is_error else "ok  "
    return f"{head}\n    {marker}{body}"


def load_calls(path: Path) -> list[tuple[str, dict]]:
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    calls: list[tuple[str, dict]] = []
    for entry in raw:
        if isinstance(entry, dict):
            calls.append((entry["name"], entry.get("arguments", {})))
        else:
            name, arguments = entry
            calls.append((name, arguments or {}))
    return calls


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", help="Project root to serve. A sandbox, ideally.")
    parser.add_argument("-b", "--binary", help="didi executable. Defaults to the newest build.")
    parser.add_argument("--call", nargs=2, metavar=("NAME", "JSON"), action="append", default=[])
    parser.add_argument("--calls", help="A JSON file of calls, run in order in one process.")
    parser.add_argument("--schema", action="append", default=[], help="Print a tool's inputSchema.")
    parser.add_argument("--list", action="store_true", help="Print every tool name and its mode.")
    parser.add_argument("--dump-tools", help="Write the whole tools/list result to this file.")
    parser.add_argument("--full", action="store_true", help="Do not fold session descriptors.")
    parser.add_argument("--width", type=int, default=700, help="Truncate each result body here.")
    parser.add_argument("--log-level", default="ERROR")
    parser.add_argument("--yolo", action="store_true", help="Start the server with --yolo.")
    args = parser.parse_args()

    extra = ["--yolo"] if args.yolo else []
    session = Session(
        project=args.project, binary=args.binary, log_level=args.log_level, extra_args=extra
    )
    try:
        if args.list or args.schema or args.dump_tools:
            tools = session.tools()
            if args.dump_tools:
                Path(args.dump_tools).write_text(json.dumps(tools, indent=1), encoding="utf-8")
                print(f"{len(tools)} tools -> {args.dump_tools}")
            if args.list:
                for tool in sorted(tools, key=lambda t: t["name"]):
                    meta = tool.get("_meta", {}).get("didi", {})
                    print(f"{tool['name']:34} {meta.get('currentMode', '?')}")
            for wanted in args.schema:
                for tool in tools:
                    if tool["name"] == wanted:
                        print(f"--- {wanted}")
                        print(json.dumps(tool["inputSchema"], indent=1))
                        break
                else:
                    print(f"--- {wanted}: no such tool")

        calls: list[tuple[str, dict]] = [(name, json.loads(raw)) for name, raw in args.call]
        if args.calls:
            calls += load_calls(Path(args.calls))
        for name, arguments in calls:
            payload, is_error = session.call(name, arguments)
            print(render(name, arguments, fold(payload, args.full), is_error, args.width))
    finally:
        session.close()
        stderr = session.stderr().strip()
        if stderr:
            print("=== server stderr ===")
            print(stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
