"""The signal bridge is admitted to production; its test seams are not.

One macro used to control both, so the feature could not be admitted without
also compiling failure-injection seams into a shipping binary. These assert the
two are now separate, and that the seam configurator stays behind the gate --
that is the property a release depends on.
"""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "gdextension" / "editor_hook.cpp"
SIGNAL_METHODS = (
    '"signal.listConnections"',
    '"signal.connect"',
    '"signal.disconnect"',
    '"signal.emit"',
)
SEAM_GUARD = "#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)"
SEAM_CONFIGURATOR = '"phase7SignalTest.configure"'


def _live_bridge_set(source: str) -> str:
    start = source.index("static const std::unordered_set<std::string> live_bridge_methods")
    return source[start:source.index("};", start)]


class Phase7SignalAdmissionSourceTests(unittest.TestCase):
    def setUp(self):
        self.source = SOURCE.read_text(encoding="utf-8")

    def test_signal_methods_are_admitted_unconditionally(self):
        # They must sit before the seam guard, or admission would once again
        # depend on compiling test seams into the binary users run.
        live_set = _live_bridge_set(self.source)
        production = live_set.split(SEAM_GUARD, 1)[0]
        for method in SIGNAL_METHODS:
            self.assertIn(method, production)

    def test_the_seam_configurator_never_reaches_production(self):
        # The safety-critical half. A shipping binary that can be told to force
        # its own failure paths is not a shipping binary.
        live_set = _live_bridge_set(self.source)
        self.assertIn(SEAM_GUARD, live_set)
        production, seam_only = live_set.split(SEAM_GUARD, 1)
        self.assertNotIn(SEAM_CONFIGURATOR, production)
        self.assertIn(SEAM_CONFIGURATOR, seam_only)

    def test_signal_methods_are_no_longer_listed_as_unimplemented(self):
        start = self.source.index(
            "static const std::unordered_set<std::string> registered_but_unimplemented"
        )
        compatibility_set = self.source[start:self.source.index("};", start)]
        for method in SIGNAL_METHODS:
            self.assertNotIn(method, compatibility_set)


if __name__ == "__main__":
    unittest.main()
