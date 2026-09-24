"""The locks #953 put around a shared file, asked over the wire.

#929 was two servers writing one project file at once: a setting or a preset
reported written and missing from the file, or on Windows most of the calls
failing. #953 made `project_set_setting` offline and
`project_add_export_preset` hold a lock under `.didi/locks` from their read to
their write, and moved the blackboard resource's `exists` under the board's
lock (#514). The native tests prove both with threads. This asks the shipped
server, holding the lock from a second process the way another server would:

* **held** -- this process holds `.didi/locks/<file>.lock`, and each tool is
  called against it. Expected: a `409` with `project_file_busy` and
  `retryable: true` after the five second wait, and the file byte for byte as
  it was. Then the lock is let go and the same call has to succeed.
* **exists** -- this process holds a board's lock, a `resources/read` of the
  board starts and waits on it, the board's file is written, and the lock is
  let go. Expected: the new state with `exists: true`. Before #953 it came
  back with the state and `exists: false`.

Every row prints what was expected against what came back, so a build without
#953 prints DIFF where each one was. The race itself, two servers each adding
twenty presets or settings, is `export_preset_writer.py --only writers`.

    python tools/vibe/probes/project_file_lock.py

Needs no editor and no Godot. Each held row costs the server's five second wait.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402
import sandbox  # noqa: E402

FAILURES = 0


class HeldLock:
    """An exclusive OS lock on byte 0 of a file, the range the server locks."""

    def __init__(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.fd = os.open(str(path), os.O_RDWR | os.O_CREAT, 0o600)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(self.fd, msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def release(self) -> None:
        if self.fd < 0:
            return
        if os.name == "nt":
            import msvcrt
            os.lseek(self.fd, 0, os.SEEK_SET)
            msvcrt.locking(self.fd, msvcrt.LK_UNLCK, 1)
        os.close(self.fd)
        self.fd = -1


def row(label: str, expected: object, observed: object) -> None:
    global FAILURES
    same = expected == observed
    if not same:
        FAILURES += 1
    print(f"  {'ok  ' if same else 'DIFF'} {label}: expected {expected!r}, observed {observed!r}")


def error_of(payload: object) -> dict:
    if isinstance(payload, dict) and isinstance(payload.get("error"), dict):
        return payload["error"]
    return {}


def fresh_project(parent: Path, label: str) -> Path:
    return sandbox.create(parent / label, name=f"Lock{label}", with_addon=False)


def snapshot(path: Path) -> bytes | None:
    return path.read_bytes() if path.exists() else None


def held(parent: Path) -> None:
    cases = [
        ("project_set_setting", "project.godot",
         {"setting": "vibe/lock/held", "value": 1}),
        ("project_add_export_preset", "export_presets.cfg",
         {"name": "Held", "platform": "Linux"}),
    ]
    for tool, file_name, arguments in cases:
        print(f"\n== held: {tool} while another process holds {file_name}'s lock")
        project = fresh_project(parent, "held_" + file_name.replace(".", "_"))
        target = project / file_name
        with Session(project, editor_log=False) as s:
            lock = HeldLock(project / ".didi" / "locks" / (file_name + ".lock"))
            before = snapshot(target)
            started = time.monotonic()
            try:
                payload, errored = s.call(tool, arguments)
            finally:
                waited = time.monotonic() - started
                lock.release()
            error = error_of(payload)
            data = error.get("data") or {}
            row("refused", True, bool(errored))
            row("error.code", 409, error.get("code"))
            row("data.code", "project_file_busy", data.get("code"))
            row("data.retryable", True, data.get("retryable"))
            row("data.file", "res://" + file_name, data.get("file"))
            row("waited at least 4.5 s", True, waited >= 4.5)
            row(f"{file_name} unchanged", True, snapshot(target) == before)
            if errored:
                print(f"       message: {error.get('message')}")

            payload, errored = s.call(tool, arguments)
            row("the same call succeeds once the lock is let go", True, not errored)
            if errored:
                print(f"       answer: {json.dumps(payload)[:300]}")


def exists(parent: Path) -> None:
    print("\n== exists: a board created while its reader waits on the lock (#514)")
    project = fresh_project(parent, "exists")
    board = f"raced-{os.getpid()}"
    board_file = project / ".didi" / "blackboard" / (board + ".json")
    with Session(project, editor_log=False) as s:
        lock = HeldLock(board_file.with_suffix(".lock"))
        answer: dict = {}

        def reader() -> None:
            answer.update(s.request("resources/read", {"uri": f"blackboard://{board}/state"}))

        thread = threading.Thread(target=reader)
        thread.start()
        try:
            # Long enough for the server to reach the lock and wait on it.
            time.sleep(0.5)
            board_file.write_text(json.dumps({"version": 1, "state": {"seed": 1}}),
                                  encoding="utf-8")
        finally:
            lock.release()
        thread.join(timeout=15)
        contents = (answer.get("result") or {}).get("contents") or [{}]
        try:
            read = json.loads(contents[0].get("text", "{}"))
        except json.JSONDecodeError:
            read = {}
        row("state.seed", 1, (read.get("state") or {}).get("seed"))
        row("exists", True, read.get("exists"))
        if not read:
            print(f"       answer: {json.dumps(answer)[:300]}")


SECTIONS = {"held": held, "exists": exists}


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", choices=sorted(SECTIONS), action="append")
    parser.add_argument("--keep", action="store_true", help="Keep the throwaway projects.")
    args = parser.parse_args()

    parent = Path(tempfile.mkdtemp(prefix="vibe_file_lock_"))
    print(f"projects under {parent}")
    try:
        for key, section in SECTIONS.items():
            if args.only and key not in args.only:
                continue
            section(parent)
    finally:
        if not args.keep:
            shutil.rmtree(parent, ignore_errors=True)
    print(f"\n{FAILURES} row(s) differ from what #953 promises.")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
