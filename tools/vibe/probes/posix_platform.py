"""The states only a POSIX host can put a project into, asked of the readers.

Eleven sessions ran on Windows. Session eight asked "what does the host
operating system do to an argument the server already accepted" and answered it
with a case-insensitive filesystem, device names and dot segments -- all
Windows. The other two supported platforms have their own version of that
question, and it is not the same question:

* a file the process is not allowed to read (`chmod 000`), which Windows can
  express only through an ACL nobody sets by hand;
* a directory the process is not allowed to write into;
* a symlink, which is the ordinary way a POSIX project points at something
  outside itself, and which the containment check has never been shown;
* a filename that is not valid UTF-8, because a POSIX filename is bytes and a
  JSON response is text -- on Windows a name is UTF-16 and always converts;
* a filename containing a newline, which POSIX permits and Windows forbids;
* a FIFO where a `.gd` is expected, which blocks its opener until someone
  writes;
* and the *absence* of case folding, which is what makes `res://Player.gd` and
  `res://player.gd` two files on Linux, one file on Windows, and -- because the
  default APFS volume is case-insensitive -- one file on macOS too.

Every row prints what a correct build should say beside what this build said,
so the same script is the reproduction and the regression guard. Rows whose
precondition the host cannot create are skipped by name rather than silently
dropped; a skipped row is not a passing row.

    python tools/vibe/sandbox.py SANDBOX
    python tools/vibe/probes/posix_platform.py -p SANDBOX

No editor and no addon are needed: every tool here answers offline.
"""

from __future__ import annotations

import argparse
import os
import platform
import stat
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402

POSIX = os.name != "nt"
SOURCE = "extends Node\n\nfunc greet() -> String:\n\treturn \"hi\"\n"

skipped: list[str] = []


def row(label: str, expected: str, observed: str) -> None:
    verdict = "ok  " if expected == observed else "DIFF"
    print(f"  {verdict} {label:<44} expected={expected:<26} observed={observed}")


def skip(label: str, why: str) -> None:
    skipped.append(f"{label}: {why}")
    print(f"  skip {label:<44} {why}")


def says(payload, *keys: str) -> str:
    """A short description of a response, error or not.

    Deliberately not the whole payload: the question every row asks is whether
    the answer *distinguishes* two states, and the fields that could do that
    are the code and the message. Printed in full -- a probe's own width hiding
    the one field that mattered has cost this harness a session before.
    """
    if isinstance(payload, str):
        return f"prose:{payload[:80]!r}"
    if not isinstance(payload, dict):
        return repr(payload)[:80]
    code = payload.get("code") or payload.get("error_code")
    message = payload.get("message") or payload.get("error") or ""
    if code or message:
        return f"{code} {str(message)[:90]!r}"
    return " ".join(f"{key}={payload.get(key)!r}" for key in keys) or str(payload)[:90]


def permission_rows(session: Session, project: Path) -> None:
    """A file that is there and cannot be read, against one that is not there.

    The control is the point. "Not found" for an unreadable file is only a
    finding because the same words come back for a path with nothing behind
    it, and a caller has to tell an access problem from a typo.
    """
    print("\nA file the process may not read (chmod 000) against one that is absent")
    if not POSIX:
        skip("chmod 000", "needs a POSIX host")
        return
    locked = project / "locked.gd"
    locked.write_text(SOURCE, encoding="utf-8")
    os.chmod(locked, 0)
    if os.access(locked, os.R_OK):
        skip("chmod 000", "the process can read it anyway (running as root?)")
        # Unlink rather than restore a mode: deleting a file needs write on the
        # directory, not on the file, so there is nothing to put back. Every
        # chmod here stays owner-only on purpose (CodeQL's py/overly-permissive-file
        # is right about a probe that hands a scratch file to group and other).
        locked.unlink()
        return
    for tool, arguments in (
        ("script_check_syntax", {"file_path": "res://locked.gd"}),
        ("script_get_symbols", {"file_path": "res://locked.gd"}),
    ):
        unreadable, _ = session.call(tool, dict(arguments))
        absent, _ = session.call(tool, {"file_path": "res://no_such_file.gd"})
        row(f"{tool} unreadable vs absent",
            "two different answers",
            "the same answer" if says(unreadable) == says(absent) else "two different answers")
        print(f"       unreadable: {says(unreadable, 'has_errors', 'symbol_count_total')}")
        print(f"       absent:     {says(absent, 'has_errors', 'symbol_count_total')}")
    locked.unlink()


def unwritable_directory(session: Session, project: Path) -> None:
    print("\nA directory the process may not write into (chmod 500)")
    if not POSIX:
        skip("chmod 500 directory", "needs a POSIX host")
        return
    readonly = project / "readonly"
    readonly.mkdir(exist_ok=True)
    os.chmod(readonly, stat.S_IRUSR | stat.S_IXUSR)
    if os.access(readonly, os.W_OK):
        skip("chmod 500 directory", "the process can write anyway (running as root?)")
        os.chmod(readonly, stat.S_IRWXU)
        return
    payload, is_error = session.call(
        "script_create", {"script_path": "res://readonly/new.gd", "source_text": SOURCE})
    row("script_create into an unwritable directory", "isError=True", f"isError={is_error}")
    print(f"       {says(payload, 'script_path', 'status')}")
    landed = (readonly / "new.gd").exists()
    row("...and nothing was created", "created=False", f"created={landed}")
    # Owner-only, not 0o755: this is a scratch directory in a throwaway project
    # and nothing else needs to read it.
    os.chmod(readonly, stat.S_IRWXU)


