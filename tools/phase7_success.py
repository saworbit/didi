#!/usr/bin/env python3
"""Hold real Phase 7 answers to the success contract each schema declares (#861).

Each file in ``schemas/phase7`` has two halves. ``$defs.request`` is generated
into the tool's ``inputSchema``. ``$defs.success`` describes a successful
answer, and nothing used to compare it with one, so it drifted:
``signal_list_connections`` went on forbidding fields it had answered with
since they were added. This compares answers with it.

The live harness collects every successful Phase 7 answer it gets, one JSON
line each, and runs this on the file; ``tests/test_phase7_schema_contract.py``
runs it on the answers the contract snapshots recorded, with no engine.

    python tools/phase7_success.py <answers.jsonl>

Each line is ``{"tool": <name>, "response": <the JSON-RPC response>}``. Exit 0
when every answer matches, 1 when one does not, 2 when the file is unusable.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterable

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_DIR = ROOT / "schemas" / "phase7"

# Fields that are not one tool's own, so no Phase 7 contract repeats them. The
# bridge marks every live answer and names the session kind it ran in; every
# edit of the open scene says whether the scene is saved, with a limitation
# when it is not (#1049); the Phase 7 forwarder names the tool and any
# transport repeat; the server attaches the session and the follow-up steps
# (Q6), and the engine's own lines when it printed any.
ENVELOPE = frozenset({
    "execution_mode", "is_live_engine", "session_kind",
    "scene_saved", "limitation",
    "tool", "canonical_tool", "transport",
    "session", "follow_up", "engine_diagnostics", "engine_diagnostics_note",
})


def contract_names() -> list[str]:
    """Every Phase 7 name with a schema file, the three blocked ones included."""
    return sorted(path.name.removesuffix(".schema.json") for path in SCHEMA_DIR.glob("*.schema.json"))


def success_schema(name: str) -> dict:
    """The success contract as a standalone schema, with the definitions it refers to."""
    document = json.loads((SCHEMA_DIR / f"{name}.schema.json").read_text(encoding="utf-8"))
    return {**document["$defs"]["success"], "$defs": document["$defs"]}


def payload_of(response: dict) -> dict | None:
    """A successful answer's structuredContent, or None for anything else."""
    result = response.get("result")
    if not isinstance(result, dict) or result.get("isError"):
        return None
    payload = result.get("structuredContent")
    if payload is None:
        texts = [block.get("text") for block in result.get("content", []) if block.get("type") == "text"]
        payload = json.loads(texts[0]) if texts else None
    return payload if isinstance(payload, dict) else None


def problems(answers: Iterable[tuple[str, dict]]) -> tuple[list[str], set[str]]:
    """Every way the answers break their contracts, and the tools that answered."""
    validators = {name: Draft202012Validator(success_schema(name)) for name in contract_names()}
    found: list[str] = []
    answered: set[str] = set()
    for tool, payload in answers:
        if tool not in validators:
            continue
        answered.add(tool)
        own = {key: value for key, value in payload.items() if key not in ENVELOPE}
        for error in validators[tool].iter_errors(own):
            where = "/".join(str(part) for part in error.absolute_path) or "(top level)"
            found.append(f"{tool} at {where}: {error.message}")
    return sorted(set(found)), answered


def read_answers(path: Path) -> list[tuple[str, dict]]:
    """The successful answers in a JSON-lines file, as (tool, structuredContent)."""
    answers = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        record: Any = json.loads(line)
        payload = payload_of(record["response"])
        if payload is not None:
            answers.append((record["tool"], payload))
    return answers


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("answers", type=Path, help="a JSON-lines file of tool names and responses")
    args = parser.parse_args(argv)
    try:
        answers = read_answers(args.answers)
    except (OSError, ValueError, KeyError) as error:
        print(f"phase7_success: cannot read {args.answers}: {error}", file=sys.stderr)
        return 2
    found, answered = problems(answers)
    for problem in found:
        print(problem)
    print(f"Phase 7 success contracts: {len(answers)} answers from {len(answered)} tools "
          f"({', '.join(sorted(answered))}); {len(found)} problems.")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
