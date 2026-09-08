"""Static invariants of the Control Room page and its build-time embedding.

These are the requirements in docs/CONTROL_ROOM_DESIGN.md section 6 that hold
over the page source itself rather than over any running process. Each one fails
if its guard is removed from resources/control_room.html.

The page renders values that originate in files a project can contain -- node
names, tool descriptions, paths, log lines. A project is not a trust boundary,
so those values are treated as hostile, and the way that is enforced is that no
path exists from data to parsed markup at all.
"""

from __future__ import annotations

import hashlib
import importlib.util
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PAGE = REPO / "resources" / "control_room.html"
GENERATOR = REPO / "tools" / "generate_ui_app.py"
CMAKE = REPO / "CMakeLists.txt"

# The one absolute URL a self-contained page legitimately contains: an SVG
# namespace identifier, which is a name and never fetched.
ALLOWED_ABSOLUTE_URLS = {"http://www.w3.org/2000/svg"}


def load_generator():
    spec = importlib.util.spec_from_file_location("generate_ui_app", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PageExists(unittest.TestCase):
    def test_the_page_is_present_and_is_a_document(self):
        self.assertTrue(PAGE.is_file(), f"{PAGE} is missing")
        text = PAGE.read_text(encoding="utf-8")
        self.assertTrue(text.lstrip().lower().startswith("<!doctype html>"))
        self.assertIn("<title>Didi Control Room</title>", text)


class NoMarkupInjection(unittest.TestCase):
    """A2. No path from data to parsed markup."""

    # Every API that turns a string into DOM. textContent and createElement are
    # the two that do not, and they are what the page is written with.
    FORBIDDEN = (
        "innerHTML",
        "outerHTML",
        "insertAdjacentHTML",
        "document.write",
        "createContextualFragment",
        "DOMParser",
        "eval(",
        "new Function",
        "setTimeout(\"",
        "setInterval(\"",
        # A javascript: URL is markup-flavoured code by another route.
        "javascript:",
    )

    def test_no_markup_parsing_api_appears_in_the_page(self):
        text = PAGE.read_text(encoding="utf-8")
        found = [needle for needle in self.FORBIDDEN if needle in text]
        self.assertEqual(
            found,
            [],
            "The Control Room page must never turn data into markup. Found: "
            + ", ".join(found)
            + ". Use textContent and createElement instead.",
        )

    def test_the_page_actually_uses_the_safe_api(self):
        # Guards against the forbidden list passing because the page stopped
        # rendering anything at all.
        text = PAGE.read_text(encoding="utf-8")
        self.assertGreater(text.count("textContent"), 3)
        self.assertIn("createElement", text)


class NoExternalOrigin(unittest.TestCase):
    """A5. The page loads nothing, so no CSP domain has to be declared."""

    def test_no_external_url_is_referenced(self):
        text = PAGE.read_text(encoding="utf-8")
        urls = set(re.findall(r"https?://[^\s\"'<>)]+", text))
        unexpected = urls - ALLOWED_ABSOLUTE_URLS
        self.assertEqual(
            unexpected,
            set(),
            "The page must stay self-contained: the server declares no csp "
            f"domains, so the host's default-src 'none' applies. Found: {unexpected}",
        )

    def test_no_protocol_relative_or_remote_loading_element(self):
        text = PAGE.read_text(encoding="utf-8")
        self.assertNotIn('src="//', text)
        self.assertNotIn("@import", text)
        # A stylesheet link would be a remote fetch under a policy that forbids one.
        self.assertNotIn("<link", text.lower())
        # An external script tag likewise. The only script is the inline one.
        self.assertEqual(len(re.findall(r"<script\b[^>]*\ssrc=", text)), 0)


class HostIsTheOnlySpeaker(unittest.TestCase):
    """A4. Only window.parent is listened to."""

    def test_inbound_messages_are_checked_against_the_parent(self):
        text = PAGE.read_text(encoding="utf-8")
        self.assertIn("event.source !== window.parent", text)

    def test_the_check_precedes_any_dispatch(self):
        text = PAGE.read_text(encoding="utf-8")
        guard = text.index("event.source !== window.parent")
        # Every branch that acts on a message must come after the guard.
        for handled in ("ui/notifications/tool-result", "ui/resource-teardown"):
            self.assertGreater(
                text.index(handled, guard),
                guard,
                f"{handled} is dispatched before the sender is checked",
            )


class NoSecretNames(unittest.TestCase):
    """A1, held over the page as well as over the payload."""

    def test_the_page_never_names_the_descriptor_token_field(self):
        text = PAGE.read_text(encoding="utf-8")
        self.assertNotIn(
            "token",
            text.lower(),
            "The page must not name the session token field. A page that reads it "
            "is a page that can render it.",
        )


class Embedding(unittest.TestCase):
    """The page reaches the binary intact, or the build fails."""

    def test_the_generator_round_trips_the_page_byte_for_byte(self):
        module = load_generator()
        original = PAGE.read_bytes()
        rendered = module.render_source(original, "resources/control_room.html")

        # Recover the literals and unescape them the way a C++ compiler would.
        literals = re.findall(r'^    "(.*)",$', rendered, re.MULTILINE)
        self.assertGreater(len(literals), 1, "the page should be chunked")
        recovered = bytearray()
        for literal in literals:
            index = 0
            while index < len(literal):
                char = literal[index]
                if char != "\\":
                    recovered.extend(char.encode("utf-8"))
                    index += 1
                    continue
                nxt = literal[index + 1]
                simple = {"n": 10, "t": 9, "r": 13, "\\": 92, '"': 34, "?": 63}
                if nxt in simple:
                    recovered.append(simple[nxt])
                    index += 2
                else:
                    octal = literal[index + 1 : index + 4]
                    self.assertRegex(octal, r"^[0-7]{3}$", f"bad escape in {literal[:40]!r}")
                    recovered.append(int(octal, 8))
                    index += 4
        self.assertEqual(bytes(recovered), original)
        self.assertIn(hashlib.sha256(original).hexdigest(), rendered)

    def test_every_chunk_is_within_the_msvc_literal_limit(self):
        module = load_generator()
        rendered = module.render_source(PAGE.read_bytes(), "p")
        for literal in re.findall(r'^    "(.*)",$', rendered, re.MULTILINE):
            # MSVC C2026 rejects a single literal over 16380 characters.
            self.assertLess(len(literal), 16380)

    def test_the_generator_refuses_an_empty_page(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            empty = root / "empty.html"
            empty.write_bytes(b"")
            result = subprocess.run(
                [sys.executable, str(GENERATOR), "--page", str(empty),
                 "--header", str(root / "h.hpp"), "--source", str(root / "s.cpp")],
                capture_output=True, text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("required", result.stderr)

    def test_the_build_names_the_page_and_the_generator(self):
        # Without both DEPENDS entries an edit to the page or the generator does
        # not rebuild, and the binary quietly serves the previous dashboard.
        cmake = CMAKE.read_text(encoding="utf-8")
        self.assertIn("resources/control_room.html", cmake)
        self.assertIn("tools/generate_ui_app.py", cmake)
        self.assertIn("${CONTROL_ROOM_GENERATED_SOURCE}", cmake)
        depends = re.search(
            r"DEPENDS[^\n]*generate_ui_app\.py[^\n]*CONTROL_ROOM_PAGE", cmake
        )
        self.assertIsNotNone(depends, "the codegen must depend on both inputs")


class Accessibility(unittest.TestCase):
    def test_the_page_declares_a_language_and_a_charset(self):
        text = PAGE.read_text(encoding="utf-8")
        self.assertIn('<html lang="en">', text)
        self.assertIn('<meta charset="utf-8">', text)

    def test_theme_variables_have_fallbacks(self):
        # The host may supply none of its variables. Every var() the page reads
        # from the host must therefore carry a fallback, or the page renders
        # invisible text on an invisible background.
        text = PAGE.read_text(encoding="utf-8")
        for match in re.finditer(r"var\((--color-[a-z-]+|--font-[a-z-]+)([^)]*)\)", text):
            self.assertTrue(
                match.group(2).strip().startswith(","),
                f"{match.group(0)} has no fallback",
            )


if __name__ == "__main__":
    unittest.main()