def symlink_rows(session: Session, project: Path) -> None:
    """The ordinary POSIX way to point at something outside the project.

    Containment is decided by resolving the path and comparing it against the
    root (#534), and `weakly_canonical` resolves symlinks -- so these rows
    should all be refusals. They are here because nothing has ever shown that
    they are, and because the write path and the read path resolve separately.
    """
    print("\nA symlink out of the project")
    if not POSIX:
        skip("symlinks", "needs a POSIX host")
        return
    outside = project.parent / "outside_the_project"
    outside.mkdir(exist_ok=True)
    secret = outside / "secret.gd"
    secret.write_text("extends Node\n# outside\n", encoding="utf-8")

    link = project / "escape.gd"
    link_dir = project / "escape_dir"
    for path in (link, link_dir):
        if path.is_symlink() or path.exists():
            path.unlink()
    try:
        link.symlink_to(secret)
        link_dir.symlink_to(outside, target_is_directory=True)
    except OSError as error:
        skip("symlinks", f"cannot create one here: {error}")
        return

    payload, is_error = session.call("script_get_symbols", {"file_path": "res://escape.gd"})
    row("read through a symlink out of the project", "isError=True", f"isError={is_error}")
    print(f"       {says(payload, 'symbol_count_total')}")

    payload, is_error = session.call(
        "script_create",
        {"script_path": "res://escape.gd", "source_text": "# overwritten\n", "overwrite": True})
    row("overwrite a symlink out of the project", "isError=True", f"isError={is_error}")
    print(f"       {says(payload, 'script_path', 'status')}")
    row("...and the target is untouched",
        "untouched", "untouched" if "outside" in secret.read_text() else "REWRITTEN")

    payload, is_error = session.call(
        "script_create", {"script_path": "res://escape_dir/planted.gd", "source_text": SOURCE})
    row("create through a symlinked directory", "isError=True", f"isError={is_error}")
    print(f"       {says(payload, 'script_path', 'status')}")
    row("...and nothing landed outside", "absent",
        "absent" if not (outside / "planted.gd").exists() else "PLANTED")

    for path in (link, link_dir):
        if path.is_symlink():
            path.unlink()


def odd_filenames(session: Session, project: Path) -> None:
    """Names a POSIX filesystem accepts and a JSON document may not.

    A POSIX filename is a byte string with two forbidden bytes, `/` and NUL.
    Everything else is legal, including bytes that are not valid UTF-8 and
    bytes that are newlines. A response is JSON, which is text. The walkers
    that enumerate the project are the ones that have to reconcile that, and
    they have never met a name they could not decode.
    """
    print("\nFilenames a POSIX filesystem allows and a JSON response may not")
    if not POSIX:
        skip("non-UTF-8 and newline filenames", "needs a POSIX host")
        return
    made: list[bytes] = []
    root = os.fsencode(str(project))
    for raw in (b"latin1_caf\xe9.gd", b"two\nlines.gd"):
        target = root + b"/" + raw
        try:
            with open(target, "wb") as handle:
                handle.write(SOURCE.encode())
            made.append(target)
        except OSError as error:
            skip(raw.decode(errors="replace"), f"cannot create: {error}")
    if not made:
        return

    for tool, arguments in (
        ("project_search_text", {"query": "greet"}),
        ("project_list_resources", {}),
        ("project_audit_assets", {}),
        ("project_get_uid_map", {}),
    ):
        try:
            payload, is_error = session.call(tool, dict(arguments))
        except Exception as error:  # noqa: BLE001 - a dead server is the finding
            row(f"{tool} with an undecodable name present",
                "an answer", f"the client lost the server: {error}")
            continue
        text = str(payload)
        names = ("caf" in text, "two" in text)
        row(f"{tool} with an undecodable name present",
            "an answer that names both files", f"isError={is_error} latin1={names[0]} newline={names[1]}")
        print(f"       {says(payload, 'total_matches', 'resource_count', 'asset_count', 'entry_count')}")
    for target in made:
        os.unlink(target)


