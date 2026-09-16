"""What `scene_call_method` will call, against what it says it will call.

`method_name` is described as "A method the node's **script** declares. Names
beginning with an underscore are refused: that is Godot's mark for an engine
callback or a private helper." That is two rules, and the first is the wide one:
every `Node` has hundreds of *native* methods that no script declares, and some
of them destroy things. `queue_free`, `set_script`, `replace_by`, `set_owner`
and `add_child` are all public, all underscore-free, and all mutations the
confirmation gate exists for.

The expression sandbox next door answers the same question with an allowlist and
a denylist and refuses nineteen escape attempts by name. This asks whether the
method channel has the same floor.

Rows, each printed with what the scene looked like afterwards:

* a method the script really declares (the control);
* a method nothing declares;
* an underscore method, which the description promises is refused;
* native methods that mutate, destroy, or attach code.

Needs a live editor and `--fixtures`. It runs against a throwaway node it makes
itself, so a row that succeeds does not take the sandbox with it.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

ROWS = [
    ("a method the script declares (the control)", "take_damage", [1]),
    ("a method nothing declares", "no_such_method", []),
    ("an underscore method", "_ready", []),
    ("a native getter", "get_name", []),
    ("a native setter", "set_name", ["Renamed"]),
    ("free the node", "queue_free", []),
    ("free it now", "free", []),
    ("detach the node's script", "set_script", [None]),
    ("re-own the node", "set_owner", [None]),
    ("emit one of its signals", "emit_signal", ["ready"]),
    ("call another method by name", "call", ["queue_free"]),
    ("connect a signal to a method", "connect", ["ready", "queue_free"]),
    ("set a property the writer would gate", "set", ["name", "SetViaCall"]),
]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    with Session(project=args.project) as session:
        session.call("scene_open", {"scene_path": "res://main.tscn"})
        print(f"{'row':<44} {'verdict':<58} node afterwards")
        print("-" * 130)
        for label, method, arguments in ROWS:
            # A fresh subject per row, so one row destroying it does not decide
            # the next. The script is a @tool one: a plain script does not run in the editor and every
            # row then reads "not a @tool script", which is a fact about the setup.
            session.call("scene_instantiate_node",
                         {"parent_path": "/root/Main", "node_type": "Node2D", "name": "Subject"})
            attach = {"target_node": "/root/Main/Subject", "script_path": "res://toolsubject.gd"}
            # player.gd extends Node2D, so the subject is a Node2D; attaching a
            # script the node cannot take fails and every later row then reads
            # "the node has no script", which is a fact about the setup.
            preview_attach, _ = session.call("script_attach_to_node", dict(attach, dry_run=True))
            attach_token = ((preview_attach.get("mutation_preview") or {}).get("confirmation_token")
                            if isinstance(preview_attach, dict) else None)
            if attach_token:
                attach = dict(attach, confirmation_token=attach_token)
            session.call("script_attach_to_node", attach)
            call = {"target_node": "/root/Main/Subject", "method_name": method,
                    "arguments": arguments}
            # The whole tool is confirmation-gated, so a bare call is refused
            # before any rule about the method is reached. The preview is the
            # first place the method name is looked at, and #571 is why it is
            # asked separately: the preview path is not the call path.
            preview, preview_error = session.call("scene_call_method", dict(call, dry_run=True))
            token = ((preview.get("mutation_preview") or {}).get("confirmation_token")
                     if isinstance(preview, dict) else None)
            if preview_error:
                message = ((preview.get("error") or {}).get("message", "")
                           if isinstance(preview, dict) else str(preview))
                verdict = f"preview REFUSED: {message[:41]}"
            elif not token:
                verdict = "preview issued no token"
            else:
                payload, is_error = session.call(
                    "scene_call_method", dict(call, confirmation_token=token))
                if is_error:
                    message = ((payload.get("error") or {}).get("message", "")
                               if isinstance(payload, dict) else str(payload))
                    verdict = f"previewed, then REFUSED: {message[:33]}"
                else:
                    verdict = (f"CALLED:  {json.dumps(payload.get('result', payload))[:40]}"
                               if isinstance(payload, dict) else f"CALLED: {payload!r}")
            tree, _ = session.call("scene_get_hierarchy", {})
            names = json.dumps(tree).count('"Subject"') if isinstance(tree, dict) else -1
            after = "still there" if names else "GONE"
            print(f"{label:<44} {verdict:<58} {after}")
            session.call("scene_remove_node", {"target_node": "/root/Main/Subject"})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
