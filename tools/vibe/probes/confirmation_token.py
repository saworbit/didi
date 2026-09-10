"""Walk the confirmation gate end to end, because a token cannot survive a file.

A `dry_run` mints a confirmation token in the server process's memory. It is not
written anywhere, so a probe list cannot carry it between two `probe.py` runs --
the second process has never heard of it. That is why this probe is a script
while the others are JSON.

It reproduced two findings from the 2026-09-11 session, both since fixed. It is
kept because this sequence is the only way to exercise the gate at all, and a
regression in it would be silent:

* **#399** -- the dry run issued a token for arguments the real call refuses.
  `script_patch_method` takes `new_definition`; the preview accepted `new_body`
  and signed it anyway.
* **#398** -- a confirm that failed its own binding check still consumed the
  token, so the *correct* retry was rejected as "unknown or already used".

Run it against a sandbox from `sandbox.py`, which supplies `res://player.gd`::

    python tools/vibe/probes/confirmation_token.py --project SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

PATCH = {
    "file_path": "res://player.gd",
    "method_name": "take_damage",
    "new_definition": "func take_damage(amount: int) -> void:\n\t_hp -= amount * 2\n",
}
WRONG_PARAMETER = {
    "file_path": "res://player.gd",
    "method_name": "take_damage",
    "new_body": "\t_hp -= amount * 2\n",
}


def show(label: str, payload: object, is_error: bool | None) -> None:
    marker = "ERR " if is_error else "ok  "
    print(f"--- {label}\n    {marker}{json.dumps(payload)[:400]}")


def token_of(payload: object) -> str | None:
    """The confirmation token, or None when the call refused to mint one.

    Refusing is the fixed behaviour for a bad argument set, so this probe has to
    survive it: crashing on the absent key would report a failure in the one
    case that is now correct.
    """
    if isinstance(payload, dict) and isinstance(payload.get("mutation_preview"), dict):
        return payload["mutation_preview"].get("confirmation_token")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    with Session(project=args.project, binary=args.binary) as session:
        print("== #399: a preview for arguments the tool does not take")
        payload, is_error = session.call("script_patch_method", dict(WRONG_PARAMETER, dry_run=True))
        show("dry_run with new_body", payload, is_error)
        token = token_of(payload)
        if token is None:
            print("    (fixed: no token is minted for arguments the tool refuses)")
        else:
            payload, is_error = session.call(
                "script_patch_method", dict(WRONG_PARAMETER, confirmation_token=token)
            )
            show("spend that token", payload, is_error)

        print("\n== #398: a mismatched confirm must not burn a token minted for a valid call")
        payload, is_error = session.call("script_patch_method", dict(PATCH, dry_run=True))
        show("dry_run with new_definition", payload, is_error)
        token = token_of(payload)
        if token is None:
            print("    no token for a valid dry run -- that is itself a finding, stopping here")
            return 1

        mismatched = {key: value for key, value in PATCH.items() if key != "new_definition"}
        payload, is_error = session.call(
            "script_patch_method", dict(mismatched, confirmation_token=token)
        )
        show("confirm with a dropped argument (expect a 4xx that is not a spend)", payload, is_error)

        payload, is_error = session.call(
            "script_patch_method", dict(PATCH, confirmation_token=token)
        )
        show("confirm with the exact previewed arguments", payload, is_error)
        print(
            "\nThe last call is the one the token was minted for. Under #398 it failed "
            "with 409 'unknown or already used' and res://player.gd was left unchanged; "
            "it should now apply the patch."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
