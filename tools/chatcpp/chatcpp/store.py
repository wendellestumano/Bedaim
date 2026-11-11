"""SQLite-backed durable local memory, task queue, findings and audit log."""
from __future__ import annotations

import json
import sqlite3
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def utcnow() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


SCHEMA = """
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS tasks (
  id TEXT PRIMARY KEY, title TEXT NOT NULL, description TEXT NOT NULL DEFAULT '',
  area TEXT NOT NULL DEFAULT 'general', priority INTEGER NOT NULL DEFAULT 5,
  status TEXT NOT NULL DEFAULT 'queued', attempts INTEGER NOT NULL DEFAULT 0,
  max_attempts INTEGER NOT NULL DEFAULT 3, branch TEXT, result_json TEXT,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_tasks_status_priority ON tasks(status, priority DESC, created_at);
CREATE TABLE IF NOT EXISTS findings (
  id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL UNIQUE, severity TEXT NOT NULL,
  area TEXT NOT NULL, title TEXT NOT NULL, details TEXT NOT NULL,
  file_path TEXT, line_number INTEGER, source TEXT NOT NULL DEFAULT 'static-scanner',
  status TEXT NOT NULL DEFAULT 'open', created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS memory_entries (
  id TEXT PRIMARY KEY, kind TEXT NOT NULL, title TEXT NOT NULL, content TEXT NOT NULL,
  tags TEXT NOT NULL DEFAULT '[]', source_ref TEXT, confidence REAL NOT NULL DEFAULT 1.0,
  created_at TEXT NOT NULL, updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS experiments (
  id TEXT PRIMARY KEY, task_id TEXT, hypothesis TEXT NOT NULL, action TEXT NOT NULL,
  outcome TEXT NOT NULL, evidence TEXT NOT NULL DEFAULT '', learned TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL,
  FOREIGN KEY(task_id) REFERENCES tasks(id) ON DELETE SET NULL
);
CREATE TABLE IF NOT EXISTS cycles (
  id TEXT PRIMARY KEY, started_at TEXT NOT NULL, finished_at TEXT,
  status TEXT NOT NULL, summary TEXT NOT NULL DEFAULT '', files_scanned INTEGER NOT NULL DEFAULT 0,
  findings_created INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS priorities (
  area TEXT PRIMARY KEY, weight INTEGER NOT NULL CHECK(weight BETWEEN 1 AND 10), updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS audit_events (
  id TEXT PRIMARY KEY, timestamp TEXT NOT NULL, actor TEXT NOT NULL,
  action TEXT NOT NULL, details_json TEXT NOT NULL DEFAULT '{}'
);
"""


