"""project_run_tests against the real GUT and GdUnit4, on every engine line.

CI has neither framework, so the live harness runs project_run_tests against a
stand-in that follows GUT's command-line contract (tests/test_runner_fixture).
This is the other half: the real frameworks, run through the real tool, with
the verdict each case must give. It is how Q9 part 2 (#1212) was verified
before it shipped, and the cases are the ones that made the exit code
untrustworthy:

    fail      one test wrong on purpose        fail   tests_failed
    pass      the same suite, fixed            pass
    empty     a test directory with no tests   error  no_tests
    broken    a passing suite and a test file  error  scripts_did_not_load
              that does not parse
    risky     one real test, one with no       pass   (counted apart)
              assertion, one skipped
    noimport  the project never imported      error  no_report, not_imported

Each framework is copied into a throwaway project per engine and imported once
with that engine (`--headless --import`); the cases are copies of it. Prints
DIFF for any case whose verdict or reason differs.

Needs both addons on disk. GUT ships as a folder from its releases or the asset
library; GdUnit4 as `addons/gdUnit4` in its source archive. Verified with GUT
9.7.1 and GdUnit4 6.2.2.

    python tools/vibe/probes/test_frameworks.py --gut D:/Bonk/ArenaCombat/addons/gut --gdunit4 PATH/addons/gdUnit4
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mcp_client import Session  # noqa: E402

DEFAULT_ENGINES = [
    "C:/Godot/Godot_v4.5.1-stable_win64_console.exe",
    "C:/Godot/Godot_v4.6.2-stable_win64_console.exe",
    "C:/Godot/Godot_v4.7.2-stable_win64_console.exe",
]

CALCULATOR = "class_name Calculator\nextends RefCounted\n\nfunc add(a: int, b: int) -> int:\n\treturn a + b\n"

SUITES = {
    "gut": {
        "base": "extends GutTest\n",
        "file": "test/test_calculator.gd",
        "broken": "test/test_broken.gd",
        "assert": "assert_eq(Calculator.new().add({a}, {b}), {want})",
        "nothing": "pass",
        "skip": "func test_later() -> void:\n\tpending(\"not written yet\")\n",
    },
    "gdunit4": {
        "base": "extends GdUnitTestSuite\n",
        "file": "test/calculator_test.gd",
        "broken": "test/broken_test.gd",
        "assert": "assert_int(Calculator.new().add({a}, {b})).is_equal({want})",
        "nothing": "pass",
        "skip": "func test_later(do_skip := true, skip_reason := \"not written yet\") -> void:\n\tassert_int(1).is_equal(1)\n",
    },
}

EXPECTED = {
    "fail": ("fail", "tests_failed"),
    "pass": ("pass", None),
    "empty": ("error", "no_tests"),
    "broken": ("error", "scripts_did_not_load"),
    "risky": ("pass", None),
    "noimport": ("error", "no_report"),
}


def test_function(name: str, body: str) -> str:
    return f"\nfunc {name}() -> void:\n\t{body}\n"


def suite_source(framework: str, case: str) -> str:
    suite = SUITES[framework]
    check = suite["assert"]
    source = suite["base"]
    source += test_function("test_adds", check.format(a=2, b=3, want=5))
    if case == "risky":
        source += test_function("test_asserts_nothing", suite["nothing"])
        source += "\n" + suite["skip"]
        return source
    source += test_function("test_adds_negatives", check.format(a=-2, b=-3, want=-5))
    want = 5 if case == "fail" else 4
    source += test_function("test_two_and_two", check.format(a=2, b=2, want=want))
    return source


def build(base: Path, framework: str, addon: Path, engine: str) -> None:
    (base / "src").mkdir(parents=True)
    (base / "test").mkdir()
    (base / "addons").mkdir()
    (base / "project.godot").write_text('config_version=5\n\n[application]\n\nconfig/name="test frameworks probe"\n',
                                        encoding="utf-8")
    (base / "src" / "calculator.gd").write_text(CALCULATOR, encoding="utf-8")
    shutil.copytree(addon, base / "addons" / addon.name)
    subprocess.run([engine, "--headless", "--path", str(base), "--import"], capture_output=True, timeout=300)


def make_case(base: Path, target: Path, framework: str, case: str) -> None:
    shutil.copytree(base, target)
    suite = SUITES[framework]
    if case == "noimport":
        shutil.rmtree(target / ".godot", ignore_errors=True)
    if case != "empty":
        (target / suite["file"]).write_text(suite_source(framework, "pass" if case == "broken" else case),
                                            encoding="utf-8")
    if case == "broken":
        (target / suite["broken"]).write_text(suite["base"] + "\nfunc test_never_parses() -> void\n\tpass\n",
                                              encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gut", type=Path, help="An addons/gut folder.")
    parser.add_argument("--gdunit4", type=Path, help="An addons/gdUnit4 folder.")
    parser.add_argument("--godot", action="append", help="A Godot console executable; repeat for more.")
    parser.add_argument("--binary", help="The didi server; defaults to the build tree's.")
    args = parser.parse_args()
    frameworks = {name: path for name, path in (("gut", args.gut), ("gdunit4", args.gdunit4)) if path}
    if not frameworks:
        parser.error("name at least one of --gut and --gdunit4")
    engines = args.godot or [e for e in DEFAULT_ENGINES if Path(e).exists()]
    diffs = 0
    with tempfile.TemporaryDirectory(prefix="didi_test_frameworks_") as folder:
        root = Path(folder)
        for engine in engines:
            line = Path(engine).name
            for framework, addon in frameworks.items():
                base = root / f"{framework}-{line}-base"
                build(base, framework, addon, engine)
                for case, (want_verdict, want_reason) in EXPECTED.items():
                    target = root / f"{framework}-{line}-{case}"
                    make_case(base, target, framework, case)
                    with Session(target, binary=args.binary, env={"GODOT_BIN": engine}, editor_log=False) as session:
                        body, _ = session.call("project_run_tests", {})
                    verdict, reason = body.get("verdict"), body.get("reason")
                    ok = verdict == want_verdict and reason == want_reason
                    if case == "noimport":
                        ok = ok and body.get("not_imported") is True
                    diffs += 0 if ok else 1
                    print(f"{'ok  ' if ok else 'DIFF'} {line:40} {framework:8} {case:9} "
                          f"{verdict}/{reason} counts={body.get('counts')} "
                          f"addon_not_loaded={len(body.get('addon_scripts_did_not_load') or [])}")
                    if not ok:
                        print(f"     expected {want_verdict}/{want_reason}: {body.get('summary')}")
    print(f"\n{diffs} case(s) differ.")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
