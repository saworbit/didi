"""The preview path against the call path, one level below argument names.

#399 was "the dry run issued a token for arguments the real call refuses", and
it was closed by checking argument *names* before previewing. Session nine's
`preview_vs_real.py` then found the same seam in node-path resolution (#571).
This is the third layer: argument **values**.

`signal_emit` validates its `arguments` against four rules in
`handleSignalEmit` -- nesting depth over 8, an array over 64 entries, an object
over 64 keys, and a 32 KB cap on the serialised whole. All four run on the
confirm path only. A `dry_run` on values that break them returns
`preview_kind: "target_state"`, `target_read: true`, a real `before`, and a
confirmation token; spending that token on the identical arguments is refused
`400 unsupported_signal_emit_argument` (#616).

The control in the same run is a value that breaks none of them, which must
preview *and* confirm. Without it this probe cannot tell "the gate now refuses
the bad values" from "the gate refuses everything".

Both halves have to happen in one process, because a confirmation token lives in
the server's memory and a probe list cannot carry it between two `probe.py`
runs. That is why this is a script.

    python tools/vibe/sandbox.py SANDBOX --fixtures --launch GODOT
    python tools/vibe/probes/preview_value_rules.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402


def nested(depth: int) -> object:
    value: object = 1
    for _ in range(depth):
        value = {"k": value}
    return value


# (label, arguments, what a correct build does at the dry run)
CASES = [
    # `renamed` is declared with no parameters, so the control has to send none.
    # Sending one refuses at confirm with "The number of arguments given does not
    # match..." -- which is the engine, correctly, and is not this finding. It is
    # worth noticing separately that the preview does not check declared arity
    # either, though a signal's parameter list is introspectable.
    ("control: no arguments", [], "mints a token"),
    ("nested 9 deep (cap 8)", [nested(9)], "refuses"),
    ("array of 200 (cap 64)", [list(range(200))], "refuses"),
    ("object of 200 keys (cap 64)", [{str(i): i for i in range(200)}], "refuses"),
    ("40 KB string (cap 32 KB)", ["x" * 40000], "refuses"),
]


def row(label: str, expected: str, observed: str) -> None:
    verdict = "ok " if expected == observed else "DIFF"
    print(f"  {verdict} {label:32} expected={expected:<14} observed={observed}")


def token_of(payload: object) -> str | None:
    if isinstance(payload, dict) and isinstance(payload.get("mutation_preview"), dict):
        return payload["mutation_preview"].get("confirmation_token")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--node", default="/root/Domain",
                        help="Any node in the open scene; `renamed` is a Node signal.")
    args = parser.parse_args()

    session = Session(project=args.project)
    try:
        session.call("scene_open", {"scene_path": "res://domain.tscn"})
        # The control has to have a listener. `signal_emit` refuses outright when
        # nothing is connected to the signal (#624), so without this the control
        # fails for a reason that has nothing to do with the preview path and the
        # probe reads as "the gate refuses everything".
        connection = {"emitter_node": args.node, "signal_name": "renamed",
                      "target_node": "/root/Domain/Shaded", "target_method": "show"}
        session.call("signal_connect", connection)
        for label, arguments, expected in CASES:
            base = {"target_node": args.node, "signal_name": "renamed", "arguments": arguments}
            payload, is_error = session.call("signal_emit", dict(base, dry_run=True))
            token = token_of(payload)
            if not token:
                row(label, expected, "refuses" if is_error else "no token, no refusal")
                continue
            # A token was minted. The finding is only real if the confirm then
            # refuses the very same arguments, so ask.
            confirmed, confirm_error = session.call(
                "signal_emit", dict(base, confirmation_token=token))
            if confirm_error:
                message = confirmed.get("error", {}).get("message", "?") if isinstance(confirmed, dict) else "?"
                row(label, expected, f"token minted, confirm refused: {message[:38]}")
            else:
                row(label, expected, "mints a token")
    finally:
        try:
            session.call("signal_disconnect", connection)
        except Exception:
            pass
        session.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
