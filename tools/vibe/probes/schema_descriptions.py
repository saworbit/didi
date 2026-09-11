"""Count the parameters a caller has to guess at.

`surface_census.py` asks what the server *answers*. This asks what it *offers*:
for every registered tool, how many of its parameters carry a `description`.

The question earns a census rather than a glance because the answer is only
interesting in aggregate. One undocumented parameter is a gap; a hundred and
ninety-nine of them is a missing default in whatever generates the schemas, and
the two want different fixes. The vibe README already lists five parameter names
that are not the obvious guess (`target_node` not `node_path`, `emitter_node` not
`source_node`, and so on) -- each of those cost a round trip to someone, and a
one-line description is what would have saved it.

`dry_run` is excluded. It is described everywhere, it is not tool-specific, and
leaving it in flatters the numbers on every gated tool.

Usage::

    python tools/vibe/probe.py -p SANDBOX --list --dump-tools tools.json
    python tools/vibe/probes/schema_descriptions.py tools.json

    # or go and fetch the surface itself
    python tools/vibe/probes/schema_descriptions.py --project SANDBOX
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

IGNORED = {"dry_run"}


def load_tools(arguments: argparse.Namespace) -> list[dict]:
    if arguments.dump:
        raw = json.loads(Path(arguments.dump).read_text(encoding="utf-8"))
        return raw if isinstance(raw, list) else raw["tools"]
    with Session(project=arguments.project) as session:
        return session.tools()


def parameters(tool: dict) -> dict:
    properties = (tool.get("inputSchema") or {}).get("properties") or {}
    return {name: schema for name, schema in properties.items() if name not in IGNORED}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dump", nargs="?", help="A --dump-tools file from probe.py.")
    parser.add_argument("--project", help="Ask a server directly instead.")
    arguments = parser.parse_args()
    if not arguments.dump and not arguments.project:
        parser.error("pass a dumped tools file or --project")

    tools = load_tools(arguments)
    total = 0
    undocumented = 0
    silent: list[tuple[str, int]] = []
    for tool in tools:
        props = parameters(tool)
        if not props:
            continue
        missing = [name for name, schema in props.items() if not (schema or {}).get("description")]
        total += len(props)
        undocumented += len(missing)
        if missing and len(missing) == len(props):
            silent.append((tool["name"], len(props)))

    silent.sort(key=lambda entry: (-entry[1], entry[0]))
    print(f"tools: {len(tools)}")
    print(f"parameters: {total}, undocumented: {undocumented}")
    print(f"tools documenting none of their parameters: {len(silent)}")
    for name, count in silent:
        print(f"  {name:34} {count} parameters, 0 documented")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
