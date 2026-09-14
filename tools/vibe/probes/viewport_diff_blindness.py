"""The visual regression tool, reporting perfect agreement about a frame that changed.

`viewport_diff_capture` exists to answer one question -- has the viewport
changed since this baseline? -- and it is the tool a caller would build a visual
regression check on. It reports `bit_identical`, `changed_pixels`,
`changed_ratio` and `ssim`, which is the vocabulary of an image comparison that
means it.

It also takes its own comparison capture, and that is where it goes wrong.
`viewport_capture_frame` carries `select_main_screen`, whose description states
the constraint plainly: "An editor viewport has no size unless its main screen
is showing, so without this an unattended agent cannot capture one at all."
`viewport_diff_capture` has no such parameter -- it refuses it as an unknown
argument -- so the comparison frame it takes for itself is exactly the capture
the sibling's parameter exists to make possible, taken without it.

The result is not an error. It is `bit_identical: true`, `changed_pixels: 0`,
`ssim: 1.0` for a frame in which 55% of the pixels changed. A check that cannot
fail is worse than no check.

The sequence below is deliberately three diffs against **one** baseline, with
nothing changing in the scene between them, so the only variable is which
editor main screen happens to be showing. Rows two and three disagree about the
same pair of images.

Run against a sandbox with a live editor. Mutates the edited scene.

    python tools/vibe/probes/viewport_diff_blindness.py --project SANDBOX --out DIR
"""

from __future__ import annotations

import argparse
import base64
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402


def capture(session: Session, out: Path | None, name: str) -> tuple[str | None, bytes]:
    """One 2D capture; returns its id and the PNG bytes, written out when asked.

    Reading the bytes here rather than trusting the tool is the point: the
    probe has to be able to say the frame changed without asking the tool under
    test whether the frame changed.
    """
    response = session.request(
        "tools/call",
        {
            "name": "viewport_capture_frame",
            "arguments": {"camera_identifier": "editor_2d", "select_main_screen": True},
        },
    )
    result = response.get("result", {})
    structured = result.get("structuredContent") or {}
    images = [block for block in (result.get("content") or []) if block.get("type") == "image"]
    payload = base64.b64decode(images[0]["data"]) if images else b""
    if payload and out is not None:
        (out / f"{name}.png").write_bytes(payload)
    return structured.get("capture_id"), payload


def diff(session: Session, baseline: str | None, label: str) -> None:
    payload, is_error = session.call(
        "viewport_diff_capture", {"baseline_capture_id": baseline, "camera_identifier": "editor_2d"}
    )
    if is_error:
        print(f"    {label:<48} ERR {json.dumps(payload)[:130]}")
        return
    print(
        f"    {label:<48} bit_identical={payload.get('bit_identical')} "
        f"changed_pixels={payload.get('changed_pixels')} "
        f"ssim={round(payload.get('ssim') or 0, 4)}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("-b", "--binary")
    parser.add_argument("--out", help="Where to write the two PNGs, if you want to look at them.")
    args = parser.parse_args()

    out = Path(args.out) if args.out else None
    if out is not None:
        out.mkdir(parents=True, exist_ok=True)

    with Session(project=args.project, binary=args.binary) as session:
        # Start from a scene with nothing drawn in it.
        session.call("scene_remove_node", {"target_node": "/root/Main/Big"})
        time.sleep(2)

        baseline_id, before = capture(session, out, "before")
        print(f"== baseline {baseline_id}, {len(before)} bytes")

        _, is_error = session.call("scene_instantiate_node", {"node_type": "ColorRect", "name": "Big"})
        print(f"   instantiate ColorRect: {'ERR' if is_error else 'ok'}")
        for name, value in (
            ("size", {"x": 900, "y": 700}),
            ("color", {"r": 1, "g": 0, "b": 0, "a": 1}),
        ):
            _, is_error = session.call(
                "scene_set_property",
                {"target_node": "/root/Main/Big", "property_name": name, "value": value},
            )
            print(f"   set {name}: {'ERR' if is_error else 'ok'}")
        time.sleep(3)

        # The diff is asked FIRST, before anything else touches the viewport.
        # Order is the whole experiment. A `viewport_capture_frame` carrying
        # `select_main_screen` makes the 2D viewport render, and once it has
        # rendered, the diff sees the new frame -- so a probe that captures
        # before it diffs hides the finding behind its own setup. That cost an
        # hour here.
        print("== ask the diff first, before anything else touches the viewport")
        diff(session, baseline_id, "1. as an unattended agent would call it")

        _, after = capture(session, out, "after")
        print("\n== only now take a second capture, which forces a render")
        print(f"   after the change, {len(after)} bytes")
        print(f"   the two PNGs are byte-identical: {before == after}")
        print("   (the probe read those bytes itself; the tool was not asked)\n")

        print("== the identical diff, asked again against the same baseline")
        diff(session, baseline_id, "2. after a capture forced the viewport to render")

        print("\n== can the caller ask for the screen the way the sibling allows?")
        payload, is_error = session.call(
            "viewport_diff_capture",
            {
                "baseline_capture_id": baseline_id,
                "camera_identifier": "editor_2d",
                "select_main_screen": True,
            },
        )
        print(f"   {'ERR ' if is_error else 'ok  '}{json.dumps(payload)[:220]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
