"""`csharp_check_build` given something to build, and given a broken toolchain.

Fourteen sessions have listed this tool as unswept, because a C# project is the
one fixture the sandbox never had. It is the third member of the family
`script_check_syntax` and `shader_check_compile` belong to -- assemble an answer
by shelling out to a compiler and merging its diagnostics into your own -- and
#677 is what the first of those does when the subprocess never runs.

Four states, and a control that must stay green:

* a real `.csproj` with one error and one warning in it, so the counts have a
  known answer printed by MSBuild itself in the same output;
* `DOTNET_BIN` at a real file that is not dotnet -- #677's exact shape;
* `DOTNET_BIN` at a path with nothing behind it;
* `DOTNET_BIN` at a directory;
* a project with no `.csproj` anywhere, the ordinary state of a GDScript game.

Prints each answer beside the diagnostic counts MSBuild's own summary states,
because the payload carries both and nothing compares them.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import batch, call, unwrap  # noqa: E402

SUMMARY = re.compile(r"^\s*(\d+) (Warning|Error)\(s\)", re.MULTILINE)


def run(project: str, env: dict[str, str] | None, arguments: dict) -> tuple[object, bool | None]:
    responses, _ = batch([call("csharp_check_build", arguments, 1)], project=project, env=env)
    return unwrap(responses[-1])


def show(label: str, payload: object, is_error: bool | None) -> None:
    print(f"\n--- {label} ---")
    if not isinstance(payload, dict):
        print(f"  isError={is_error} {payload!r}")
        return
    if is_error:
        print(f"  isError=True {json.dumps(payload)[:400]}")
        return
    counts = {k: payload.get(k) for k in
              ("success", "has_errors", "exit_code", "diagnostics_count", "project_file")}
    print(f"  {json.dumps(counts)}")
    raw = payload.get("raw_output") or ""
    stated = {kind.lower(): int(n) for n, kind in SUMMARY.findall(raw)}
    if stated:
        print(f"  MSBuild's own summary says: {stated}"
              f"  (total {sum(stated.values())})")
    severities: dict[str, int] = {}
    for d in payload.get("diagnostics") or []:
        severities[d.get("severity", "?")] = severities.get(d.get("severity", "?"), 0) + 1
    print(f"  the tool reports:            {severities}  (total {payload.get('diagnostics_count')})")
    paths = {d.get("path") for d in payload.get("diagnostics") or []}
    for p in sorted(x for x in paths if x):
        print(f"  diagnostics[].path: {p!r}")
    print(f"  fields naming which toolchain ran: "
          f"{sorted(k for k in payload if 'engine' in k or 'dotnet' in k or 'sdk' in k) or 'none'}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("csharp_project", help="a sandbox holding a .csproj")
    parser.add_argument("--plain-project", help="a sandbox holding no .csproj at all")
    parser.add_argument("--not-dotnet", default=sys.executable,
                        help="a real executable that is not dotnet")
    args = parser.parse_args()

    show("a real .csproj, one error and one warning", *run(args.csharp_project, None, {}))
    show("DOTNET_BIN at a real file that is not dotnet",
         *run(args.csharp_project, {"DOTNET_BIN": args.not_dotnet}, {}))
    show("DOTNET_BIN at a path with nothing behind it",
         *run(args.csharp_project, {"DOTNET_BIN": str(Path(args.csharp_project) / "no-such-tool")}, {}))
    show("DOTNET_BIN at a directory",
         *run(args.csharp_project, {"DOTNET_BIN": args.csharp_project}, {}))
    if args.plain_project:
        show("a project with no .csproj (the control)", *run(args.plain_project, None, {}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
