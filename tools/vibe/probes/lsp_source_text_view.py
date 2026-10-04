"""Whether a source_text check leaves the editor's view of the file as it is on disk.

#1142: script_check_syntax sends source_text to the attached editor's GDScript
language server, under the file's own path when file_path names one. Godot 4.5
keeps one parse of each path for every client and its didClose does nothing, so
the text sent would stay the editor's view of that file until something parsed
it again. Didi sends the file's own text back after the check on 4.5. 4.6 and
4.7 keep what a client opened per client, so nothing outlives the check there.

This asks a second client. `shared.gd` declares a method `keep()` on disk, and
the buffer sent for it declares `other()` instead. The second client asks the
server for `shared.gd`'s symbols without opening it, which reads the server's
own parse of the file:

- control: the second client itself sends the buffer for `shared.gd` and closes
  it. If the symbols then list `other`, the view outlived the close;
- the check: Didi's script_check_syntax sends the buffer with file_path. After
  it the symbols must list `keep` and not `other`, on every line.

Measured 2026-10-04: clean on 4.5.1, 4.6.2 and 4.7.2, and the control showed
the disk's symbols after a closed buffer on all three, 4.5.1 included, so the
leak 4.5's source suggests did not show through documentSymbol. Didi still
sends the file's text back on 4.5; this probe is what would show it stop
mattering or start.

    python tools/vibe/sandbox.py SANDBOX --build-tree build-ninja
    python tools/vibe/probes/lsp_source_text_view.py -p SANDBOX --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402
from wait_for_session import same_project  # noqa: E402

SHARED = '''class_name SharedProbe
extends RefCounted

func keep() -> int:
	return 1
'''

BUFFER = '''class_name SharedProbe
extends RefCounted

func other() -> int:
	return undeclared_in_buffer
'''


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def file_uri(path: str) -> str:
    path = path.replace("\\", "/")
    return ("file://" if path.startswith("/") else "file:///") + path


def same_uri(a: str, b: str) -> bool:
    return a.lower().replace("%3a", ":") == b.lower().replace("%3a", ":")


class LanguageClient:
    """The least of an LSP client: framed JSON-RPC over TCP."""

    def __init__(self, port: int, root: str) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.buffer = b""
        self.next_id = 1
        self.root = root.replace("\\", "/").rstrip("/")
        self.send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                   "params": {"processId": None, "rootPath": self.root, "rootUri": file_uri(self.root),
                              "capabilities": {}}})
        while True:
            message = self.read(60)
            if message.get("method") == "gdscript_client/changeWorkspace":
                self.root = message.get("params", {}).get("path", self.root).replace("\\", "/").rstrip("/")
            if message.get("id") == 1 and "method" not in message:
                break
        self.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

    def send(self, message: dict) -> None:
        body = json.dumps(message).encode("utf-8")
        self.sock.sendall(b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)

    def read(self, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            header_end = self.buffer.find(b"\r\n\r\n")
            if header_end >= 0:
                length = 0
                for line in self.buffer[:header_end].split(b"\r\n"):
                    if line.lower().startswith(b"content-length:"):
                        length = int(line.split(b":", 1)[1])
                if len(self.buffer) >= header_end + 4 + length:
                    body = self.buffer[header_end + 4:header_end + 4 + length]
                    self.buffer = self.buffer[header_end + 4 + length:]
                    message = json.loads(body)
                    if "method" in message and "id" in message:
                        # A request from the server: answer it so it moves on.
                        self.send({"jsonrpc": "2.0", "id": message["id"], "result": None})
                        continue
                    return message
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("the language server sent nothing in time")
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("the language server closed the connection")
            self.buffer += chunk

    def uri(self, relative: str) -> str:
        return file_uri(self.root + "/" + relative)

    def open_and_close(self, relative: str, text: str) -> None:
        uri = self.uri(relative)
        self.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                   "params": {"textDocument": {"uri": uri, "languageId": "gdscript", "version": 1, "text": text}}})
        while True:
            message = self.read(30)
            if (message.get("method") == "textDocument/publishDiagnostics"
                    and same_uri(message.get("params", {}).get("uri", ""), uri)):
                break
        self.send({"jsonrpc": "2.0", "method": "textDocument/didClose",
                   "params": {"textDocument": {"uri": uri}}})

    def symbols(self, relative: str) -> set[str]:
        """The names the server's own parse of the file declares."""
        self.next_id += 1
        asked = self.next_id
        self.send({"jsonrpc": "2.0", "id": asked, "method": "textDocument/documentSymbol",
                   "params": {"textDocument": {"uri": self.uri(relative)}}})
        while True:
            message = self.read(30)
            if message.get("id") == asked and "method" not in message:
                break
        names: set[str] = set()

        def walk(items: list) -> None:
            for item in items or []:
                if isinstance(item, dict):
                    names.add(str(item.get("name", "")))
                    walk(item.get("children", []))
        walk(message.get("result") or [])
        return names


