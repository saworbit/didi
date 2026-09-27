"""What a Claude host actually sends for each argument whose shape is not a string.

A client fills a gap in a schema its own way. Claude Code sends a top-level
argument that declares no JSON `type` as a string, whatever the model meant:
asked for the integer 1152, the boolean false and the array ["a", "b"], it put
"1152", "false" and "[\"a\", \"b\"]" on the wire for `project_set_setting.value`
(#1000). With an editor attached each was refused as a String, and offline the
quoted text was written into project.godot and reported as success. The server
could not tell, because what it received was a well-formed string.

Nothing in the repository can see that from the inside: every test client sends
the types its author wrote. This probe asks a real Claude Code host, run as a
subprocess against the server under test, to send one typed value to each
top-level argument whose accepted shape is not a string, then reads two things:

* the type each value arrived as, from the host's own stream of tool calls;
* the three project settings it wrote offline, from project.godot.

Each row prints `same` when the value arrived with the type asked for, and `DIFF`
when it did not. Against a server from before #1000 the three untyped arguments
print DIFF and project.godot holds quoted strings.

It spends real model tokens, about the cost of one short session, so it is a
probe to run when a schema changes shape, not a test. It needs the `claude`
command on PATH.

    python tools/vibe/probes/claude_host_argument_types.py
    python tools/vibe/probes/claude_host_argument_types.py --binary build-ninja/didi.exe --model sonnet
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from mcp_client import resolve_binary  # noqa: E402


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


FIELD_TRIAL = HERE.parents[1] / "field-trial"
transcripts = load("field_trial_transcripts", FIELD_TRIAL / "transcripts.py")

# (tool, argument, the JSON type asked for, how the prompt spells the call)
ROWS = [
    ("project_set_setting", "value", "integer",
     'setting "display/window/size/viewport_width", value 1152 (integer)'),
    ("project_set_setting", "value", "boolean",
     'setting "application/boot_splash/show_image", value false (boolean)'),
    ("project_set_setting", "value", "array",
     'setting "application/config/tags", value ["a", "b"] (array)'),
    ("blackboard_write", "value", "object", 'path "probe.slots", value {"slots": 3} (object)'),
    ("blackboard_task_complete", "artifacts", "array",
     'task_id "t1", agent_id "a1", artifacts ["res://a.gd"] (array)'),
    ("audio_configure_bus", "bus", "integer", "bus 1 (the integer index), mute true"),
    ("viewport_toggle_debug_draw", "wireframe", "boolean", "wireframe false (boolean)"),
    ("physics_raycast_query", "from", "object",
     'from {"x": 0, "y": 0} (object), to {"x": 10, "y": 0} (object)'),
    ("nav_query_path", "start_point", "object",
     'start_point {"x": 0, "y": 0, "z": 0} (object), end_point {"x": 5, "y": 0, "z": 5} (object)'),
    ("viewport_set_camera_transform", "position", "object",
     'camera_path "/root/Camera3D", position {"x": 1, "y": 2, "z": 3} (object)'),
    ("resource_create", "properties", "object",
     'save_path "res://probe.tres", resource_type "Resource", '
     'properties {"resource_name": "p"} (object), dry_run true'),
]

# What the three settings must look like in project.godot once written. A value
# that arrived as a string is written quoted.
SETTINGS = {
    "window/size/viewport_width": "1152",
    "boot_splash/show_image": "false",
    "config/tags": '["a", "b"]',
}

PROMPT_HEAD = (
    "Call these Didi tools exactly as written, one call each, in order. Pass every "
    "value with the JSON type shown: numbers as JSON numbers, booleans as JSON "
    "booleans, arrays as JSON arrays and objects as JSON objects, never as strings. "
    "Errors are expected, because no editor is running; do not retry, do not call "
    "any other tool, and reply DONE at the end.\n"
)


def json_type(value: object) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    return "object"


def setting_lines(project_file: Path) -> dict[str, str]:
    lines: dict[str, str] = {}
    if not project_file.is_file():
        return lines
    for line in project_file.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition("=")
        if sep and key.strip() in SETTINGS:
            lines[key.strip()] = value.strip()
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--binary", help="didi server to test; defaults to the build tree's")
    parser.add_argument("--model", help="model for the host; defaults to the host's own")
    parser.add_argument("--budget-usd", type=float, default=2.0,
                        help="spending ceiling passed to the host")
    parser.add_argument("--keep", action="store_true", help="keep the throwaway project")
    args = parser.parse_args()

    claude = shutil.which("claude")
    if claude is None:
        print("The claude command is not on PATH; this probe needs a Claude Code host.")
        return 2
    binary = Path(args.binary).resolve() if args.binary else resolve_binary()

    work = Path(tempfile.mkdtemp(prefix="didi-host-types-"))
    project = work / "project"
    project.mkdir()
    (project / "project.godot").write_text(
        'config_version=5\n\n[application]\n\nconfig/name="HostTypes"\n', encoding="utf-8")
    config = work / "mcp.json"
    config.write_text(json.dumps({"mcpServers": {"didi": {
        "command": str(binary), "args": ["--project", str(project)]}}}), encoding="utf-8")

    prompt = PROMPT_HEAD + "".join(
        f"{index}. {tool}: {spelled}\n" for index, (tool, _, _, spelled) in enumerate(ROWS, 1))
    command = [claude, "--print", "--output-format", "stream-json", "--verbose",
               "--mcp-config", str(config), "--strict-mcp-config",
               "--allowed-tools", "mcp__didi", "--max-budget-usd", str(args.budget_usd)]
    if args.model:
        command += ["--model", args.model]
    # The prompt goes in on stdin: a trailing positional is swallowed by
    # whichever variadic flag comes last (tools/field-trial/runner.py).
    run = subprocess.run(command, input=prompt, capture_output=True, text=True,
                         encoding="utf-8", cwd=work, timeout=900)
    (work / "stream.jsonl").write_text(run.stdout, encoding="utf-8")

    calls = transcripts.iter_invocations(run.stdout.splitlines())
    print(f"server  {binary}")
    print(f"host    {len(calls)} didi call(s), exit {run.returncode}")
    differences = 0
    seen: dict[str, int] = {}
    for tool, argument, wanted, _ in ROWS:
        # The nth row for a tool reads the nth call to it.
        matching = [call for call in calls if call.tool == tool]
        index = seen.get(tool, 0)
        seen[tool] = index + 1
        if index >= len(matching) or argument not in (matching[index].arguments or {}):
            print(f"  MISSING  {tool}.{argument}: the host made no such call")
            differences += 1
            continue
        value = matching[index].arguments[argument]
        arrived = json_type(value)
        verdict = "same" if arrived == wanted else "DIFF"
        differences += verdict == "DIFF"
        print(f"  {verdict:4}  {tool}.{argument}: asked {wanted}, arrived {arrived} "
              f"{json.dumps(value)[:48]}")

    written = setting_lines(project / "project.godot")
    print("project.godot")
    for key, wanted in SETTINGS.items():
        actual = written.get(key)
        verdict = "same" if actual == wanted else "DIFF"
        differences += verdict == "DIFF"
        print(f"  {verdict:4}  {key}: expected {wanted}, found {actual}")

    if args.keep:
        print(f"kept {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    print(f"{differences} difference(s)")
    return 1 if differences else 0


if __name__ == "__main__":
    sys.exit(main())
