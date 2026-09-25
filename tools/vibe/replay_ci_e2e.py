"""Replay ci.yml's end-to-end MCP check against a local build.

The step "Run End-to-End MCP Integration Test" in .github/workflows/ci.yml is a
Python script written inside the workflow. Neither the native suite nor the
Python suite reaches it, so a change that breaks one of its assertions goes red
only on a runner, after a push. This lifts the script out of the workflow as it
stands today, points it at a local binary and at a tool manifest dumped from
that same binary, and runs it from the repository root, where CI runs it.

It changes three strings and nothing else: the Windows and POSIX paths to the
binary and the manifest's path. If the workflow stops spelling any of them the
way this expects, the replay refuses to run rather than testing the wrong
binary. Session twenty-two replayed it by hand before #963 pushed.

    python tools/vibe/replay_ci_e2e.py
    python tools/vibe/replay_ci_e2e.py --binary build/didi.exe
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from mcp_client import REPOSITORY_ROOT, resolve_binary  # noqa: E402

STEP = "Run End-to-End MCP Integration Test"
HEREDOC = "\"$DIDI_PYTHON\" - << 'EOF'"


def lift_script(workflow: Path) -> str:
    lines = workflow.read_text(encoding="utf-8").splitlines()
    step = next((i for i, line in enumerate(lines) if line.strip() == f"- name: {STEP}"), None)
    if step is None:
        raise SystemExit(f"{workflow} has no step named {STEP!r}")
    start = next((i for i in range(step, len(lines)) if lines[i].strip() == HEREDOC), None)
    if start is None:
        raise SystemExit(f"the step {STEP!r} no longer runs a {HEREDOC} heredoc")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].strip() == "EOF"), None)
    if end is None:
        raise SystemExit("the heredoc never closes")
    body = lines[start + 1:end]
    indent = min(len(line) - len(line.lstrip()) for line in body if line.strip())
    return "\n".join(line[indent:] for line in body) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--binary", help="the didi server to test (default: the fresher of build and build-ninja)")
    args = parser.parse_args()
    binary = Path(args.binary).resolve() if args.binary else resolve_binary()
    script = lift_script(REPOSITORY_ROOT / ".github" / "workflows" / "ci.yml")

    with tempfile.TemporaryDirectory() as scratch:
        manifest = Path(scratch) / "tool_manifest.json"
        dumped = subprocess.run([str(binary), "--dump-tool-manifest"], capture_output=True, timeout=120)
        if dumped.returncode != 0 or not dumped.stdout:
            print(f"{binary} --dump-tool-manifest failed: {dumped.stderr.decode(errors='replace')[:400]}")
            return 2
        manifest.write_bytes(dumped.stdout)
        replacements = {
            "'./build/didi.exe'": repr(binary.as_posix()),
            "'./build/didi'": repr(binary.as_posix()),
            "build/tool_manifest.json": manifest.as_posix(),
        }
        for old, new in replacements.items():
            if old not in script:
                print(f"the workflow's script no longer contains {old}, so this replay would not test "
                      f"{binary}; update replay_ci_e2e.py")
                return 2
            script = script.replace(old, new)
        path = Path(scratch) / "ci_e2e.py"
        path.write_text(script, encoding="utf-8")
        print(f"replaying {STEP!r} ({script.count(chr(10))} lines) against {binary}")
        ran = subprocess.run([sys.executable, str(path)], cwd=REPOSITORY_ROOT)
        print(f"exit {ran.returncode}")
        return ran.returncode


if __name__ == "__main__":
    raise SystemExit(main())
