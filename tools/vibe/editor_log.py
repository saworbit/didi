"""The editor's own console, read by every probe rather than by a person watching it.

Godot prints its ERROR and WARNING lines to its console and, with `--log-file`,
to a file. Nothing in this directory read that file except
`probes/editor_log_delta.py`, which had to be run on purpose, so a probe could
print a clean row while the editor under it printed an error for the same call.
Session seventeen found three findings in a console Shane pasted rather than in
any probe output: a `.res` that `resource_create` wrote and the editor could not
load on every startup, and ten "Missing .uid file" warnings every time the addon
folder is replaced. Both had been on screen all along.

So the client reads it. `Session` finds `editor.log` beside the project (the
place `sandbox.py --launch` writes it), reports what the editor printed before
the session began, and after every call prints any new ERROR or WARNING line
under that call, with the `at:` location Godot gives. The lines are also kept on
the session, so a probe can assert on them.

    from editor_log import EditorLog
    log = EditorLog.beside(project)       # None when there is no log file
    print(log.summary(log.startup()))
    ...
    for line in log.delta(): print(line)
"""

from __future__ import annotations

import os
import re
from collections import Counter
from pathlib import Path

# What Godot prefixes a line with when something went wrong or might have.
# `USER ERROR` and `USER WARNING` come from push_error and push_warning.
PROBLEM = re.compile(r"^\s*(ERROR|WARNING|SCRIPT ERROR|USER ERROR|USER WARNING|SCRIPT WARNING)\b")
LOCATION = re.compile(r"^\s+at: ")


class EngineLine:
    """One ERROR or WARNING line and the location Godot printed under it."""

    def __init__(self, text: str, location: str = "") -> None:
        self.text = text.strip()
        self.location = location.strip()

    @property
    def severity(self) -> str:
        match = PROBLEM.match(self.text)
        return match.group(1) if match else ""

    def __str__(self) -> str:
        return f"{self.text}  ({self.location})" if self.location else self.text

    def __repr__(self) -> str:
        return f"EngineLine({self.text!r})"


def parse(text: str) -> list[EngineLine]:
    """Every problem line in a stretch of log, each with the `at:` line after it."""
    lines = text.splitlines()
    found: list[EngineLine] = []
    for index, line in enumerate(lines):
        if not PROBLEM.match(line):
            continue
        location = ""
        if index + 1 < len(lines) and LOCATION.match(lines[index + 1]):
            location = lines[index + 1].split("at:", 1)[1]
        found.append(EngineLine(line, location))
    return found


class EditorLog:
    """An editor's --log-file, read from the start and then as a delta."""

    def __init__(self, path: str | os.PathLike[str]) -> None:
        self.path = Path(path)
        self.offset = 0

    @classmethod
    def beside(cls, project: str | os.PathLike[str] | None) -> "EditorLog | None":
        """The log `sandbox.py --launch` writes next to the project, if there is one."""
        if not project:
            return None
        candidate = Path(project).resolve().parent / "editor.log"
        return cls(candidate) if candidate.is_file() else None

    def _read_from(self, offset: int) -> tuple[str, int]:
        try:
            with self.path.open("rb") as handle:
                handle.seek(0, os.SEEK_END)
                end = handle.tell()
                # The editor truncates its log when it restarts; start over.
                if end < offset:
                    offset = 0
                handle.seek(offset)
                data = handle.read()
        except OSError:
            return "", offset
        return data.decode("utf-8", errors="replace"), offset + len(data)

    def startup(self) -> list[EngineLine]:
        """Everything the editor printed before this call, and move past it."""
        text, self.offset = self._read_from(0)
        return parse(text)

    def delta(self) -> list[EngineLine]:
        """What the editor printed since the last read."""
        text, self.offset = self._read_from(self.offset)
        return parse(text)

    @staticmethod
    def summary(lines: list[EngineLine]) -> str:
        """The lines grouped with a count, run-specific tokens and ids folded so repeats group."""
        if not lines:
            return "  (the editor printed no ERROR or WARNING lines)"
        folded = Counter(re.sub(r"[0-9a-f]{4,}", "#", str(line)) for line in lines)
        return "\n".join(f"  {count:4}x {text}" for text, count in folded.most_common())
