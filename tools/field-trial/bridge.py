"""Say which GDExtension actually served a field trial, read from the transcript.

A trial is seeded against a server binary and scored against that binary's
manifest. The live half of every call is served by a different file, the
GDExtension the tester copies into the project by hand, and nothing in the seed
can see it. Field trial 03 spent about an hour concluding that a shipped
capability did not exist, because the bridge answering its calls was six days
older than the server being tested. The coverage report for that run is honest
about which tools were called and silent about which build answered them.

Since #326 the server reports the pairing on the local session calls. Two things
about that report are easy to get backwards, so they are encoded here and tested
rather than remembered:

* `server_build_id` is always present; `bridge_build_matches` appears **only**
  when the two disagree. A match is the absence of a complaint, not a field.
* An extension too old to publish `build_id` at all counts as a mismatch, and
  the server already reports it as one. Absence of the field on the bridge side
  is not absence of evidence.

A run in which no observation appeared at all is the third outcome and the one
worth naming loudest: nothing here proves the tester ever attached to a live
session, so the live half of that run is unaccounted for rather than clean.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Iterable

MATCHED = "matched"
MISMATCHED = "mismatched"
NOT_OBSERVED = "not_observed"


def _blocks(record: object) -> list[dict]:
    if not isinstance(record, dict):
        return []
    message = record.get("message")
    if not isinstance(message, dict):
        return []
    content = message.get("content")
    if not isinstance(content, list):
        return []
    return [block for block in content if isinstance(block, dict)]


def _result_text(block: dict) -> str:
    """The text of a tool result, whichever shape the client wrote it in.

    A result arrives as a plain string or as a list of content blocks depending
    on the tool and the client version, and a reader that handles only the shape
    it happened to see first reports a clean bridge for a run it could not read.
    """
    content = block.get("content")
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "\n".join(
            part.get("text", "")
            for part in content
            if isinstance(part, dict) and isinstance(part.get("text"), str)
        )
    return ""


def extract_observations(
    transcript_lines: Iterable[str], server: str = "didi"
) -> list[dict]:
    """Every server/bridge pairing this run observed, in the order it saw them.

    Results are correlated back to the call that produced them rather than
    scanned loose, so a log file the tester happened to read, or a payload it
    quoted into a note, cannot be counted as evidence about the bridge.
    """
    prefix = f"mcp__{server}__"
    pending: dict[str, str] = {}
    observations: list[dict] = []
    for line in transcript_lines:
        stripped = line.strip()
        if not stripped:
            continue
        try:
            record = json.loads(stripped)
        except json.JSONDecodeError:
            continue
        for block in _blocks(record):
            kind = block.get("type")
            if kind == "tool_use":
                name = block.get("name")
                identifier = block.get("id")
                if isinstance(name, str) and name.startswith(prefix) and isinstance(identifier, str):
                    pending[identifier] = name[len(prefix) :]
                continue
            if kind != "tool_result":
                continue
            tool = pending.get(block.get("tool_use_id"))
            if tool is None:
                continue
            try:
                payload = json.loads(_result_text(block))
            except json.JSONDecodeError:
                continue
            if not isinstance(payload, dict):
                continue
            build = payload.get("server_build_id")
            if not isinstance(build, str) or not build:
                continue
            observations.append(
                {
                    "tool": tool,
                    "server_build_id": build,
                    # Absent means matched. Present means the server said no.
                    "matches": payload.get("bridge_build_matches", True) is not False,
                }
            )
    return observations


def bridge_verdict(observations: list[dict], seeded_build_id: str | None = None) -> dict:
    """What the observations say about the build that served the run.

    `seeded_build_id` is what the harness handed the tester. Comparing it here
    catches the case the server cannot see for itself: a tester that ignored the
    seeded configuration and pointed its client at some other didi.exe. The
    server would report that build happily agreeing with its own extension,
    which is true and answers a different question than the one the trial asked.
    """
    builds = sorted({observation["server_build_id"] for observation in observations})
    mismatched = [observation for observation in observations if not observation["matches"]]

    if not observations:
        verdict, note = NOT_OBSERVED, (
            "No live session call reported a build pairing, so this run does not say which "
            "GDExtension served it. Treat the live half as unaccounted for rather than clean: "
            "either the tester never attached to a session, or the server predates the field."
        )
    elif mismatched:
        verdict, note = MISMATCHED, (
            f"{len(mismatched)} of {len(observations)} observations reported a bridge from a "
            "different build than the server. Findings from this run about live behaviour are "
            "about that pairing, not about the build the trial was seeded with."
        )
    else:
        verdict, note = MATCHED, (
            f"All {len(observations)} observations reported the bridge and the server from the "
            "same build."
        )

    unexpected = [build for build in builds if seeded_build_id and build != seeded_build_id]
    if unexpected:
        verdict = MISMATCHED
        note += (
            f" The run also answered from a server build the harness did not seed: seeded "
            f"{seeded_build_id}, observed {', '.join(unexpected)}. The client was pointed at a "
            "different binary than the one this trial is scored against."
        )

    return {
        "verdict": verdict,
        "note": note,
        "observations": len(observations),
        "mismatched_observations": len(mismatched),
        "server_build_ids": builds,
        "seeded_build_id": seeded_build_id,
        "tools_observed": sorted({observation["tool"] for observation in observations}),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transcript", required=True, type=Path, help="Session transcript .jsonl")
    parser.add_argument("--server", default="didi", help="MCP server alias in the client config")
    parser.add_argument("--seeded-build-id", help="build id the harness handed the tester")
    parser.add_argument("--output", type=Path, help="Write the report here instead of stdout")
    args = parser.parse_args(argv)

    with args.transcript.open(encoding="utf-8", errors="replace") as handle:
        observations = extract_observations(handle, server=args.server)
    report = bridge_verdict(observations, seeded_build_id=args.seeded_build_id)
    rendered = json.dumps(report, indent=2, sort_keys=True)

    if args.output is None:
        print(rendered)
    else:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    return 0 if report["verdict"] != MISMATCHED else 1


if __name__ == "__main__":
    raise SystemExit(main())
