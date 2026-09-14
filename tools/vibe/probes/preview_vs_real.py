"""What a dry run promises, against what the real call then refuses.

#399 was "the dry run issued a token for arguments the real call refuses", and
it was closed by checking the argument *names* on the preview path. This asks
the same question one level down: an argument whose name and type are fine and
whose **value** the handler rejects.

`scene_add_to_group` with `target_node: ".."` is the case that started this.
The real call answers `400 Parent-relative '..' paths are not allowed in the
edited scene`. The dry run returns a preview describing the mutation as
planned. Both readers on the same path -- `scene_get_property` and
`scene_get_hierarchy` -- refuse `..` outright, so the rule is not in doubt and
is not new; it simply is not run before a preview is written.

That matters more than a wasted round trip, because the preview is the artifact
a human approves. A preview is a promise about a call that will be made later;
a preview for a call that cannot succeed is a promise the server already knows
it cannot keep, and the response says `requires_confirmation` and
`planned_mutation` in the same breath.

Some tools say so honestly -- `preview_kind: "argument_binding"` with a `before`
that reads "not read: this tool has no preview probe". That is a disclosure,
not a defence: the tools that carry it still print `kind: planned_mutation` for
a plan that is going to 400.

The census pairs each call: dry run first, then the identical call for real,
and prints the pair only when they disagree.

Destroys scene state. Throwaway sandbox, live editor.

    python tools/vibe/probes/preview_vs_real.py --project SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# Each case is a value the handler is known to refuse, with an argument set
# that is otherwise complete and well typed, so nothing upstream of the
# handler can answer first.
CASES: list[tuple[str, dict, str]] = [
    ("scene_add_to_group", {"target_node": "..", "group": "probe"}, "parent-relative node path"),
    ("scene_add_to_group", {"target_node": "../..", "group": "probe"}, "walking above the root"),
    (
        "scene_add_to_group",
        {"target_node": "/root/Main/../Main", "group": "probe"},
        "parent segment in the middle",
    ),
    ("scene_remove_node", {"target_node": ".."}, "parent-relative node path"),
    (
        "scene_set_property",
        {"target_node": "..", "property_name": "name", "value": "Hijacked"},
        "parent-relative node path",
    ),
    (
        "scene_reparent_node",
        {"target_node": "/root/Main/Child", "new_parent_path": ".."},
        "parent-relative new parent",
    ),
    (
        "scene_duplicate_node",
        {"target_node": ".."},
        "parent-relative node path",
    ),
    (
        "scene_add_to_group",
        {"target_node": "/root/Nope", "group": "probe"},
        "a node that is not there",
    ),
    (
        "scene_remove_node",
        {"target_node": "/root/Nope"},
        "a node that is not there",
    ),
]


def describe(payload: object, is_error: bool | None) -> str:
    if is_error:
        if isinstance(payload, dict):
            error = payload.get("error", payload)
            if isinstance(error, dict):
                return f"{error.get('code')} {error.get('message', '')[:90]}"
        return f"error {json.dumps(payload)[:90]}"
    if isinstance(payload, dict) and isinstance(payload.get("mutation_preview"), dict):
        preview = payload["mutation_preview"]
        changes = preview.get("changes") or [{}]
        return (
            f"preview kind={preview.get('preview_kind')} "
            f"change={changes[0].get('kind')} "
            f"token={'yes' if preview.get('confirmation_token') else 'no'}"
        )
    return f"ok {json.dumps(payload)[:90]}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    parser.add_argument("--all", action="store_true", help="Print agreeing pairs too.")
    args = parser.parse_args()

    disagreements = 0
    with Session(project=args.project, binary=args.binary) as session:
        for name, arguments, why in CASES:
            preview, preview_error = session.call(name, dict(arguments, dry_run=True))
            real, real_error = session.call(name, arguments)

            # A preview that refused and a real call that refused agree, whatever
            # the wording. The finding is a preview that promised and a real call
            # that would not.
            agreed = bool(preview_error) == bool(real_error)
            if agreed and not args.all:
                continue
            if not agreed:
                disagreements += 1
            print(f"--- {name}  ({why})")
            print(f"    arguments: {json.dumps(arguments)}")
            print(f"    dry_run  : {describe(preview, preview_error)}")
            print(f"    for real : {describe(real, real_error)}")

    print(f"\n{disagreements} of {len(CASES)} cases preview a call the server then refuses.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
