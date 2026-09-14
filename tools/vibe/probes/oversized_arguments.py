"""The other end of the string: what is the largest argument a tool accepts?

Session eight sent `""` to every required string parameter and sorted the
answers by whether the schema carried `minLength`. That probe asked what the
smallest accepted string is. Nobody has asked what the largest is.

The two questions are not symmetric. A missing `minLength` costs a confusing
error message; a missing `maxLength` is the server agreeing to hold whatever a
caller sends, on a transport with no frame limit, before any handler has looked
at it. An agent that loops while building a `new_definition`, or pastes a file
into a `method_name`, finds that edge by accident.

Three parts:

* a census of `maxLength` across every string parameter on the surface, beside
  the `minLength` count session eight established, so the asymmetry is a number
  rather than an impression;
* a megabyte sent to required string parameters of several kinds -- a path, an
  identifier, a body, a search needle -- to see which layer answers;
* the same size sent as a *file*, since a tool that bounds its arguments may
  still read an unbounded file into a response.

Destroys files. Throwaway sandbox only.

    python tools/vibe/probes/oversized_arguments.py --project SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402


def census(session: Session) -> None:
    tools = session.tools()
    string_params = 0
    with_max = 0
    with_min = 0
    unbounded_required: list[str] = []
    for tool in tools:
        schema = tool.get("inputSchema") or {}
        required = set(schema.get("required") or [])
        for name, spec in (schema.get("properties") or {}).items():
            if spec.get("type") != "string":
                continue
            string_params += 1
            has_max = "maxLength" in spec
            if has_max:
                with_max += 1
            if "minLength" in spec:
                with_min += 1
            if not has_max and name in required:
                unbounded_required.append(f"{tool['name']}.{name}")
    print(f"    string parameters on the surface : {string_params}")
    print(f"    carrying minLength               : {with_min}")
    print(f"    carrying maxLength               : {with_max}")
    print(f"    required and unbounded           : {len(unbounded_required)}")
    print(f"    first twelve: {unbounded_required[:12]}")


def timed(session: Session, name: str, arguments: dict) -> tuple[object, bool | None, float]:
    started = time.monotonic()
    payload, is_error = session.call(name, arguments)
    return payload, is_error, time.monotonic() - started


def show(label: str, payload: object, is_error: bool | None, seconds: float, width: int = 260) -> None:
    marker = "ERR " if is_error else "ok  "
    body = json.dumps(payload)
    print(f"--- {label}  [{seconds:.2f}s, {len(body)} bytes back]\n    {marker}{body[:width]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    parser.add_argument("--megabytes", type=int, default=1)
    args = parser.parse_args()

    root = Path(args.project)
    blob = "A" * (args.megabytes * 1024 * 1024)

    with Session(project=args.project, binary=args.binary) as session:
        print("== A: how many string parameters are bounded at all?")
        census(session)

        print(f"\n== B: {args.megabytes} MB sent to required string parameters")
        cases = [
            ("script_create", {"file_path": f"res://{blob}.gd", "source_text": "extends Node\n"}),
            ("script_create", {"file_path": "res://big.gd", "source_text": blob}),
            ("script_get_symbols", {"file_path": f"res://{blob}.gd"}),
            ("project_search_text", {"query": blob}),
            (
                "script_patch_method",
                {
                    "file_path": "res://player.gd",
                    "method_name": blob,
                    "new_definition": "func x() -> void:\n\tpass\n",
                    "dry_run": True,
                },
            ),
            ("blackboard_write", {"key": blob, "value": "x"}),
            ("blackboard_write", {"key": "big", "value": blob}),
            ("scene_get_property", {"node_path": blob, "property": "name"}),
        ]
        for name, arguments in cases:
            label_args = {
                key: (f"<{len(value)} bytes>" if isinstance(value, str) and len(value) > 200 else value)
                for key, value in arguments.items()
            }
            payload, is_error, seconds = timed(session, name, arguments)
            show(f"{name} {json.dumps(label_args)}", payload, is_error, seconds)

        print(f"\n== C: a {args.megabytes} MB file read back through the tools")
        big = root / "huge.gd"
        body = "extends Node\n\n" + "".join(
            f"func m{index}() -> int:\n\treturn {index}\n\n" for index in range(20000)
        )
        big.write_text(body, encoding="utf-8", newline="")
        print(f"    wrote {big.stat().st_size} bytes, 20000 methods")
        for name, arguments in [
            ("script_get_symbols", {"file_path": "res://huge.gd"}),
            ("script_check_syntax", {"file_path": "res://huge.gd"}),
            ("analyze_script_diagnostics", {"file_path": "res://huge.gd"}),
            ("project_search_text", {"query": "return 19999"}),
        ]:
            payload, is_error, seconds = timed(session, name, arguments)
            show(f"{name} {json.dumps(arguments)}", payload, is_error, seconds)
        big.unlink(missing_ok=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
