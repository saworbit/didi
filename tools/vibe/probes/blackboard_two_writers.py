"""Two agents writing the blackboard at the same time, which is why it exists.

The blackboard is the one part of this surface whose whole premise is more than
one client: the task lease, `author`, `reason`, a board per team. Session seven
put two servers against one editor and session ten claimed a task from two
servers and got a clean `409 already_leased`. Nobody has had two servers *write*
at once.

The lease covers tasks. Keys have no lease, no revision and no compare-and-set
parameter -- `blackboard_write` takes `path` and `value`, and the only thing it
says about what was there before is `replaced`, a boolean. So this asks the two
questions that separates:

* Can one agent lose the other's update to the *same* key without either being
  told? `replaced: true` cannot tell "I replaced the value I read" from "I
  replaced a value somebody else wrote after I read it", which is the whole
  difference.
* Can one agent lose the other's write to a *different* key? A board that is
  one file rewritten whole has that failure available to it, and it is the
  worse one, because the two agents were never touching the same data and
  nothing in either call is about the other's key.

Interleaving is forced by hand rather than hoped for: each `Session` is its own
server process, and the calls are ordered read-A, read-B, write-A, write-B, so
the race is deterministic. A probe that fires two threads and hopes is a probe
whose green run means nothing.

    python tools/vibe/probes/blackboard_two_writers.py -p SANDBOX

The board name carries the run's own pid: a board is a file that outlives the
process, so a fixed name means the second run meets the first run's leftovers
and reports them as findings (the lesson `blackboard_rules.py` learned the
hard way).
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402


def line(label: str, payload: object, is_error: object) -> None:
    text = json.dumps(payload) if not isinstance(payload, str) else payload
    print(f"  {label}: isError={is_error} {text[:360]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    board = f"two-writers-{os.getpid()}"
    project = Path(args.project)

    with Session(project=project, binary=args.binary) as a, \
            Session(project=project, binary=args.binary) as b:
        print(f"board: {board}")
        print(f"server A pid {a.process.pid}, server B pid {b.process.pid}")

        print("\n== 1. the same key, read by both before either writes")
        line("A seed ", *a.call("blackboard_write", {"board": board, "path": "counter", "value": {"n": 0}, "author": "A"}))
        line("A read ", *a.call("blackboard_read", {"board": board, "path": "counter", "include_metadata": True}))
        line("B read ", *b.call("blackboard_read", {"board": board, "path": "counter", "include_metadata": True}))
        line("A write", *a.call("blackboard_write", {"board": board, "path": "counter", "value": {"n": 1}, "author": "A"}))
        line("B write", *b.call("blackboard_write", {"board": board, "path": "counter", "value": {"n": 1}, "author": "B"}))
        payload, is_error = a.call("blackboard_read", {"board": board, "path": "counter", "include_metadata": True})
        line("final  ", payload, is_error)
        print("  expected if updates compose: n == 2, or a refusal on the second write.")

        print("\n== 2. two different keys, written without either server re-reading")
        # Both servers have now read the board once. Each writes a key of its
        # own: nothing here is contended, and an agent has no reason to expect
        # its neighbour's key to be involved at all.
        line("A -> alpha", *a.call("blackboard_write", {"board": board, "path": "alpha", "value": {"owner": "A"}, "author": "A"}))
        line("B -> beta ", *b.call("blackboard_write", {"board": board, "path": "beta", "value": {"owner": "B"}, "author": "B"}))
        line("keys      ", *a.call("blackboard_list_keys", {"board": board}))
        print("  expected: alpha and beta and counter all present.")

        print("\n== 3. interleaved patch, same shape as 1")
        line("A seed  ", *a.call("blackboard_write", {"board": board, "path": "doc", "value": {"items": []}, "author": "A"}))
        line("A read  ", *a.call("blackboard_read", {"board": board, "path": "doc"}))
        line("B read  ", *b.call("blackboard_read", {"board": board, "path": "doc"}))
        line("A patch ", *a.call("blackboard_patch", {"board": board, "author": "A", "operations": [
            {"op": "set", "path": "doc.items", "value": ["from-A"]}]}))
        line("B patch ", *b.call("blackboard_patch", {"board": board, "author": "B", "operations": [
            {"op": "set", "path": "doc.other", "value": "from-B"}]}))
        line("final   ", *a.call("blackboard_read", {"board": board, "path": "doc", "deep": True}))
        print("  expected: both items and other survive.")

        print("\n== 4. does a write say anything about the value it displaced?")
        print("  `replaced` is the only field about the previous value. It is true")
        print("  whether the displaced value was the caller's own or a stranger's.")

        print("\n== 5. one server clears the board the other is working on")
        line("A create ", *a.call("blackboard_task_create", {"board": board, "title": "held by A", "author": "A"}))
        payload, _ = a.call("blackboard_task_list", {"board": board})
        task_id = None
        if isinstance(payload, dict):
            for task in payload.get("tasks", []) or []:
                if isinstance(task, dict) and task.get("title") == "held by A":
                    task_id = task.get("id") or task.get("task_id")
        print(f"  task id: {task_id}")
        if task_id:
            line("A claim  ", *a.call("blackboard_task_claim", {"board": board, "task_id": task_id, "author": "A"}))
        line("B clear  ", *b.call("blackboard_clear", {"board": board, "author": "B", "dry_run": True}))
        line("B keys   ", *b.call("blackboard_list_keys", {"board": board}))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
