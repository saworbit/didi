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

# The rule is about refusals, not about two files. It used to name these two
# and the extension emits refusals from five, so #890 and #892 both landed in
# the unwatched ones: the router handed `session_kind_rejected` over as the
# message while the hook, four hundred lines away and inside the guard, put the
# same identifier under `data.code` where it belongs.
#
# `godot_bridge.cpp` is not in this list. It answers to the rule above instead,
# which is about `bridgeError` and the message position, and its refusals are
# built through helpers this scan cannot read line by line.
ROUTER = ROOT / "src" / "gdextension" / "runtime_request_router.cpp"
SANDBOX = ROOT / "src" / "gdextension" / "expression_sandbox.cpp"
RUNTIME_BRIDGE = ROOT / "src" / "gdextension" / "runtime_bridge.cpp"
IPC = ROOT / "src" / "gdextension" / "gdextension_ipc.cpp"

# Every file whose refusals this scan understands, and the shape it finds them
# in. A file added to src/gdextension that answers with either shape belongs
# here; test_every_refusal_emitting_file_is_scanned is what says so out loud.
LITERAL_REFUSAL_FILES = (HOOK, ROUTER, IPC)
ERROR_JSON_FILES = (SANDBOX, RUNTIME_BRIDGE)

# `godot_bridge.cpp` is held to the rule for the two statuses where the floor's
# name loses something a caller acts on, and not for the rest. A 409 is a state
# the caller can do something about and a 422 says which of several things was
# wrong with what it sent, so `conflict` and `unprocessable` were covering four
# "resend it with this flag" refusals, "save the scene first", "the node has no
# shader" and "there is nothing to undo" (#894).
#
# The 500s are left deliberately. `internal_error` is the honest name for one,
# there are 184 of them, and naming each would bury the two dozen that matter.
# 404 and 501 keep `not_found` and `unimplemented`, which say everything there
# is to say.
PARTIAL_ERROR_JSON_FILES = ((BRIDGE, (409, 422)),)

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


