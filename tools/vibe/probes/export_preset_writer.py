"""`project_add_export_preset`, asked what a caller will get wrong about it,
with the engine as the witness for every row the tool accepts.

Session eighteen's subject was the week's export work: the reader that says
which presets Godot will detect (#921, #924), and the tool that writes one
(#779, #926). The harness proves the arc it was built for. This asks what an
agent sends instead, and never believes the tool's own read-back, because the
tool reads the file with the same reader it wrote it for:

* **names** -- every spelling a caller might choose, each exported with
  `project_export` in `pack` mode, so the engine says whether the name it was
  handed on its command line is the name in the file.
* **export paths** -- what is stored for each form of `export_path`, and where
  Godot then writes when it is asked to use the preset's own path.
* **platforms** -- one preset per platform, each exported as a pack by the
  engine on this host.
* **templates** -- the tool's `next_step` promises that `project_export` "says
  so" when a release build has no export templates. Asked.
* **files it did not write** -- CRLF, a byte-order mark, no final newline and
  trailing blank lines, each appended to and then both presets exported.
* **two writers** -- two servers adding presets to one project at once, and
  two setting project settings at once (#929): what each call was told
  against what is in the file afterwards.
* **length** -- the schema's `maxLength` against the handler's byte limit.

`--live SANDBOX` adds the rows that need an editor on a sandbox made by
`sandbox.py --launch`: adds with the editor attached, then the editor made to
write its own list, read back byte for byte.

Needs no Didi editor for the offline rows, but does need a Godot for the
witness: pass `--godot` (the engine `project_export` runs is the same one).

    python tools/vibe/probes/export_preset_writer.py --godot C:\\Godot\\Godot_v4.5.1-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402
import sandbox  # noqa: E402

HERE = Path(__file__).resolve()
SAVE_PROBE = HERE.parents[3] / "tests" / "godot_smoke" / "export_preset_save_probe.gd"


def brief(payload: object, errored: bool | None, width: int = 260) -> str:
    if errored:
        error = (payload or {}).get("error", payload) if isinstance(payload, dict) else payload
        if isinstance(error, dict):
            data = error.get("data") or {}
            extra = {k: data[k] for k in ("reason", "did_you_mean", "parameter", "code")
                     if k in data}
            return f"REFUSED {error.get('code')} {json.dumps(extra)} {error.get('message')}"[:width]
        return f"REFUSED {error}"[:width]
    return "ok"


def fresh_project(parent: Path, label: str) -> Path:
    return sandbox.create(parent / label, name=f"Presets{label}", with_addon=False)


def session(project: Path, godot: str) -> Session:
    return Session(project, env={"GODOT_BIN": godot}, editor_log=False)


def listed(s: Session) -> list[dict]:
    payload, errored = s.call("project_list_export_presets", {})
    if errored:
        print("  list refused:", brief(payload, errored))
        return []
    return payload.get("presets", [])


def export_pack(s: Session, preset: str, out: str) -> str:
    started = time.monotonic()
    payload, errored = s.call("project_export", {"preset": preset, "output_path": out,
                                                 "mode": "pack", "overwrite": True,
                                                 "timeout_seconds": 45})
    if errored:
        took = f" after {time.monotonic() - started:.0f}s"
        error = payload.get("error", payload) if isinstance(payload, dict) else {}
        seen = (error.get("data") or {}).get("detected_presets") if isinstance(error, dict) else None
        return brief(payload, errored, 160) + took + (f" engine listed {seen}" if seen else "")
    return f"ok {payload.get('size_bytes')} bytes"


# ---------------------------------------------------------------------------


NAMES = [
    "Plain",
    'Quote "q"',
    "Back\\slash",
    "Trail\\",
    " Padded ",
    " ",
    "\u00dcn\u00efc\u00f8d\u00e9 \u2713",
    "Emoji \U0001F3AE",
    "[preset.9]",
    "a=b",
    ";semicolon",
    "#hash",
    "--headless",
    "-",
    "plain",
    "Windows Desktop",
    "Tab\tInside",
]


def names(parent: Path, godot: str) -> None:
    print("\n== names: added, listed, then exported as a pack by the engine")
    project = fresh_project(parent, "names")
    with session(project, godot) as s:
        accepted = []
        for i, name in enumerate(NAMES):
            payload, errored = s.call("project_add_export_preset",
                                      {"name": name, "platform": "Linux"})
            if errored:
                print(f"  {name!r:28} add: {brief(payload, errored)}")
            else:
                accepted.append((i, name))
        presets = {p.get("name"): p for p in listed(s)}
        for i, name in accepted:
            back = presets.get(name)
            same = "listed" if back is not None else "NOT LISTED"
            detected = back.get("detected") if back else None
            result = export_pack(s, name, f"res://out/name_{i}.pck")
            print(f"  {name!r:28} {same:10} detected={detected!s:5} export: {result}")


EXPORT_PATHS = [
    "builds/game.x86_64",
    "res://builds/game.x86_64",
    "builds\\game.x86_64",
    "./builds/game.x86_64",
    "builds//game.x86_64",
    "builds/../game.x86_64",
    "RES://builds/game.x86_64",
    "B\u00fbilds/g\u00e4m\u00e9 two.x86_64",
    "builds/",
    "existing_dir",
    "../outside.x86_64",
    "user://game.x86_64",
    "project.godot",
    "export_presets.cfg",
    "main.tscn",
    "res://",
    "",
]


def export_paths(parent: Path, godot: str) -> None:
    print("\n== export_path: what is stored, and where the engine writes when asked "
          "to use the preset's own path")
    project = fresh_project(parent, "paths")
    (project / "existing_dir").mkdir()
    with session(project, godot) as s:
        for i, form in enumerate(EXPORT_PATHS):
            name = f"P{i}"
            payload, errored = s.call("project_add_export_preset",
                                      {"name": name, "platform": "Linux", "export_path": form})
            if errored:
                print(f"  {form!r:36} {brief(payload, errored)}")
                continue
            stored = (payload.get("preset") or {}).get("export_path")
            print(f"  {form!r:36} stored {stored!r}")
    # Godot's own answer to a stored path: --export-pack with no path uses
    # the preset's export_path. A pack has to end in .pck or .zip, which the
    # .x86_64 rows above do not, so these are separate presets whose stored
    # path the engine will take: a plain one, one with non-ASCII and a space,
    # and one given as res://. Each row prints where the pack landed.
    witnesses = [("W plain", "builds/game.pck", "builds/game.pck"),
                 ("W unicode", "B\u00fbilds/g\u00e4m\u00e9 two.pck", "B\u00fbilds/g\u00e4m\u00e9 two.pck"),
                 ("W res", "res://packs/game.pck", "packs/game.pck")]
    with session(project, godot) as s:
        for name, form, _ in witnesses:
            payload, errored = s.call("project_add_export_preset",
                                      {"name": name, "platform": "Linux", "export_path": form})
            if errored:
                print(f"  {name}: {brief(payload, errored)}")
    # Twice: before the folder exists, and after. Godot resolves the stored
    # path against the project, and does not create a folder that is not there.
    for folder_made in (False, True):
        for name, form, expected in witnesses:
            if folder_made:
                (project / expected).parent.mkdir(parents=True, exist_ok=True)
            run = subprocess.run([godot, "--headless", "--path", str(project),
                                  "--export-pack", name.replace(" ", "%20")],
                                 capture_output=True, text=True, encoding="utf-8",
                                 errors="replace", timeout=180)
            landed = (project / expected).is_file()
            errors = [line for line in run.stdout.splitlines() + run.stderr.splitlines()
                      if "Can't open" in line][:1]
            print(f"  engine --export-pack {name!r}, folder {'made' if folder_made else 'absent'}: "
                  f"sent {form!r}; pack at {expected!r}: {'yes' if landed else 'NO'} "
                  f"{errors[0].strip() if errors else ''}")


def platforms(parent: Path, godot: str) -> None:
    print("\n== platforms: one preset each, exported as a pack")
    project = fresh_project(parent, "platforms")
    with session(project, godot) as s:
        for platform in ("Windows Desktop", "Linux", "macOS", "Android", "iOS", "Web",
                         "visionOS"):
            payload, errored = s.call("project_add_export_preset",
                                      {"name": f"Each {platform}", "platform": platform})
            if errored:
                print(f"  {platform:16} add: {brief(payload, errored)}")
                continue
            print(f"  {platform:16} export: "
                  f"{export_pack(s, f'Each {platform}', f'res://out/{platform[:3]}.pck')}")
        for wrong in ("Linux/X11", "windows desktop", "HTML5", "Mac OSX", "Windows"):
            payload, errored = s.call("project_add_export_preset",
                                      {"name": f"Wrong {wrong}", "platform": wrong})
            print(f"  {wrong!r:18} {brief(payload, errored)}")


def templates(parent: Path, godot: str) -> None:
    print("\n== templates: the next_step promise, asked of a release and a debug build")
    project = fresh_project(parent, "templates")
    with session(project, godot) as s:
        payload, errored = s.call("project_add_export_preset",
                                  {"name": "Desktop", "platform": "Windows Desktop",
                                   "export_path": "builds/game.exe"})
        print("  next_step:", (payload or {}).get("next_step"))
        for mode in ("release", "debug"):
            payload, errored = s.call("project_export", {"preset": "Desktop",
                                                         "output_path": f"res://out/{mode}.exe",
                                                         "mode": mode, "overwrite": True})
            if not errored:
                print(f"  {mode}: ok {payload}")
                continue
            error = payload.get("error", payload) if isinstance(payload, dict) else {}
            data = error.get("data") or {}
            engine = data.get("engine_output", "")
            mentions = [line.strip() for line in engine.splitlines()
                        if "template" in line.lower()][:2]
            print(f"  {mode}: {error.get('code')} [{data.get('code')}] reason="
                  f"{data.get('reason')} message={error.get('message')!r}")
            print(f"      template lines in engine_output: {mentions or 'none'}")
            print(f"      data keys: {sorted(data)}")


def files_it_did_not_write(parent: Path, godot: str) -> None:
    print("\n== files it did not write: kept as a prefix, and both presets exported")
    one = ('[preset.0]\n\nname="Mine"\nplatform="Linux"\nrunnable=true\n'
           'export_filter="all_resources"\ninclude_filter=""\nexclude_filter=""\n'
           'export_path=""\n\n[preset.0.options]\n\ncustom_template/debug=""\n')
    forms = {
        "lf": one.encode(),
        "crlf": one.replace("\n", "\r\n").encode(),
        "bom": b"\xef\xbb\xbf" + one.encode(),
        "no final newline": one.rstrip("\n").encode(),
        "three blank lines": (one + "\n\n\n").encode(),
        "options last, no newline": one.encode()[:-1],
    }
    for label, data in forms.items():
        project = fresh_project(parent, "file_" + label.replace(" ", "_").replace(",", ""))
        (project / "export_presets.cfg").write_bytes(data)
        with session(project, godot) as s:
            payload, errored = s.call("project_add_export_preset",
                                      {"name": "Added", "platform": "Linux"})
            after = (project / "export_presets.cfg").read_bytes()
            prefix = after.startswith(data)
            joined = after[len(data) - 2:len(data) + 4] if prefix else b""
            outcome = brief(payload, errored) if errored else "ok"
            names = [p.get("name") for p in listed(s)]
            mine = export_pack(s, "Mine", "res://out/mine.pck")
            added = export_pack(s, "Added", "res://out/added.pck") if not errored else "-"
            print(f"  {label:26} add {outcome[:90]}; prefix kept={prefix} join={joined!r}")
            print(f"      listed {names}; engine: Mine {mine[:70]} / Added {added[:70]}")


def two_writers(parent: Path, godot: str, rounds: int = 20) -> None:
    """Two servers, one project, the same read-modify-write at once (#929).

    Printed per tool: how many calls each side was told succeeded, how many of
    those are in the file afterwards (a lost update is one that was not), how
    many calls failed although their write is in the file, and every distinct
    failure with its count. project_set_setting is the other tool that edits a
    shared project file in this process, so it is asked the same way.
    """
    cases = [
        ("project_add_export_preset", lambda name: {"name": name, "platform": "Linux"},
         lambda project: set(re.findall(r'^name="([^"]*)"',
                                        (project / "export_presets.cfg").read_text(encoding="utf-8"),
                                        re.M))),
        ("project_set_setting", lambda name: {"setting": f"vibe/race/{name}", "value": 1},
         lambda project: set(re.findall(r"^race/(\w+)=",
                                        (project / "project.godot").read_text(encoding="utf-8"),
                                        re.M))),
    ]
    for tool, arguments, written in cases:
        print(f"\n== two writers: two servers calling {tool} {rounds} times each, at once")
        project = fresh_project(parent, "two_writers_" + tool)
        reports: dict[str, list[tuple[str, str]]] = {"A": [], "B": []}
        start = threading.Barrier(2)

        def writer(tag: str) -> None:
            with session(project, godot) as s:
                start.wait()
                for i in range(rounds):
                    payload, errored = s.call(tool, arguments(f"{tag}{i}"))
                    outcome = "ok" if not errored else re.sub(
                        r"[A-Z]:/\S+", "<path>", brief(payload, errored, 140))
                    reports[tag].append((f"{tag}{i}", outcome))

        threads = [threading.Thread(target=writer, args=(tag,)) for tag in reports]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        present = written(project)
        rows = [row for tagged in reports.values() for row in tagged]
        ok = [name for name, outcome in rows if outcome == "ok"]
        lost = [name for name in ok if name not in present]
        landed_anyway = [name for name, outcome in rows if outcome != "ok" and name in present]
        failures: dict[str, int] = {}
        for _, outcome in rows:
            if outcome != "ok":
                failures[outcome] = failures.get(outcome, 0) + 1
        print(f"  reported ok: {len(ok)}; in the file: {len(present)}; "
              f"reported ok and not in the file (lost): {len(lost)} {lost[:6]}")
        print(f"  failed, and written anyway: {len(landed_anyway)} {landed_anyway[:6]}")
        for outcome, count in sorted(failures.items(), key=lambda item: -item[1]):
            print(f"  {count:3} x {outcome}")


def lengths(parent: Path, godot: str) -> None:
    print("\n== length: the schema's maxLength (characters) against the handler's bytes")
    project = fresh_project(parent, "lengths")
    rows = [("x" * 256, "256 ASCII"), ("x" * 257, "257 ASCII"),
            ("\u00e9" * 128, "128 x e-acute = 256 bytes"),
            ("\u00e9" * 129, "129 x e-acute = 258 bytes"),
            ("\U0001F3AE" * 64, "64 emoji = 256 bytes"),
            ("\U0001F3AE" * 65, "65 emoji = 260 bytes, 65 characters")]
    with session(project, godot) as s:
        for name, label in rows:
            payload, errored = s.call("project_add_export_preset",
                                      {"name": name, "platform": "Linux"})
            print(f"  {label:38} {brief(payload, errored, 200)}")


def live(project: Path, godot: str) -> None:
    print("\n== live: adds with the editor attached, then the editor made to write its list")
    if not SAVE_PROBE.is_file():
        print("  the harness's save probe is missing:", SAVE_PROBE)
        return
    shutil.copy(SAVE_PROBE, project / "vibe_preset_save_probe.gd")
    # --yolo: scene_call_method is behind the confirmation gate, and the gate
    # is not what this section asks about.
    with Session(project, env={"GODOT_BIN": godot}, extra_args=["--yolo"]) as s:
        sessions, _ = s.call("runtime_list_sessions", {})
        mine = [x for x in (sessions or {}).get("sessions", [])
                if Path(x.get("project_path", "")).resolve() == project.resolve()]
        if not mine:
            print("  no editor on this project; launch one with sandbox.py --launch")
            return
        s.call("runtime_attach_session", {"session_id": mine[0]["session_id"]})
        run = int(time.time()) % 10000
        names_live = [f'Live {run} "quoted"', f"Live {run} ünïcødé",
                      f"Live {run} plain"]
        for name in names_live:
            payload, errored = s.call("project_add_export_preset",
                                      {"name": name, "platform": "Windows Desktop",
                                       "export_path": "builds/live.exe"})
            print(f"  add {name!r:24} {brief(payload, errored)} editor_reloaded="
                  f"{(payload or {}).get('editor_reloaded')} "
                  f"diagnostics={(payload or {}).get('engine_diagnostics')}")
        before = (project / "export_presets.cfg").read_bytes()
        s.call("scene_open", {"scene_path": "res://main.tscn"})
        s.call("scene_instantiate_node", {"node_type": "Node", "parent_path": "/root/Main",
                                          "name": "VibePresetSave"})
        s.call("script_attach_to_node", {"target_node": "/root/Main/VibePresetSave",
                                         "script_path": "res://vibe_preset_save_probe.gd"})
        payload, errored = s.call("scene_call_method",
                                  {"target_node": "/root/Main/VibePresetSave",
                                   "method_name": "force_preset_save", "arguments": [],
                                   "timeout_seconds": 20})
        print("  forced save:", brief(payload, errored), (payload or {}).get("returned"))
        s.call("scene_remove_node", {"target_node": "/root/Main/VibePresetSave"})
        after = (project / "export_presets.cfg").read_bytes()
        print(f"  file {len(before)} bytes before the editor's save, {len(after)} after")
        presets = listed(s)
        for p in presets:
            print(f"    {p.get('name')!r:28} platform={p.get('platform')!r} "
                  f"export_path={p.get('export_path')!r} detected={p.get('detected')}")
        for name in names_live:
            if name not in [p.get("name") for p in presets]:
                print(f"  DROPPED by the editor's save: {name!r}")
        text = after.decode("utf-8", "replace")
        for key in ("name=", "export_path="):
            print(f"  lines the editor wrote for {key}",
                  [line for line in text.splitlines() if line.startswith(key)])


SECTIONS = {
    "names": names,
    "paths": export_paths,
    "platforms": platforms,
    "templates": templates,
    "files": files_it_did_not_write,
    "writers": two_writers,
    "lengths": lengths,
}


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--godot", required=True)
    parser.add_argument("--only", choices=sorted(SECTIONS) + ["live"], action="append")
    parser.add_argument("--live", metavar="SANDBOX",
                        help="A sandbox with an editor open on it (sandbox.py --launch).")
    parser.add_argument("--keep", action="store_true", help="Keep the throwaway projects.")
    args = parser.parse_args()

    parent = Path(tempfile.mkdtemp(prefix="vibe_presets_"))
    print(f"projects under {parent}; engine {args.godot}")
    try:
        for key, section in SECTIONS.items():
            if (args.only and key not in args.only) or (args.live and not args.only):
                continue
            section(parent, args.godot)
        if args.live and (not args.only or "live" in args.only):
            live(Path(args.live), args.godot)
    finally:
        if not args.keep:
            shutil.rmtree(parent, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
