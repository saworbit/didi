"""An audio stream with no audio in it, and what the editor does with it.

Walking the failing workflow for #958, an agent reaching for a looping track
tried `resource_create` with an `AudioStreamOggVorbis` and `loop = true`. The
tool wrote it and said so: a stream with the flag set and no audio, because the
audio lives in data an imported track holds and a caller cannot write. Once, on
4.7.2, the editor then printed two errors of its own while the next call ran:

    ERROR: Condition "packet_sequence.is_null()" is true. Returning: nullptr
        at: instantiate_playback (modules/vorbis/audio_stream_ogg_vorbis.cpp)
    ERROR: Condition "playback.is_null()" is true. Returning: Ref<Texture2D>()
        at: generate (editor/inspector/editor_preview_plugins.cpp)

which is the editor's preview generator trying to draw a waveform for a stream
that cannot play. It did not come back when session twenty-two tried again, so
it is unfiled. This is the reproduction to run when it is worth another look:
one empty stream of each kind the importers make, then a few quiet calls, then
`resource_inspect`, with the editor's log read after every call so any line
lands on the call that was running.

Needs a live editor on a sandbox. A windowed one is likelier to show it, because
only a windowed editor draws previews for the FileSystem dock.

    python tools/vibe/probes/empty_audio_stream.py -p SANDBOX
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mcp_client import Session  # noqa: E402
from probes.animation_library import attach, confirmed  # noqa: E402

RUN = "%04x" % (int(time.time()) & 0xFFFF)
STREAMS = (
    ("ogg", "AudioStreamOggVorbis", [{"name": "loop", "value": True}]),
    ("mp3", "AudioStreamMP3", [{"name": "loop", "value": True}]),
    ("wav", "AudioStreamWAV", [{"name": "loop_mode", "value": 1}]),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("-p", "--project", required=True)
    parser.add_argument("--settle", type=float, default=1.0,
                        help="seconds between the quiet calls that give the editor time to preview")
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    project = Path(args.project).resolve()
    with Session(project=str(project)) as session:
        if attach(session, project) is None:
            print("No live editor on this project.")
            return 2
        for label, stream_type, properties in STREAMS:
            path = f"res://vibe_empty_{RUN}_{label}.tres"
            payload, errored = confirmed(session, "resource_create", {
                "save_path": path, "resource_type": stream_type, "properties": properties})
            written = (payload or {}).get("properties_written") if isinstance(payload, dict) else None
            print(f"  resource_create {stream_type:22} {'REFUSED' if errored else 'written'} {written}")
            for _ in range(3):
                time.sleep(args.settle)
                session.call("audio_list_buses", {})
            payload, errored = session.call("resource_inspect", {"resource_path": path})
            declared = (payload or {}).get("resource_type") if isinstance(payload, dict) else None
            print(f"  resource_inspect {path}: resource_type {declared}")
        print()
        print(session.engine_summary())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
