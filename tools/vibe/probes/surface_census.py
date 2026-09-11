"""Three censuses of the whole tool surface, in one pass each.

A single tool behaving oddly is a finding. The same oddity on 76 of 126 tools is
a different finding with a different fix, and the only way to tell which one you
have is to ask every tool the same question. These three are cheap enough to
re-run at the start of a session and are worth diffing against the numbers in
the README's session log.

    python tools/vibe/probes/surface_census.py SANDBOX --which unknown-arguments
    python tools/vibe/probes/surface_census.py SANDBOX --which execution-modes
    python tools/vibe/probes/surface_census.py SANDBOX --which error-envelopes

`unknown-arguments` sends one property no tool declares and counts who says so.
`execution-modes` groups tools by the `execution_mode` they report -- run it
with an editor attached, which is when `offline_fallback` is interesting.
`error-envelopes` counts who answers a semantic failure with a bare string
instead of the structured `{"error": {"code", "message"}}` envelope.

Each census fills required arguments with plausible junk to get past the
required-argument check, which is not what is being measured.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from mcp_client import Session  # noqa: E402

UNKNOWN = "bogus_property_xyz"


def junk_arguments(schema: dict) -> dict:
    """Something type-correct for every required property, and nothing more."""
    arguments: dict = {}
    for name in schema.get("required", []):
        declared = schema.get("properties", {}).get(name, {}).get("type")
        if isinstance(declared, list):
            declared = declared[0]
        if declared == "string":
            arguments[name] = "res://player.gd" if ("path" in name or "file" in name) else "zzz"
        elif declared == "integer":
            arguments[name] = 1
        elif declared == "number":
            arguments[name] = 1.0
        elif declared == "boolean":
            arguments[name] = True
        elif declared == "array":
            arguments[name] = ["zzz"]
        elif declared == "object":
            arguments[name] = {}
        else:
            arguments[name] = "zzz"
    return arguments


def unknown_arguments(session: Session, tools: list[dict]) -> None:
    rejects, accepts = [], []
    for tool in tools:
        arguments = dict(junk_arguments(tool.get("inputSchema", {})), **{UNKNOWN: 1})
        payload, _ = session.call(tool["name"], arguments)
        blob = json.dumps(payload)
        (rejects if (UNKNOWN in blob and "Unknown argument" in blob) else accepts).append(tool["name"])
    print(f"rejects the unknown property ({len(rejects)}):\n  {', '.join(sorted(rejects))}\n")
    print(f"accepts it silently ({len(accepts)}):\n  {', '.join(sorted(accepts))}")


def execution_modes(session: Session, tools: list[dict]) -> None:
    grouped = defaultdict(list)
    for tool in tools:
        payload, _ = session.call(tool["name"], junk_arguments(tool.get("inputSchema", {})))
        mode = payload.get("execution_mode") if isinstance(payload, dict) else None
        if mode is None and isinstance(payload, dict) and isinstance(payload.get("error"), dict):
            mode = payload["error"].get("data", {}).get("execution_mode")
        grouped[mode].append(tool["name"])
    for mode, names in sorted(grouped.items(), key=lambda item: -len(item[1])):
        print(f"{mode} ({len(names)}): {', '.join(sorted(names))}\n")


def error_envelopes(session: Session, tools: list[dict]) -> None:
    bare = []
    for tool in tools:
        payload, is_error = session.call(tool["name"], junk_arguments(tool.get("inputSchema", {})))
        # An unimplemented tool answers with a bare string by design; that is a
        # registration fact, not a failure to wrap an error.
        if is_error and isinstance(payload, str) and "unimplemented" not in payload:
            bare.append((tool["name"], payload))
    print(f"bare-string errors ({len(bare)}):")
    for name, message in bare:
        print(f"  {name:28} | {message[:110]}")


CENSUSES = {
    "unknown-arguments": unknown_arguments,
    "execution-modes": execution_modes,
    "error-envelopes": error_envelopes,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("--which", choices=sorted(CENSUSES), required=True)
    arguments = parser.parse_args()

    with Session(project=arguments.project) as session:
        CENSUSES[arguments.which](session, session.tools())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
