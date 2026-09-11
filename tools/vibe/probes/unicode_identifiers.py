"""Symbol extraction against identifiers that are not ASCII.

Godot permits Unicode letters in GDScript identifiers and parses every fixture
here without a diagnostic (`--headless --check-only`, exit 0). Didi's symbol
extractor does not: an accented name comes back truncated at the first
non-ASCII byte, and an all-Cyrillic file comes back empty. Both report
`isError: false`.

The fixtures are written to the sandbox directly rather than through
`script_create`, because the point is what the *reader* does with a file that
already exists.

    python tools/vibe/probes/unicode_identifiers.py SANDBOX
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from mcp_client import Session  # noqa: E402

FIXTURES = {
    # An accented Latin name: truncated at the accent, and the two members
    # whose names start with one vanish while `plain` on line 7 is found.
    "accent.gd": (
        "extends Node\n"
        "class_name Caf\u00e9Menu\n"
        "signal pr\u00eat\n"
        "var \u00e9tat := 0\n"
        "func d\u00e9marrer():\n"
        "\tpass\n"
        "func plain():\n"
        "\tpass\n"
    ),
    # Nothing ASCII at all: every list comes back empty.
    "cyr.gd": (
        "extends Node\n"
        "class_name \u0418\u0433\u0440\u043e\u043a\n"
        "func \u0431\u0435\u0433():\n"
        "\tpass\n"
    ),
    # The control: a non-ASCII *filename* is handled correctly, which is what
    # narrows this to the identifier rather than the path.
    "\u65e5\u672c\u8a9e.gd": (
        "extends Node\n"
        "class_name AsciiClass\n"
        "func hello():\n"
        "\tpass\n"
    ),
}


def main() -> int:
    project = Path(sys.argv[1])
    for name, text in FIXTURES.items():
        (project / name).write_text(text, encoding="utf-8")

    with Session(project=project) as session:
        for name in FIXTURES:
            payload, is_error = session.call("script_get_symbols", {"file_path": f"res://{name}"})
            print(f"--- script_get_symbols res://{name}")
            print(f"    {'ERR ' if is_error else 'ok  '}{json.dumps(payload, sort_keys=True)}")

        # The two search tools disagree about the same file, and neither says why.
        for tool in ("project_search_symbols", "project_search_text"):
            payload, is_error = session.call(tool, {"query": "Caf\u00e9Menu"})
            matches = payload.get("matches") if isinstance(payload, dict) else payload
            print(f"--- {tool} Caf\u00e9Menu\n    {json.dumps(matches, sort_keys=True)}")

        # The truncated signal name reappears as a false audit finding.
        payload, _ = session.call("project_audit_assets", {})
        print(f"--- project_audit_assets dead_signals\n    {json.dumps(payload.get('dead_signals'), sort_keys=True)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
