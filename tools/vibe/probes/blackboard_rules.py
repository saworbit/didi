"""The blackboard's own stated rules, tested against what the handler does.

Every assertion here is quoted from a parameter description on the published
schema: *"Only the lease holder may [complete a task], because completing
someone else's releases its lease"*, *"How far along the task is, 0 to 1"*,
*"board name … letters, digits, underscore and hyphen"*, *"Segments cannot be
empty, '.' or '..'"*. A description is a claim about the handler and nothing
checks it -- that is the lesson #482 and #484 left behind.

Most of these hold, and the bounds are genuinely well guarded: board names,
path segments, nesting depth, lease seconds, ttl and value size all refuse the
right things with the right codes. Three did not:

* `progress` is documented as 0 to 1 and the handler wants an integer 0 to 100,
  so the documented range holds two legal values and a completed task reports
  `100` (#528);
* a contended `blackboard_task_claim` answers `isError: false` with
  `claimed: false` and an English `reason`, where `task_complete` and
  `task_update` return a `409 conflict` for the same state -- a failure hidden
  in a success payload, where no error census will look (#529);
* completing an already-completed task is `400 invalid_arguments` when the
  arguments were all valid and the state was not (#530).

Contention on a claim is what the board is *for*, so these are the paths a
multi-agent session takes most often.

Usage::

    python tools/vibe/probes/blackboard_rules.py SANDBOX

Writes to a board named `leases-<epoch>`. A board is a file under
`.didi/blackboard/` and it outlives the process, so a fixed name meant the
second run of this probe met the first run's completed task and reported its
own leftovers as conflicts. One board per run, and the task id comes from the
create call rather than being assumed to be `TASK-1`. No editor needed.

All three findings are fixed as of 2.0.0, and the probe is kept for the reason
every probe here is kept: re-run it and the claim conflict is a `409` carrying
`reason_code: already_leased`, the second completion is a `409` carrying
`already_completed`, and `progress` is a whole percentage on the way in and on
the way out. A probe that starts showing the old answers again is a regression
nobody wrote a test for.
"""

from __future__ import annotations

import argparse
import json
import secrets
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# One per run: see the module docstring. A fixed name makes the second run
# report the first run's leftovers as findings. Random rather than a clock,
# because two runs inside the same second collided on an epoch suffix.
BOARD = f"leases-{secrets.token_hex(4)}"

# Board names. The rule is letters, digits, underscore, hyphen, 1 to 64 chars;
# boards are files under .didi/blackboard, so anything that reaches the
# filesystem as written is worth asking about.
BOARDS = ["../escape", "a/b", "..", "", "CON", "x" * 300, "héllo"]

# Path segments. The rule is that a segment cannot be empty, '.', '..', or hold
# a control character -- which is the rule file paths do not have (#525).
PATHS = ["..", "a..b", "a/../b", "", ".", "a//b", "ctl" + chr(0) + "x"]


def show(label: str, result: tuple, limit: int = 320) -> None:
    payload, is_error = result
    print(f"\n=== {label}  isError={is_error}\n{json.dumps(payload, ensure_ascii=False)[:limit]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project", help="A sandbox project.")
    args = parser.parse_args()
    session = Session(project=args.project)

    print("--- the lease rules the schema states")
    created = session.call("blackboard_task_create",
                           {"board": BOARD, "title": "t1", "description": "d"})
    show("create", created)
    # Whatever the board called it. Assuming TASK-1 is what made a re-run read
    # the previous run's task.
    task_id = created[0]["task"]["task_id"]
    show("alice claims", session.call("blackboard_task_claim",
                                      {"board": BOARD, "agent_id": "alice"}))
    # The same conflict, asked of three tools. Two answer 409; one answers 200.
    show("bob claims the same task (#529)",
         session.call("blackboard_task_claim",
                      {"board": BOARD, "agent_id": "bob", "task_id": task_id}))
    show("bob completes alice's task",
         session.call("blackboard_task_complete",
                      {"board": BOARD, "agent_id": "bob", "task_id": task_id}))
    show("bob updates alice's task",
         session.call("blackboard_task_update",
                      {"board": BOARD, "agent_id": "bob", "task_id": task_id, "note": "hi"}))

    print("\n\n--- progress: the schema says 0 to 1 (#528)")
    for value in (0.5, 5.0, -3, 100):
        show(f"progress={value!r}",
             session.call("blackboard_task_update",
                          {"board": BOARD, "agent_id": "alice", "task_id": task_id,
                           "progress": value}), limit=200)
    show("alice completes -- note the progress it reports",
         session.call("blackboard_task_complete",
                      {"board": BOARD, "agent_id": "alice", "task_id": task_id}))
    show("alice completes again (#530)",
         session.call("blackboard_task_complete",
                      {"board": BOARD, "agent_id": "alice", "task_id": task_id}), limit=240)

    print("\n\n--- lease bounds")
    show("create t2", session.call("blackboard_task_create",
                                   {"board": BOARD, "title": "t2", "description": "d"}), limit=160)
    for seconds in (0, -60):
        show(f"claim lease_seconds={seconds}",
             session.call("blackboard_task_claim",
                          {"board": BOARD, "agent_id": "alice", "task_id": "TASK-2",
                           "lease_seconds": seconds}), limit=200)

    print("\n\n--- board names: letters, digits, underscore, hyphen")
    for board in BOARDS:
        payload, is_error = session.call("blackboard_write",
                                         {"board": board, "path": "k", "value": 1})
        print(f"  {board[:24]!r:30} isError={is_error!s:5} "
              f"{json.dumps(payload, ensure_ascii=False)[:150]}")

    print("\n--- path segments: not empty, not '.', not '..', no control characters")
    for path in PATHS:
        payload, is_error = session.call("blackboard_write", {"path": path, "value": 1})
        print(f"  {path!r:20} isError={is_error!s:5} "
              f"{json.dumps(payload, ensure_ascii=False)[:150]}")

    print("\n--- value bounds")
    deep: dict = {"a": 1}
    for _ in range(200):
        deep = {"n": deep}
    show("200 levels of nesting", session.call("blackboard_write",
                                               {"path": "deep", "value": deep}), limit=240)
    show("ttl_seconds=-5", session.call("blackboard_write",
                                        {"path": "ttl", "value": 1, "ttl_seconds": -5}), limit=240)

    session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
