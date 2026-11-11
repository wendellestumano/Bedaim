"""Command-line interface for ChatC++ Core V0.1."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from .config import Settings
from .scheduler import Scheduler
from .scanner import scan_repository
from .store import MemoryStore


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="chatcpp", description="BedAim engineering intelligence core")
    parser.add_argument("--config", type=Path, help="Path to ChatC++ JSON configuration")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("doctor", help="Validate environment and configuration")
    sub.add_parser("status", help="Show memory and queue counters")
    sub.add_parser("scan", help="Run a conservative read-only repository scan")
    cycle = sub.add_parser("cycle", help="Run one inspection cycle or an explicit loop")
    cycle.add_argument("--loop", action="store_true", help="Keep running until interrupted")
    cycle.add_argument("--interval", type=int, help="Minutes between cycles")
    cycle.add_argument("--max-cycles", type=int, help="Stop after this many cycles")
    cycle.add_argument("--if-due", action="store_true", help="Skip unless the configured interval has elapsed")
    memory = sub.add_parser("memory", help="Manage persistent project memory")
    memory_sub = memory.add_subparsers(dest="memory_command", required=True)
    add = memory_sub.add_parser("add")
    add.add_argument("title")
    add.add_argument("content")
    add.add_argument("--kind", default="note")
    add.add_argument("--tag", action="append", default=[])
    search = memory_sub.add_parser("search")
    search.add_argument("query")
    search.add_argument("--limit", type=int, default=10)
    export = memory_sub.add_parser("export")
    export.add_argument("destination", type=Path)
    task = sub.add_parser("task", help="Manage engineering tasks")
    task_sub = task.add_subparsers(dest="task_command", required=True)
    task_add = task_sub.add_parser("add")
    task_add.add_argument("title")
    task_add.add_argument("--description", default="")
    task_add.add_argument("--area", default="general")
    task_add.add_argument("--priority", type=int, default=None)
    task_sub.add_parser("list")
    findings = sub.add_parser("findings", help="List open scanner findings")
    findings.add_argument("--limit", type=int, default=100)
    priority = sub.add_parser("priority", help="Configure per-area priority weights")
    priority_sub = priority.add_subparsers(dest="priority_command", required=True)
    priority_set = priority_sub.add_parser("set")
    priority_set.add_argument("area")
    priority_set.add_argument("weight", type=int)
    priority_sub.add_parser("list")
    experiment = sub.add_parser("experiment", help="Record an experiment and lessons learned")
    experiment_sub = experiment.add_subparsers(dest="experiment_command", required=True)
    experiment_add = experiment_sub.add_parser("add")
    experiment_add.add_argument("hypothesis")
    experiment_add.add_argument("action")
    experiment_add.add_argument("outcome")
    experiment_add.add_argument("--evidence", default="")
    experiment_add.add_argument("--learned", default="")
    experiment_add.add_argument("--task-id")
    experiment_sub.add_parser("list")
    export_state = sub.add_parser("export-state", help="Export all persistent state to JSON")
    export_state.add_argument("destination", type=Path)
    import_state = sub.add_parser("import-state", help="Merge a JSON state snapshot into the local database")
    import_state.add_argument("source", type=Path)
    return parser


def main() -> None:
    args = build_parser().parse_args()
    settings = Settings.load(args.config)
    store = MemoryStore(settings.database)
    if args.command == "doctor":
        report = {"python_ok": True, "python_version": __import__("sys").version.split()[0], "workspace_exists": settings.workspace.is_dir(), "workspace": str(settings.workspace), "database": str(settings.database), "database_exists": settings.database.exists(), "model_provider": settings.model_provider, "model_calls_enabled": settings.model_provider != "disabled", "monthly_budget_brl": settings.monthly_budget_brl, "status": "ready" if settings.workspace.is_dir() else "workspace-missing"}
        print(json.dumps(report, ensure_ascii=False, indent=2))
    elif args.command == "status":
        print(json.dumps(store.status(), ensure_ascii=False, indent=2))
    elif args.command == "scan":
        report = scan_repository(settings.workspace, settings.max_files_per_scan, settings.max_file_bytes)
        created = sum(1 for finding in report["findings"] if store.add_finding(finding))
        report["new_findings"] = created
        print(json.dumps(report, ensure_ascii=False, indent=2))
    elif args.command == "cycle":
        if args.if_due and not store.cycle_due(args.interval or settings.interval_minutes):
            print("Inspection skipped: configured interval has not elapsed.")
            return
        loop_requested = args.loop or settings.continuous
        Scheduler(settings, store).run(once=not loop_requested, interval_minutes=args.interval, max_cycles=args.max_cycles)
    elif args.command == "memory":
        if args.memory_command == "add":
            print(store.add_memory(args.kind, args.title, args.content, args.tag))
        elif args.memory_command == "search":
            print(json.dumps(store.search_memory(args.query, args.limit), ensure_ascii=False, indent=2))
        else:
            print(f"Exported {store.export_memory(args.destination)} memory entries to {args.destination}")
    elif args.command == "task":
        if args.task_command == "add":
            configured_priority = store.list_priorities().get(args.area.lower(), settings.priorities.get(args.area.lower(), 5))
            priority = args.priority if args.priority is not None else configured_priority
            print(store.add_task(args.title, args.description, args.area, priority))
        else:
            print(json.dumps(store.list_tasks(), ensure_ascii=False, indent=2))
    elif args.command == "findings":
        print(json.dumps(store.list_findings(args.limit), ensure_ascii=False, indent=2))
    elif args.command == "priority":
        if args.priority_command == "set":
            store.set_priority(args.area, args.weight)
            print(f"Priority for {args.area.lower()} set to {args.weight}/10")
        else:
            merged = {**settings.priorities, **store.list_priorities()}
            print(json.dumps(merged, ensure_ascii=False, indent=2))
    elif args.command == "experiment":
        if args.experiment_command == "add":
            print(store.add_experiment(args.hypothesis, args.action, args.outcome, args.evidence, args.learned, args.task_id))
        else:
            print(json.dumps(store.list_experiments(), ensure_ascii=False, indent=2))
    elif args.command == "export-state":
        print(f"Exported {store.export_state(args.destination)} records to {args.destination}")
    elif args.command == "import-state":
        print(f"Imported {store.import_state(args.source)} records from {args.source}")


if __name__ == "__main__":
    main()
