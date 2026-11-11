"""Configuration loading and validation for ChatC++."""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

DEFAULT_PRIORITIES = {
    "stability": 10,
    "aimassist": 9,
    "blockesp": 8,
    "oreesp": 8,
    "hitbox": 7,
    "player_radar": 6,
    "hud": 5,
    "documentation": 4,
    "tooling": 7,
}


@dataclass(slots=True)
class Settings:
    workspace: Path = Path.cwd()
    database: Path = Path.home() / ".local/share/chatcpp/bedaim.sqlite3"
    interval_minutes: int = 30
    continuous: bool = False
    max_cycles_per_process: int = 1
    max_files_per_scan: int = 20_000
    max_file_bytes: int = 2_000_000
    monthly_budget_brl: float = 0.0
    daily_model_call_limit: int = 30
    model_provider: str = "disabled"
    model_name: str = ""
    priorities: dict[str, int] = field(default_factory=lambda: dict(DEFAULT_PRIORITIES))

    @classmethod
    def load(cls, path: Path | None = None) -> "Settings":
        raw: dict[str, Any] = {}
        if path and path.exists():
            raw = json.loads(path.read_text(encoding="utf-8"))
        settings = cls(
            workspace=Path(os.getenv("CHATCPP_WORKSPACE", raw.get("workspace", str(Path.cwd())))).expanduser(),
            database=Path(os.getenv("CHATCPP_DATABASE", raw.get("database", str(Path.home() / ".local/share/chatcpp/bedaim.sqlite3")))).expanduser(),
            interval_minutes=int(os.getenv("CHATCPP_INTERVAL_MINUTES", raw.get("interval_minutes", 30))),
            continuous=str(os.getenv("CHATCPP_CONTINUOUS", raw.get("continuous", False))).lower() in {"1", "true", "yes"},
            max_cycles_per_process=int(raw.get("max_cycles_per_process", 1)),
            max_files_per_scan=int(raw.get("max_files_per_scan", 20_000)),
            max_file_bytes=int(raw.get("max_file_bytes", 2_000_000)),
            monthly_budget_brl=float(raw.get("monthly_budget_brl", 0.0)),
            daily_model_call_limit=int(raw.get("daily_model_call_limit", 30)),
            model_provider=str(raw.get("model_provider", "disabled")),
            model_name=str(raw.get("model_name", "")),
            priorities={**DEFAULT_PRIORITIES, **raw.get("priorities", {})},
        )
        settings.validate()
        return settings

    def validate(self) -> None:
        if self.interval_minutes not in {5, 10, 15, 30, 60, 180, 360, 720, 1440}:
            raise ValueError("interval_minutes must be one of 5, 10, 15, 30, 60, 180, 360, 720, 1440")
        if self.max_cycles_per_process < 1:
            raise ValueError("max_cycles_per_process must be >= 1")
        if self.max_files_per_scan < 1 or self.max_file_bytes < 1:
            raise ValueError("scan limits must be positive")
        if self.monthly_budget_brl < 0:
            raise ValueError("monthly_budget_brl cannot be negative")
        if self.daily_model_call_limit < 0:
            raise ValueError("daily_model_call_limit cannot be negative")
        for key, value in self.priorities.items():
            if not isinstance(value, int) or not 1 <= value <= 10:
                raise ValueError(f"priority {key!r} must be an integer from 1 to 10")
