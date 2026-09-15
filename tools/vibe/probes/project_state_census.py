"""The failures a project's *state* can reach, which argument censuses cannot.

`handler_error_census.py` builds its arguments from each tool's own schema, so
it can only reach failures an argument can cause. It reports zero bare strings
on this build. Two survive anyway, and both need a file on disk to be present
and wrong rather than an argument to be malformed:

    project_list_export_presets -> 'export_presets.cfg is malformed or contains
                                    no complete unique presets'
    project_export              -> 'Export preset not found: <name>'

That is the same blind spot #460 opened -- "the argument check answers junk
first" -- one layer further out. There the well-formed-and-wrong *argument* was
the key; here it is the well-formed-and-wrong *project*.

So: take each file a tool reads about the project, put it into every state it
can be in (absent, unreadable bytes, structurally valid but empty, structurally
valid and incomplete, correct), and ask the readers. Two questions per row:

* is the failure an envelope with a code, or prose?
* do two different states get two different answers?

The second is the one that keeps finding things. An absent `export_presets.cfg`
is `isError: false, preset_count: 0`; a present one holding no presets is an
error. Both mean "this project has no export presets".

    python tools/vibe/sandbox.py SANDBOX
    python tools/vibe/probes/project_state_census.py SANDBOX

Offline; no editor needed. It writes and removes files in the sandbox.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# One entry per project file: the file, the states to put it in, and the tools
# that read it. `None` means "remove the file"; a state that would break the
# server's own startup is not here, because the server has to be running to be
# asked.
SUBJECTS = [
    {
        "file": "export_presets.cfg",
        "restore": None,
        "states": {
            "absent": None,
            "unreadable bytes": b"\x00\x01 not an ini \xff\n",
            "valid ini, no preset sections": b"[other]\n\nkey=1\n",
            "a preset missing its name": b"[preset.0]\n\nplatform=\"Linux\"\n",
            "one complete preset": b'[preset.0]\n\nname="Linux"\nplatform="Linux"\nexport_path="res://out/g"\n',
        },
        "calls": [
            ("project_list_export_presets", {}),
            ("project_export", {"dry_run": True, "preset": "Linux", "output_path": "res://out/g"}),
            ("project_export", {"preset": "Linux", "output_path": "res://out/g"}),
        ],
    },
    {
        "file": "player.gd.uid",
        "restore": None,
        "states": {
            "absent": None,
            "not a uid": b"this is not a uid\n",
            "empty": b"",
            "a well-formed uid": b"uid://bqvhxmvqxqxqx\n",
        },
        "calls": [
            ("project_get_uid_map", {}),
            ("resource_inspect", {"resource_path": "res://player.gd"}),
        ],
    },
]


def shape(payload) -> str:
    if isinstance(payload, str):
        return "BARE"
    if isinstance(payload, dict) and "error" in payload:
        return "envelope"
    return "ok"


def summary(payload) -> str:
    if isinstance(payload, str):
        return repr(payload)
    if isinstance(payload, dict) and "error" in payload:
        error = payload["error"]
        return f'{error.get("code")} {error.get("data", {}).get("code")} {str(error.get("message"))[:70]!r}'
    return json.dumps(payload)[:110]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project", help="A sandbox project. This probe writes files.")
    arguments = parser.parse_args()
    project = Path(arguments.project)

    shapes: Counter[str] = Counter()
    bare: list[str] = []

    with Session(project=str(project)) as session:
        for subject in SUBJECTS:
            target = project / subject["file"]
            original = target.read_bytes() if target.exists() else None
            print(f"\n===== {subject['file']}")
            answers: dict[tuple[str, str], str] = {}
            for state, payload in subject["states"].items():
                if payload is None:
                    target.unlink(missing_ok=True)
                else:
                    target.write_bytes(payload)
                print(f"  -- {state}")
                for tool, tool_arguments in subject["calls"]:
                    answer, is_error = session.call(tool, dict(tool_arguments))
                    kind = shape(answer)
                    shapes[kind] += 1
                    if kind == "BARE":
                        bare.append(f"{tool} with {subject['file']} {state}: {summary(answer)}")
                    label = tool + (" (dry_run)" if tool_arguments.get("dry_run") else "")
                    answers[(state, label)] = summary(answer)
                    print(f"     {label:34} isError={str(is_error):5} {kind:8} {summary(answer)[:110]}")

            # The second question: which states are indistinguishable?
            print("  -- states that answer identically")
            labels = {label for _, label in answers}
            for label in sorted(labels):
                by_answer: dict[str, list[str]] = {}
                for (state, this_label), text in answers.items():
                    if this_label == label:
                        by_answer.setdefault(text, []).append(state)
                collisions = [states for states in by_answer.values() if len(states) > 1]
                for states in collisions:
                    print(f"     {label:34} cannot tell apart: {', '.join(states)}")

            if original is None:
                target.unlink(missing_ok=True)
            else:
                target.write_bytes(original)

    print(f"\n{sum(shapes.values())} answers: {dict(shapes)}")
    if bare:
        print("Bare-string failures -- nothing for a caller to branch on:")
        for line in bare:
            print(f"  {line}")
    else:
        print("No bare-string failures reached from project state.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
