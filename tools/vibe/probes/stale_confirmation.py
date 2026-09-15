"""The world moves between the preview and the confirm.

The confirmation gate has been probed twice before, and both times the question
was about the *token*: is it minted for arguments the tool refuses (#399), is it
consumed by a confirm that failed its own binding check (#398), can it cross a
process boundary (it cannot -- it lives in server memory). Every one of those
asks whether the token matches the call.

None of them asks whether the token still matches the *world*. A dry run reads
the file, computes a preview, and signs it. The preview is what the human reads
before saying yes -- `before.size_bytes`, the diff, the node that will be
removed. The confirm arrives some time later, and in between the file can have
been edited, truncated, or deleted by anything: the human in the editor, a
second agent on the blackboard, a git checkout, the other half of this
harness.

A gate whose answer is "the arguments match, apply it" is approving a plan
against a world it has not looked at since. That is the classic time-of-check
to time-of-use seam, and for a tool whose whole safety story is "read the
preview, then confirm" it is the seam that matters most.

Each case writes the fixture with `Path.write_text`, not with a didi tool, so
the server has no way to know the file changed other than by looking.

Run against a throwaway sandbox with the editor open::

    python tools/vibe/probes/stale_confirmation.py --project SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

ORIGINAL = (
    "extends Node2D\n"
    "class_name Player\n"
    "\n"
    "var _hp := 10\n"
    "\n"
    "func take_damage(amount: int) -> void:\n"
    "\t_hp -= amount\n"
)

# What the human approves.
PATCH = {
    "file_path": "res://drift.gd",
    "method_name": "take_damage",
    "new_definition": "func take_damage(amount: int) -> void:\n\t_hp -= amount * 2\n",
}


def show(label: str, payload: object, is_error: bool | None, width: int = 700) -> None:
    marker = "ERR " if is_error else "ok  "
    print(f"--- {label}\n    {marker}{json.dumps(payload)[:width]}")


def preview_of(payload: object) -> dict:
    if isinstance(payload, dict) and isinstance(payload.get("mutation_preview"), dict):
        return payload["mutation_preview"]
    return {}


def restore(target: Path, text: str = ORIGINAL) -> None:
    with target.open("w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    root = Path(args.project)
    target = root / "drift.gd"

    with Session(project=args.project, binary=args.binary) as session:
        print("== A: the file grows between the preview and the confirm")
        restore(target)
        payload, is_error = session.call("script_patch_method", dict(PATCH, dry_run=True))
        show("dry_run on the original file", payload, is_error)
        preview = preview_of(payload)
        token = preview.get("confirmation_token")
        print(f"    preview before: {json.dumps(preview.get('before'))}")

        # Somebody else edits the file. Same method still present, different
        # body, and a second method the preview never saw.
        restore(
            target,
            ORIGINAL.replace("\t_hp -= amount\n", "\t_hp -= amount\n\tprint('audited')\n")
            + "\nfunc heal(amount: int) -> void:\n\t_hp += amount\n",
        )
        print(f"    file is now {target.stat().st_size} bytes")

        if token:
            payload, is_error = session.call("script_patch_method", dict(PATCH, confirmation_token=token))
            show("spend the token against the changed file", payload, is_error)
        print("    file after:")
        print("      " + target.read_text(encoding="utf-8").replace("\n", "\n      "))

        print("\n== B: the file is deleted between the preview and the confirm")
        restore(target)
        payload, is_error = session.call("script_patch_method", dict(PATCH, dry_run=True))
        token = preview_of(payload).get("confirmation_token")
        show("dry_run", payload, is_error, width=300)
        target.unlink()
        print("    file deleted")
        if token:
            payload, is_error = session.call("script_patch_method", dict(PATCH, confirmation_token=token))
            show("spend the token against a file that is gone", payload, is_error)
        print(f"    file exists afterwards: {target.exists()}")

        print("\n== C: the method named by the preview is gone")
        restore(target)
        payload, is_error = session.call("script_patch_method", dict(PATCH, dry_run=True))
        token = preview_of(payload).get("confirmation_token")
        restore(target, "extends Node2D\nclass_name Player\n\nvar _hp := 10\n")
        print("    take_damage removed from the file")
        if token:
            payload, is_error = session.call("script_patch_method", dict(PATCH, confirmation_token=token))
            show("spend the token; the method it previewed is absent", payload, is_error)
        print("    file after:")
        print("      " + target.read_text(encoding="utf-8").replace("\n", "\n      "))

        print("\n== D: a second dry run while the first token is unspent")
        restore(target)
        first, _ = session.call("script_patch_method", dict(PATCH, dry_run=True))
        second, _ = session.call("script_patch_method", dict(PATCH, dry_run=True))
        token_one = preview_of(first).get("confirmation_token")
        token_two = preview_of(second).get("confirmation_token")
        print(f"    first  token: {token_one}")
        print(f"    second token: {token_two}")
        print(f"    identical: {token_one == token_two}")
        if token_one and token_one != token_two:
            payload, is_error = session.call(
                "script_patch_method", dict(PATCH, confirmation_token=token_one)
            )
            show("spend the older token", payload, is_error, width=300)

        restore(target)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
