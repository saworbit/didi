"""Requests that overlap, because a host is not obliged to wait.

Every probe in this directory until now has sent one request and read its
answer before sending the next. `Session.request` is literally write-then-
readline, so eight sessions have only ever tested didi as a strictly
alternating conversation.

That is not what the transport promises. JSON-RPC carries an `id` precisely so
that a client may have several requests outstanding, and MCP inherits it -- a
host that batches an agent's tool calls, or a UI that refreshes a status panel
while a mutation is running, will put two frames on the pipe before the first
answer comes back. Nothing in this repository has ever done that.

Three questions, none of which the alternating harness can ask:

* Does every request get an answer at all when they arrive together?
* Do the answers carry the right `id`, and does anything depend on their order?
* When two mutations to the same file overlap, is the result one of the two,
  or a mixture?

This writes frames straight to stdin without reading, then drains stdout, so
the server sees them arrive back to back.

    python tools/vibe/probes/pipelined_requests.py --project SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session, call, unwrap  # noqa: E402


def drain(session: Session, expected: int, timeout: float = 60.0) -> list[dict]:
    """Read `expected` lines of stdout, or as many as arrive before the timeout."""
    lines: list[dict] = []

    def reader() -> None:
        while len(lines) < expected:
            try:
                lines.append(session._read())
            except Exception as exc:  # noqa: BLE001 - the shape of the failure is the finding
                lines.append({"__read_failed__": str(exc)})
                return

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    thread.join(timeout)
    return lines


def send(session: Session, message: dict) -> None:
    session._write(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    root = Path(args.project)

    print("== A: four reads sent before any answer is read")
    with Session(project=args.project, binary=args.binary) as session:
        outstanding = [
            call("didi_control_room", {}, request_id=101),
            call("script_get_symbols", {"file_path": "res://player.gd"}, request_id=102),
            call("project_list_resources", {}, request_id=103),
            {"jsonrpc": "2.0", "id": 104, "method": "ping"},
        ]
        for message in outstanding:
            send(session, message)
        answers = drain(session, len(outstanding))
        print(f"    sent {len(outstanding)} ids 101-104, got {len(answers)} answers")
        print(f"    ids back, in order: {[a.get('id') for a in answers]}")
        for answer in answers:
            if "__read_failed__" in answer:
                print(f"    READ FAILED: {answer['__read_failed__']}")

    print("\n== B: two writes to the same file, overlapped")
    target = root / "race.gd"
    target.write_text(
        "extends Node\n\nfunc value() -> int:\n\treturn 0\n", encoding="utf-8", newline=""
    )
    with Session(project=args.project, binary=args.binary) as session:
        first = call(
            "script_patch_method",
            {
                "file_path": "res://race.gd",
                "method_name": "value",
                "new_definition": "func value() -> int:\n\treturn 111\n",
            },
            request_id=201,
        )
        second = call(
            "script_patch_method",
            {
                "file_path": "res://race.gd",
                "method_name": "value",
                "new_definition": "func value() -> int:\n\treturn 222\n",
            },
            request_id=202,
        )
        send(session, first)
        send(session, second)
        answers = drain(session, 2)
        for answer in answers:
            payload, is_error = unwrap(answer)
            marker = "ERR " if is_error else "ok  "
            print(f"    id {answer.get('id')}: {marker}{json.dumps(payload)[:220]}")
        print("    file after:\n      " + target.read_text(encoding="utf-8").replace("\n", "\n      "))

    print("\n== C: a read overlapped with a slow live call")
    with Session(project=args.project, binary=args.binary) as session:
        send(session, call("scene_get_hierarchy", {}, request_id=301))
        send(session, {"jsonrpc": "2.0", "id": 302, "method": "ping"})
        send(session, call("didi_control_room", {}, request_id=303))
        answers = drain(session, 3)
        print(f"    ids back, in order: {[a.get('id') for a in answers]}")
        for answer in answers:
            payload, is_error = unwrap(answer)
            marker = "ERR " if is_error else "ok  "
            print(f"    id {answer.get('id')}: {marker}{json.dumps(payload)[:160]}")

    print("\n== D: duplicate ids in flight")
    with Session(project=args.project, binary=args.binary) as session:
        send(session, call("didi_control_room", {}, request_id=401))
        send(session, call("project_list_resources", {}, request_id=401))
        answers = drain(session, 2)
        print(f"    two requests, both id 401; answers: {[a.get('id') for a in answers]}")
        print(f"    distinguishable: {len({json.dumps(a)[:80] for a in answers}) > 1}")

    target.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
