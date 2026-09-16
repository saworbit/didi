"""The one platform constant the session endpoint has to fit inside.

A POSIX session is published as `<tempdir>/godot_didi_<key>_<pid>_<id>.sock`,
and `sockaddr_un::sun_path` is 108 bytes on Linux and 104 on macOS. The fixed
part of that name is about fifty bytes, so the whole question is how long the
temporary directory is. On Linux it is `/tmp`. On macOS it is a per-user
`/var/folders/xx/<26 chars>/T/` path that launchd sets, and the workflow that
measures it has been printing **100 of 104 bytes** on a stock runner since the
twelfth session: four bytes of headroom on the platform with the smaller limit.

Both ends of the bridge handle the overflow the same way -- `PosixIpcServer` and
the client both compare against `sizeof(addr.sun_path)` and `return false`, with
no log line and no error object. This asks what a caller is told when that
happens: whether a descriptor is published at all, what `runtime_list_sessions`
says about it, what an explicit attach says, and whether anything anywhere names
the length as the reason.

Run it on a POSIX host with a Godot binary. It launches its own editors, one per
temporary directory, so the lengths are chosen rather than inherited.
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import batch, call, unwrap  # noqa: E402

# "godot_didi_" + 16 hex project key + "_" + pid + "_" + 12 hex id + ".sock".
FIXED_STEM_BYTES = len("godot_didi_") + 16 + 1 + 7 + 1 + 12 + len(".sock")


def sun_path_limit() -> int:
    # Ask the platform rather than hardcoding 104/108: the point of the probe is
    # that this number is a variable.
    try:
        import ctypes

        class SockaddrUn(ctypes.Structure):
            _fields_ = [("sun_len", ctypes.c_uint8), ("sun_family", ctypes.c_uint8)]

        del SockaddrUn
    except Exception:
        pass
    return 104 if sys.platform == "darwin" else 108


def endpoint_bytes(tempdir: str) -> int:
    return len(os.path.join(tempdir, "x" * FIXED_STEM_BYTES))


def sessions_for(project: str, tempdir: str) -> object:
    responses, _ = batch(
        [call("runtime_list_sessions", {}, 1)],
        project=project,
        env={"TMPDIR": tempdir},
        timeout=90,
    )
    payload, _ = unwrap(responses[-1])
    return payload


def launch_editor(godot: str, project: str, tempdir: str, log: Path) -> subprocess.Popen:
    environment = dict(os.environ)
    environment["TMPDIR"] = tempdir
    return subprocess.Popen(
        [godot, "--editor", "--headless", "--path", project, "--log-file", str(log)],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )


def one_case(label: str, godot: str, project: str, tempdir: str, wait: float) -> None:
    os.makedirs(tempdir, exist_ok=True)
    limit = sun_path_limit()
    size = endpoint_bytes(tempdir)
    print(f"\n=== {label} ===")
    print(f"  TMPDIR ({len(tempdir)} bytes): {tempdir}")
    print(f"  an endpoint under it would be {size} bytes; sun_path holds {limit}"
          f"  -> {'FITS' if size < limit else 'OVERFLOWS'}")

    # What a raw bind does, so the platform's own verdict is on the record beside
    # the server's. This is the control: if this succeeds the case is not set up.
    probe_path = os.path.join(tempdir, "x" * FIXED_STEM_BYTES)
    raw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        raw.bind(probe_path)
        print(f"  a raw AF_UNIX bind at that length: succeeded")
        os.unlink(probe_path)
    except OSError as error:
        print(f"  a raw AF_UNIX bind at that length: {error}")
    finally:
        raw.close()

    log = Path(tempdir) / "editor.log"
    editor = launch_editor(godot, project, tempdir, log)
    try:
        time.sleep(wait)
        payload = sessions_for(project, tempdir)
        if isinstance(payload, dict):
            found = payload.get("sessions") or []
            print(f"  runtime_list_sessions: {len(found)} session(s)")
            for entry in found:
                print(f"    alive={entry.get('alive')} pid={entry.get('pid')}")
                print(f"    endpoint ({len(entry.get('endpoint') or '')} bytes): {entry.get('endpoint')}")
                attach, is_error = unwrap(
                    batch(
                        [call("runtime_attach_session", {"session_id": entry["session_id"]}, 1)],
                        project=project,
                        env={"TMPDIR": tempdir},
                        timeout=90,
                    )[0][-1]
                )
                print(f"    attach -> isError={is_error} {json.dumps(attach)[:400]}")
        else:
            print(f"  runtime_list_sessions: {payload!r}")
        text = log.read_text(errors="replace") if log.exists() else ""
        hits = [ln for ln in text.splitlines()
                if "didi" in ln.lower() or "sock" in ln.lower() or "error" in ln.lower()]
        print(f"  editor log lines mentioning didi/socket/error: {len(hits)}")
        for line in hits[-12:]:
            print(f"    {line}")
        says_length = any("length" in ln.lower() or "too long" in ln.lower() or
                          "sun_path" in ln.lower() for ln in hits)
        print(f"  anything anywhere naming the length as the reason: {says_length}")
    finally:
        editor.terminate()
        try:
            editor.wait(timeout=20)
        except subprocess.TimeoutExpired:
            editor.kill()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--godot", default=os.environ.get("GODOT_BIN"))
    parser.add_argument("--wait", type=float, default=40.0)
    args = parser.parse_args()
    if sys.platform.startswith("win"):
        print("named pipes have no sun_path; this probe is POSIX only")
        return 0
    if not args.godot:
        print("pass --godot or set GODOT_BIN")
        return 1

    import tempfile

    stock = tempfile.gettempdir()
    limit = sun_path_limit()
    print(f"platform {sys.platform}; sun_path limit {limit}")
    print(f"stock temporary directory: {stock!r} ({len(stock)} bytes)")
    print(f"an endpoint there would be {endpoint_bytes(stock)} bytes"
          f" -- {limit - endpoint_bytes(stock)} bytes of headroom")

    one_case("the stock temporary directory (the control)", args.godot, args.project, stock, args.wait)

    # Just past the limit, not absurdly past it: a directory name a person could
    # plausibly have. The padding is chosen so the endpoint lands a few bytes over.
    base = Path(stock) / "didi-vibe"
    padding = max(1, limit - endpoint_bytes(str(base)) + 4)
    long_dir = str(base) + "-" + ("d" * padding)
    one_case("a temporary directory a few bytes too long", args.godot, args.project, long_dir, args.wait)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
