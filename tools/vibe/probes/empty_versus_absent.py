"""Nothing there, against no such thing -- asked of every reader that can answer both.

The oldest lesson in this directory, and the one that keeps paying: most
findings here are one response shape standing in for two states a caller must be
able to tell apart. "No presets configured" against "the file is unreadable"
(#403). "Nothing depends on this" against "you typo'd the path" (#404). "Here is
your subtree" against "there is no such node" (#401). None of them is a crash,
so nothing else catches them.

Every one of those was found by narrowing a single tool. This asks the whole
question at once: for each reader that can return an empty answer, put the empty
case beside the absent case and print whether the two responses differ at all.
A row where they are identical is a caller who cannot tell "this node has no
animation tracks" from "this node is not in the scene".

The pairs are chosen so that the *only* difference is the state, not the
arguments: same tool, same shape of argument, one naming something real and
empty and one naming something that is not there. A pair whose two calls differ
in more than that proves nothing.

    python tools/vibe/probes/empty_versus_absent.py -p SANDBOX

Live rows need an editor; they say so rather than quietly reporting a 503 pair
as a match, because two identical refusals are not the finding this is looking
for.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

ABSENT_NODE = "/root/Main/NoSuchNodeAnywhere"

# (label, tool, arguments naming something real and empty, arguments naming
#  something that is not there)
PAIRS: list[tuple[str, str, dict, dict]] = [
    (
        "groups on a node in none / on a node that is not there",
        "scene_list_groups",
        {"target_node": "/root/Main/Child"},
        {"target_node": ABSENT_NODE},
    ),
    (
        "a resource file that holds nothing / a resource path with no file",
        "resource_inspect",
        {"resource_path": "res://empty_probe.gd"},
        {"resource_path": "res://not_written.tres"},
    ),
    (
        "tracks on a node with no AnimationPlayer / on a node that is not there",
        "anim_list_tracks",
        {"animation_player_path": "/root/Main/Child"},
        {"animation_player_path": ABSENT_NODE},
    ),
    (
        "uniforms on a node with no ShaderMaterial / on a node that is not there",
        "shader_list_uniforms",
        {"target_node": "/root/Main/Child", "property_name": "material"},
        {"target_node": ABSENT_NODE, "property_name": "material"},
    ),
    (
        "connections on a node with none / on a node that is not there",
        "signal_list_connections",
        {"target_node": "/root/Main/Child"},
        {"target_node": ABSENT_NODE},
    ),
    (
        "symbols in an empty script / in a script that is not there",
        "script_get_symbols",
        {"file_path": "res://empty_probe.gd"},
        {"file_path": "res://not_written.gd"},
    ),
    (
        "text search with no match / in a directory that is not there",
        "project_search_text",
        {"query": "zzzz_no_such_string_zzzz"},
        {"query": "extends", "search_path": "res://no_such_directory"},
    ),
    (
        "a blackboard key that expired / one never written",
        "blackboard_read",
        {"board": "eva-probe", "path": "expired", "include_metadata": True},
        {"board": "eva-probe", "path": "never", "include_metadata": True},
    ),
    (
        "keys of an empty board / of a board that was never created",
        "blackboard_list_keys",
        {"board": "eva-probe-empty"},
        {"board": "eva-probe-absent"},
    ),
]


def strip(payload: object) -> object:
    """Drop the fields that differ on every call regardless of the state."""
    if not isinstance(payload, dict):
        return payload
    noisy = {"session", "handshake", "captured_at", "path", "target_node", "script_path",
             "group", "board", "query", "file_path", "requested_root_path",
             "resource_path", "animation_player_path", "search_path"}
    return {key: strip(value) for key, value in payload.items() if key not in noisy}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    project = Path(args.project)
    with Session(project=project, binary=args.binary) as session:
        # The "real and empty" halves have to actually exist, or the row is two
        # absent cases wearing different names and its match means nothing.
        (project / "empty_probe.gd").write_text("", encoding="utf-8")
        session.call("blackboard_write", {"board": "eva-probe", "path": "expired",
                                          "value": {"v": 1}, "ttl_seconds": 1})
        session.call("blackboard_write", {"board": "eva-probe-empty", "path": "seed", "value": 1})
        session.call("blackboard_clear", {"board": "eva-probe-empty", "dry_run": True})
        import time

        time.sleep(2)

        identical = 0
        for label, tool, empty_args, absent_args in PAIRS:
            empty, empty_error = session.call(tool, empty_args)
            absent, absent_error = session.call(tool, absent_args)
            same = json.dumps(strip(empty), sort_keys=True) == json.dumps(strip(absent), sort_keys=True)
            identical += same
            print(f"\n--- {tool}: {label}")
            print(f"    empty : isError={empty_error} {json.dumps(strip(empty), sort_keys=True)[:320]}")
            print(f"    absent: isError={absent_error} {json.dumps(strip(absent), sort_keys=True)[:320]}")
            print(f"    indistinguishable: {same}")

        print(f"\n{identical} of {len(PAIRS)} pairs answer identically.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
