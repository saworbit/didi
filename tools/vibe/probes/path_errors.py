"""Ask every path-taking tool about a path that is not there.

The error-envelope census in `surface_census.py` sends junk arguments, so the
argument validator answers first and the census reports zero bare-string errors.
That is the census asking the wrong question: the tools that skip the envelope
skip it in their *path* validator, which only runs once the arguments are
well-formed. A well-typed `res://no_such.gd` gets past the first gate and into
the second.

So this probe sends arguments that are all valid and all wrong in the same way --
a file that does not exist, a directory that does not exist, a parent traversal --
and asks what shape comes back. A bare string here is the same defect as #420 in
a place #420's fix did not reach.

Usage::

    python tools/vibe/probes/path_errors.py SANDBOX
    python tools/vibe/probes/path_errors.py SANDBOX --verbose
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

MISSING_SCRIPT = "res://no_such.gd"
TRAVERSAL = "res://sub/../a.gd"

CASES: dict[str, dict] = {
    "script_check_syntax": {"file_path": MISSING_SCRIPT},
    "analyze_script_diagnostics": {"file_path": MISSING_SCRIPT},
    "script_get_symbols": {"file_path": MISSING_SCRIPT},
    "script_create": {"script_path": TRAVERSAL, "source_text": "extends Node\n"},
    "patch_script_symbols": {
        "file_path": MISSING_SCRIPT,
        "method_name": "x",
        "new_definition": "func x():\n\tpass\n",
    },
    "script_patch_method": {
        "file_path": MISSING_SCRIPT,
        "method_name": "x",
        "new_definition": "func x():\n\tpass\n",
    },
    "script_attach_to_node": {"target_node": "/root/Main/Child", "script_path": MISSING_SCRIPT},
    "resource_inspect": {"resource_path": "res://no_such.tres"},
    "shader_check_compile": {"shader_path": "res://no_such.gdshader"},
    "project_analyze_impact": {"target": MISSING_SCRIPT},
    "project_rename_references": {"target": MISSING_SCRIPT, "new_name": "other.gd"},
    "project_search_text": {"query": "x", "search_path": "res://no_such_search_dir"},
    "project_search_symbols": {"query": "x", "search_path": "res://no_such_search_dir"},
    "project_set_autoload": {"name": "Foo", "path": MISSING_SCRIPT},
    "project_export": {"preset": "Nope", "output_path": "res://out.exe"},
    "gridmap_export_mesh_library": {"source_scene": "res://no_such.tscn", "output_path": "res://out.tres"},
    "scene_open": {"scene_path": "res://no_such.tscn"},
    "scene_create": {"scene_path": "res://sub/../a.tscn"},
    "scene_pack_branch": {"target_node": "/root/Main/Child", "scene_path": "res://sub/../b.tscn"},
    "scene_instantiate_node": {"parent_path": "/root/Main", "scene_path": "res://no_such.tscn"},
    "asset_reimport": {"paths": ["res://no_such.png"]},
    "viewport_create_test_lab": {"target_resource_path": "res://no_such.tscn"},
    "create_visual_test_lab": {"target_resource_path": "res://no_such.tscn"},
    "execute_test_session": {"scene_path": "res://no_such.tscn"},
    "project_list_resources": {"search_path": "res://no_such_search_dir"},
    "blackboard_read": {"path": "no/such/key"},
}


def shape(payload: object) -> str:
    if isinstance(payload, str):
        return "bare-string"
    if isinstance(payload, dict) and "error" in payload:
        return "envelope"
    return "no-error"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("--verbose", action="store_true", help="Print every answer, not just the bare ones.")
    arguments = parser.parse_args()

    bare: list[str] = []
    quiet: list[str] = []
    with Session(project=arguments.project) as session:
        for name, args in CASES.items():
            payload, _ = session.call(name, args)
            kind = shape(payload)
            if kind == "bare-string":
                bare.append(name)
            elif kind == "no-error":
                quiet.append(name)
            if arguments.verbose or kind != "envelope":
                print(f"{name:30} {kind:12} {json.dumps(payload)[:140]}")

    print(f"\nbare-string errors ({len(bare)}): {bare}")
    print(f"no error at all ({len(quiet)}): {quiet}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
