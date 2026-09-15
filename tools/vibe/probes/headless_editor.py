"""The live surface with nothing to draw on: an editor started `--headless`.

Twelve sessions have attached to an editor with a window. That editor has a
main screen, a rendering device and a viewport with a size, which is the one
configuration where every tool whose answer is a *picture* can succeed. The
ordinary state of a build machine, a container, a box reached over ssh -- and
the only way an editor runs on a CI runner at all -- is `--headless`, where
none of those exist.

So this asks the tools that need a frame what they say when there is no frame,
and the tools that need a dialog what they say when there is no dialog. The
interesting answer is not a refusal. A refusal that names the reason is the
right answer and takes one line to check. The finding is a tool that reports
success and hands back a picture of nothing, because the caller's next move --
"the diff says nothing changed, so my edit did nothing" -- is then wrong for a
reason the response does not contain.

Three of the tools here have never been called by any probe on any platform:
`viewport_capture_passes`, `viewport_create_test_lab` and
`viewport_set_camera_transform`, plus `asset_reimport` against a real imported
asset and `csharp_check_build` against a project with no C# in it. They are in
the same file because they are the same question -- what does a tool that needs
something from the environment do when the environment has not got it -- and
because a headless editor is the cheapest way to have an editor at all.

Every image is decoded and counted here rather than trusted: a capture that
comes back 256x192 of one colour is reported as one colour, whatever the tool
said about it. Read back with something other than the tool that wrote.

    python tools/vibe/probes/headless_editor.py -p SANDBOX

Needs `--fixtures` in the sandbox for the reimport row (`tiles.png`) and the
test-lab row (`sub.tscn`). Rows whose precondition is missing say so by name
rather than passing.
"""

from __future__ import annotations

import argparse
import base64
import json
import platform
import struct
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session, unwrap  # noqa: E402


def png_summary(data: bytes) -> str:
    """Width, height and how many distinct colours -- without an image library.

    The point of decoding here is that the tool under test is the one making
    the claim. A frame that is entirely one colour is what an editor with no
    rendering device produces, and no response field says so.
    """
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return f"not a PNG ({len(data)} bytes, starts {data[:8]!r})"
    width, height = struct.unpack(">II", data[16:24])
    # Walk the chunks for IDAT; a capture is a single-image PNG.
    offset, idat = 8, b""
    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I", data[offset : offset + 4])[0], data[offset + 4 : offset + 8]
        if kind == b"IDAT":
            idat += data[offset + 8 : offset + 8 + length]
        offset += 12 + length
    try:
        raw = zlib.decompress(idat)
    except zlib.error as error:
        return f"{width}x{height}, IDAT would not inflate ({error})"
    # Unfilter is more than this probe needs: a single-colour image is
    # single-colour in every filter type only if every filtered byte after the
    # first row is zero, so count distinct filtered rows as a floor instead and
    # say which measurement it is.
    stride = width * 4 + 1
    rows = {raw[i : i + stride] for i in range(0, min(len(raw), stride * height), stride)}
    first = raw[1 : 1 + min(stride - 1, 64)]
    flat = len(set(first[i : i + 4] for i in range(0, len(first) - 3, 4))) == 1
    return (
        f"{width}x{height}, {len(data)} bytes, {len(rows)} distinct filtered rows, "
        f"first row uniform: {flat}"
    )


def images(envelope: dict) -> list[str]:
    """Every image content block in a tools/call result, decoded and summarised."""
    result = envelope.get("result") or {}
    out = []
    for block in result.get("content") or []:
        if block.get("type") == "image" and isinstance(block.get("data"), str):
            try:
                out.append(png_summary(base64.b64decode(block["data"])))
            except Exception as error:  # a malformed payload is itself the finding
                out.append(f"undecodable image block: {error}")
    return out


def show(label: str, envelope: dict, keys: tuple[str, ...] = ()) -> dict:
    payload, is_error = unwrap(envelope)
    print(f"\n--- {label}")
    print(f"    isError: {is_error}")
    if isinstance(payload, dict):
        code = payload.get("code") or payload.get("error_code")
        if code:
            print(f"    code: {code}")
        message = payload.get("message") or payload.get("error")
        if message:
            print(f"    message: {message}")
        for key in keys:
            if key in payload:
                print(f"    {key}: {json.dumps(payload[key])[:400]}")
    else:
        print(f"    payload: {str(payload)[:400]}")
    for summary in images(envelope):
        print(f"    image: {summary}")
    return payload if isinstance(payload, dict) else {}


