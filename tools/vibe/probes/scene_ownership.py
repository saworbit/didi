"""Godot's own rules for a scene, asked of the tools that edit one.

Every earlier session mutated a flat scene the tool itself had created. A real
project is instanced sub-scenes, inherited scenes and scripts whose `extends`
does not match the node. The editor refuses most of what this probe asks for;
the question is whether the tools do, and if not, what the saved file holds.

Parts, selectable with --part: A an instanced sub-scene's internals (#589,
#591), B an inherited scene (#589), C a script whose base type the node cannot
take (#603), D signal connections (all refused correctly), E reach and a scene
instanced into itself (#590). Reads the saved file after every save; the file
is the only witness. Needs the `--fixtures` sandbox and, before Godot 4.7,
`scene_close` with `discard_unsaved` or a "reload" is only a tab switch.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mcp_client import Session  # noqa: E402

STRIP = [re.compile(r', "session": \{[^{}]*\}'), re.compile(r'"session": \{[^{}]*\}, '),
         re.compile(r'"endpoint": "[^"]*", '), re.compile(r'"available_without_engine": \[[^\]]*\], ')]


def show(label, result, width=900):
    payload, is_error = result
    text = json.dumps(payload) if not isinstance(payload, str) else payload
    for pat in STRIP:
        text = pat.sub("", text)
    if len(text) > width:
        text = text[:width] + f" ...(+{len(text) - width})"
    print(f"--- {label}\n    {'ERR' if is_error else 'ok '} {text}")
    return payload


def names(tree, depth=0, out=None):
    out = out if out is not None else []
    if not isinstance(tree, dict):
        return out
    extra = {k: v for k, v in tree.items() if k not in ("children", "name", "path", "type", "properties", "child_count")}
    out.append("  " * depth + f"{tree.get('name')} ({tree.get('type')}) {json.dumps(extra) if extra else ''}")
    for c in tree.get("children", []):
        names(c, depth + 1, out)
    return out


def hierarchy(S, label):
    payload, err = S.call("scene_get_hierarchy")
    if err or not isinstance(payload, dict):
        print(f"--- {label}: ERR {json.dumps(payload)[:400]}")
        return payload
    keys = {k: v for k, v in payload.items() if k not in ("scene_tree", "session")}
    print(f"--- {label}: {json.dumps(keys)[:300]}")
    for line in names(payload.get("scene_tree")):
        print("      " + line)
    return payload


def cat(project, rel):
    p = Path(project) / rel
    print(f"=== {rel} ({p.stat().st_size if p.exists() else 'missing'} bytes)")
    if p.exists():
        for line in p.read_text(encoding="utf-8").splitlines():
            print("    | " + line)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--project", required=True)
    ap.add_argument("--part", default="ABCDE")
    args = ap.parse_args()
    project = Path(args.project)
    S = Session(project=project)
    show("session", S.call("runtime_get_session"), width=160)

    if "A" in args.part:
        print("\n######## A: an instanced sub-scene's internals")
        show("open main", S.call("scene_open", {"scene_path": "res://main.tscn"}), width=200)
        show("instantiate sub.tscn", S.call("scene_instantiate_node", {"scene_path": "res://sub.tscn", "parent_path": "/root/Main", "name": "SubInst"}), width=500)
        hierarchy(S, "hierarchy with instance")
        show("Inner.owner", S.call("scene_get_property", {"target_node": "/root/Main/SubInst/Inner", "property_name": "owner"}), width=400)
        show("SubInst.scene_file_path", S.call("scene_get_property", {"target_node": "/root/Main/SubInst", "property_name": "scene_file_path"}), width=400)
        show("override Inner.position", S.call("scene_set_property", {"target_node": "/root/Main/SubInst/Inner", "property_name": "position", "value": {"x": 7, "y": 8}}), width=400)
        show("save", S.call("editor_save_scene", {}), width=200)
        cat(project, "main.tscn")
        show("remove Inner dry_run", S.call("scene_remove_node", {"dry_run": True, "target_node": "/root/Main/SubInst/Inner"}), width=700)
        show("remove Inner", S.call("scene_remove_node", {"target_node": "/root/Main/SubInst/Inner"}), width=500)
        hierarchy(S, "hierarchy after removing Inner")
        show("save", S.call("editor_save_scene", {}), width=200)
        cat(project, "main.tscn")
        show("close", S.call("scene_close", {"discard_unsaved": True}), width=200)
        show("open main", S.call("scene_open", {"scene_path": "res://main.tscn"}), width=200)
        hierarchy(S, "hierarchy after reload (is Inner back?)")
        show("reparent Inner out of its instance", S.call("scene_reparent_node", {"target_node": "/root/Main/SubInst/Inner", "new_parent_path": "/root/Main"}), width=500)
        hierarchy(S, "hierarchy after reparent")
        show("save", S.call("editor_save_scene", {}), width=200)
        cat(project, "main.tscn")
        show("close", S.call("scene_close", {"discard_unsaved": True}), width=100)
        show("open main", S.call("scene_open", {"scene_path": "res://main.tscn"}), width=100)
        hierarchy(S, "hierarchy after reload (where is Inner?)")
        show("duplicate SubInst", S.call("scene_duplicate_node", {"target_node": "/root/Main/SubInst"}), width=500)
        show("add Leaf under SubInst", S.call("scene_instantiate_node", {"node_type": "Node2D", "parent_path": "/root/Main/SubInst", "name": "Leaf"}), width=300)
        hierarchy(S, "hierarchy after duplicate+leaf")
        show("save", S.call("editor_save_scene", {}), width=200)
        cat(project, "main.tscn")
        show("undo", S.call("editor_undo", {}), width=300)
        show("undo", S.call("editor_undo", {}), width=300)
        hierarchy(S, "after two undos")

    if "B" in args.part:
        print("\n######## B: an inherited scene")
        show("open derived", S.call("scene_open", {"scene_path": "res://derived.tscn"}), width=300)
        hierarchy(S, "derived hierarchy (does it say it inherits?)")
        show("remove inherited Child dry", S.call("scene_remove_node", {"dry_run": True, "target_node": "/root/Main/Child"}), width=500)
        show("remove inherited Child", S.call("scene_remove_node", {"target_node": "/root/Main/Child"}), width=500)
        hierarchy(S, "derived after remove")
        show("add Label", S.call("scene_instantiate_node", {"node_type": "Label", "parent_path": "/root/Main", "name": "Added"}), width=300)
        show("rename inherited root?", S.call("scene_set_property", {"target_node": "/root/Main", "property_name": "name", "value": "Renamed"}), width=300)
        show("save", S.call("editor_save_scene", {}), width=200)
        cat(project, "derived.tscn")
        show("close", S.call("scene_close", {"discard_unsaved": True}), width=100)
        show("open derived", S.call("scene_open", {"scene_path": "res://derived.tscn"}), width=100)
        hierarchy(S, "derived after reload")

    if "C" in args.part:
        print("\n######## C: a script whose extends does not match the node")
        show("open main", S.call("scene_open", {"scene_path": "res://main.tscn"}), width=100)
        show("attach Node3D script to Sprite2D dry", S.call("script_attach_to_node", {"dry_run": True, "target_node": "/root/Main/Child", "script_path": "res://bad3d.gd"}), width=600)
        show("attach Node3D script to Sprite2D", S.call("script_attach_to_node", {"target_node": "/root/Main/Child", "script_path": "res://bad3d.gd"}), width=600)
        show("Child.script", S.call("scene_get_property", {"target_node": "/root/Main/Child", "property_name": "script"}), width=400)
        show("call hello()", S.call("scene_call_method", {"target_node": "/root/Main/Child", "method_name": "hello"}), width=400)
        show("engine errors", S.call("runtime_read_logs", {"minimum_level": "error"}), width=700)
        show("save", S.call("editor_save_scene", {}), width=200)
        cat(project, "main.tscn")
        show("detach", S.call("script_detach_from_node", {"target_node": "/root/Main/Child"}), width=300)

    if "D" in args.part:
        print("\n######## D: signals")
        show("open main", S.call("scene_open", {"scene_path": "res://main.tscn"}), width=100)
        show("attach player.gd", S.call("script_attach_to_node", {"target_node": "/root/Main/Child", "script_path": "res://player.gd"}), width=200)
        show("connect died -> Main.no_such_method", S.call("signal_connect", {"emitter_node": "/root/Main/Child", "signal_name": "died", "target_node": "/root/Main", "target_method": "no_such_method"}), width=600)
        show("connect no_such_signal", S.call("signal_connect", {"emitter_node": "/root/Main/Child", "signal_name": "no_such_signal", "target_node": "/root/Main", "target_method": "print_tree"}), width=600)
        show("connect died -> Main.print_tree", S.call("signal_connect", {"emitter_node": "/root/Main/Child", "signal_name": "died", "target_node": "/root/Main", "target_method": "print_tree"}), width=600)
        show("connect same again", S.call("signal_connect", {"emitter_node": "/root/Main/Child", "signal_name": "died", "target_node": "/root/Main", "target_method": "print_tree"}), width=600)
        show("list connections", S.call("signal_list_connections", {"target_node": "/root/Main/Child"}), width=900)
        show("emit died with 0 args (declared 1)", S.call("signal_emit", {"target_node": "/root/Main/Child", "signal_name": "died", "arguments": []}), width=600)
        show("emit died with 2 args", S.call("signal_emit", {"target_node": "/root/Main/Child", "signal_name": "died", "arguments": ["x", "extra"]}), width=600)
        show("emit died with 1 arg (int not String)", S.call("signal_emit", {"target_node": "/root/Main/Child", "signal_name": "died", "arguments": [5]}), width=600)
        show("engine errors after emits", S.call("runtime_read_logs", {"minimum_level": "error"}), width=900)
        show("disconnect never connected", S.call("signal_disconnect", {"emitter_node": "/root/Main/Child", "signal_name": "died", "target_node": "/root/Main", "target_method": "never"}), width=500)
        show("save", S.call("editor_save_scene", {}), width=200)
        cat(project, "main.tscn")
        show("close", S.call("scene_close", {"discard_unsaved": True}), width=100)
        show("open main", S.call("scene_open", {"scene_path": "res://main.tscn"}), width=100)
        show("list connections after reload", S.call("signal_list_connections", {"target_node": "/root/Main/Child"}), width=900)
        show("engine errors after reload", S.call("runtime_read_logs", {"minimum_level": "error"}), width=900)

    if "E" in args.part:
        print("\n######## E: reach and recursion")
        show("open main", S.call("scene_open", {"scene_path": "res://main.tscn"}), width=100)
        show("call_method queue_free on root dry", S.call("scene_call_method", {"dry_run": True, "target_node": "/root/Main", "method_name": "queue_free"}), width=700)
        show("call get_child_count on /root", S.call("scene_call_method", {"target_node": "/root", "method_name": "get_child_count"}), width=500)
        show("get_property on /root", S.call("scene_get_property", {"target_node": "/root", "property_name": "title"}), width=400)
        show("hierarchy root_path /root depth1", S.call("scene_get_hierarchy", {"root_path": "/root", "max_depth": 1}), width=700)
        show("instantiate main.tscn into itself dry", S.call("scene_instantiate_node", {"dry_run": True, "scene_path": "res://main.tscn", "parent_path": "/root/Main", "name": "Self"}), width=600)
        show("instantiate main.tscn into itself", S.call("scene_instantiate_node", {"scene_path": "res://main.tscn", "parent_path": "/root/Main", "name": "Self"}), width=600)
        hierarchy(S, "after self-instance")
        show("save", S.call("editor_save_scene", {}), width=300)
        cat(project, "main.tscn")
        show("close", S.call("scene_close", {"discard_unsaved": True}), width=100)
        show("open main again", S.call("scene_open", {"scene_path": "res://main.tscn"}), width=500)
        hierarchy(S, "after reload of a self-referencing scene")
        show("engine errors", S.call("runtime_read_logs", {"minimum_level": "error"}), width=900)
    S.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
