"""`tools/list` asked either side of the events that change it.

`initialize` publishes `capabilities.tools.listChanged: false`. In MCP that is
not decoration: it is the server telling the host that one `tools/list` at
startup is enough, and a host that believes it never asks again. Every session
since the sixth has read `_meta.didi.currentMode` off that listing and treated
it as the thing a host uses to decide whether to offer a tool (#503, #504).

Both cannot be true unless the listing is constant. This asks whether it is,
inside one process, across the three events that move the bridge: an editor
already attached, an explicit detach, and a re-attach. The listing is hashed
per tool so the report is the set of tools whose published description of
themselves moved, not a diff of 126 schemas.

Each comparison also prints how many `notifications/tools/list_changed` arrived
over the same span. Entries that moved with no notification beside them is the
finding; either number on its own says nothing.

The count attaches to the span rather than to a single call, because the
notification is written after the response it follows and a line-per-request
client does not see it until its next read. Each span here ends in a
`tools/list`, which is the read that collects it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

CHANGED = "notifications/tools/list_changed"


def fingerprint(tools: list[dict]) -> dict[str, str]:
    return {
        tool["name"]: hashlib.sha256(
            json.dumps(tool, sort_keys=True).encode()
        ).hexdigest()[:12]
        for tool in tools
    }


def modes(tools: list[dict]) -> dict[str, str]:
    out = {}
    for tool in tools:
        meta = (tool.get("_meta") or {}).get("didi") or {}
        out[tool["name"]] = meta.get("currentMode", "<absent>")
    return out


def compare(label: str, before: list[dict], after: list[dict],
            announced: int | None = None) -> None:
    fb, fa = fingerprint(before), fingerprint(after)
    moved = sorted(n for n in fb if n in fa and fb[n] != fa[n])
    gone = sorted(set(fb) - set(fa))
    new = sorted(set(fa) - set(fb))
    mb, ma = modes(before), modes(after)
    print(f"\n--- {label} ---")
    print(f"  tools before {len(fb)}, after {len(fa)}; added {len(new)}, removed {len(gone)}")
    print(f"  entries whose published JSON changed: {len(moved)}")
    # Only for a span that was actually drained. The cumulative comparison at
    # the end spans two that were, so counting there would report zero for
    # notifications that did arrive.
    if announced is not None:
        silent = "   <- the listing moved in silence" if moved and not announced else ""
        print(f"  notifications/tools/list_changed over the same span: {announced}{silent}")
    if moved:
        shifts: dict[tuple[str, str], list[str]] = {}
        for name in moved:
            shifts.setdefault((mb.get(name, "?"), ma.get(name, "?")), []).append(name)
        for (b, a), names in sorted(shifts.items()):
            print(f"    currentMode {b!r} -> {a!r}: {len(names)} tools")
            print(f"      e.g. {', '.join(names[:4])}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    with Session(project=args.project) as session:
        caps = session.initialize_result["result"]["capabilities"]
        print("initialize capabilities.tools :", json.dumps(caps.get("tools")))
        print("initialize capabilities.resources:", json.dumps(caps.get("resources")))
        print("initialize capabilities.prompts:", json.dumps(caps.get("prompts")))

        sessions, _ = session.call("runtime_list_sessions", {})
        live = [s for s in (sessions.get("sessions") or []) if s.get("alive")]
        print(f"\nalive editor sessions visible: {len(live)}")
        for s in live:
            print(f"  pid={s.get('pid')} project={s.get('project_path')}")
        if not live:
            print("no live editor; run this with one open on the sandbox")
            return 1
        target = live[0].get("session_id")

        at_start = session.tools()
        session.take_notifications(CHANGED)
        attached, err = session.call("runtime_attach_session", {"session_id": target})
        print(f"\nattach -> isError={err} {json.dumps(attached)[:160]}")
        after_attach = session.tools()
        compare("startup listing -> after explicit attach", at_start, after_attach,
                len(session.take_notifications(CHANGED)))

        detached, err = session.call("runtime_detach_session", {})
        print(f"\ndetach -> isError={err} {json.dumps(detached)[:160]}")
        after_detach = session.tools()
        compare("after attach -> after detach", after_attach, after_detach,
                len(session.take_notifications(CHANGED)))

        session.call("runtime_attach_session", {"session_id": target})
        after_reattach = session.tools()
        compare("after detach -> after re-attach", after_detach, after_reattach,
                len(session.take_notifications(CHANGED)))
        compare("startup listing -> after re-attach", at_start, after_reattach)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
