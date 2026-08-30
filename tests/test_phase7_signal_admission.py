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


class Phase7SignalAdmissionSourceTests(unittest.TestCase):
    def test_raw_signal_methods_are_admitted_only_by_test_seam(self):
        source = SOURCE.read_text(encoding="utf-8")
        start = source.index("static const std::unordered_set<std::string> live_bridge_methods")
        end = source.index("};", start)
        live_set = source[start:end]
        seam = "#if defined(DIDI_PHASE7_SIGNAL_TEST_SEAMS)"
        self.assertIn(seam, live_set)
        production, seam_only = live_set.split(seam, 1)
        for method in SIGNAL_METHODS:
            self.assertNotIn(method, production)
            self.assertIn(method, seam_only)

    def test_raw_signal_methods_remain_registered_but_unimplemented(self):
        source = SOURCE.read_text(encoding="utf-8")
        start = source.index("static const std::unordered_set<std::string> registered_but_unimplemented")
        end = source.index("};", start)
        compatibility_set = source[start:end]
        for method in SIGNAL_METHODS:
            self.assertIn(method, compatibility_set)


if __name__ == "__main__":
    unittest.main()
