"""The three `--ui-app` modes, diffed before any tool is called.

`--ui-app auto|always|off` is one of the flags `didi --help` lists and no session
had started the server in. Session thirteen did this for `--yolo` and found that
the only difference on the published surface was one fact inside
`didi_control_room`; the same question asked of this flag is sharper, because
the MCP Apps surface is a *negotiated extension* -- the server declares it, the
client declares it, and only then is it live.

`off` is not a negotiation. The operator has decided, and the handshake is the
one consumer of the flag that does not know (#717).

Four things per mode, all of them things a host reads before it calls anything:

* `capabilities.extensions` from `initialize`;
* whether `resources/list` carries the `ui://` resource;
* what `resources/read` does with that URI;
* the size and content of `didi_control_room`'s own `_meta`.

One row is worth watching and was not filed: in `auto`, against a client that
declares no UI extension, `resources/list` hides the `ui://` resource and
`resources/read` serves all 30 KB of it anyway. Serving something a caller asked
for by name is defensible and hiding it from a listing is the documented
behaviour, so this is a note rather than a finding -- but it is the listing and
the answer disagreeing inside one process and one mode, which is the shape of
#503 and #701.

Needs no editor.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

UI_RESOURCE = "ui://didi/control-room"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    declarations: dict[str, str] = {}
    for mode in ("auto", "always", "off"):
        print(f"\n===== --ui-app {mode} =====")
        with Session(project=args.project, extra_args=["--ui-app", mode]) as session:
            capabilities = session.initialize_result["result"]["capabilities"]
            extensions = json.dumps(capabilities.get("extensions"), sort_keys=True)
            declarations[mode] = extensions
            print(f"  initialize capabilities.extensions: {extensions}")

            listed = [r["uri"] for r in session.request("resources/list", {})["result"]["resources"]]
            print(f"  resources/list carries {UI_RESOURCE}: {UI_RESOURCE in listed}")

            response = session.request("resources/read", {"uri": UI_RESOURCE})
            if "result" in response:
                contents = (response["result"].get("contents") or [{}])[0]
                print(f"  resources/read: served {contents.get('mimeType')!r}, "
                      f"{len(contents.get('text') or '')} bytes")
            else:
                print(f"  resources/read: refused -- "
                      f"{response['error'].get('message', '')[:82]}")

            entry = {t["name"]: t for t in session.tools()}["didi_control_room"]
            meta = json.dumps(entry.get("_meta"), sort_keys=True)
            print(f"  didi_control_room _meta: {len(meta)} bytes, "
                  f"mentions the UI surface: {'ui' in meta.lower()}")

    print("\n--- the handshake declaration, across the three modes ---")
    for mode, extensions in declarations.items():
        print(f"  {mode:<7} {extensions}")
    distinct = len(set(declarations.values()))
    print(f"\n  distinct declarations: {distinct} of 3")
    if distinct == 1:
        print("  Every other consumer of the flag honours it; the handshake does not.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
