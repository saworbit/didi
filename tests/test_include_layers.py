"""Includes between source layers go one way.

The folders under ``src/`` and ``include/didi/`` are layers. ``common`` is
the bottom, ``offline`` and ``runtime`` sit on it, ``mcp`` and ``tools`` are the
server above them, and ``gdextension``, ``setup`` and ``standalone`` are what
gets built from all of that. Nothing held the order: ``common`` and
``runtime`` both reached up into ``gdextension``, ``runtime`` handed back
MCP result types, and ``offline`` took its file lock from ``runtime``, so each
new include could widen the cycle without anyone noticing (#1257).

``mcp`` and ``tools`` include each other. The registry names the handlers and
the handlers build the registry's result type, so they are one layer in two
folders, and the table says so rather than pretending otherwise.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]

# What each layer may include, besides itself.
ALLOWED = {
    "common": set(),
    "offline": {"common"},
    "runtime": {"common", "offline"},
    "mcp": {"common", "offline", "runtime", "tools"},
    "tools": {"common", "offline", "runtime", "mcp"},
    "gdextension": {"common", "offline", "runtime"},
    "setup": {"common", "offline", "runtime", "mcp"},
    "standalone": {"common", "offline", "runtime", "mcp", "tools", "setup"},
}

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]didi/([a-z_]+)/', re.MULTILINE)


def layer_folders() -> dict[str, list[Path]]:
    folders: dict[str, list[Path]] = {}
    for parent in (ROOT / "src", ROOT / "include" / "didi"):
        for folder in sorted(path for path in parent.iterdir() if path.is_dir()):
            folders.setdefault(folder.name, []).append(folder)
    return folders


def wrong_way_includes() -> list[str]:
    found = []
    for layer, folders in layer_folders().items():
        allowed = ALLOWED.get(layer, set()) | {layer}
        for folder in folders:
            for path in sorted(folder.rglob("*")):
                if path.suffix not in {".cpp", ".hpp", ".h"}:
                    continue
                text = path.read_text(encoding="utf-8", errors="replace")
                for match in INCLUDE.finditer(text):
                    if match.group(1) not in allowed:
                        line = text.count("\n", 0, match.start()) + 1
                        relative = path.relative_to(ROOT).as_posix()
                        found.append(f"{relative}:{line} ({layer} includes {match.group(1)})")
    return found


class IncludeLayerTests(unittest.TestCase):
    def test_every_layer_folder_has_a_rule(self) -> None:
        # A new folder would otherwise be checked against nothing at all.
        unknown = sorted(set(layer_folders()) - set(ALLOWED))
        self.assertEqual(unknown, [], "add each new layer folder to ALLOWED in this file")

    def test_no_include_goes_the_wrong_way(self) -> None:
        found = wrong_way_includes()
        if found:
            self.fail("\n".join(["includes against the layer order:", *found]))


if __name__ == "__main__":
    unittest.main()
