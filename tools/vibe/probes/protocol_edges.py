"""The wire, below `tools/call`.

Everything else in this directory sends well formed requests and reads the tool
result. This sends the requests a client gets wrong: a stale pagination cursor,
a null id, a tool that does not exist, `arguments` that are not an object. The
answers are where a strict surface and a lenient transport disagree, which is
what #445, #446 and #447 came out of.

    python tools/vibe/probes/protocol_edges.py SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from mcp_client import Session  # noqa: E402

# Each entry is a whole JSON-RPC message, because the point is the envelope.
MESSAGES = [
    ("ping", {"jsonrpc": "2.0", "id": 900, "method": "ping"}),
    ("tools/list stale cursor",
     {"jsonrpc": "2.0", "id": 901, "method": "tools/list", "params": {"cursor": "garbage"}}),
    ("resources/list stale cursor",
     {"jsonrpc": "2.0", "id": 902, "method": "resources/list", "params": {"cursor": "garbage"}}),
    ("prompts/list stale cursor",
     {"jsonrpc": "2.0", "id": 903, "method": "prompts/list", "params": {"cursor": "garbage"}}),
    ("unknown method", {"jsonrpc": "2.0", "id": 904, "method": "no/such/method"}),
    ("jsonrpc 1.0", {"jsonrpc": "1.0", "id": 905, "method": "ping"}),
    ("no jsonrpc field", {"id": 906, "method": "ping"}),
    ("null id", {"jsonrpc": "2.0", "id": None, "method": "ping"}),
    ("string id", {"jsonrpc": "2.0", "id": "str-id", "method": "ping"}),
    ("duplicate id", {"jsonrpc": "2.0", "id": 900, "method": "ping"}),
    ("unknown tool",
     {"jsonrpc": "2.0", "id": 907, "method": "tools/call",
      "params": {"name": "no_such_tool", "arguments": {}}}),
    ("arguments as string",
     {"jsonrpc": "2.0", "id": 908, "method": "tools/call",
      "params": {"name": "didi_control_room", "arguments": "a string"}}),
    ("tools/call without params",
     {"jsonrpc": "2.0", "id": 909, "method": "tools/call"}),
    ("resources/read unknown uri",
     {"jsonrpc": "2.0", "id": 910, "method": "resources/read", "params": {"uri": "didi://nope"}}),
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("--width", type=int, default=260)
    arguments = parser.parse_args()

    session = Session(project=arguments.project)
    try:
        for label, message in MESSAGES:
            session._write(message)
            try:
                answer = json.dumps(session._read())
            except Exception as failure:  # a closed pipe is itself an answer
                answer = f"no response: {failure}"
            print(f"{label:30} {answer[:arguments.width]}")
    finally:
        session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
