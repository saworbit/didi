#!/usr/bin/env python3
"""Generate cwd-independent, standalone Phase 7 request schemas for C++."""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
from typing import Any


NAMES = (
    "anim_list_tracks",
    "anim_play_track",
    "gridmap_set_cells",
    "nav_bake_mesh",
    "nav_query_path",
    "physics_raycast_query",
    "physics_simulate_step",
    "runtime_get_call_stack",
    "runtime_inject_input",
    "runtime_read_profiler",
    "signal_connect",
    "signal_disconnect",
    "signal_emit",
    "signal_list_connections",
    "tilemap_get_used_rect",
    "tilemap_set_cells",
    "viewport_set_camera_transform",
    "viewport_toggle_debug_draw",
)

MUTATIONS = frozenset(
    {
        "anim_play_track",
        "gridmap_set_cells",
        "nav_bake_mesh",
        "physics_simulate_step",
        "runtime_inject_input",
        "signal_connect",
        "signal_disconnect",
        "signal_emit",
        "tilemap_set_cells",
        "viewport_set_camera_transform",
        "viewport_toggle_debug_draw",
    }
)
CONFIRMED_MUTATIONS = frozenset({"signal_emit"})


def _walk_refs(value: Any):
    if isinstance(value, dict):
        ref = value.get("$ref")
        if ref is not None:
            if not isinstance(ref, str):
                raise ValueError("$ref must be a string")
            yield ref
        for child in value.values():
            yield from _walk_refs(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk_refs(child)


def _standalone_request(name: str, document: dict[str, Any]) -> dict[str, Any]:
    expected_id = f"https://didi.local/schemas/phase7/{name}.schema.json"
    if document.get("$schema") != "https://json-schema.org/draft/2020-12/schema":
        raise ValueError(f"{name}: unexpected $schema")
    if document.get("$id") != expected_id:
        raise ValueError(f"{name}: unexpected $id")
    if document.get("$ref") != "#/$defs/request":
        raise ValueError(f"{name}: root must reference #/$defs/request")

    definitions = document.get("$defs")
    if not isinstance(definitions, dict):
        raise ValueError(f"{name}: $defs must be an object")
    request = definitions.get("request")
    if not isinstance(request, dict) or request.get("type") != "object":
        raise ValueError(f"{name}: $defs.request must be an object schema")

    root = copy.deepcopy(request)
    root["$schema"] = "https://json-schema.org/draft/2020-12/schema"
    root["$id"] = f"https://didi.local/schemas/phase7/generated/{name}.request.schema.json"

    reachable: set[str] = set()
    pending = list(_walk_refs(root))
    while pending:
        ref = pending.pop()
        prefix = "#/$defs/"
        if not ref.startswith(prefix) or ref == prefix or "/" in ref[len(prefix) :]:
            raise ValueError(f"{name}: unsupported non-local or malformed $ref {ref!r}")
        definition_name = ref[len(prefix) :]
        if definition_name == "request":
            raise ValueError(f"{name}: recursive request reference is not standalone")
        if definition_name in reachable:
            continue
        definition = definitions.get(definition_name)
        if not isinstance(definition, dict):
            raise ValueError(f"{name}: missing referenced definition {definition_name!r}")
        reachable.add(definition_name)
        pending.extend(_walk_refs(definition))

    if reachable:
        root["$defs"] = {
            definition_name: copy.deepcopy(definitions[definition_name])
            for definition_name in sorted(reachable)
        }

    properties = root.setdefault("properties", {})
    if not isinstance(properties, dict):
        raise ValueError(f"{name}: request properties must be an object")
    if name in MUTATIONS:
        properties["dry_run"] = {
            "type": "boolean",
            "default": False,
            "description": "Preview this mutation without executing it.",
        }
    if name in CONFIRMED_MUTATIONS:
        properties["confirmation_token"] = {
            "type": "string",
            "minLength": 64,
            "maxLength": 64,
            "pattern": "^[0-9a-f]{64}$",
            "description": "Single-use token returned by a matching dry run.",
        }
    root["additionalProperties"] = False
    return root


def materialize_schemas(schema_dir: Path) -> dict[str, dict[str, Any]]:
    expected = {f"{name}.schema.json" for name in NAMES}
    actual = {path.name for path in schema_dir.glob("*.schema.json")}
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise ValueError(f"schema inventory mismatch: missing={missing}, extra={extra}")

    materialized: dict[str, dict[str, Any]] = {}
    source_ids: set[str] = set()
    for name in NAMES:
        source = schema_dir / f"{name}.schema.json"
        with source.open("r", encoding="utf-8") as handle:
            document = json.load(handle)
        if not isinstance(document, dict):
            raise ValueError(f"{name}: schema root must be an object")
        source_id = document.get("$id")
        if not isinstance(source_id, str) or source_id in source_ids:
            raise ValueError(f"{name}: missing or duplicate $id")
        source_ids.add(source_id)
        materialized[name] = _standalone_request(name, document)
    return materialized


def _render_header() -> str:
    return """#pragma once

#include \"didi/common/types.hpp\"

#include <span>
#include <string_view>

namespace didi::mcp::phase7 {

[[nodiscard]] std::span<const std::string_view> canonicalNames();
[[nodiscard]] const json& standaloneRequestSchema(std::string_view name);

} // namespace didi::mcp::phase7
"""


def _render_source(schemas: dict[str, dict[str, Any]]) -> str:
    lines = [
        '#include "didi/mcp/phase7_schemas.hpp"',
        "",
        "#include <array>",
        "#include <stdexcept>",
        "#include <string>",
        "",
        "namespace didi::mcp::phase7 {",
        "namespace {",
        f"constexpr std::array<std::string_view, {len(NAMES)}> kNames{{{{",
    ]
    lines.extend(f'    "{name}",' for name in NAMES)
    lines.extend(["}};", "} // namespace", ""])
    lines.extend(
        [
            "std::span<const std::string_view> canonicalNames() {",
            "    return kNames;",
            "}",
            "",
            "const json& standaloneRequestSchema(std::string_view name) {",
        ]
    )
    for index, name in enumerate(NAMES):
        compact = json.dumps(schemas[name], sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        literal = json.dumps(compact, ensure_ascii=True)
        keyword = "if" if index == 0 else "else if"
        lines.extend(
            [
                f'    {keyword} (name == "{name}") {{',
                f"        static const json schema = json::parse({literal});",
                "        return schema;",
                "    }",
            ]
        )
    lines.extend(
        [
            '    throw std::out_of_range("Unknown Phase 7 request schema: " + std::string(name));',
            "}",
            "",
            "} // namespace didi::mcp::phase7",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema-dir", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()

    schemas = materialize_schemas(args.schema_dir.resolve())
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.source.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(_render_header(), encoding="utf-8", newline="\n")
    args.source.write_text(_render_source(schemas), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
