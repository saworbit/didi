"""resource_inspect's two type fields, against the header each text resource declares.

Session twenty-two read `type: Resource` for an `AudioStreamOggVorbis` it had
just written and nearly filed it. It is documented: `type` is the class a file's
extension implies, which for a `.tres` or `.res` is never more specific than
`Resource`, and `resource_type` carries the class the file's own
`[gd_resource]` header declares. This checks both, for every `.tres` at the
top of the project, and for an imported asset checks the `import` block against
the importer its `.import` file names. Green; kept as the regression probe, so
the next reading of `type: Resource` has somewhere to look first.

Works offline or with an editor attached, because resource_inspect is offline.

    python tools/vibe/probes/resource_inspect_types.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402


def row(label: str, expected: object, observed: object) -> bool:
    ok = expected == observed
    print(f"  {'ok  ' if ok else 'DIFF'} {label:64} expected={expected} observed={observed}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    project = Path(args.project).resolve()
    clean = True
    with Session(project=str(project), editor_log=False) as session:
        resources = sorted(project.glob("*.tres"))
        imported = sorted(p.with_suffix("") for p in project.glob("*.import"))
        print(f"{len(resources)} text resources and {len(imported)} imported assets at the top of {project}")
        for path in resources:
            header = path.read_text(encoding="utf-8", errors="replace").splitlines()[0]
            declared = (re.search(r'type="([^"]+)"', header) or [None, None])[1]
            payload, errored = session.call("resource_inspect", {"resource_path": "res://" + path.name})
            if errored:
                clean = row(f"{path.name} inspects", "ok", "refused") and clean
                continue
            clean = row(f"{path.name} type", "Resource", payload.get("type")) and clean
            clean = row(f"{path.name} resource_type", declared, payload.get("resource_type")) and clean
        for asset in imported:
            if not asset.is_file():
                continue
            sidecar = asset.with_name(asset.name + ".import").read_text(encoding="utf-8", errors="replace")
            importer = (re.search(r'^importer="([^"]+)"', sidecar, re.M) or [None, None])[1]
            payload, errored = session.call("resource_inspect", {"resource_path": "res://" + asset.name})
            if errored:
                clean = row(f"{asset.name} inspects", "ok", "refused") and clean
                continue
            block = payload.get("import") or {}
            clean = row(f"{asset.name} import.importer", importer, block.get("importer")) and clean
    return 0 if clean else 1


if __name__ == "__main__":
    raise SystemExit(main())
