#!/usr/bin/env python3
"""Trim extension_api.json into the class reference Didi ships.

The full dump is 6.9 MB and most of it is detail offline reflection has no use
for: method hashes, argument metadata, native struct layouts. This keeps the
parts a caller asks `script_reflect_class` for -- inheritance, properties,
methods, signals and enums -- which is about a fifth of the size and small
enough to sit next to the binary.

The output is committed as resources/didi_class_reference.json and read at
runtime. It is committed rather than generated during the build because
extension_api.json is a local artifact, produced by `godot --dump-extension-api`
and excluded by .gitignore, so no build machine has one. It is read at runtime
rather than compiled in because a megabyte and a half of generated C++ would be
paid for on every build on every platform.

Refresh it when the pinned Godot version moves:

    godot --headless --dump-extension-api --path .
    godot --headless --script tools/typed_array_setters.gd -- --output typed_array_setters.json
    python tools/generate_class_reference.py --api extension_api.json \
        --typed-setters typed_array_setters.json \
        --output resources/didi_class_reference.json

The second file is the element type behind an untyped Array property whose
setter takes a typed array, which the dump does not carry when the setter is
hidden. resource_create writes each element as that type (#1294).
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def method_signature(method: dict) -> dict:
    """One method, in the shape reflectClass already returns."""
    arguments = []
    for argument in method.get("arguments", []):
        rendered = f"{argument['name']}: {argument['type']}"
        if "default_value" in argument:
            rendered += f" = {argument['default_value']}"
        arguments.append(rendered)
    if method.get("is_vararg"):
        arguments.append("...")

    entry = {
        "returns": (method.get("return_value") or {}).get("type", "void"),
        "args": arguments,
    }
    # Only carry the flags that change how a caller may use the method.
    for flag in ("is_static", "is_const", "is_virtual"):
        if method.get(flag):
            entry[flag.removeprefix("is_")] = True
    return entry


def trim_class(entry: dict, element_types: dict | None = None) -> dict:
    properties = {}
    for prop in entry.get("properties", []):
        detail = {"type": prop.get("type", "")}
        element_type = (element_types or {}).get(prop["name"])
        if element_type and detail["type"] == "Array":
            detail["element_type"] = element_type
        # A property with no setter is read-only, which is worth knowing before
        # a caller tries scene_set_property on it.
        if "setter" not in prop or not prop["setter"]:
            detail["read_only"] = True
        properties[prop["name"]] = detail

    methods = {name: signature for name, signature in (
        (method["name"], method_signature(method)) for method in entry.get("methods", [])
    )}

    enums = {}
    for enum in entry.get("enums", []):
        enums[enum["name"]] = {value["name"]: value["value"] for value in enum.get("values", [])}

    trimmed = {
        "inherits": entry.get("inherits", ""),
        "properties": properties,
        "methods": methods,
        "signals": [signal["name"] for signal in entry.get("signals", [])],
    }
    if enums:
        trimmed["enums"] = enums
    if entry.get("is_refcounted"):
        trimmed["is_refcounted"] = True
    if entry.get("is_instantiable"):
        trimmed["is_instantiable"] = True
    return trimmed


def build_reference(api: dict, typed_setters: dict | None = None) -> dict:
    header = api.get("header", {})
    typed_setters = typed_setters or {}
    classes = {
        entry["name"]: trim_class(entry, typed_setters.get(entry["name"]))
        for entry in api.get("classes", [])
    }
    return {
        "schema": 1,
        # Recorded so a caller can see which Godot the reflection describes. The
        # dump is pinned in the repository and will not always match the editor
        # the caller is running.
        "api_version": header.get("version_full_name", "unknown"),
        "classes": classes,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--api", type=Path, required=True, help="path to extension_api.json")
    parser.add_argument("--output", type=Path, required=True, help="path to write the reference")
    parser.add_argument("--typed-setters", type=Path,
                        help="tools/typed_array_setters.gd output for the same engine")
    arguments = parser.parse_args()

    api = json.loads(arguments.api.read_text(encoding="utf-8"))
    typed_setters = (json.loads(arguments.typed_setters.read_text(encoding="utf-8"))
                     if arguments.typed_setters else None)
    reference = build_reference(api, typed_setters)

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(reference, separators=(",", ":"), sort_keys=True), encoding="utf-8"
    )
    print(
        f"Wrote {len(reference['classes'])} classes for {reference['api_version']} "
        f"to {arguments.output} ({arguments.output.stat().st_size} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
