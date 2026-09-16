"""`resources/subscribe`, driven by a second process that actually writes.

`initialize` publishes `resources.subscribe: true`, and the server runs a board
watcher that sends `notifications/resources/updated` when a blackboard file
stamp moves. Fourteen sessions have read the subscription *surface* -- #513
found `resources/read` serving a non-default board as `text/plain` because only
`default` is a registered URI -- and none has ever subscribed and then made the
thing change. The blackboard exists because two agents are expected, so the
notification is the whole point of it.

The subscriber and the writer are separate server processes, because one process
writing to its own board is the configuration where this cannot go wrong.

Rows:

* subscribe to a registered board URI, have the other process write, read the
  notification off the wire;
* the same for `blackboard://<other board>/state`, which `resources/list` does
  not carry;
* subscribe to a `godot://` URI, which nothing publishes updates for;
* subscribe to a URI nobody serves at all;
* unsubscribe, write again, and check the stream went quiet.

Each row prints what arrived and how long it took, because a watcher that polls
has a floor and a caller cannot see it from the capability flag.
"""

from __future__ import annotations

import argparse
import json
import queue
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session, batch, call  # noqa: E402


class Listening(Session):
    """A Session whose stdout is drained by a thread, so notifications survive.

    `Session._read` takes one line per request. A notification arriving between
    two requests is read as the answer to the second one, which is how a probe
    of this surface silently becomes a probe of request/response ordering.
    """

    def __init__(self, *args, **kwargs) -> None:
        self._answers: "queue.Queue[dict]" = queue.Queue()
        self._notices: "queue.Queue[tuple[float, dict]]" = queue.Queue()
        self._pump: threading.Thread | None = None
        super().__init__(*args, **kwargs)
        self._pump = threading.Thread(target=self._drain, daemon=True)
        self._pump.start()

    def _drain(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            try:
                message = json.loads(line.decode())
            except ValueError:
                continue
            if "id" in message and message["id"] is not None:
                self._answers.put(message)
            else:
                self._notices.put((time.monotonic(), message))

    def _read(self) -> dict:
        if self._pump is None:
            return super()._read()
        return self._answers.get(timeout=60)

    def notices(self, within: float) -> list[tuple[float, dict]]:
        deadline = time.monotonic() + within
        out: list[tuple[float, dict]] = []
        while time.monotonic() < deadline:
            try:
                out.append(self._notices.get(timeout=max(0.05, deadline - time.monotonic())))
            except queue.Empty:
                break
        return out


def write_from_another_process(project: str, board: str, value: int) -> None:
    """The control. A write that silently failed reads exactly like a watcher
    that never fired, and the first run of this probe was the second when it was
    really the first -- a leading slash made an empty path segment."""
    responses, _ = batch(
        [call("blackboard_write",
              {"board": board, "path": "counter", "value": value, "author": "the-other-agent"}, 1)],
        project=project,
        timeout=60,
    )
    from mcp_client import unwrap

    payload, is_error = unwrap(responses[-1])
    marker = "WROTE" if not is_error else "THE WRITE FAILED"
    print(f"    [{marker}] {json.dumps(payload)[:170]}")


def row(session: Listening, project: str, uri: str, board: str | None,
        value: int, wait: float) -> None:
    print(f"\n--- subscribe {uri}")
    response = session.request("resources/subscribe", {"uri": uri})
    if "error" in response:
        print(f"    refused: {json.dumps(response['error'])[:220]}")
        return
    print(f"    accepted: {json.dumps(response.get('result'))[:160]}")
    if board is None:
        print("    (nothing to change; listening anyway)")
        arrivals = session.notices(wait)
    else:
        start = time.monotonic()
        write_from_another_process(project, board, value)
        arrivals = session.notices(wait)
        arrivals = [(t - start, m) for t, m in arrivals]
    if not arrivals:
        print(f"    nothing arrived in {wait:.0f}s")
        return
    for delay, message in arrivals:
        delay_text = f"{delay:.1f}s" if isinstance(delay, float) and delay < 1e6 else "?"
        print(f"    +{delay_text} {message.get('method')} "
              f"{json.dumps(message.get('params'))[:160]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--wait", type=float, default=12.0)
    args = parser.parse_args()

    with Listening(project=args.project) as session:
        caps = session.initialize_result["result"]["capabilities"]
        print(f"capabilities.resources: {json.dumps(caps.get('resources'))}")
        listed = [r["uri"] for r in session.request("resources/list", {})["result"]["resources"]]
        print(f"resources/list carries: {listed}")

        row(session, args.project, "blackboard://default/state", "default", 1, args.wait)
        row(session, args.project, "blackboard://sub14/state", "sub14", 1, args.wait)
        row(session, args.project, "godot://project/tree", None, 0, 3.0)
        row(session, args.project, "blackboard://default/nonsense", "default", 2, 3.0)
        row(session, args.project, "file:///etc/passwd", None, 0, 1.0)

        print("\n--- unsubscribe blackboard://default/state, then write again")
        response = session.request("resources/unsubscribe", {"uri": "blackboard://default/state"})
        print(f"    {json.dumps(response.get('result', response.get('error')))[:200]}")
        write_from_another_process(args.project, "default", 99)
        after = session.notices(args.wait)
        print(f"    notifications after unsubscribing: {len(after)}")
        for _, message in after:
            print(f"      {message.get('method')} {json.dumps(message.get('params'))[:140]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