def fifo_row(session: Session, project: Path) -> None:
    """A FIFO where a script is expected: opening it blocks until someone writes.

    `is_regular_file` should keep every reader away from it. The row exists
    because a reader that opens first and checks afterwards does not fail --
    it stops, and a stopped server looks exactly like a slow one.
    """
    print("\nA FIFO named like a script")
    if not POSIX or not hasattr(os, "mkfifo"):
        skip("mkfifo", "needs a POSIX host")
        return
    fifo = project / "pipe.gd"
    if fifo.exists():
        fifo.unlink()
    try:
        os.mkfifo(fifo)
    except OSError as error:
        skip("mkfifo", f"cannot create: {error}")
        return
    for tool in ("script_check_syntax", "script_get_symbols"):
        try:
            payload, is_error = session.call(tool, {"file_path": "res://pipe.gd"})
            row(f"{tool} on a FIFO", "isError=True", f"isError={is_error}")
            print(f"       {says(payload, 'has_errors', 'symbol_count_total')}")
        except Exception as error:  # noqa: BLE001
            row(f"{tool} on a FIFO", "isError=True", f"never answered: {error}")
    try:
        payload, is_error = session.call("project_search_text", {"query": "greet"})
        row("project_search_text with a FIFO in the tree", "an answer", f"isError={is_error}")
    except Exception as error:  # noqa: BLE001
        row("project_search_text with a FIFO in the tree", "an answer", f"never answered: {error}")
    fifo.unlink()


def case_rows(session: Session, project: Path) -> None:
    """Whether the host folds case, and whether the reported path says so.

    #546 was a Windows finding: `script_create` with `overwrite` replaced
    res://player.gd, reported res://PLAYER.gd, and the preview described a file
    that did not exist. The fix takes the on-disk spelling from
    `std::filesystem::canonical`. Linux never folds; the default macOS volume
    does, which makes macOS the platform where that fix has to work and has
    never been run.
    """
    print("\nCase folding, and whether the reported path is the on-disk one (#546)")
    lower = project / "casecheck.gd"
    lower.write_text(SOURCE, encoding="utf-8")
    folds = (project / "CASECHECK.gd").exists()
    print(f"       host folds case: {folds}  ({platform.system()})")

    # `overwrite: true` is confirmation-gated, so the interesting call cannot be
    # made in one shot. The first version of this row sent it anyway, got
    # `428 confirmation_required`, and printed `observed=None` for the reported
    # path -- a row that looked like a finding and was the probe failing to
    # reach the subject. Dry run first, carry the token; the token lives in this
    # server process's memory, which is why this is a Session and not a batch.
    arguments = {"script_path": "res://CASECHECK.gd", "source_text": "# replaced\n",
                 "overwrite": True}
    preview, _ = session.call("script_create", dict(arguments, dry_run=True))
    token = None
    if isinstance(preview, dict):
        token = (preview.get("mutation_preview") or {}).get("confirmation_token")
        before = ((preview.get("mutation_preview") or {}).get("changes") or [{}])[0]
        print(f"       preview says before={before.get('before')!r}")
    payload, is_error = session.call(
        "script_create", dict(arguments, **({"confirmation_token": token} if token else {})))
    reported = payload.get("script_path") or payload.get("file_path") \
        if isinstance(payload, dict) else None
    landed = sorted(p.name for p in project.glob("*.gd") if p.name.lower() == "casecheck.gd")
    if folds:
        row("overwrite through the other spelling reports the file it wrote",
            "res://casecheck.gd", str(reported))
        row("...and no second file appeared", "['casecheck.gd']", str(landed))
    else:
        row("the other spelling is a different file", "isError=False", f"isError={is_error}")
        row("...and it is its own path", "res://CASECHECK.gd", str(reported))
    for name in landed:
        (project / name).unlink(missing_ok=True)
    lower.unlink(missing_ok=True)


def godot_bin_rows(session: Session, project: Path) -> None:
    """`GODOT_BIN` pointing at a directory, which is what a Mac user types.

    On macOS the thing called Godot is `/Applications/Godot.app`, a directory;
    the executable is four levels inside it. A message that does not say so
    leaves a reader looking at a path that is plainly there.
    """
    print("\nGODOT_BIN pointing at a directory (the /Applications/Godot.app mistake)")
    directory = project / "Godot.app"
    directory.mkdir(exist_ok=True)
    probe = Session(project=str(project), env={"GODOT_BIN": str(directory)})
    try:
        payload, is_error = probe.call("script_check_syntax", {"file_path": "res://player.gd"})
        message = says(payload, "has_errors", "engine_available")
        row("the refusal names the bundle layout", "names Contents/MacOS",
            "names it" if "Contents/MacOS" in str(payload) else f"does not: {message}")
        print(f"       isError={is_error} {message}")
    finally:
        probe.close()
    directory.rmdir()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    args = parser.parse_args()
    project = Path(args.project).resolve()

    print(f"host: {platform.system()} {platform.release()} ({platform.machine()})")
    print(f"posix: {POSIX}  euid: {os.geteuid() if POSIX else 'n/a'}")

    session = Session(project=str(project))
    try:
        permission_rows(session, project)
        unwritable_directory(session, project)
        symlink_rows(session, project)
        odd_filenames(session, project)
        fifo_row(session, project)
        case_rows(session, project)
    finally:
        session.close()
    godot_bin_rows(session, project)

    if skipped:
        print("\nSkipped rows -- a skipped row is not a passing row:")
        for line in skipped:
            print(f"  {line}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
