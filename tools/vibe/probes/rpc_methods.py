"""The JSON-RPC methods around `tools/call` that no probe had sent.

`protocol_edges.py` covers the frame -- a stale cursor, a null id, an unknown
tool, `arguments` that is not an object. Session six swept `resources/read` and
`prompts/*`. What was left is everything a host sends *around* a tool call:
`initialize` and its version negotiation, `ping`, the subscription methods, a
batch array, a cancellation, a progress token, and the capability-gated methods
a host probes before it knows what the server supports.

Two of this session's findings are here. `initialize` returned the same result
for `"2025-06-18"`, an empty string, a missing key and the integer `5`, so a
client could not tell a negotiated version from an ignored one (#531). And
`resources/subscribe` refused every `godot://` resource because "nothing else
changes without a tool call from this client", which is the opposite of true for
the runtime log stream and the editor state (#532).

Both are fixed as of 2.0.0, and this file is the regression probe for them.
Re-run it: a served revision comes back as itself, an unserved one comes back as
`2024-11-05`, a missing or non-string one is refused with `-32602` carrying
`supported` and `requested`, and the subscribe refusal names what Didi does not
publish rather than asserting the resource does not change.

Usage::

    python tools/vibe/probes/rpc_methods.py SANDBOX
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import CLIENT_INFO, INITIALIZE, Session, resolve_binary  # noqa: E402

# Every one of these is a malformed or unsupported `protocolVersion`, and the
# point of the list is that the answer never varies.
# "2026-07-28" and "2024-11-05" are served, so they must come back as
# themselves; everything else must come back as something else or be
# refused. Without a served revision in this list the probe could only ever
# show one half of the answer, which is how #531 read as "they are all the
# same" rather than "none of these is negotiated".
VERSIONS = ["2026-07-28", "2024-11-05", "2025-06-18", "2025-03-26",
            "1999-01-01", "", None, 5]


def show(label: str, value: object, limit: int = 600) -> None:
    text = json.dumps(value, ensure_ascii=False)
    if len(text) > limit:
        text = text[:limit] + " ...<truncated>"
    print(f"\n=== {label}\n{text}")


def raw_initialize(project: str | None, version: object) -> object:
    """One server process, initialized by hand so the version can be wrong.

    `Session` sends a well-formed handshake in its constructor, which is exactly
    what this needs to avoid.
    """
    argv = [str(resolve_binary()), "--log-level", "ERROR"]
    if project:
        argv += ["--project", project]
    process = subprocess.Popen(
        argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=dict(os.environ),
    )
    params: dict = {"capabilities": {}, "clientInfo": CLIENT_INFO}
    if version is not None:
        params["protocolVersion"] = version
    message = {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": params}
    assert process.stdin is not None and process.stdout is not None
    process.stdin.write((json.dumps(message) + "\n").encode())
    process.stdin.flush()
    line = process.stdout.readline()
    process.stdin.close()
    process.wait(timeout=10)
    return json.loads(line.decode()) if line else "<no answer>"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project", nargs="?", help="A project the server can open.")
    args = parser.parse_args()

    session = Session(project=args.project)
    show("initialize result", session.initialize_result)

    show("ping", session.request("ping"))
    show("second initialize, after notifications/initialized",
         session.request("initialize", INITIALIZE["params"]))

    # Capability-gated methods. A host may send these before it has read the
    # capabilities, so the refusal is part of the surface.
    show("logging/setLevel", session.request("logging/setLevel", {"level": "debug"}))
    show("completion/complete", session.request("completion/complete", {
        "ref": {"type": "ref/prompt", "name": "nope"},
        "argument": {"name": "a", "value": ""}}))

    # Subscriptions, against the URIs the server itself publishes.
    listed = session.request("resources/list", {})
    uris = [entry["uri"] for entry in listed.get("result", {}).get("resources", [])]
    print("\npublished resource uris:", uris)
    for uri in uris:
        show(f"subscribe {uri}", session.request("resources/subscribe", {"uri": uri}), limit=300)
    # Idempotency, both ways. `unsubscribe` on something never subscribed
    # answers `changed: false` rather than erroring, which is the shape
    # `runtime_detach_session` does not use (#537).
    show("unsubscribe never subscribed",
         session.request("resources/unsubscribe", {"uri": "blackboard://never/state"}), limit=300)
    show("subscribe twice",
         session.request("resources/subscribe", {"uri": "blackboard://b1/state"}), limit=300)
    show("subscribe twice, again",
         session.request("resources/subscribe", {"uri": "blackboard://b1/state"}), limit=300)

    # A params-level `_meta.progressToken`: does anything come back for it?
    session._next_id += 1  # noqa: SLF001 -- deliberately below the Session API
    session._write({"jsonrpc": "2.0", "id": session._next_id, "method": "tools/call",  # noqa: SLF001
                    "params": {"name": "didi_control_room", "arguments": {},
                               "_meta": {"progressToken": "tok-1"}}})
    show("tools/call with a progressToken", session._read(), limit=300)  # noqa: SLF001

    # A cancellation for a request that has already been answered, then an
    # unknown notification. Neither should disturb the connection.
    session.notify("notifications/cancelled", {"requestId": session._next_id, "reason": "probe"})
    show("ping after notifications/cancelled", session.request("ping"))
    session.notify("notifications/nonexistent", {"x": 1})
    show("ping after an unknown notification", session.request("ping"))

    # A JSON-RPC batch array.
    session._next_id += 1  # noqa: SLF001
    first = session._next_id  # noqa: SLF001
    session._next_id += 1  # noqa: SLF001
    second = session._next_id  # noqa: SLF001
    session._write([  # noqa: SLF001
        {"jsonrpc": "2.0", "id": first, "method": "ping"},
        {"jsonrpc": "2.0", "id": second, "method": "tools/call",
         "params": {"name": "didi_control_room", "arguments": {}}},
    ])
    try:
        show("batch array", session._read(), limit=400)  # noqa: SLF001
    except (RuntimeError, ValueError) as exc:
        show("batch array", f"raised {exc!r}")
    show("ping after the batch", session.request("ping"))

    session.close()

    print("\n--- protocolVersion negotiation, one process each")
    for version in VERSIONS:
        answer = raw_initialize(args.project, version)
        negotiated = None
        if isinstance(answer, dict):
            negotiated = answer.get("result", {}).get("protocolVersion", answer.get("error"))
        print(f"  sent {version!r:14} -> {json.dumps(negotiated, ensure_ascii=False)}")
    print("\nA served revision must answer with itself and an unserved one must")
    print("answer with something else, or a client cannot tell a negotiated")
    print("version from an ignored one. Every row answering the same string was")
    print("#531; a non-string is not a version and is now refused with -32602.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
