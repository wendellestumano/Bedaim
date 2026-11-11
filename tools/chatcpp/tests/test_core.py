import tempfile
import unittest
from pathlib import Path

from chatcpp.config import Settings
from chatcpp.scanner import scan_repository
from chatcpp.store import MemoryStore


class ChatCppCoreTests(unittest.TestCase):
    def test_memory_roundtrip_and_search(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = MemoryStore(Path(tmp) / "memory.sqlite3")
            entry_id = store.add_memory("decision", "AimAssist baseline", "Keep the last known working baseline isolated.", ["aimassist", "baseline"])
            results = store.search_memory("AimAssist")
            self.assertEqual(results[0]["id"], entry_id)

    def test_duplicate_finding_is_deduplicated(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = MemoryStore(Path(tmp) / "memory.sqlite3")
            finding = {"fingerprint": "abc", "severity": "low", "area": "maintenance", "title": "TODO", "details": "TODO later"}
            self.assertTrue(store.add_finding(finding))
            self.assertFalse(store.add_finding(finding))

    def test_scanner_is_read_only_and_finds_markers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / "sample.cpp"
            source.write_text("// TODO: validate FOV\nint main() { return 0; }\n", encoding="utf-8")
            report = scan_repository(root)
            self.assertEqual(report["files_scanned"], 1)
            self.assertEqual(len(report["findings"]), 1)
            self.assertTrue(source.exists())
            self.assertEqual(report["findings"][0]["line_number"], 1)

    def test_priority_and_experiment_learning(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = MemoryStore(Path(tmp) / "memory.sqlite3")
            store.set_priority("AimAssist", 10)
            self.assertEqual(store.list_priorities()["aimassist"], 10)
            experiment_id = store.add_experiment("Hypothesis", "Try patch", "failed", "compiler output", "This approach does not compile")
            self.assertTrue(experiment_id)
            self.assertEqual(store.search_memory("approach")[0]["kind"], "lesson")

    def test_settings_load_without_config(self):
        settings = Settings.load()
        self.assertEqual(settings.interval_minutes, 30)
        self.assertEqual(settings.model_provider, "disabled")

    def test_state_snapshot_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = MemoryStore(root / "first.sqlite3")
            first.add_memory("decision", "Known baseline", "AimAssist baseline verified", ["aimassist"])
            first.add_task("Review BlockESP", area="blockesp", priority=8)
            snapshot = root / "state.json"
            first.export_state(snapshot)
            second = MemoryStore(root / "second.sqlite3")
            imported = second.import_state(snapshot)
            self.assertGreaterEqual(imported, 2)
            self.assertEqual(second.search_memory("baseline")[0]["title"], "Known baseline")
            self.assertEqual(second.list_tasks()[0]["area"], "blockesp")

    def test_cycle_due_without_history(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = MemoryStore(Path(tmp) / "memory.sqlite3")
            self.assertTrue(store.cycle_due(30))

    def test_settings_reject_invalid_interval(self):
        settings = Settings(interval_minutes=7)
        with self.assertRaises(ValueError):
            settings.validate()


if __name__ == "__main__":
    unittest.main()
