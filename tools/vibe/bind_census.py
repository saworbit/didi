"""Every method bind in the extension, checked against each engine's API dump.

A GDExtension method bind is pinned by (class, method, signature hash). A hash
the running engine does not know is a null bind: a 501 at call time, or an
ERROR line at startup. Godot keeps the old hash of a changed signature in
`hash_compatibility`, so a differing hash is only a break when it is in
neither list.

    python bind_census.py D:/didi/src/gdextension API_DIR [API_DIR ...]
"""
from __future__ import annotations

import json
import re
import sys
from collections import defaultdict
from pathlib import Path

TRIPLE = re.compile(r'"([A-Za-z0-9_]+)",\s*"([A-Za-z0-9_]+)",\s*(-?[0-9]+)LL')
PAIR = re.compile(r'\{"([a-z_0-9]+)",\s*(-?[0-9]+)LL\}')
CONST = re.compile(r'constexpr\s+(?:int64_t|long long|auto)\s+(k[A-Za-z0-9_]+)\s*=\s*(-?[0-9]+)LL')


def load_api(path: Path):
    api = json.loads(path.read_text(encoding="utf-8"))
    version = api["header"]["version_full_name"]
    by_class: dict[str, dict[str, set[int]]] = defaultdict(lambda: defaultdict(set))
    by_method: dict[str, set[int]] = defaultdict(set)
    for section in ("classes", "builtin_classes"):
        for klass in api.get(section, []):
            for m in klass.get("methods", []) or []:
                hashes = {m["hash"]} if "hash" in m else set()
                hashes |= set(m.get("hash_compatibility", []) or [])
                by_class[klass["name"]][m["name"]] |= hashes
                by_method[m["name"]] |= hashes
    for fn in api.get("utility_functions", []):
        by_class["@Utility"][fn["name"]].add(fn["hash"])
        by_method[fn["name"]].add(fn["hash"])
    return version, by_class, by_method


def main() -> int:
    src = Path(sys.argv[1])
    apis = [load_api(Path(p) / "extension_api.json") for p in sys.argv[2:]]
    triples: dict[tuple[str, str, int], set[str]] = defaultdict(set)
    pairs: dict[tuple[str, int], set[str]] = defaultdict(set)
    consts: dict[str, int] = {}
    for file in sorted(src.glob("*.cpp")):
        text = file.read_text(encoding="utf-8", errors="replace")
        for m in CONST.finditer(text):
            consts[m.group(1)] = int(m.group(2))
        for line_no, line in enumerate(text.splitlines(), 1):
            for m in TRIPLE.finditer(line):
                triples[(m.group(1), m.group(2), int(m.group(3)))].add(f"{file.name}:{line_no}")
            for m in PAIR.finditer(line):
                pairs[(m.group(1), int(m.group(2)))].add(f"{file.name}:{line_no}")
    print(f"{len(triples)} class/method/hash triples, {len(pairs)} method/hash pairs, {len(consts)} named hashes")

    for version, by_class, by_method in apis:
        print(f"\n===== {version}")
        bad = 0
        for (klass, method, h), where in sorted(triples.items()):
            if klass not in by_class:
                # Some first strings are not classes (log tags). Only report if the
                # method name exists somewhere with that hash -- then it is a real bind
                # whose class we mis-parsed; otherwise skip quietly.
                if method in by_method and h not in by_method[method]:
                    print(f"  ? {klass}.{method} {h}: class unknown, method exists elsewhere with hashes {sorted(by_method[method])[:4]}  [{sorted(where)[0]}]")
                continue
            if method not in by_class[klass]:
                # Could be inherited: search ancestors is heavy; fall back to any-class check.
                if h in by_method.get(method, set()):
                    continue
                bad += 1
                print(f"  X {klass}.{method} {h}: method not on class and hash unknown everywhere  [{sorted(where)[0]}]")
                continue
            if h not in by_class[klass][method]:
                bad += 1
                print(f"  X {klass}.{method} {h}: engine has {sorted(by_class[klass][method])}  [{sorted(where)[0]}]")
        for (method, h), where in sorted(pairs.items()):
            if method not in by_method:
                print(f"  ? pair {method} {h}: method name unknown to this engine  [{sorted(where)[0]}]")
                continue
            if h not in by_method[method]:
                bad += 1
                print(f"  X pair {method} {h}: engine has {sorted(by_method[method])[:6]}  [{sorted(where)[0]}]")
        for name, h in sorted(consts.items()):
            hits = [m for m, hs in by_method.items() if h in hs]
            if not hits:
                bad += 1
                print(f"  X const {name} {h}: no method in this engine has that hash")
        print(f"  {bad} mismatches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
