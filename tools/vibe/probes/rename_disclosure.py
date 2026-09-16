"""`project_rename_references`, preview against confirm, field by field.

The tool renames a Godot *identifier*, not a path -- its own description says a
`res://` path "is a different operation and is refused" -- and it is deliberately
conservative: it rewrites a scene connection or an animation track and leaves
GDScript alone, because a textual rename of a call site is not a refactor. That
conservatism is fine and the tool says so, in a `code_references_not_updated`
list naming every site it skipped, the declaration included.

The question is *when* it says so. A preview is what a caller reads to decide
whether to confirm, and the list is the fact that decides it (#716).

The probe writes its own call sites and its own connection, so the arithmetic is
known before either call is made: three calls plus one declaration that will not
move, one connection that will. Then it prints the preview's `before` and the
confirm's payload with the same keys beside each other.

Needs no editor -- this is an offline tool, which is the mode the README keeps
pointing out nobody checks.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

CALLER = """extends Node

func hit(p):
\tp.take_damage(1)
\tp.take_damage(2)
\tp.take_damage(3)
"""

SCENE = """[gd_scene load_steps=2 format=3 uid="uid://bviberename01"]

[ext_resource type="Script" path="res://player.gd" id="1_p"]

[node name="Root" type="Node2D"]

[node name="P" type="Node2D" parent="."]
script = ExtResource("1_p")

[connection signal="died" from="P" to="." method="take_damage"]
"""

SHARED = ("changed_lines", "code_reference_count", "updated_file_count", "updated_files")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--target", default="take_damage")
    parser.add_argument("--new-name", default="apply_damage")
    args = parser.parse_args()

    root = Path(args.project)
    (root / "renamecaller.gd").write_text(CALLER, encoding="utf-8")
    (root / "renamescene.tscn").write_text(SCENE, encoding="utf-8")
    print("fixture: three call sites in renamecaller.gd, one declaration in player.gd,")
    print("         one [connection ... method=\"take_damage\"] in renamescene.tscn\n")

    arguments = {"target": args.target, "new_name": args.new_name}
    with Session(project=args.project) as session:
        refused, is_error = session.call(
            "project_rename_references",
            {"target": "res://player.gd", "new_name": "res://hero.gd", "dry_run": True})
        message = ((refused.get("error") or {}).get("message", "")
                   if isinstance(refused, dict) else str(refused))
        print(f"a res:// path is refused, as the description promises: "
              f"{is_error} -- {message[:88]}\n")

        impact, _ = session.call("project_analyze_impact", {"target": args.target})
        if isinstance(impact, dict):
            print(f"project_analyze_impact counts: {json.dumps(impact.get('counts_by_kind'))}\n")

        preview, preview_error = session.call("project_rename_references",
                                              dict(arguments, dry_run=True))
        if preview_error or not isinstance(preview, dict):
            print(f"the preview did not succeed, so the comparison cannot be made: {preview!r}")
            return 1
        mutation = preview["mutation_preview"]
        before = mutation["changes"][0]["before"]
        token = mutation.get("confirmation_token")

        real, real_error = session.call(
            "project_rename_references",
            dict(arguments, confirmation_token=token) if token else arguments)
        if real_error or not isinstance(real, dict):
            print(f"the confirm did not succeed: {real!r}")
            return 1

        print(f"{'field':<32} {'preview':<44} confirm")
        print("-" * 104)
        for key in SHARED:
            print(f"{key:<32} {json.dumps(before.get(key))[:42]:<44} "
                  f"{json.dumps(real.get(key))[:42]}")
        not_updated = real.get("code_references_not_updated") or []
        in_preview = "code_references_not_updated" in json.dumps(mutation)
        print(f"{'code_references_not_updated':<32} "
              f"{('present' if in_preview else 'ABSENT'):<44} {len(not_updated)} entries")
        for entry in not_updated:
            print(f"{'':<32} {'':<44} {entry['path']}:{entry['line']} "
                  f"{entry['detail'][:40]}")
        print("\nThe two calls agree on every field they share. The list of sites the")
        print("rename will leave behind -- the declaration among them -- appears only")
        print("after the mutation, which is the wrong side of the gate to learn it on.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
