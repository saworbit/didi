"""The handshake, and what the server does with it once it is past.

`protocol_edges.py` covers the JSON-RPC frame and `rpc_methods.py` covers the
methods around `tools/call`. Neither asks about the lifecycle itself: a call
that arrives before `initialize`, a second `initialize` mid-session, a
`notifications/initialized` that never comes or comes twice, two requests
carrying the same id.

The gate in front of `initialize` is correct and answers `-32002`. The gate
never closes again: a second `initialize` from a different `clientInfo` is
accepted, and a confirmation token minted before it is still spendable after
(#552). That matters less for what it is than for what it shows -- the
confirmation gate binds a token to arguments and to a tool, and not to the
client that previewed the mutation.

Both halves print, so the run shows the refusal that works beside the one that
does not.

Usage::

    python tools/vibe/probes/session_lifecycle.py SANDBOX
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import mcp_client  # noqa: E402
from mcp_client import INITIALIZE, INITIALIZED, Session  # noqa: E402

PATCH = {
    "file_path": "res://player.gd",
    "method_name": "take_damage",
    "new_definition": "func take_damage(amount: int) -> void:\n\t_hp -= amount * 2\n",
}


def raw(binary: str, project: str, messages: list[dict], timeout: float = 30.0) -> list[dict]:
    """Drive the process by hand; `batch()` prepends the handshake we are testing."""
    process = subprocess.Popen(
        [binary, "--log-level", "ERROR", "--project", project],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=dict(os.environ),
    )
    payload = "".join(json.dumps(message) + "\n" for message in messages).encode()
    try:
        out, _err = process.communicate(payload, timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        out, _err = process.communicate()
    responses: list[dict] = []
    for line in out.decode(errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            responses.append(json.loads(line))
        except ValueError:
            responses.append({"__raw__": line})
    return responses


def show(label: str, responses: list[dict]) -> None:
    print(f"--- {label}")
    for response in responses:
        print("    " + json.dumps(response)[:260])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()
    binary = str(Path(args.binary) if args.binary else mcp_client.resolve_binary())

    print("== before the handshake: the gate that works")
    show("tools/list with no initialize", raw(binary, args.project, [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}},
    ]))
    show("tools/call with no initialize", raw(binary, args.project, [
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
         "params": {"name": "project_get_info", "arguments": {}}},
    ]))

    print("\n== after it: the same gate, open")
    show("a second initialize, different client, different revision", raw(binary, args.project, [
        INITIALIZE, INITIALIZED,
        {"jsonrpc": "2.0", "id": 5, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {},
                    "clientInfo": {"name": "a-different-client", "version": "9"}}},
    ]))
    show("notifications/initialized never sent, then a ping", raw(binary, args.project, [
        INITIALIZE, {"jsonrpc": "2.0", "id": 8, "method": "ping", "params": {}},
    ]))
    show("notifications/initialized twice, then a ping", raw(binary, args.project, [
        INITIALIZE, INITIALIZED, INITIALIZED,
        {"jsonrpc": "2.0", "id": 9, "method": "ping", "params": {}},
    ]))
    show("two requests carrying the same id", raw(binary, args.project, [
        INITIALIZE, INITIALIZED,
        {"jsonrpc": "2.0", "id": 7, "method": "ping", "params": {}},
        {"jsonrpc": "2.0", "id": 7, "method": "ping", "params": {}},
    ]))

    print("\n== what the open gate is worth: a token across a client change")
    with Session(project=args.project, binary=args.binary) as session:
        preview, _ = session.call("script_patch_method", dict(PATCH, dry_run=True))
        token = None
        if isinstance(preview, dict):
            token = (preview.get("mutation_preview") or {}).get("confirmation_token")
        print(f"    token minted by the first client: {token}")
        envelope = session.request("initialize", {
            "protocolVersion": "2024-11-05", "capabilities": {},
            "clientInfo": {"name": "a-different-client", "version": "9"},
        })
        print("    second initialize -> error: %s, serverInfo: %s" % (
            envelope.get("error"),
            json.dumps(envelope.get("result", {}).get("serverInfo")),
        ))
        payload, is_error = session.call("script_patch_method", dict(PATCH, confirmation_token=token))
        print("    spent by the second client: %s%s" % (
            "ERR " if is_error else "ok  ", json.dumps(payload, ensure_ascii=False)[:200]))

    print(
        "\nA second initialize should be refused on an initialized session, or it "
        "should reset what belongs to a client -- outstanding tokens, the attached "
        "session selection. Accepting it and changing nothing is the reading under "
        "which the token binding is weakest."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