def _error_json_arguments(source: str) -> list[tuple[int, list[str]]]:
    """Every ``errorJson(`` call as its line number and argument list."""
    calls = []
    for match in re.finditer(r'\berrorJson\(', source):
        depth = 0
        for index in range(match.end() - 1, len(source)):
            if source[index] == '(':
                depth += 1
            elif source[index] == ')':
                depth -= 1
                if depth == 0:
                    break
        calls.append((source.count("\n", 0, match.start()) + 1,
                      _split_top_level(source[match.end():index])))
    return calls


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

    def test_every_refusal_above_400_carries_a_code_everywhere_it_is_emitted(self) -> None:
        offenders = []
        for path in LITERAL_REFUSAL_FILES:
            lines = path.read_text(encoding="utf-8").splitlines()
            for index, line in enumerate(lines):
                match = REFUSAL.search(line)
                if not match:
                    continue
                status = int(match.group(1))
                if status == 400:
                    continue
                window = "\n".join(lines[index:index + 9])
                if re.search(r'\{"data", \{\{"code", "[a-z][a-z0-9_]+"\}', window) is None:
                    offenders.append(f"{path.name}:{index + 1} ({status})")
        for path in ERROR_JSON_FILES:
            source = path.read_text(encoding="utf-8")
            for line, arguments in _error_json_arguments(source):
                status = arguments[0].strip()
                if not status.isdigit() or int(status) == 400:
                    continue
                if len(arguments) < 3:
                    offenders.append(f"{path.name}:{line} ({status})")
        for path, statuses in PARTIAL_ERROR_JSON_FILES:
            source = path.read_text(encoding="utf-8")
            for line, arguments in _error_json_arguments(source):
                status = arguments[0].strip()
                if not status.isdigit() or int(status) not in statuses:
                    continue
                if len(arguments) < 3 or '"code"' not in arguments[2]:
                    offenders.append(f"{path.name}:{line} ({status})")
        self.assertEqual(
            offenders, [],
            "A refusal with no data.code gets one from applyErrorDataFloor, "
            "which knows only the status, so unrelated failures sharing a "
            "number arrive under one name:\n  " + "\n  ".join(offenders))

    def test_every_refusal_emitting_file_is_scanned(self) -> None:
        # The list above is the thing that goes stale, which is how the router
        # sat outside it. A new file in src/gdextension that answers with a
        # refusal shape this test understands has to be added to it.
        scanned = {path.name for path in LITERAL_REFUSAL_FILES + ERROR_JSON_FILES}
        scanned.add(BRIDGE.name)
        unscanned = []
        for path in sorted((ROOT / "src" / "gdextension").glob("*.cpp")):
            if path.name in scanned:
                continue
            source = path.read_text(encoding="utf-8")
            emits = REFUSAL.search(source) or re.search(r'\berrorJson\(\d+,', source)
            if emits:
                unscanned.append(path.name)
        self.assertEqual(
            unscanned, [],
            "These emit refusals and no list above names them, so the rule "
            "that every refusal names itself does not reach them:\n  "
            + "\n  ".join(unscanned))

    def test_the_session_kind_refusal_is_a_sentence_on_both_paths(self) -> None:
        # The hook and the router refuse the same thing. The router used to
        # send the identifier as the message and no code at all (#892).
        source = ROUTER.read_text(encoding="utf-8")
        index = source.find('"session_kind_rejected"')
        self.assertNotEqual(index, -1, "the router no longer names this refusal")
        for literal in re.findall(r'\{"message", "([^"\\\n]*)"\}', source):
            self.assertFalse(
                IDENTIFIER.match(literal),
                f"{ROUTER.name} hands {literal} to the caller as the "
                "explanation. The identifier belongs under data.code and the "
                "message belongs in sentences.")

    def test_a_refusal_that_names_an_argument_carries_it(self) -> None:
        """A sentence telling the caller which argument to pass says it twice.

        Five tools refuse a colliding output with the same words and every one
        of them reads the argument its sentence names. Until #900 one of them
        put that in `retry_with` and four left it in the prose, so a client had
        to read English to find the fix that was sitting in a field on the
        tool next door.

        The rule is narrow on purpose: it fires on a message that names the
        argument to pass, which is a promise about a specific argument, and
        asks that the same refusal carries it. Refusals with nothing to add are
        not its business.

        It read one phrasing, `pass <argument>: true`, and the surface uses
        three: that one, `pass <argument> to ...` and `set <argument>=true`.
        Two refusals for create_if_missing and one for accept_current_files
        left the remedy in prose (#902). Two things that read the same are
        skipped: a value rather than an argument (`pass false to fold` in a
        parameter description), and a note in a successful result's
        `limitations`, where there is nothing to retry.
        """
        phrasings = (
            re.compile(r"pass ([a-z_]+): true"),
            re.compile(r"pass ([a-z_]+) to\b"),
            re.compile(r"set ([a-z_]+)=true"),
        )
        offenders = []
        for path in sorted((ROOT / "src").rglob("*.cpp")):
            source = path.read_text(encoding="utf-8")
            for phrasing in phrasings:
                for match in phrasing.finditer(source):
                    argument = match.group(1)
                    if argument in ("true", "false", "null"):
                        continue
                    if '"limitations"' in source[max(0, match.start() - 400):match.start()]:
                        continue
                    window = source[match.start():match.start() + 600]
                    wanted = '{"retry_with", {{"%s", true}}}' % argument
                    if wanted not in window:
                        line = source.count("\n", 0, match.start()) + 1
                        offenders.append(f"{path.name}:{line} ({argument})")
        self.assertEqual(
            offenders, [],
            "The sentence names the argument that fixes the call. retry_with "
            "is where a client reads it without parsing prose:\n  "
            + "\n  ".join(offenders))

    def test_retry_with_is_always_the_arguments_to_send(self) -> None:
        """`retry_with` carries arguments, not the name of one.

        It shipped with #705 as `retry_with: {"overwrite": true}`, an object a
        caller can merge into the arguments it already has. #894 added eight
        more refusals that carry it and wrote the name as a bare string, so one
        key had two types and a client could not read it without checking which
        it had been handed (#897).

        Cheap and exact: the value opens a brace. A string does not.
        """
        offenders = []
        for path in sorted((ROOT / "src").rglob("*.cpp")):
            source = path.read_text(encoding="utf-8")
            for match in re.finditer(r'\{"retry_with",\s*', source):
                if source[match.end()] != "{":
                    line = source.count("\n", 0, match.start()) + 1
                    offenders.append(f"{path.name}:{line}")
        self.assertEqual(
            offenders, [],
            "retry_with is an object of arguments to add, so it carries the "
            "value and not only the name:\n  " + "\n  ".join(offenders))

    def test_a_rolled_back_mutation_says_rolled_back(self) -> None:
        """The failed-mutation outcome vocabulary has one word for success.

        `godot_bridge.cpp` answers a postcondition failure with
        `outcome: rolled_back` when the undo took and `outcome: unknown` when
        it did not. Nine sites do. One said `reverted` instead (#896), which
        nothing read and nothing checked, so a caller branching on the word its
        siblings use saw a mutation it could not classify.

        `tests/phase7_signal_bridge_probe.cpp` pins `rolled_back` against a
        live editor for the signal connect and disconnect paths. This is the
        cheap half of the same rule: the word is not back.

        Not a general check on `outcome`. That key names several unrelated
        things in this file, from viewport projections to undo availability,
        and the vocabulary that matters here cannot be told from the others by
        reading the source text.
        """
        source = BRIDGE.read_text(encoding="utf-8")
        self.assertNotIn(
            '"reverted"', source,
            "The failed-mutation outcome is rolled_back when the undo took and "
            "unknown when it did not. reverted is a tenth word for the nine "
            "sites' first one, and the live signal probe asserts rolled_back.")

    def test_the_cancelled_command_is_not_called_a_timeout(self) -> None:
        source = HOOK.read_text(encoding="utf-8")
        index = source.find('"Command cancelled before execution"')
        self.assertNotEqual(index, -1)
        self.assertIn('"command_cancelled"', source[index:index + 400],
                      "Nothing waited and nothing ran, so the 504 floor's "
                      "timeout is the wrong name for it.")


if __name__ == "__main__":
    unittest.main()
