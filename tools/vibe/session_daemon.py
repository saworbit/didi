"""Hold one Didi server open and take calls from other processes, one at a time.

`Session` lives as long as the Python process that made it, so a session
driven by hand -- one call, read the answer, decide the next -- either keeps a
single script open or starts a fresh server for every call. The second loses
everything a server holds in memory between calls: a confirmation token, an
explicit attachment, a request_id job. This daemon keeps the server and lets
`session_call.py` talk to it over a localhost socket, so a long authoring arc
can be driven the way an agent drives it: one tool call per step.

    python tools/vibe/session_daemon.py --project D:/game --port 8765 --log calls.jsonl
    python tools/vibe/session_call.py scene_get_hierarchy '{}'

Every call and its full answer is appended to `--log` as a JSON line, with the
engine lines the editor printed during it, so a session leaves a transcript
behind that a finding can quote.

The socket listens on 127.0.0.1 with no authentication, so any process of any
local user can send calls through this server and its attachment. Run it for a
session on a machine you own and stop it after; it is a test harness, not a way
to share a server.
"""

from __future__ import annotations

import argparse
import json
import socketserver
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcp_client import Session  # noqa: E402

LOCK = threading.Lock()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--project", required=True)
    parser.add_argument("--binary")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--editor-log", help="The editor's --log-file, read after every call.")
    parser.add_argument("--extra", action="append", default=[], help="Extra server argument.")
    args = parser.parse_args()

    session = Session(
        args.project,
        binary=args.binary,
        extra_args=args.extra,
        editor_log=args.editor_log if args.editor_log else False,
        echo_engine=False,
    )
    log = args.log.open("a", encoding="utf-8")
    log.write(json.dumps({"event": "start", "argv": session.argv,
                          "initialize": session.initialize_result}) + "\n")
    log.flush()

    class Handler(socketserver.StreamRequestHandler):
        def handle(self) -> None:
            request = json.loads(self.rfile.readline().decode())
            with LOCK:
                started = time.time()
                if request.get("method"):
                    envelope = session.request(request["method"], request.get("params"))
                    payload, is_error = envelope, None
                    engine = []
                else:
                    payload, is_error = session.call(request["name"], request.get("arguments") or {})
                    engine = [line.text for line in session.last_engine_lines]
                elapsed = round(time.time() - started, 3)
                notes = session.take_notifications()
                answer = {"payload": payload, "is_error": is_error, "engine": engine,
                          "elapsed_s": elapsed, "notifications": notes}
                log.write(json.dumps({"event": "call", "request": request, **answer}) + "\n")
                log.flush()
            self.wfile.write((json.dumps(answer) + "\n").encode())

    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True

    with Server(("127.0.0.1", args.port), Handler) as server:
        print(f"listening on {args.port}", flush=True)
        server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
