"""How a scene names an imported asset it references, asked of the engine alone.

The resource index types an imported asset by what a scene's `[ext_resource]`
line says, not by the class it loads as (#1461), and checks a uid's form one way
everywhere (#1446). This probe keeps the evidence for both, on every engine it
is given:

* **Imported types.** A PNG and a WAV are imported with `--import`, then loaded,
  given to a `Sprite2D` and an `AudioStreamPlayer`, and the scene is packed and
  saved. Recorded: the class each loads as, the `[remap] type` in its `.import`
  sidecar, and the `[ext_resource]` lines the saved scene carries. On 4.5.1,
  4.6.2 and 4.7.2 they load as `CompressedTexture2D` and `AudioStreamWAV`, the
  sidecars say the same, and the scene writes `Texture2D` and `AudioStream`.
* **A malformed uid.** A scene whose `[ext_resource]` beside a real path holds
  `uid="uid://ABC"`, loaded and instanced. Recorded: whether the texture
  arrived, the path it came from, `ResourceUID.text_to_id("uid://ABC")`, and the
  engine's lines. The engine warns `invalid UID`, loads the path, and reads the
  text as -1: only `a` to `z` and `0` to `9` after `uid://` make a uid.

Needs no Didi build. Every project is a throwaway under `--out`.

    python tools/vibe/probes/ext_resource_type_engine.py --godot C:/Godot/Godot_v4.5.1-stable_win64_console.exe \\
        --godot C:/Godot/Godot_v4.6.2-stable_win64_console.exe --godot C:/Godot/Godot_v4.7.2-stable_win64_console.exe
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
import zlib
from pathlib import Path

SAVE_SCRIPT = """extends SceneTree
func _init():
\tvar tex = load("res://icon.png")
\tvar snd = load("res://hit.wav")
\tprint("PROBE classes ", tex.get_class(), " ", snd.get_class())
\tvar root := Node.new()
\troot.name = "Root"
\tvar sprite := Sprite2D.new()
\tsprite.name = "S"
\tsprite.texture = tex
\troot.add_child(sprite)
\tsprite.owner = root
\tvar player := AudioStreamPlayer.new()
\tplayer.name = "P"
\tplayer.stream = snd
\troot.add_child(player)
\tplayer.owner = root
\tvar packed := PackedScene.new()
\tpacked.pack(root)
\tResourceSaver.save(packed, "res://s.tscn")
\tfor line in FileAccess.get_file_as_string("res://s.tscn").split("\\n"):
\t\tif line.begins_with("[ext_resource"):
\t\t\tprint("PROBE ", line)
\troot.free()
\tquit()
"""

BAD_UID_SCENE = ('[gd_scene load_steps=2 format=3]\n\n'
                 '[ext_resource type="Texture2D" uid="uid://ABC" path="res://icon.png" id="1"]\n\n'
                 '[node name="Root" type="Sprite2D"]\ntexture = ExtResource("1")\n')
BAD_UID_SCRIPT = """extends SceneTree
func _init():
\tvar node = load("res://bad_uid.tscn").instantiate()
\tprint("PROBE texture ", node.texture != null, " ", node.texture.resource_path if node.texture else "")
\tprint("PROBE text_to_id ", ResourceUID.text_to_id("uid://ABC"))
\tnode.free()
\tquit()
"""


def run(command: list[str], timeout: int = 300) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          timeout=timeout)


def engine_lines(output: str) -> list[str]:
    return [line.strip() for line in output.splitlines() if "ERROR" in line or "WARNING" in line]


def probe_lines(output: str) -> list[str]:
    return [line[len("PROBE "):] for line in output.splitlines() if line.startswith("PROBE ")]


def png_bytes() -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + b"\xff\x00\x00" * 4 for _ in range(4))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def remap_type(sidecar: Path) -> str | None:
    if not sidecar.exists():
        return None
    for line in sidecar.read_text(encoding="utf-8").splitlines():
        if line.startswith("type="):
            return line.split("=", 1)[1].strip().strip('"')
    return None


def probe(godot: str, out: Path) -> dict:
    project = out / "project"
    if project.exists():
        shutil.rmtree(project)
    project.mkdir(parents=True)
    (project / "project.godot").write_bytes(b'config_version=5\n\n[application]\n\nconfig/name="probe"\n')
    (project / "icon.png").write_bytes(png_bytes())
    with wave.open(str(project / "hit.wav"), "wb") as sound:
        sound.setnchannels(1)
        sound.setsampwidth(2)
        sound.setframerate(22050)
        sound.writeframes(b"\x00\x00" * 2205)
    imported = run([godot, "--headless", "--path", str(project), "--import"])
    (project / "save.gd").write_bytes(SAVE_SCRIPT.encode("utf-8"))
    saved = run([godot, "--headless", "--path", str(project), "--script", "res://save.gd"])
    (project / "bad_uid.tscn").write_bytes(BAD_UID_SCENE.encode("utf-8"))
    (project / "bad_uid.gd").write_bytes(BAD_UID_SCRIPT.encode("utf-8"))
    bad = run([godot, "--headless", "--path", str(project), "--script", "res://bad_uid.gd"])
    return {
        "imported_types": {"png_remap": remap_type(project / "icon.png.import"),
                           "wav_remap": remap_type(project / "hit.wav.import"),
                           "saved_scene": probe_lines(saved.stdout),
                           "engine": engine_lines(imported.stdout + imported.stderr + saved.stdout + saved.stderr)},
        "malformed_uid": {"read": probe_lines(bad.stdout), "engine": engine_lines(bad.stdout + bad.stderr)},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--godot", action="append", required=True, help="a Godot console executable; repeat per line")
    parser.add_argument("--out", type=Path, default=None, help="where the throwaway projects go")
    args = parser.parse_args()
    out = args.out or Path(tempfile.mkdtemp(prefix="didi-ext-resource-"))
    reports = {}
    for godot in args.godot:
        version = run([godot, "--version"]).stdout.strip()
        reports[version] = probe(godot, out / Path(godot).stem)
    print(json.dumps(reports, indent=2))
    print(f"projects kept under {out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
