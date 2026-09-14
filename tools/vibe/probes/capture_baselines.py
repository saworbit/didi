"""The visual regression tool, asked about a baseline that never existed.

`viewport_diff_capture` takes a `baseline_capture_id` -- an id handed out by an
earlier `viewport_capture_frame` -- and reports how far the current frame has
drifted from it. It is the one tool on the surface whose entire job is to
answer "has this changed?", which makes it the one tool where a false "no"
costs the most: it is what a caller would build a visual regression check on,
and a check that cannot fail is worse than no check.

Capture ids look like confirmation tokens: a hex string minted by one call and
spent by another. Confirmation tokens are held in the server process's memory
and a token from a dead process is refused (`409 unknown or already used`).
This asks whether capture ids have the same property, and what happens when
they do not.

The cases run from "an id this process really did mint" down to "an id nobody
ever minted", and print `bit_identical` beside `changed_pixels` for each. Any
row that reports agreement for an id the server cannot have is a row where the
tool answered a different question from the one asked.

    python tools/vibe/probes/capture_baselines.py --project SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402


def row(label: str, payload: object, is_error: bool | None) -> None:
    if is_error:
        error = payload.get("error", payload) if isinstance(payload, dict) else payload
        message = error.get("message", "") if isinstance(error, dict) else str(error)
        code = error.get("code") if isinstance(error, dict) else "?"
        print(f"    {label:<44} ERR {code} {str(message)[:80]}")
        return
    if isinstance(payload, dict):
        print(
            f"    {label:<44} ok  bit_identical={payload.get('bit_identical')} "
            f"changed_pixels={payload.get('changed_pixels')} "
            f"ratio={payload.get('changed_ratio')} "
            f"baseline_echoed={str(payload.get('baseline_capture_id'))[:12]}"
        )
        return
    print(f"    {label:<44} ok  {str(payload)[:80]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    print("== a baseline this process really minted")
    with Session(project=args.project, binary=args.binary) as session:
        payload, _ = session.call("viewport_capture_frame", {})
        real_id = payload.get("capture_id") if isinstance(payload, dict) else None
        print(f"    minted: {real_id}")
        payload, is_error = session.call("viewport_diff_capture", {"baseline_capture_id": real_id})
        row("its own baseline", payload, is_error)

    print("\n== the same id, from a server that never minted it")
    with Session(project=args.project, binary=args.binary) as session:
        payload, is_error = session.call("viewport_diff_capture", {"baseline_capture_id": real_id})
        row("an id from the previous process", payload, is_error)

        # A well-formed id nobody has ever handed out. If this answers the same
        # way, the baseline is not being looked up at all.
        for label, made_up in [
            ("32 zeroes", "0" * 32),
            ("32 f's", "f" * 32),
            ("a plausible random id", "9a3c17e4bb02d5f6104e8c7d2b39af51"),
        ]:
            payload, is_error = session.call(
                "viewport_diff_capture", {"baseline_capture_id": made_up}
            )
            row(label, payload, is_error)

    print("\n== for contrast, the same question asked of a confirmation token")
    with Session(project=args.project, binary=args.binary) as session:
        payload, _ = session.call(
            "script_patch_method",
            {
                "file_path": "res://player.gd",
                "method_name": "take_damage",
                "new_definition": "func take_damage(amount: int) -> void:\n\t_hp -= amount\n",
                "dry_run": True,
            },
        )
        token = (payload.get("mutation_preview") or {}).get("confirmation_token") if isinstance(payload, dict) else None
    with Session(project=args.project, binary=args.binary) as session:
        payload, is_error = session.call(
            "script_patch_method",
            {
                "file_path": "res://player.gd",
                "method_name": "take_damage",
                "new_definition": "func take_damage(amount: int) -> void:\n\t_hp -= amount\n",
                "confirmation_token": token,
            },
        )
        row("a token from the previous process", payload, is_error)

    print("\n== does the diff notice a change it should notice?")
    with Session(project=args.project, binary=args.binary) as session:
        payload, _ = session.call("viewport_capture_frame", {})
        baseline = payload.get("capture_id") if isinstance(payload, dict) else None
        # Something that changes what the editor viewport shows.
        session.call("viewport_toggle_debug_draw", {"mode": "wireframe"})
        payload, is_error = session.call("viewport_diff_capture", {"baseline_capture_id": baseline})
        row("after toggling debug draw", payload, is_error)
        session.call("viewport_toggle_debug_draw", {"mode": "disabled"})

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
