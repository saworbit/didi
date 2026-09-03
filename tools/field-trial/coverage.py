"""Report which Didi tools a field trial actually invoked.

The server logs `Method: tools/call` at DEBUG and names the tool only when a
call throws, so the server log cannot say which tools a tester reached for. The
client transcript can: every invocation is a `tool_use` block named
`mcp__<server>__<tool>`. This reads that transcript and compares it against the
manifest the trial was seeded with, so the interesting number is not how much
worked but which implemented tools never occurred to the tester at all.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable


def _tool_use_blocks(record: object) -> list[dict]:
    if not isinstance(record, dict):
        return []
    message = record.get("message")
    if not isinstance(message, dict):
        return []
    content = message.get("content")
    if not isinstance(content, list):
        return []
    return [
        block
        for block in content
        if isinstance(block, dict) and block.get("type") == "tool_use"
    ]


def extract_invocations(
    transcript_lines: Iterable[str], server: str = "didi"
) -> dict[str, int]:
    """Count invocations by bare tool name, keyed off the MCP server alias.

    A transcript is appended to while a session runs and can hold partial or
    non-message records, so an unreadable line is skipped rather than fatal.
    """
    prefix = f"mcp__{server}__"
    counts: dict[str, int] = {}
    for line in transcript_lines:
        stripped = line.strip()
        if not stripped:
            continue
        try:
            record = json.loads(stripped)
        except json.JSONDecodeError:
            continue
        for block in _tool_use_blocks(record):
            name = block.get("name")
            if isinstance(name, str) and name.startswith(prefix):
                tool = name[len(prefix) :]
                counts[tool] = counts.get(tool, 0) + 1
    return counts


def build_report(counts: dict[str, int], implemented_names: Iterable[str]) -> dict:
    """Split observed calls against the manifest's implemented set.

    `unknown` is deliberately kept out of every total. A name the manifest does
    not know is either a legacy alias or a typo, and letting either raise the
    coverage number would make the one figure people quote the least reliable.
    """
    implemented = set(implemented_names)
    called = {name: count for name, count in sorted(counts.items()) if name in implemented}
    unknown = sorted(name for name in counts if name not in implemented)
    uncalled = sorted(implemented - set(called))
    coverage = (100.0 * len(called) / len(implemented)) if implemented else 0.0
    return {
        "called": called,
        "uncalled": uncalled,
        "unknown": unknown,
        "totals": {
            "implemented": len(implemented),
            "distinct_called": len(called),
            "invocations": sum(called.values()),
            "coverage_percent": round(coverage, 1),
        },
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--transcript", required=True, type=Path, help="Session transcript .jsonl"
    )
    parser.add_argument(
        "--manifest", required=True, type=Path, help="tool-manifest.json from the build"
    )
    parser.add_argument("--server", default="didi", help="MCP server alias in the client config")
    parser.add_argument("--output", type=Path, help="Write the report here instead of stdout")
    args = parser.parse_args(argv)

    with args.transcript.open(encoding="utf-8", errors="replace") as handle:
        counts = extract_invocations(handle, server=args.server)

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    report = build_report(counts, manifest["names"]["implemented"])
    rendered = json.dumps(report, indent=2, sort_keys=True)

    if args.output is None:
        print(rendered)
    else:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
