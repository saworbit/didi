"""Which didi binary the end to end tests drive.

This repository is built into build/ by CI and into build-ninja/ by hand, so
both trees exist at once on a developer machine and one of them is usually
stale. Every test file used to pick by a fixed directory order that tried
build/ first, so a local run drove a binary from days ago and reported failures
that read exactly like real regressions. Working out that the failures came
from an old binary rather than the change under test cost a long detour.

Newest modification time wins instead. That is almost always the build you just
made, and when there was more than one to choose from the choice is said out
loud once, so a surprising result names the binary that produced it rather than
leaving it to be inferred.

DIDI_TEST_BINARY still wins over all of it. CI sets it for the steps that care
which build they are driving, and it is the way to be explicit here too.
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]

# Every path a build can land on: both generators, both configurations, both
# platforms. Order carries no meaning any more, only membership.
CANDIDATES = (
    "build/Release/didi.exe",
    "build/Debug/didi.exe",
    "build/didi",
    "build-ninja/didi.exe",
    "build-ninja/didi",
)

# The announcement is worth making once per process, not once per test class.
_announced = False


def resolve() -> Path:
    """The binary to drive.

    Raises SkipTest when nothing is built, which is how a checkout without a
    build has always behaved. Raises RuntimeError when an override names a file
    that is not there, because someone who set the variable meant that path and
    should be told it is wrong rather than watch a subprocess fail obscurely.
    """
    # DIDI_EXECUTABLE is read second and only for compatibility: the command
    # line test has always used that spelling.
    override = os.environ.get("DIDI_TEST_BINARY") or os.environ.get("DIDI_EXECUTABLE")
    if override:
        path = Path(override)
        if not path.is_file():
            raise RuntimeError(
                f"DIDI_TEST_BINARY names {path}, which is not a file. "
                f"Unset it to search the build directories instead."
            )
        return path

    built = [
        path for path in (REPOSITORY_ROOT / name for name in CANDIDATES) if path.is_file()
    ]
    if not built:
        raise unittest.SkipTest("didi executable not built")

    built.sort(key=lambda path: path.stat().st_mtime, reverse=True)
    _announce(built)
    return built[0]


def _announce(built: list[Path]) -> None:
    global _announced
    if _announced or len(built) == 1:
        return
    _announced = True
    relative = [str(path.relative_to(REPOSITORY_ROOT)) for path in built]
    # stderr, so this cannot land in stdout a test is capturing.
    print(
        f"didi tests: driving {relative[0]}, the newest of {len(built)} builds. "
        f"Older: {', '.join(relative[1:])}. Set DIDI_TEST_BINARY to choose.",
        file=sys.stderr,
    )
