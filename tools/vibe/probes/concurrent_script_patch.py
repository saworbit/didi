"""Two servers patching one script at once, and the script's lock held from outside.

#954: `script_patch_method` read a `.gd` file, spliced one method in and wrote
the whole file back with nothing held in between. Two agents patching
different methods of one script could both read the old text, and the second
write replaced the first while both reported success. On Windows the overlap
more often failed one call with a `500` instead. #968 holds the script's lock
under `.didi/locks` from the read to the write, the one #953 gave
`project.godot` and `export_presets.cfg`.

* **race** -- two servers on one project, each patching twenty different
  methods of `player.gd` from its own thread, started together. Expected: all
  forty calls succeed and all forty methods are in the file.
* **held** -- this process holds `.didi/locks/player.gd.lock`, the lock the
  tool takes, and the tool is called against it. Expected: a `409` with
  `project_file_busy` and `retryable: true` after the five second wait, and the
  script byte for byte as it was. Then the lock is let go and the same call has
  to succeed.

Both servers run with `--yolo`, because the tool is always confirmed and the
confirmation gate is not what this asks about. A build before #968 prints DIFF
in the race, as lost methods, refused calls or both.

    python tools/vibe/probes/concurrent_script_patch.py

Needs no editor and no Godot.
"""

from __future__ import annotations

import argparse
import collections
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_client import Session  # noqa: E402
from project_file_lock import HeldLock, error_of  # noqa: E402
import sandbox  # noqa: E402

EACH = 20
FAILURES = 0


def row(label: str, expected: object, observed: object) -> None:
    global FAILURES
    same = expected == observed
    if not same:
        FAILURES += 1
    print(f"  {'ok  ' if same else 'DIFF'} {label}: expected {expected!r}, observed {observed!r}")


def script_with(methods: list[str]) -> str:
    return "extends Node\n" + "".join(f"\nfunc {name}():\n\tpass\n" for name in methods)


def method_name(writer: int, index: int) -> str:
    return f"w{writer}_{index}"


def definition(writer: int, index: int) -> str:
    return f"func {method_name(writer, index)}():\n\treturn {1000 + writer * 100 + index}\n"


def race(parent: Path) -> None:
    print(f"\n== race: two servers, {EACH} different methods each, one script")
    project = sandbox.create(parent / "race", name="PatchRace", with_addon=False)
    script = project / "player.gd"
    script.write_text(script_with([method_name(w, i) for w in (0, 1) for i in range(EACH)]),
                      encoding="utf-8")
    outcomes: dict[int, collections.Counter] = {0: collections.Counter(), 1: collections.Counter()}
    start = threading.Barrier(2)

    def writer(index: int) -> None:
        with Session(project, extra_args=["--yolo"], editor_log=False) as s:
            start.wait()
            for i in range(EACH):
                payload, errored = s.call("script_patch_method", {
                    "file_path": "res://player.gd",
                    "method_name": method_name(index, i),
                    "new_definition": definition(index, i),
                })
                code = error_of(payload).get("code") if errored else "ok"
                outcomes[index][code] += 1

    threads = [threading.Thread(target=writer, args=(w,)) for w in (0, 1)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=300)

    text = script.read_text(encoding="utf-8")
    landed = sum(1 for w in (0, 1) for i in range(EACH)
                 if f"\treturn {1000 + w * 100 + i}\n" in text)
    reported = sum(counter["ok"] for counter in outcomes.values())
    refused = {code: count for counter in outcomes.values()
               for code, count in counter.items() if code != "ok"}
    print(f"       answers: {dict(outcomes[0])} and {dict(outcomes[1])}")
    row("calls reported patched", 2 * EACH, reported)
    row("methods in the file", 2 * EACH, landed)
    row("calls refused", {}, refused)


def held(parent: Path) -> None:
    print("\n== held: script_patch_method while another process holds the script's lock")
    project = sandbox.create(parent / "held", name="PatchHeld", with_addon=False)
    script = project / "player.gd"
    script.write_text(script_with(["tick"]), encoding="utf-8")
    arguments = {"file_path": "res://player.gd", "method_name": "tick",
                 "new_definition": "func tick():\n\treturn 1\n"}
    with Session(project, extra_args=["--yolo"], editor_log=False) as s:
        lock = HeldLock(project / ".didi" / "locks" / "player.gd.lock")
        before = script.read_bytes()
        started = time.monotonic()
        try:
            payload, errored = s.call("script_patch_method", arguments)
        finally:
            waited = time.monotonic() - started
            lock.release()
        error = error_of(payload)
        data = error.get("data") or {}
        row("refused", True, bool(errored))
        row("error.code", 409, error.get("code"))
        row("data.code", "project_file_busy", data.get("code"))
        row("data.retryable", True, data.get("retryable"))
        row("waited at least 4.5 s", True, waited >= 4.5)
        row("player.gd unchanged", True, script.read_bytes() == before)

        payload, errored = s.call("script_patch_method", arguments)
        row("the same call succeeds once the lock is let go", True, not errored)


SECTIONS = {"race": race, "held": held}


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", choices=sorted(SECTIONS), action="append")
    parser.add_argument("--keep", action="store_true", help="Keep the throwaway projects.")
    args = parser.parse_args()

    parent = Path(tempfile.mkdtemp(prefix="vibe_script_patch_"))
    print(f"projects under {parent}")
    try:
        for key, section in SECTIONS.items():
            if args.only and key not in args.only:
                continue
            section(parent)
    finally:
        if not args.keep:
            shutil.rmtree(parent, ignore_errors=True)
    print(f"\n{FAILURES} row(s) differ from what #968 promises.")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    raise SystemExit(main())
