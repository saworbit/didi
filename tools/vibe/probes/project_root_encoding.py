"""What the host operating system does to the project path before the server reads it.

Session eight asked what Windows does to a `res://` path the server already
accepted. This asks the question one step earlier: what reaches the server at
all, given that MSVC's narrow `main(int argc, char** argv)` converts the wide
command line through the system ANSI codepage.

The answer splits by codepage, and neither half is right (#611):

* a character cp1252 **can** encode -- `ó`, `ü`, `ñ` -- arrives as a lone
  high byte, which is not valid UTF-8. `resolveExplicitProjectRoot` is written
  for exactly this and catches `std::filesystem::filesystem_error`; the throw is
  a `std::system_error`, so it escapes `main` and the process fast-fails with
  `0xC0000409` and no output at all. The message that was written for this case
  -- "The project root must be valid UTF-8" -- has never been reachable.
* a character cp1252 **cannot** encode -- Cyrillic, CJK, an emoji -- becomes
  `?`, so the server looks for a path that does not exist and refuses with
  "The explicit project root is not an accessible directory". The directory is
  perfectly accessible; the server just cannot spell it.

This probe needs no editor and no addon: it is a startup question. It writes one
sandbox per case and sends a single `initialize`.

A Windows username containing an accented character is ordinary, and a project
under `C:\\Users\\<name>\\...` inherits it, so the crashing row is what those
users get from every MCP host: a dead process and nothing to read.

    python tools/vibe/probes/project_root_encoding.py
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import sandbox  # noqa: E402
from mcp_client import resolve_binary  # noqa: E402

INITIALIZE = json.dumps({
    "jsonrpc": "2.0", "id": 1, "method": "initialize",
    "params": {"protocolVersion": "2024-11-05", "capabilities": {},
               "clientInfo": {"name": "vibe", "version": "1"}},
}) + "\n"

# (label, directory name, what a correct build should do)
CASES = [
    ("ascii", "plain", "answers"),
    ("ascii with space", "with space", "answers"),
    ("latin1 o-acute", "pr\u00f3jekt", "answers"),
    ("latin1 u-umlaut", "gr\u00fcn", "answers"),
    ("latin1 n-tilde", "a\u00f1o", "answers"),
    ("cyrillic", "\u043f\u0440\u043e\u0435\u043a\u0442", "answers"),
    ("cjk", "\u65e5\u672c", "answers"),
    ("emoji", "proj-\U0001F600", "answers"),
]

FAST_FAIL = 3221226505  # 0xC0000409, STATUS_STACK_BUFFER_OVERRUN


def outcome(completed: subprocess.CompletedProcess) -> str:
    if completed.stdout.startswith(b'{"id":1'):
        return "answers"
    if completed.returncode == FAST_FAIL:
        return "CRASH 0xC0000409, no output"
    message = completed.stderr.decode("utf-8", "replace").strip().splitlines()
    return f"refused: {message[-1][:60]}" if message else f"exit {completed.returncode}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default=None,
                        help="Directory to create the sandboxes under. Defaults to a temp path.")
    parser.add_argument("--binary", default=None)
    args = parser.parse_args()

    import tempfile
    base = Path(args.base) if args.base else Path(tempfile.gettempdir()) / "vibe-root-encoding"
    base.mkdir(parents=True, exist_ok=True)
    binary = args.binary or resolve_binary()

    for label, folder, expected in CASES:
        destination = base / folder
        if destination.exists():
            shutil.rmtree(destination)
        # No addon: this never gets far enough to want one.
        sandbox.create(destination, name="RootEncoding", with_addon=False)
        completed = subprocess.run(
            [str(binary), "--project", str(destination).replace("\\", "/")],
            input=INITIALIZE.encode(), capture_output=True, timeout=60)
        observed = outcome(completed)
        verdict = "ok " if observed == expected else "DIFF"
        print(f"  {verdict} {label:18} expected={expected:<10} observed={observed}")

    shutil.rmtree(base, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
