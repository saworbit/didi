"""Every refusal names what fixes it.

Q6 in docs/BUILD_QUEUE.md, principle P5 in docs/DESIGN_PRINCIPLES.md. A
refusal's error.data carries the argument or the call that fixes it, and the
error floor fills one from src/mcp/refusal_remedies.cpp when its site did not.
That only works for a code the table knows, so this holds the table, which
the manifest publishes as refusals.remedied and refusals.without_remedy,
against every error code the source emits and every code the floor derives
from a status. The live harness checks the refusals themselves.

A failure answered as plain text rather than an envelope has no data at all,
so nothing can name its fix. CallToolResult keeps the constructor that makes
one private, so a new one does not compile; this holds that in place, and the
live harness fails a plain-text failure that reaches a caller anyway.
"""

import json
import re
import subprocess
import unittest
from pathlib import Path

try:
    import didi_binary
except ImportError:
    from tests import didi_binary

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src"
ERROR_DATA = SOURCE / "mcp" / "error_data.cpp"
PROTOCOL = ROOT / "include" / "didi" / "mcp" / "mcp_protocol.hpp"

# Every shape a code is set in: an envelope literal, an assignment, and the
# two helpers the extension builds refusals through.
CODE_PATTERNS = (
    re.compile(r'\{"code",\s*"([a-z][a-z0-9_]+)"\}'),
    re.compile(r'\["code"\]\s*=\s*"([a-z][a-z0-9_]+)"'),
    re.compile(r'"code",\s*"([a-z][a-z0-9_]+)"'),
    # godot_bridge.cpp: bridgeError(status, "identifier", ...).
    re.compile(r'bridgeError\(\s*[^,()]+,\s*"([a-z][a-z0-9_]+)"'),
    # The extension's errorJson(status, message, "identifier"): the identifier
    # is the call's last argument, after a message that can span lines.
    re.compile(r'errorJson\((?:[^;]*?),\s*"([a-z][a-z0-9_]+)"\s*\)', re.S),
)
# godot_bridge.cpp keeps one sentence per identifier in a table, whose keys are
# every identifier its bridgeError calls use, including through variables.
SENTENCE_TABLE_KEY = re.compile(r'^\s*\{"([a-z][a-z0-9_]+)",\s*$', re.M)
BRIDGE = SOURCE / "gdextension" / "godot_bridge.cpp"
# Any argument, not only a string literal. The census this replaced matched a
# literal alone and counted 156 of the 199 there were.
PLAIN_TEXT_ERROR = re.compile(r'CallToolResult::error\(')


def emitted_codes():
    codes = set()
    for path in SOURCE.rglob("*.cpp"):
        text = path.read_text(encoding="utf-8", errors="replace")
        for pattern in CODE_PATTERNS:
            codes.update(pattern.findall(text))
    bridge = BRIDGE.read_text(encoding="utf-8")
    table = bridge[bridge.index("bridgeErrorSentences()"):]
    table = table[:table.index("};")]
    codes.update(SENTENCE_TABLE_KEY.findall(table))
    return codes


def status_codes():
    """The names errorCodeForStatus gives, which a site that set none gets."""
    text = ERROR_DATA.read_text(encoding="utf-8")
    body = text[text.index("errorCodeForStatus(int status)"):text.index("bool retryableForStatus")]
    # Every name the function can return, including the one in its closing ternary.
    return set(re.findall(r'"([a-z_]+)"', body))


def plain_text_error_sites():
    sites = []
    for path in sorted(SOURCE.rglob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in PLAIN_TEXT_ERROR.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            sites.append(f"{path.relative_to(ROOT).as_posix()}:{line}")
    return sites


def call_tool_result_body():
    text = PROTOCOL.read_text(encoding="utf-8")
    start = text.index("struct CallToolResult {")
    return text[start:text.index("\n};", start)]


def manifest_refusals():
    completed = subprocess.run([str(didi_binary.resolve()), "--dump-tool-manifest"],
                               capture_output=True, text=True, timeout=60)
    return json.loads(completed.stdout)["refusals"]


class Census(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.refusals = manifest_refusals()
        cls.remedied = set(cls.refusals["remedied"])
        cls.without = cls.refusals["without_remedy"]

    def test_every_code_the_source_emits_has_a_remedy_or_a_reason(self):
        known = self.remedied | set(self.without)
        missing = sorted((emitted_codes() | status_codes()) - known)
        self.assertEqual(missing, [], "error codes with neither a remedy in "
                                      "src/mcp/refusal_remedies.cpp nor a reason for having none")

    def test_the_table_names_no_code_nothing_emits(self):
        stale = sorted((self.remedied | set(self.without)) - emitted_codes() - status_codes())
        self.assertEqual(stale, [])

    def test_a_code_is_either_remedied_or_without_one(self):
        self.assertEqual(sorted(self.remedied & set(self.without)), [])
        for code, reason in self.without.items():
            self.assertGreaterEqual(len(reason.split()), 4, f"{code} gives no reason")


class PlainTextFailures(unittest.TestCase):
    def test_no_failure_answers_in_plain_text(self):
        self.assertEqual(
            plain_text_error_sites(), [],
            "A failure answered as plain text has no error.data, so it cannot name its fix; "
            "answer with CallToolResult::errorJson, fromError or notConnected instead.")

    def test_the_plain_text_constructor_stays_private(self):
        body = call_tool_result_body()
        constructor = body.index("static CallToolResult error(std::string")
        private = body.rfind("private:", 0, constructor)
        self.assertNotEqual(private, -1, "CallToolResult::error(std::string) is public again")
        self.assertEqual(body.rfind("public:", private, constructor), -1,
                         "CallToolResult::error(std::string) is public again")


if __name__ == "__main__":
    unittest.main()
