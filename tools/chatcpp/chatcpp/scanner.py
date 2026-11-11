"""Conservative, read-only repository inventory and marker scanner."""
from __future__ import annotations

import hashlib
import subprocess
from collections import Counter
from pathlib import Path
from typing import Any, Iterator

SKIP_DIRS = {".git", ".venv", "venv", "node_modules", "build", "dist", "out", ".cache", ".gradle", "__pycache__", ".chatcpp"}
SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm", ".java", ".py", ".cmake", ".lua"}
TEXT_EXTENSIONS = SOURCE_EXTENSIONS | {".md", ".txt", ".json", ".xml", ".yml", ".yaml", ".toml", ".ini", ".gradle", ".properties"}
MARKERS = ("TODO", "FIXME", "HACK", "XXX")


def iter_files(root: Path, max_files: int) -> Iterator[Path]:
    count = 0
    for path in root.rglob("*"):
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        if path.is_file() and path.suffix.lower() in TEXT_EXTENSIONS:
            yield path
            count += 1
            if count >= max_files:
                return


def git_info(root: Path) -> dict[str, str | None]:
    def run(*args: str) -> str | None:
        try:
            result = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, timeout=5, check=False)
            return result.stdout.strip() if result.returncode == 0 else None
        except (OSError, subprocess.TimeoutExpired):
            return None
    return {"branch": run("branch", "--show-current"), "commit": run("rev-parse", "--short", "HEAD"), "status": "clean" if run("status", "--porcelain") == "" else "dirty"}


def scan_repository(root: Path, max_files: int = 20_000, max_file_bytes: int = 2_000_000) -> dict[str, Any]:
    root = root.resolve()
    if not root.is_dir():
        raise NotADirectoryError(root)
    languages: Counter[str] = Counter()
    findings: list[dict[str, Any]] = []
    files_scanned = total_bytes = 0
    for path in iter_files(root, max_files):
        try:
            size = path.stat().st_size
            if size > max_file_bytes:
                continue
            content = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        files_scanned += 1
        total_bytes += size
        ext = path.suffix.lower() or "[no extension]"
        languages[ext] += 1
        if ext not in SOURCE_EXTENSIONS:
            continue
        relative = path.relative_to(root).as_posix()
        for line_number, line in enumerate(content.splitlines(), start=1):
            stripped = line.strip()
            for marker in MARKERS:
                if marker in stripped:
                    fingerprint = hashlib.sha256(f"{relative}:{line_number}:{marker}:{stripped}".encode()).hexdigest()
                    findings.append({
                        "fingerprint": fingerprint,
                        "severity": "low",
                        "area": "code-maintenance",
                        "title": f"{marker} marker in {relative}:{line_number}",
                        "details": stripped[:500],
                        "file_path": relative,
                        "line_number": line_number,
                        "source": "static-marker-scanner",
                    })
                    break
    return {
        "workspace": str(root),
        "files_scanned": files_scanned,
        "total_text_bytes": total_bytes,
        "extensions": dict(languages.most_common()),
        "findings": findings,
        "git": git_info(root),
        "limitations": [
            "This scan detects repository inventory and TODO/FIXME/HACK/XXX markers only.",
            "It does not prove the presence or absence of C++ defects.",
            "No source files are modified.",
        ],
    }
