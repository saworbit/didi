"""Which asset_reimport makes the editor print "Task 'reimport' already exists".

The live harness printed three engine errors on every run and passed, because
nothing read the editor's log:

    ERROR: Task 'reimport' already exists.            (progress_dialog.cpp add_task)
    ERROR: Condition "!tasks.has(p_task)" is true. Returning: canceled   (task_step)
    ERROR: Condition "!tasks.has(p_task)" is true.    (end_task)

That is two EditorProgress("reimport") tasks alive at once, the inner one
removing the task the outer is still stepping. This replays the harness's import
sequence against a live editor -- a never-seen PNG with a script, then the same
PNG again at once -- and prints what each call's answer carries under
`engine_diagnostics`, which says which call the lines belong to. Needs a build
with per-call engine diagnostics.

    python tools/vibe/probes/reimport_overlap.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402
from probes.animation_library import attach  # noqa: E402

VALID_PNG = ("iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAYAAADED76LAAAAG0lEQVR4nGNgYPj//z8DAwhhp/FKMoCo4WACAIMXf4EBK1dXAAAAAElFTkSuQmCC")
RUN = "%04x" % (int(time.time()) & 0xFFFF)


def diagnostics(payload: object) -> list[str]:
    if not isinstance(payload, dict):
        return []
    found = payload.get("engine_diagnostics")
    if found is None:
        found = ((payload.get("error") or {}).get("data") or {}).get("engine_diagnostics")
    return [f"{d.get('level')}: {d.get('message')} ({d.get('function')})" for d in (found or [])]


def step(session: Session, label: str, paths: list[str]) -> None:
    payload, errored = session.call("asset_reimport", {"paths": paths, "timeout_ms": 10000})
    summary = {k: payload.get(k) for k in ("imported", "reimported", "announced", "failed")
               if isinstance(payload, dict) and k in payload}
    print(f"  {label:44} {'REFUSED' if errored else 'ok'} {json.dumps(summary)}")
    if errored:
        error = payload.get("error", payload) if isinstance(payload, dict) else payload
        print(f"      refusal: {json.dumps(error)[:600]}")
    for line in diagnostics(payload):
        print(f"      engine_diagnostics: {line}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()
    project = Path(args.project).resolve()

    with Session(project=str(project)) as session:
        if attach(session, project) is None:
            print("No live editor on this project.")
            return 2
        first = f"vibe_ri_{RUN}_a.png"
        second = f"vibe_ri_{RUN}_b.png"
        (project / first).write_bytes(base64.b64decode(VALID_PNG))
        print("=== the harness's sequence ===")
        step(session, "a never-seen PNG and a script", [f"res://{first}", "res://player.gd"])
        step(session, "the same PNG again, at once", [f"res://{first}"])
        print("=== a reimport while a new file waits to be scanned ===")
        (project / second).write_bytes(base64.b64decode(VALID_PNG))
        step(session, "the first PNG again, second one on disk", [f"res://{first}"])
        step(session, "then the second one", [f"res://{second}"])
        print()
        print(session.engine_summary())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
