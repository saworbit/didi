"""Block until an editor or game session shows up on a project, or give up.

A live probe run that begins with `sleep 20` is two bugs waiting: on a fast
machine it throws away time, and on a slow one -- a CI runner importing a
project for the first time -- it reports "no session" for an editor that was
still starting. Both look exactly like the finding the run is there to look for,
which is the worst way to be wrong.

So poll the thing the probe will actually use: `runtime_list_sessions`, through
the same server binary, on the same project root. It prints each attempt, so a
log shows how long the editor took as a matter of record rather than folklore.

    python tools/vibe/wait_for_session.py PROJECT [--kind editor] [--timeout 120]

Exit status is 0 once a session of that kind is alive, 1 if the wait ran out --
so a workflow step can decide whether the probes after it mean anything.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_client import batch, call, unwrap  # noqa: E402


def sessions(project: Path, binary: str | None) -> list[dict]:
    responses, _ = batch([call("runtime_list_sessions", {}, 1)], project=project, binary=binary)
    for response in responses:
        payload, _ = unwrap(response)
        if isinstance(payload, dict) and "sessions" in payload:
            return [entry for entry in payload["sessions"] if isinstance(entry, dict)]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("-b", "--binary")
    parser.add_argument("--kind", default="editor", help="editor or game.")
    parser.add_argument("--timeout", type=float, default=120.0, help="Seconds.")
    parser.add_argument("--interval", type=float, default=3.0)
    args = parser.parse_args()

    project = Path(args.project)
    deadline = time.monotonic() + args.timeout
    attempt = 0
    while time.monotonic() < deadline:
        attempt += 1
        try:
            found = sessions(project, args.binary)
        except Exception as error:  # a server that will not start is worth printing, not hiding
            print(f"attempt {attempt}: the server did not answer ({error})", flush=True)
            found = []
        alive = [s for s in found if s.get("kind") == args.kind and s.get("alive") is not False]
        print(
            f"attempt {attempt}: {len(found)} session(s), {len(alive)} alive {args.kind}",
            flush=True,
        )
        if alive:
            for entry in alive:
                endpoint = entry.get("endpoint", "")
                print(f"  pid={entry.get('pid')} engine={entry.get('engine_version')}", flush=True)
                print(f"  endpoint ({len(str(endpoint))} bytes): {endpoint}", flush=True)
            return 0
        time.sleep(args.interval)

    print(f"no alive {args.kind} session after {args.timeout:.0f}s", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
