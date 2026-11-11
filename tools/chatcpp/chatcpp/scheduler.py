"""Bounded inspection cycles. Continuous mode is explicit and interruptible."""
from __future__ import annotations

import json
import signal
from threading import Event
from typing import Any

from .config import Settings
from .scanner import scan_repository
from .store import MemoryStore


class Scheduler:
    def __init__(self, settings: Settings, store: MemoryStore):
        self.settings = settings
        self.store = store
        self.stop_event = Event()

    def stop(self, *_: Any) -> None:
        self.stop_event.set()

    def run_cycle(self) -> dict[str, Any]:
        cycle_id = self.store.start_cycle()
        try:
            report = scan_repository(self.settings.workspace, self.settings.max_files_per_scan, self.settings.max_file_bytes)
            created = sum(1 for finding in report["findings"] if self.store.add_finding(finding))
            summary = f"Scanned {report['files_scanned']} files; discovered {len(report['findings'])} maintenance markers; {created} new findings."
            self.store.finish_cycle(cycle_id, "completed", summary, report["files_scanned"], created)
            self.store.add_memory("scan_report", f"Repository scan {cycle_id}", json.dumps(report, ensure_ascii=False), ["scan", "inventory", "bedaim"], report.get("git", {}).get("commit"))
            report["cycle_id"] = cycle_id
            report["new_findings"] = created
            report["summary"] = summary
            return report
        except Exception as exc:
            self.store.finish_cycle(cycle_id, "failed", f"{type(exc).__name__}: {exc}", 0, 0)
            self.store.add_memory("failure", f"Failed scan {cycle_id}", f"{type(exc).__name__}: {exc}", ["scheduler", "failure"], cycle_id, 1.0)
            raise

    def run(self, once: bool = True, interval_minutes: int | None = None, max_cycles: int | None = None) -> int:
        interval = interval_minutes or self.settings.interval_minutes
        if interval < 1:
            raise ValueError("interval must be >= 1 minute")
        if once:
            report = self.run_cycle()
            print(json.dumps(report, ensure_ascii=False, indent=2))
            return 0
        signal.signal(signal.SIGINT, self.stop)
        signal.signal(signal.SIGTERM, self.stop)
        cycles = 0
        while not self.stop_event.is_set() and (max_cycles is None or cycles < max_cycles):
            report = self.run_cycle()
            cycles += 1
            print(report["summary"], flush=True)
            if max_cycles is not None and cycles >= max_cycles:
                break
            self.stop_event.wait(interval * 60)
        return 0
