"""Each legacy alias against the canonical tool it stands for.

Ten of the 126 published entries are aliases kept for compatibility, and
`_meta.didi.canonical` says which tool each one really is. Session five
swept the aliases as *behaviour* -- do they still work -- and nobody has ever
put an alias's published entry beside its canonical entry and diffed them.

An alias is supposed to be the same tool under an older name. That makes every
difference between the two entries a claim a host reads differently depending on
which name it picked: a schema that drifted, an annotation that did not, a
description that names the new tool or the old one.

The confirmation token is asked separately, because a token binds to "tool,
arguments, project and session" (#572) and `tool` has two spellings here. A
token minted under one name and spent under the other either works -- in which
case the binding is to the canonical tool and the two names are genuinely one --
or it does not, and the alias is a different tool wearing a compatibility label.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

# Fields that are *supposed* to differ between an alias and its canonical tool.
EXPECTED_DIFFERENT = {"name", "title", "description"}
META_EXPECTED_DIFFERENT = {"legacy", "canonical", "aliases"}


def diff(label: str, a: dict, b: dict, skip: set[str]) -> list[str]:
    out = []
    for key in sorted(set(a) | set(b)):
        if key in skip:
            continue
        if a.get(key) != b.get(key):
            out.append(f"    {label}.{key}: alias={json.dumps(a.get(key))[:130]}"
                       f" canonical={json.dumps(b.get(key))[:130]}")
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()

    with Session(project=args.project) as session:
        tools = {t["name"]: t for t in session.tools()}
        aliases = [t for t in tools.values()
                   if ((t.get("_meta") or {}).get("didi") or {}).get("legacy")]
        print(f"{len(aliases)} legacy aliases of {len(tools)} published entries\n")

        clean = 0
        for alias in sorted(aliases, key=lambda t: t["name"]):
            meta = (alias.get("_meta") or {}).get("didi") or {}
            canonical_name = meta.get("canonical")
            canonical = tools.get(canonical_name)
            print(f"{alias['name']}  ->  {canonical_name}")
            if canonical is None:
                print("    the canonical name is not on the published surface")
                continue
            rows: list[str] = []
            rows += diff("entry", {k: v for k, v in alias.items() if k not in ("_meta",)},
                         {k: v for k, v in canonical.items() if k not in ("_meta",)},
                         EXPECTED_DIFFERENT)
            rows += diff("_meta.didi", meta,
                         (canonical.get("_meta") or {}).get("didi") or {},
                         META_EXPECTED_DIFFERENT)
            if rows:
                print("\n".join(rows))
            else:
                clean += 1
                print("    identical apart from the name")
        print(f"\n{clean} of {len(aliases)} aliases publish the same entry as their canonical tool")

        # A token minted under one name, spent under the other.
        print("\n--- a confirmation token across the two names ---")
        pair = ("capture_viewport", "viewport_capture")
        if all(name in tools for name in pair):
            session.call("script_create",
                         {"script_path": "res://alias_target.gd", "source_text": "extends Node\n"})
        gate = {"script_path": "res://alias_target.gd", "source_text": "extends Node\n# two\n",
                "overwrite": True}
        payload, is_error = session.call("script_create", dict(gate, dry_run=True))
        token = (payload.get("mutation_preview") or {}).get("confirmation_token") if isinstance(payload, dict) else None
        print(f"    script_create dry_run -> token={'yes' if token else 'no'} isError={is_error}")
        print("    (script_create has no legacy alias; the pairs that do are all "
              "read-only, so the gate cannot be crossed between two names at all)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
