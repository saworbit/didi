"""Every refusal the extension emits names itself.

Two rules, one invariant. A client branches on ``error.data.code``, and a
person reads ``error.message``. A refusal that publishes neither leaves the
client reading English and the person reading an identifier.

``godot_bridge.cpp`` has ``bridgeError``, which pairs an identifier with a
sentence and puts the identifier in ``data.code``. Three shader refusals called
``errorJson`` instead and handed the identifier over as the whole message
(#865). Nothing checked that every identifier reached a caller that way, which
is why they survived the sweep that removed the rest.

``editor_hook.cpp`` builds its refusals as literals rather than through a
helper, and a refusal with no ``data.code`` gets one from
``applyErrorDataFloor``, which knows only the status. That is how a press the
engine refused mid-window arrived as ``invalid_arguments`` -- the caller's
mistake -- and a cancelled command arrived as ``timeout`` (#867).

A 400 is the exception, and a real one: the floor's ``invalid_arguments`` is
the true name for an argument this file validated and rejected.
"""

from __future__ import annotations

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
BRIDGE = ROOT / "src" / "gdextension" / "godot_bridge.cpp"
HOOK = ROOT / "src" / "gdextension" / "editor_hook.cpp"

# A bare identifier: lower case, at least one underscore, no spaces. The thing
# a caller should never be shown as an explanation.
IDENTIFIER = re.compile(r'^[a-z][a-z0-9]*(?:_[a-z0-9]+)+$')

REFUSAL = re.compile(r'\{"error", \{\{"code", (\d+)\}')

# The shader trio #865 was about, kept by name so the fix cannot be undone
# quietly by a rewrite that keeps the shape.
SHADER_IDENTIFIERS = (
    "invalid_shader_set_uniform_request",
    "invalid_shader_get_visual_graph_request",
    "invalid_shader_list_uniforms_request",
)


def _split_top_level(text: str) -> list[str]:
    """Split a C++ argument list on its top-level commas."""
    parts = []
    depth = 0
    start = 0
    index = 0
    while index < len(text):
        char = text[index]
        if char == '"':
            index += 1
            while index < len(text) and text[index] != '"':
                index += 2 if text[index] == "\\" else 1
        elif char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(text[start:index])
            start = index + 1
        index += 1
    parts.append(text[start:])
    return parts


def _error_json_messages(source: str) -> list[tuple[int, str]]:
    """The message argument of every ``errorJson(`` call, with its line."""
    messages = []
    for match in re.finditer(r'\berrorJson\(', source):
        depth = 0
        index = match.end() - 1
        for index in range(match.end() - 1, len(source)):
            if source[index] == '(':
                depth += 1
            elif source[index] == ')':
                depth -= 1
                if depth == 0:
                    break
        arguments = _split_top_level(source[match.end():index])
        if len(arguments) < 2:
            continue
        messages.append((source.count("\n", 0, match.start()) + 1, arguments[1]))
    return messages


class BridgeIdentifiersReachCallersThroughBridgeError(unittest.TestCase):
    def test_no_errorjson_call_passes_a_bare_identifier(self) -> None:
        source = BRIDGE.read_text(encoding="utf-8")
        offenders = []
        for line, message in _error_json_messages(source):
            # The message position only. A `data` value may legitimately be an
            # identifier -- that is where identifiers belong.
            for literal in re.findall(r'"([^"\\\n]*)"', message):
                if IDENTIFIER.match(literal):
                    offenders.append(f"{BRIDGE.name}:{line} -> {literal}")
        self.assertEqual(
            offenders, [],
            "errorJson hands the message straight to the caller, so a bare "
            "identifier here is what a person is shown. Use bridgeError, "
            "which says a sentence and keeps the identifier in data.code:\n  "
            + "\n  ".join(offenders))

    def test_the_shader_trio_has_sentences(self) -> None:
        source = BRIDGE.read_text(encoding="utf-8")
        table = source.split("bridgeErrorSentences()", 1)[1]
        table = table.split("return sentences;", 1)[0]
        for identifier in SHADER_IDENTIFIERS:
            self.assertIn(f'{{"{identifier}",', table,
                          f"{identifier} has no sentence, so bridgeError would "
                          "fall back to printing it in brackets.")


class HookRefusalsNameThemselves(unittest.TestCase):
    def test_every_refusal_above_400_carries_a_code(self) -> None:
        lines = HOOK.read_text(encoding="utf-8").splitlines()
        offenders = []
        for index, line in enumerate(lines):
            match = REFUSAL.search(line)
            if not match:
                continue
            status = int(match.group(1))
            if status == 400:
                # Argument validation. invalid_arguments is the true name.
                continue
            window = "\n".join(lines[index:index + 9])
            # A data block is not enough: `retryable` alone still leaves the
            # floor to name the refusal.
            named = re.search(r'\{"data", \{\{"code", "[a-z][a-z0-9_]+"\}', window)
            if named is None:
                message = re.search(r'\{"message", ([^\n]{0,60})', window)
                offenders.append(
                    f"{HOOK.name}:{index + 1} ({status}) "
                    f"{message.group(1) if message else '?'}")
        self.assertEqual(
            offenders, [],
            "A refusal with no data.code gets one from applyErrorDataFloor, "
            "which knows only the status: every 409 becomes conflict and "
            "every 504 becomes timeout, whatever happened:\n  "
            + "\n  ".join(offenders))

    def test_the_cancelled_command_is_not_called_a_timeout(self) -> None:
        source = HOOK.read_text(encoding="utf-8")
        index = source.find('"Command cancelled before execution"')
        self.assertNotEqual(index, -1)
        self.assertIn('"command_cancelled"', source[index:index + 400],
                      "Nothing waited and nothing ran, so the 504 floor's "
                      "timeout is the wrong name for it.")


if __name__ == "__main__":
    unittest.main()