def stale(names: set[str]) -> bool:
    return "other" in names and "keep" not in names


def mine(sessions: dict, project: Path) -> list[dict]:
    return [x for x in (sessions or {}).get("sessions", [])
            if x.get("kind", "editor") == "editor" and same_project(x, project)]


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True, help="a sandbox.py project with the addon")
    parser.add_argument("--godot", required=True, help="a Godot console binary")
    args = parser.parse_args()
    project = Path(args.project).resolve()
    if not (project / "addons" / "didi").is_dir():
        print("no addon in this project; make it with sandbox.py --build-tree")
        return 2
    folder = project / "lsp_probe"
    folder.mkdir(exist_ok=True)
    (folder / "shared.gd").write_text(SHARED, encoding="utf-8", newline="\n")
    subprocess.run([args.godot, "--headless", "--path", str(project), "--import"],
                   capture_output=True, timeout=600)
    port = free_port()
    log = project.parent / "editor.log"
    log.unlink(missing_ok=True)
    editor_process = subprocess.Popen(
        [args.godot, "--headless", "--editor", "--path", str(project), "--lsp-port", str(port),
         "--log-file", str(log)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        editor, deadline = None, time.monotonic() + 180
        while editor is None and time.monotonic() < deadline:
            time.sleep(3)
            with Session(project, editor_log=False) as look:
                found = mine(look.call("runtime_list_sessions", {})[0], project)
            editor = found[0]["session_id"] if found else None
        if editor is None:
            print("the editor published no session in 180 s")
            return 1
        time.sleep(3)
        second = LanguageClient(port, str(project))
        baseline = second.symbols("lsp_probe/shared.gd")
        second.open_and_close("lsp_probe/shared.gd", BUFFER)
        control = second.symbols("lsp_probe/shared.gd")
        second.open_and_close("lsp_probe/shared.gd", SHARED)
        restored_by_hand = second.symbols("lsp_probe/shared.gd")
        print(f"  before anything: shared.gd's symbols {sorted(baseline)}")
        print(f"  control, after a second client sent the buffer and closed it: {sorted(control)} "
              f"({'the view outlived the close' if stale(control) else 'the close dropped it'})")
        print(f"  after that client sent the disk text back: {sorted(restored_by_hand)}")
        with Session(project) as s:
            s.call("runtime_attach_session", {"session_id": editor})
            checked, errored = s.call("script_check_syntax",
                                      {"file_path": "res://lsp_probe/shared.gd", "source_text": BUFFER})
        body = checked if isinstance(checked, dict) else {}
        from_server = [d for d in body.get("diagnostics", []) if d.get("rule") == "godot_language_server"]
        print(f"  Didi's source_text check: engine_backend {body.get('engine_backend')}, "
              f"{len(from_server)} language server diagnostic(s), "
              f"{'REFUSED' if errored else 'answered'}")
        after = second.symbols("lsp_probe/shared.gd")
        print(f"  after Didi's check: shared.gd's symbols {sorted(after)}")
        clean = (not errored and body.get("engine_backend") == "language_server" and bool(from_server)
                 and not stale(baseline) and not stale(restored_by_hand) and not stale(after))
        verdict = "the buffer Didi sent" if stale(after) else "as on disk"
        print()
        print(f"  {'clean' if clean else 'NOT clean'}: the file's view is {verdict} after the check")
        return 0 if clean else 1
    finally:
        editor_process.terminate()
        try:
            editor_process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            editor_process.kill()


if __name__ == "__main__":
    raise SystemExit(main())
