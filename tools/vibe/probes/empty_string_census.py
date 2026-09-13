"""The empty string, sent to every required string parameter on the surface.

#484 asked the numeric version of this question -- `max_depth` had no `minimum`
where every other limit on the surface had one, so -5 was accepted and clamped,
and the schema was the only thing that could have said no. The string version
had never been asked.

An empty string is a well-formed value of the declared type, so a schema without
`minLength` passes it through, and the handler behind it decides what "" means.
Two things to look for, and they travel together: a failure that is a bare
string rather than the error envelope, and a message that says the argument was
*missing* when it was supplied and empty (#553).

The run prints the parameters that carry `minLength` beside the ones that do
not, because the ones that carry it are the fix: the argument check answers
first, in the envelope, naming the property. A parameter that moves from the
second list to the first is the regression signal.

Usage::

    python tools/vibe/probes/empty_string_census.py SANDBOX
    python tools/vibe/probes/empty_string_census.py SANDBOX --verbose
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

SKIP = {"runtime_detach_session", "runtime_launch", "runtime_stop", "project_export",
        "editor_reload_project", "runtime_recover_editor"}

MISSING_WORDS = ("is required", "are required", "missing")


def filler(schema: dict) -> object:
    """A value the argument check will accept, for the parameters not under test."""
    if "enum" in schema:
        return schema["enum"][0]
    kind = schema.get("type")
    if isinstance(kind, list):
        kind = kind[0]
    if kind == "integer":
        return max(1, schema.get("minimum", 1))
    if kind == "number":
        return float(max(1, schema.get("minimum", 1)))
    if kind == "boolean":
        return False
    if kind == "array":
        return []
    if kind == "object":
        return {}
    return "x"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("project")
    parser.add_argument("-b", "--binary")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    rows: list[tuple[str, str, bool, str, str]] = []
    with Session(project=args.project, binary=args.binary) as session:
        for tool in sorted(session.tools(), key=lambda entry: entry["name"]):
            name = tool["name"]
            if name in SKIP:
                continue
            schema = tool.get("inputSchema", {})
            properties = schema.get("properties", {})
            required = schema.get("required", [])
            for target in required:
                declared = properties.get(target, {})
                if declared.get("type") != "string" or "enum" in declared:
                    continue
                arguments = {
                    entry: ("" if entry == target else filler(properties.get(entry, {})))
                    for entry in required
                }
                result = session.request(
                    "tools/call", {"name": name, "arguments": arguments}
                ).get("result") or {}
                bounded = "minLength" in declared
                if not result.get("isError"):
                    rows.append((name, target, bounded, "ok", ""))
                    continue
                text = (result.get("content") or [{}])[0].get("text", "")
                try:
                    body = json.loads(text)
                    message = body.get("error", {}).get("message", "") if isinstance(body, dict) else ""
                    rows.append((name, target, bounded, "envelope", message))
                except ValueError:
                    rows.append((name, target, bounded, "BARE", text))

    bounded = [row for row in rows if row[2]]
    print(f'{len(rows)} required string parameters asked with ""')
    print(f"  {len(bounded)} carry minLength; {len(rows) - len(bounded)} do not\n")

    print("-- the schema said no (minLength present): the answer to aim for")
    for name, target, is_bounded, kind, message in rows:
        if is_bounded and (args.verbose or len(bounded) < 6):
            print(f"   {name}.{target:22} {kind:9} {message[:90]}")
    if bounded and not args.verbose:
        sample = bounded[0]
        print(f"   e.g. {sample[0]}.{sample[1]}: {sample[4][:100]}")

    print('\n-- answered with a bare string (no code, no data)')
    for name, target, is_bounded, kind, message in rows:
        if kind == "BARE":
            print(f"   {name}.{target:22} minLength={is_bounded}\n       {message[:150]}")

    print("\n-- called the argument missing, though it was supplied and empty")
    for name, target, is_bounded, kind, message in rows:
        if any(word in message.lower() for word in MISSING_WORDS):
            print(f"   {name}.{target:22} minLength={is_bounded}\n       {message[:150]}")

    print('\n-- accepted "" and reported success')
    for name, target, is_bounded, kind, _message in rows:
        if kind == "ok":
            print(f"   {name}.{target:22} minLength={is_bounded}")

    print("\ncounts:", Counter(row[3] for row in rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