class MemoryStore:
    def __init__(self, database: Path):
        self.database = database.expanduser()
        self.database.parent.mkdir(parents=True, exist_ok=True)
        self._initialize()

    def connect(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.database, timeout=30)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA busy_timeout = 5000")
        db.execute("PRAGMA foreign_keys = ON")
        return db

    def _initialize(self) -> None:
        with self.connect() as db:
            db.executescript(SCHEMA)
            try:
                db.execute("CREATE VIRTUAL TABLE IF NOT EXISTS memory_fts USING fts5(id UNINDEXED, title, content, tags)")
                db.execute("INSERT INTO memory_fts(id,title,content,tags) SELECT id,title,content,tags FROM memory_entries WHERE id NOT IN (SELECT id FROM memory_fts)")
            except sqlite3.OperationalError:
                pass

    def audit(self, actor: str, action: str, details: dict[str, Any] | None = None) -> None:
        with self.connect() as db:
            db.execute("INSERT INTO audit_events VALUES(?,?,?,?,?)", (str(uuid.uuid4()), utcnow(), actor, action, json.dumps(details or {}, ensure_ascii=False)))

    def add_memory(self, kind: str, title: str, content: str, tags: list[str] | None = None, source_ref: str | None = None, confidence: float = 1.0) -> str:
        if not 0.0 <= confidence <= 1.0:
            raise ValueError("confidence must be between 0 and 1")
        entry_id, now = str(uuid.uuid4()), utcnow()
        tag_json = json.dumps(tags or [], ensure_ascii=False)
        with self.connect() as db:
            db.execute("INSERT INTO memory_entries VALUES(?,?,?,?,?,?,?,?,?)", (entry_id, kind, title, content, tag_json, source_ref, confidence, now, now))
            try:
                db.execute("INSERT INTO memory_fts(id,title,content,tags) VALUES(?,?,?,?)", (entry_id, title, content, tag_json))
            except sqlite3.OperationalError:
                pass
        self.audit("system", "memory.add", {"id": entry_id, "kind": kind, "title": title})
        return entry_id

    def search_memory(self, query: str, limit: int = 10) -> list[dict[str, Any]]:
        if not query.strip():
            return []
        with self.connect() as db:
            try:
                rows = db.execute("SELECT m.* FROM memory_fts f JOIN memory_entries m ON m.id=f.id WHERE memory_fts MATCH ? ORDER BY rank LIMIT ?", (query, limit)).fetchall()
            except sqlite3.OperationalError:
                needle = f"%{query}%"
                rows = db.execute("SELECT * FROM memory_entries WHERE title LIKE ? OR content LIKE ? OR tags LIKE ? ORDER BY updated_at DESC LIMIT ?", (needle, needle, needle, limit)).fetchall()
        return [dict(row) for row in rows]

    def add_finding(self, finding: dict[str, Any]) -> bool:
        try:
            with self.connect() as db:
                db.execute("""INSERT INTO findings(id,fingerprint,severity,area,title,details,file_path,line_number,source,status,created_at)
                VALUES(?,?,?,?,?,?,?,?,?,'open',?)""", (str(uuid.uuid4()), finding["fingerprint"], finding["severity"], finding["area"], finding["title"], finding["details"], finding.get("file_path"), finding.get("line_number"), finding.get("source", "static-scanner"), utcnow()))
            return True
        except sqlite3.IntegrityError:
            return False

    def start_cycle(self) -> str:
        cycle_id = str(uuid.uuid4())
        with self.connect() as db:
            db.execute("INSERT INTO cycles(id,started_at,status) VALUES(?,?,?)", (cycle_id, utcnow(), "running"))
        return cycle_id

    def finish_cycle(self, cycle_id: str, status: str, summary: str, files_scanned: int, findings_created: int) -> None:
        with self.connect() as db:
            db.execute("UPDATE cycles SET finished_at=?,status=?,summary=?,files_scanned=?,findings_created=? WHERE id=?", (utcnow(), status, summary, files_scanned, findings_created, cycle_id))
        self.audit("scheduler", "cycle.finish", {"cycle_id": cycle_id, "status": status, "files_scanned": files_scanned, "findings_created": findings_created})

    def add_task(self, title: str, description: str = "", area: str = "general", priority: int = 5) -> str:
        if not 1 <= priority <= 10:
            raise ValueError("priority must be between 1 and 10")
        task_id, now = str(uuid.uuid4()), utcnow()
        with self.connect() as db:
            db.execute("INSERT INTO tasks(id,title,description,area,priority,created_at,updated_at) VALUES(?,?,?,?,?,?,?)", (task_id, title, description, area, priority, now, now))
        self.audit("user", "task.add", {"task_id": task_id, "title": title, "area": area, "priority": priority})
        return task_id

    def set_priority(self, area: str, weight: int) -> None:
        if not 1 <= weight <= 10:
            raise ValueError("priority must be between 1 and 10")
        with self.connect() as db:
            db.execute("INSERT INTO priorities(area,weight,updated_at) VALUES(?,?,?) ON CONFLICT(area) DO UPDATE SET weight=excluded.weight,updated_at=excluded.updated_at", (area.lower(), weight, utcnow()))
        self.audit("user", "priority.set", {"area": area.lower(), "weight": weight})

    def list_priorities(self) -> dict[str, int]:
        with self.connect() as db:
            rows = db.execute("SELECT area,weight FROM priorities ORDER BY area").fetchall()
        return {row["area"]: row["weight"] for row in rows}

    def add_experiment(self, hypothesis: str, action: str, outcome: str, evidence: str = "", learned: str = "", task_id: str | None = None) -> str:
        experiment_id = str(uuid.uuid4())
        with self.connect() as db:
            db.execute("INSERT INTO experiments(id,task_id,hypothesis,action,outcome,evidence,learned,created_at) VALUES(?,?,?,?,?,?,?,?)", (experiment_id, task_id, hypothesis, action, outcome, evidence, learned, utcnow()))
        self.audit("agent", "experiment.record", {"experiment_id": experiment_id, "outcome": outcome, "task_id": task_id})
        if learned.strip():
            self.add_memory("lesson", hypothesis, f"Action: {action}\nOutcome: {outcome}\nEvidence: {evidence}\nLesson: {learned}", ["experiment", "lesson", outcome.lower()])
        return experiment_id

    def list_experiments(self, limit: int = 50) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT * FROM experiments ORDER BY created_at DESC LIMIT ?", (limit,)).fetchall()
        return [dict(row) for row in rows]

    def list_tasks(self, status: str | None = None, limit: int = 50) -> list[dict[str, Any]]:
        with self.connect() as db:
            if status:
                rows = db.execute("SELECT * FROM tasks WHERE status=? ORDER BY priority DESC,created_at LIMIT ?", (status, limit)).fetchall()
            else:
                rows = db.execute("SELECT * FROM tasks ORDER BY priority DESC,created_at LIMIT ?", (limit,)).fetchall()
        return [dict(row) for row in rows]

    def list_findings(self, limit: int = 100) -> list[dict[str, Any]]:
        with self.connect() as db:
            rows = db.execute("SELECT * FROM findings WHERE status='open' ORDER BY CASE severity WHEN 'high' THEN 0 WHEN 'medium' THEN 1 ELSE 2 END,created_at DESC LIMIT ?", (limit,)).fetchall()
        return [dict(row) for row in rows]

    def status(self) -> dict[str, int]:
        with self.connect() as db:
            return {table: db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0] for table in ("tasks", "findings", "memory_entries", "experiments", "cycles")}

    def export_memory(self, destination: Path) -> int:
        destination.parent.mkdir(parents=True, exist_ok=True)
        with self.connect() as db, destination.open("w", encoding="utf-8") as out:
            rows = db.execute("SELECT * FROM memory_entries ORDER BY created_at").fetchall()
            for row in rows:
                out.write(json.dumps(dict(row), ensure_ascii=False) + "\n")
        return len(rows)

    def cycle_due(self, interval_minutes: int) -> bool:
        from datetime import timedelta
        with self.connect() as db:
            row = db.execute("SELECT finished_at FROM cycles WHERE status='completed' AND finished_at IS NOT NULL ORDER BY finished_at DESC LIMIT 1").fetchone()
        if not row:
            return True
        last = datetime.fromisoformat(row["finished_at"])
        return datetime.now(timezone.utc) - last >= timedelta(minutes=interval_minutes)

    def export_state(self, destination: Path) -> int:
        tables = ("tasks", "findings", "memory_entries", "experiments", "cycles", "priorities", "audit_events")
        destination.parent.mkdir(parents=True, exist_ok=True)
        snapshot: dict[str, Any] = {"schema_version": 1, "exported_at": utcnow(), "tables": {}}
        with self.connect() as db:
            for table in tables:
                snapshot["tables"][table] = [dict(row) for row in db.execute(f"SELECT * FROM {table}").fetchall()]
        destination.write_text(json.dumps(snapshot, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return sum(len(rows) for rows in snapshot["tables"].values())

    def import_state(self, source: Path) -> int:
        snapshot = json.loads(source.read_text(encoding="utf-8"))
        if snapshot.get("schema_version") != 1 or not isinstance(snapshot.get("tables"), dict):
            raise ValueError("Unsupported or invalid ChatC++ state snapshot")
        tables = ("tasks", "findings", "memory_entries", "experiments", "cycles", "priorities", "audit_events")
        allowed_columns = {
            "tasks": {"id", "title", "description", "area", "priority", "status", "attempts", "max_attempts", "branch", "result_json", "created_at", "updated_at"},
            "findings": {"id", "fingerprint", "severity", "area", "title", "details", "file_path", "line_number", "source", "status", "created_at"},
            "memory_entries": {"id", "kind", "title", "content", "tags", "source_ref", "confidence", "created_at", "updated_at"},
            "experiments": {"id", "task_id", "hypothesis", "action", "outcome", "evidence", "learned", "created_at"},
            "cycles": {"id", "started_at", "finished_at", "status", "summary", "files_scanned", "findings_created"},
            "priorities": {"area", "weight", "updated_at"},
            "audit_events": {"id", "timestamp", "actor", "action", "details_json"},
        }
        imported = 0
        with self.connect() as db:
            for table in tables:
                for row in snapshot["tables"].get(table, []):
                    if not isinstance(row, dict):
                        continue
                    columns = [key for key in row if key in allowed_columns[table]]
                    if not columns:
                        continue
                    placeholders = ",".join("?" for _ in columns)
                    sql = f"INSERT OR REPLACE INTO {table} ({','.join(columns)}) VALUES ({placeholders})"
                    db.execute(sql, [row[column] for column in columns])
                    imported += 1
            try:
                db.execute("DELETE FROM memory_fts")
                db.execute("INSERT INTO memory_fts(id,title,content,tags) SELECT id,title,content,tags FROM memory_entries")
            except sqlite3.OperationalError:
                pass
        self.audit("system", "state.import", {"source": str(source), "rows": imported})
        return imported
