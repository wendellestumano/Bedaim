import unittest

import aim_trace_analyzer as analyzer


class AimTraceAnalyzerTests(unittest.TestCase):
    def test_wrap_degrees_shortest_turn(self):
        self.assertAlmostEqual(analyzer.wrap_degrees(-179.0 - 179.0), 2.0)

    def test_metrics_and_target_switches(self):
        rows = [
            {"timestamp_ms": 0.0, "yaw": 179.0, "pitch": 8.0, "target_id": "a", "target_yaw": -179.0, "target_pitch": 10.0},
            {"timestamp_ms": 50.0, "yaw": -179.0, "pitch": 8.5, "target_id": "a", "target_yaw": -178.0, "target_pitch": 10.0},
            {"timestamp_ms": 100.0, "yaw": -176.0, "pitch": 9.0, "target_id": "b", "target_yaw": -175.0, "target_pitch": 10.0},
        ]
        frames, summary = analyzer.analyze(rows, 250.0)
        self.assertEqual(summary["target_switches"], 1)
        self.assertAlmostEqual(frames[1]["yaw_delta_deg"], 2.0)
        self.assertEqual(summary["angular_error_deg"]["samples"], 3)

    def test_reject_duplicate_timestamps(self):
        rows = [
            {"timestamp_ms": 10.0, "yaw": 0.0, "pitch": 0.0, "target_id": "", "target_yaw": None, "target_pitch": None},
            {"timestamp_ms": 10.0, "yaw": 1.0, "pitch": 1.0, "target_id": "", "target_yaw": None, "target_pitch": None},
        ]
        with self.assertRaisesRegex(ValueError, "estritamente crescente"):
            analyzer.analyze(rows, 250.0)


if __name__ == "__main__":
    unittest.main()
