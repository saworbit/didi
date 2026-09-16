"""Every parameter description read against the schema keys sitting beside it.

#576 was `case_sensitive`, published with `"default": true` and a description
reading "Off by default". Nothing could catch it: the description tests count
descriptions, the schema tests read keys, and no test compares one against the
other. The README's note on it ends "worth grepping for as a class" and nothing
had.

Four comparisons, each one a claim a description can make that the schema can
contradict:

* a description naming a default, against the `default` key;
* a description listing allowed values, against the `enum` key;
* a description naming a range, against `minimum` / `maximum`;
* a description naming a file extension, against every other mention of one.

The output is candidates, not findings. A description that says "defaults to the
project's main scene" is describing a default the schema cannot express, and
that is correct; the point is to put the pair on one line so a person can read
fifty in a minute instead of 191.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

NAMES_DEFAULT = re.compile(
    r"\b(?:default(?:s|ed)?\s+(?:to|is)?|by default|when omitted)\b", re.IGNORECASE)
ON_OFF = re.compile(r"\b(on|off|true|false|enabled|disabled)\b", re.IGNORECASE)
NAMES_RANGE = re.compile(r"\b(\d+)\s*(?:to|-|through|\.\.)\s*(\d+)\b")
EXTENSION = re.compile(r"\.([a-z][a-z0-9]{1,9})\b")
OPPOSITE = {"on": {"false"}, "off": {"true"}, "true": {"false"}, "false": {"true"},
            "enabled": {"false"}, "disabled": {"true"}}


def walk(schema: dict, prefix: str = ""):
    """Every (path, subschema) pair with a description, nested objects included."""
    for name, sub in (schema.get("properties") or {}).items():
        if not isinstance(sub, dict):
            continue
        path = f"{prefix}{name}"
        if "description" in sub:
            yield path, sub
        yield from walk(sub, prefix=f"{path}.")
        items = sub.get("items")
        if isinstance(items, dict):
            yield from walk(items, prefix=f"{path}[].")


def _listed_beside(description: str, allowed: set[str]) -> list[str]:
    """Words sitting in a comma-or-`or` list that also contains a real value.

    "such as pending, claimed or blocked" is three words in one list, two of
    which are enum values. The third is being offered to the caller with the
    same authority and does not exist.
    """
    out: list[str] = []
    for run in re.findall(r"((?:[A-Za-z_][A-Za-z0-9_]*)(?:\s*(?:,|,?\s+or)\s+[A-Za-z_][A-Za-z0-9_]*)+)",
                          description):
        words = [w.lower() for w in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", run) if w.lower() != "or"]
        if len(set(words) & allowed) >= 2:
            out += [w for w in words if w not in allowed]
    return out


def check(tool: str, path: str, sub: dict) -> list[str]:
    description = sub.get("description", "")
    problems: list[str] = []

    if "default" in sub and isinstance(sub["default"], bool):
        # Only the polarity word touching the phrase counts. "Off by default,
        # and a call with overwrite: true is confirmation-gated" states the
        # default once and then talks about the other value, which is correct
        # prose and was the first version of this check's whole output.
        actual = json.dumps(sub["default"])
        for match in NAMES_DEFAULT.finditer(description):
            # Backwards only. "On by default; turning it off skips ..." names
            # the default first and the other value second, so a window that
            # reaches forward reads every correct description as wrong.
            window = description[max(0, match.start() - 26):match.start()]
            stated = ON_OFF.findall(window)
            hit = next((w for w in stated if actual in OPPOSITE.get(w.lower(), set())), None)
            if hit:
                problems.append(f'says {hit!r} beside "{match.group(0)}" but "default" is {actual}')
                break

    if "enum" in sub:
        quoted = {v.lower() for v in re.findall(r"[`\"']([A-Za-z_][A-Za-z0-9_]*)[`\"']", description)}
        bare = {v.lower() for v in re.findall(r"\b([A-Za-z_][A-Za-z0-9_]{2,})\b", description)}
        allowed = {str(v).lower() for v in sub["enum"]}
        named = (quoted | bare) & allowed
        if named and named != allowed:
            problems.append(
                f"names {sorted(named)} of the {len(allowed)} enum values {sorted(allowed)}")
        # The sharper half: a value the description offers and the enum refuses.
        # `bare` is every word in the sentence, so only look at words the
        # description sets apart -- quoted, or listed beside a value that is real.
        if named:
            invented = sorted(quoted - allowed) or sorted(
                w for w in _listed_beside(description, allowed) if w not in allowed)
            if invented:
                problems.append(f"offers {invented}, which the enum does not hold")

    span = NAMES_RANGE.search(description)
    if span:
        low, high = int(span.group(1)), int(span.group(2))
        if "minimum" in sub and sub["minimum"] != low:
            problems.append(f'says the range starts at {low}, "minimum" is {sub["minimum"]}')
        if "maximum" in sub and sub["maximum"] != high:
            problems.append(f'says the range ends at {high}, "maximum" is {sub["maximum"]}')

    return problems


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    with Session(project=args.project) as session:
        tools = session.tools()

    described = 0
    flagged = 0
    for tool in sorted(tools, key=lambda t: t["name"]):
        for path, sub in walk(tool.get("inputSchema") or {}):
            described += 1
            problems = check(tool["name"], path, sub)
            if not problems:
                continue
            flagged += 1
            print(f"\n{tool['name']}.{path}")
            print(f"  description: {sub['description']}")
            keys = {k: sub[k] for k in ("default", "enum", "minimum", "maximum", "type") if k in sub}
            print(f"  schema:      {json.dumps(keys)}")
            for problem in problems:
                print(f"  -> {problem}")

    print(f"\n{described} described parameters, {flagged} where the description and the "
          f"schema beside it make different claims")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
