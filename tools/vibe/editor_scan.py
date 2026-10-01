"""A scan the editor runs on its own, as far as Didi can tell.

An editor session takes one client, and that client's server answers one
request at a time, so a probe cannot leave a Didi call waiting on a scan while
it sends another: `editor_reload_project` and `asset_reimport` both wait until
the scan they asked for is applied. The editor starts scans of its own, on
focus, from the file dialog and the asset installer, and nothing on the surface
waits for those. `scan_trigger/` is a small editor plugin that starts one when
a probe asks, so the commands a probe sends next meet a scan Didi did not start.

It can also hold the frame in which the scan's thread finishes. Didi's frame
runs after the editor's process step, and the editor applies a finished scan in
the process step of the next frame, so the window in which a scan is over and
not yet applied is a sliver of one frame and a probe cannot aim at it. Held,
that frame is as long as the probe needs: it sends a call, then releases.

The plugin reads trigger files from `scan_control/` beside the project and
writes markers there: `started` when it asked for a scan, `holding` once it is
holding a frame, `released` when it let go, and `applied` on every
`sources_changed`, which the editor emits once a scan is applied.
"""

from __future__ import annotations

import re
import shutil
import time
from pathlib import Path

# How long to leave a freshly started editor before the first scan. Its first
# scan starts EditorHelp's script documentation regeneration, which walks the
# file index on a worker thread, and a scan applied meanwhile replaces that
# index under it: the editor died in EditorFileSystemDirectory::get_file_type
# off the main thread twice in about a dozen starts, on 4.5.1 and 4.7.2, with
# no Didi call on that thread. The first scan has to be over as well, or 4.5.1
# prints "Task 'first_scan_filesystem' already exists".
STARTUP_SETTLE = 20.0

PLUGIN_SOURCE = Path(__file__).resolve().parent / "scan_trigger"
PLUGIN_CFG = "res://addons/didi_scan_trigger/plugin.cfg"
MARKERS = ("scan", "hold", "go", "started", "holding", "released", "applied")


def install(project: Path) -> None:
    """Copy the plugin into a sandbox and enable it beside Didi's."""
    target = project / "addons" / "didi_scan_trigger"
    target.mkdir(parents=True, exist_ok=True)
    for name in ("plugin.cfg", "plugin.gd"):
        shutil.copyfile(PLUGIN_SOURCE / name, target / name)
    settings = project / "project.godot"
    text = settings.read_text(encoding="utf-8")
    if PLUGIN_CFG not in text:
        text, count = re.subn(r'(enabled=PackedStringArray\()([^)]*)\)',
                              lambda m: f'{m.group(1)}{m.group(2)}, "{PLUGIN_CFG}")', text, count=1)
        if count == 0:
            text += f'\n[editor_plugins]\n\nenabled=PackedStringArray("{PLUGIN_CFG}")\n'
        settings.write_text(text, encoding="utf-8", newline="\n")
    (project.parent / "scan_control").mkdir(exist_ok=True)


class EditorScan:
    def __init__(self, project: Path) -> None:
        self.control = project.parent / "scan_control"
        self.control.mkdir(exist_ok=True)

    def clear(self) -> None:
        for name in MARKERS:
            (self.control / name).unlink(missing_ok=True)

    def _wait(self, name: str, timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            marker = self.control / name
            if marker.exists():
                try:
                    return marker.read_text(encoding="utf-8")
                except OSError:
                    pass
            time.sleep(0.01)
        return None

    def start(self, timeout: float = 10.0) -> bool:
        """Ask the editor for a scan. True when the plugin saw it running."""
        self.clear()
        (self.control / "scan").write_text("", encoding="utf-8")
        return self._wait("started", timeout) == "true"

    def applied(self) -> bool:
        """Whether the editor has applied a scan since the last start."""
        return (self.control / "applied").exists()

    def hold(self, timeout: float = 5.0) -> bool:
        """Hold the editor's next frame while its scan runs. False when the
        scan was over before the plugin could hold one."""
        (self.control / "hold").write_text("", encoding="utf-8")
        held = self._wait("holding", timeout) is not None
        if not held:
            (self.control / "hold").unlink(missing_ok=True)
        return held

    def release(self, timeout: float = 35.0) -> str | None:
        """Let the held frame go once the scan's thread has finished."""
        (self.control / "go").write_text("", encoding="utf-8")
        return self._wait("released", timeout)