def banner(session: Session) -> None:
    print("=" * 72)
    print(f"platform: {platform.platform()}  python temp: {tempfile.gettempdir()}")
    envelope = session.request("tools/call", {"name": "runtime_list_sessions", "arguments": {}})
    payload, _ = unwrap(envelope)
    sessions = payload.get("sessions", []) if isinstance(payload, dict) else []
    print(f"sessions visible: {len(sessions)}")
    for entry in sessions:
        endpoint = entry.get("endpoint") or entry.get("socket_path") or "<no endpoint field>"
        print(f"  kind={entry.get('kind')} pid={entry.get('pid')} engine={entry.get('engine_version')}")
        print(f"  endpoint ({len(str(endpoint))} bytes): {endpoint}")
    print("=" * 72)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    args = parser.parse_args()

    project = Path(args.project)
    with Session(project=project, binary=args.binary) as session:
        banner(session)

        def call(name: str, arguments: dict) -> dict:
            return session.request("tools/call", {"name": name, "arguments": arguments})

        attached, _ = unwrap(call("runtime_attach_session", {}))
        print(f"attach: {json.dumps(attached)[:400]}")

        # --- the frame tools, with and without the argument that exists
        # precisely because "an editor viewport has no size unless its main
        # screen is showing" (#568).
        first = show(
            "viewport_capture_frame {}",
            call("viewport_capture_frame", {}),
            ("execution_mode", "capture_id", "width", "height", "source", "notes"),
        )
        show(
            "viewport_capture_frame {select_main_screen: true}",
            call("viewport_capture_frame", {"select_main_screen": True}),
            ("execution_mode", "capture_id", "width", "height", "source", "notes"),
        )
        baseline = first.get("capture_id")
        if isinstance(baseline, str) and len(baseline) == 32:
            show(
                "viewport_diff_capture against the frame above",
                call("viewport_diff_capture", {"baseline_capture_id": baseline, "select_main_screen": True}),
                ("execution_mode", "bit_identical", "changed_pixels", "ssim", "perceptually_identical"),
            )
        else:
            print("\n--- viewport_diff_capture: skipped, the capture above minted no capture_id")

        show(
            "viewport_capture_passes color+depth+normal",
            call("viewport_capture_passes", {"passes": ["color", "depth", "normal"], "select_main_screen": True}),
            ("execution_mode", "passes", "depth_far", "notes"),
        )
        show(
            "viewport_capture_passes segmentation",
            call("viewport_capture_passes", {"passes": ["segmentation"], "select_main_screen": True}),
            ("execution_mode", "passes", "notes"),
        )

        # --- the editor-state tools that draw rather than answer
        show(
            "viewport_toggle_debug_draw {}",
            call("viewport_toggle_debug_draw", {}),
            ("execution_mode", "collision_shapes", "navigation_mesh", "wireframe"),
        )
        show(
            "editor_render_ghost_preview one 3D box",
            call(
                "editor_render_ghost_preview",
                {"previews": [{"position": {"x": 0, "y": 0, "z": 0}, "size": {"x": 1, "y": 1, "z": 1}}]},
            ),
            ("execution_mode", "preview_id", "drawn", "notes"),
        )
        show(
            "editor_clear_ghost_previews {}",
            call("editor_clear_ghost_previews", {}),
            ("execution_mode", "cleared", "notes"),
        )

        # --- a camera the tool can actually move, then read back
        show(
            "scene_add_node Camera3D",
            call("scene_add_node", {"parent_path": "/root/Main", "node_type": "Camera3D", "node_name": "Cam"}),
            ("execution_mode", "node_path"),
        )
        show(
            "viewport_set_camera_transform on it",
            call(
                "viewport_set_camera_transform",
                {"camera_path": "/root/Main/Cam", "position": {"x": 1, "y": 2, "z": 3}, "fov": 55},
            ),
            ("execution_mode", "applied", "undo_redo_registered", "notes"),
        )
        show(
            "scene_get_property /root/Main/Cam position -- did it move?",
            call("scene_get_property", {"target_node": "/root/Main/Cam", "property_name": "position"}),
            ("value", "execution_mode"),
        )
        show(
            "viewport_set_camera_transform on a Sprite2D that is not a camera",
            call(
                "viewport_set_camera_transform",
                {"camera_path": "/root/Main/Child", "position": {"x": 1, "y": 2, "z": 3}},
            ),
            ("execution_mode", "applied", "notes"),
        )

        # --- the test lab, which writes a scene. Confirmation-gated, so both
        # halves, and then the file it claims to have written.
        target = "res://sub.tscn"
        if not (project / "sub.tscn").is_file():
            print("\n--- viewport_create_test_lab: skipped, sandbox has no sub.tscn (pass --fixtures)")
        else:
            preview = show(
                "viewport_create_test_lab dry_run",
                call("viewport_create_test_lab", {"target_resource_path": target, "dry_run": True}),
                ("preview_kind", "before", "after", "confirmation_token", "target_instanced"),
            )
            token = preview.get("confirmation_token") or (preview.get("confirmation") or {}).get("token")
            show(
                "viewport_create_test_lab confirm",
                call(
                    "viewport_create_test_lab",
                    {"target_resource_path": target, **({"confirmation_token": token} if token else {})},
                ),
                ("execution_mode", "scene_path", "target_instanced", "cameras"),
            )
            lab = project / "didi_test_lab.tscn"
            print(f"    on disk: {lab.name} exists={lab.is_file()} bytes={lab.stat().st_size if lab.is_file() else 0}")

        # --- an asset the editor really did import
        if not (project / "tiles.png").is_file():
            print("\n--- asset_reimport: skipped, sandbox has no tiles.png (pass --fixtures)")
        else:
            show(
                "asset_reimport res://tiles.png",
                call("asset_reimport", {"paths": ["res://tiles.png"]}),
                ("execution_mode", "reimported", "paths", "waited_frames", "notes"),
            )
        show(
            "asset_reimport a path with no file behind it",
            call("asset_reimport", {"paths": ["res://nothing_here.png"]}),
            ("execution_mode", "reimported", "paths", "notes"),
        )

        # --- C# on a project that has none
        show(
            "csharp_check_build {} on a GDScript-only project",
            call("csharp_check_build", {}),
            ("execution_mode", "diagnostics", "exit_code", "project_file", "notes"),
        )

        show(
            "didi_control_room",
            call("didi_control_room", {}),
            ("execution_mode",),
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
