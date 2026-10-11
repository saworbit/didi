"""Send one tool call to `session_daemon.py` and print the answer in full.

    python tools/vibe/session_call.py scene_get_hierarchy '{"max_depth": 2}'
    python tools/vibe/session_call.py --file calls.json      # a list of [name, args]
    python tools/vibe/session_call.py --method tools/list --quiet

Arguments come as JSON on the command line or from `--args-file`, which keeps
backslashes and quotes away from the shell. A `--file` list runs in order and
stops at the first error unless `--keep-going`. `$TOKEN` anywhere in a later
call's arguments is replaced by the last `confirmation_token` seen, which is
how a dry run and its confirm go in one file.
"""

from __future__ import annotations

import argparse
import json
import socket

TOKEN = {"value": None}


def send(port: int, message: dict) -> dict:
    with socket.create_connection(("127.0.0.1", port), timeout=900) as connection:
        connection.sendall((json.dumps(message) + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            chunk = connection.recv(1 << 20)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode())


def find_token(value):
    if isinstance(value, dict):
        for key, inner in value.items():
            if key == "confirmation_token" and isinstance(inner, str):
                return inner
            found = find_token(inner)
            if found:
                return found
    if isinstance(value, list):
        for inner in value:
            found = find_token(inner)
            if found:
                return found
    return None


def substitute(value):
    if isinstance(value, str):
        return TOKEN["value"] if value == "$TOKEN" and TOKEN["value"] else value
    if isinstance(value, dict):
        return {key: substitute(inner) for key, inner in value.items()}
    if isinstance(value, list):
        return [substitute(inner) for inner in value]
    return value


def show(name: str, arguments: dict, answer: dict, quiet: bool, limit: int, verbose: bool = False) -> None:
    status = "ERROR" if answer.get("is_error") else "ok"
    print(f"=== {name} [{status}] {answer.get('elapsed_s')}s")
    if not quiet:
        payload = answer.get("payload")
        if isinstance(payload, dict) and not verbose:
            payload = {k: v for k, v in payload.items() if k != "session"}
        text = json.dumps(payload, indent=1, ensure_ascii=False)
        if limit and len(text) > limit:
            text = text[:limit] + f"\n... ({len(text)} chars, truncated by session_call)"
        print(text)
    for line in answer.get("engine") or []:
        print(f"  [engine] {line}")
    for note in answer.get("notifications") or []:
        print(f"  [notification] {note.get('method')}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("name", nargs="?")
    parser.add_argument("arguments", nargs="?", default="{}")
    parser.add_argument("--args-file")
    parser.add_argument("--file")
    parser.add_argument("--method")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--limit", type=int, default=6000)
    parser.add_argument("--verbose", action="store_true", help="Keep the session block.")
    args = parser.parse_args()

    if args.method:
        params = json.loads(args.arguments) if args.arguments else None
        answer = send(args.port, {"method": args.method, "params": params})
        print(json.dumps(answer["payload"], indent=1)[: args.limit or None])
        return 0

    if args.file:
        with open(args.file, encoding="utf-8") as handle:
            calls = json.load(handle)
    else:
        if args.args_file:
            with open(args.args_file, encoding="utf-8") as handle:
                arguments = json.load(handle)
        else:
            arguments = json.loads(args.arguments)
        calls = [[args.name, arguments]]

    failures = 0
    for name, arguments in calls:
        arguments = substitute(arguments)
        answer = send(args.port, {"name": name, "arguments": arguments})
        token = find_token(answer.get("payload"))
        if token:
            TOKEN["value"] = token
        show(name, arguments, answer, args.quiet, args.limit, args.verbose)
        if answer.get("is_error"):
            failures += 1
            if not args.keep_going and args.file:
                return 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
