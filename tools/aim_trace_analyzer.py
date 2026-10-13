#!/usr/bin/env python3
"""Offline descriptive analysis for Minecraft camera/target trace CSVs.

Required CSV columns: timestamp_ms,yaw,pitch
Optional columns: target_id,target_yaw,target_pitch,session,source
Uses only Python's standard library; deliberately reports measurements rather than
classifying a trace as cheating or recommending thresholds.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from pathlib import Path
from typing import Any

REQUIRED = ("timestamp_ms", "yaw", "pitch")


def wrap_degrees(value: float) -> float:
    """Normalize an angular difference to [-180, 180)."""
    return (value + 180.0) % 360.0 - 180.0


def percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * p
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def mean_or_none(values: list[float]) -> float | None:
    return statistics.fmean(values) if values else None


def median_or_none(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def as_float(row: dict[str, str], key: str, row_number: int) -> float:
    try:
        value = float(row[key])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f"linha {row_number}: '{key}' precisa ser numérico") from exc
    if not math.isfinite(value):
        raise ValueError(f"linha {row_number}: '{key}' não pode ser NaN/inf")
    return value


def load_rows(path: Path) -> list[dict[str, Any]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise ValueError("CSV vazio ou sem cabeçalho")
        missing = [name for name in REQUIRED if name not in reader.fieldnames]
        if missing:
            raise ValueError("faltam colunas obrigatórias: " + ", ".join(missing))

        rows: list[dict[str, Any]] = []
        for line_no, source in enumerate(reader, start=2):
            if not any((value or "").strip() for value in source.values()):
                continue
            timestamp = as_float(source, "timestamp_ms", line_no)
            yaw = as_float(source, "yaw", line_no)
            pitch = as_float(source, "pitch", line_no)
            target_yaw = None
            target_pitch = None
            ty = (source.get("target_yaw") or "").strip()
            tp = (source.get("target_pitch") or "").strip()
            if ty and tp:
                target_yaw = as_float(source, "target_yaw", line_no)
                target_pitch = as_float(source, "target_pitch", line_no)
            elif ty or tp:
                raise ValueError(
                    f"linha {line_no}: informe target_yaw e target_pitch juntos, ou deixe ambos vazios"
                )

            rows.append({
                "timestamp_ms": timestamp,
                "yaw": wrap_degrees(yaw),
                "pitch": pitch,
                "target_id": (source.get("target_id") or "").strip(),
                "target_yaw": target_yaw,
                "target_pitch": target_pitch,
                "source": (source.get("source") or "").strip(),
                "session": (source.get("session") or "").strip(),
            })

    if len(rows) < 2:
        raise ValueError("são necessárias pelo menos duas linhas de amostra")
    rows.sort(key=lambda item: item["timestamp_ms"])
    return rows


def analyze(rows: list[dict[str, Any]], max_gap_ms: float) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    derived: list[dict[str, Any]] = []
    yaw_speeds: list[float] = []
    pitch_speeds: list[float] = []
    angular_speeds: list[float] = []
    angular_accels: list[float] = []
    jerks: list[float] = []
    errors: list[float] = []
    yaw_errors: list[float] = []
    pitch_errors: list[float] = []
    intervals: list[float] = []
    target_switches = 0
    target_observations = 0
    previous_target: str | None = None
    previous_velocity: tuple[float, float, float] | None = None
    previous_acceleration: tuple[float, float, float] | None = None

    for index, row in enumerate(rows):
        item = dict(row)
        item.update({
            "dt_ms": "", "yaw_delta_deg": "", "pitch_delta_deg": "",
            "yaw_speed_dps": "", "pitch_speed_dps": "", "angular_speed_dps": "",
            "angular_accel_dps2": "", "angular_jerk_dps3": "",
            "yaw_error_deg": "", "pitch_error_deg": "", "angular_error_deg": "",
        })

        current_target = row["target_id"]
        if current_target:
            target_observations += 1
            if previous_target is not None and current_target != previous_target:
                target_switches += 1
            previous_target = current_target

        if row["target_yaw"] is not None and row["target_pitch"] is not None:
            yaw_error = wrap_degrees(row["target_yaw"] - row["yaw"])
            pitch_error = row["target_pitch"] - row["pitch"]
            error = math.hypot(yaw_error, pitch_error)
            item["yaw_error_deg"] = yaw_error
            item["pitch_error_deg"] = pitch_error
            item["angular_error_deg"] = error
            yaw_errors.append(abs(yaw_error))
            pitch_errors.append(abs(pitch_error))
            errors.append(error)

        if index > 0:
            prev = rows[index - 1]
            dt_ms = row["timestamp_ms"] - prev["timestamp_ms"]
            if dt_ms <= 0:
                raise ValueError("timestamp_ms precisa ser estritamente crescente e sem duplicatas")
            dt_s = dt_ms / 1000.0
            item["dt_ms"] = dt_ms
            if dt_ms <= max_gap_ms:
                yaw_delta = wrap_degrees(row["yaw"] - prev["yaw"])
                pitch_delta = row["pitch"] - prev["pitch"]
                yaw_speed = yaw_delta / dt_s
                pitch_speed = pitch_delta / dt_s
                angular_speed = math.hypot(yaw_speed, pitch_speed)
                item.update({
                    "yaw_delta_deg": yaw_delta,
                    "pitch_delta_deg": pitch_delta,
                    "yaw_speed_dps": yaw_speed,
                    "pitch_speed_dps": pitch_speed,
                    "angular_speed_dps": angular_speed,
                })
                yaw_speeds.append(abs(yaw_speed))
                pitch_speeds.append(abs(pitch_speed))
                angular_speeds.append(angular_speed)
                intervals.append(dt_ms)

                velocity = (yaw_speed, pitch_speed, angular_speed)
                if previous_velocity is not None:
                    yaw_acc = (velocity[0] - previous_velocity[0]) / dt_s
                    pitch_acc = (velocity[1] - previous_velocity[1]) / dt_s
                    angular_acc = (velocity[2] - previous_velocity[2]) / dt_s
                    acceleration_mag = math.hypot(yaw_acc, pitch_acc)
                    item["angular_accel_dps2"] = acceleration_mag
                    angular_accels.append(acceleration_mag)
                    acceleration = (yaw_acc, pitch_acc, angular_acc)
                    if previous_acceleration is not None:
                        jerk_mag = math.sqrt(sum(
                            ((acceleration[i] - previous_acceleration[i]) / dt_s) ** 2
                            for i in range(3)
                        ))
                        item["angular_jerk_dps3"] = jerk_mag
                        jerks.append(jerk_mag)
                    previous_acceleration = acceleration
                previous_velocity = velocity
            else:
                # Avoid derivatives across pauses, app switches, or missing packet bursts.
                previous_velocity = None
                previous_acceleration = None

    duration_ms = rows[-1]["timestamp_ms"] - rows[0]["timestamp_ms"]
    summary = {
        "input_samples": len(rows),
        "duration_ms": round(duration_ms, 3),
        "median_sample_interval_ms": round(median_or_none(intervals), 3) if intervals else None,
        "derivative_gap_limit_ms": max_gap_ms,
        "target_id_observations": target_observations,
        "target_switches": target_switches,
        "target_switches_per_minute": round(target_switches / max(duration_ms / 60000.0, 1e-9), 3),
        "yaw_speed_abs_dps": {
            "mean": mean_or_none(yaw_speeds), "median": median_or_none(yaw_speeds),
            "p95": percentile(yaw_speeds, 0.95), "max": max(yaw_speeds) if yaw_speeds else None,
        },
        "pitch_speed_abs_dps": {
            "mean": mean_or_none(pitch_speeds), "median": median_or_none(pitch_speeds),
            "p95": percentile(pitch_speeds, 0.95), "max": max(pitch_speeds) if pitch_speeds else None,
        },
        "angular_speed_dps": {
            "mean": mean_or_none(angular_speeds), "median": median_or_none(angular_speeds),
            "p95": percentile(angular_speeds, 0.95), "max": max(angular_speeds) if angular_speeds else None,
        },
        "angular_acceleration_dps2": {
            "median": median_or_none(angular_accels), "p95": percentile(angular_accels, 0.95),
            "max": max(angular_accels) if angular_accels else None,
        },
        "angular_jerk_dps3": {
            "median": median_or_none(jerks), "p95": percentile(jerks, 0.95),
            "max": max(jerks) if jerks else None,
        },
        "absolute_yaw_error_deg": {
            "samples": len(yaw_errors), "mean": mean_or_none(yaw_errors),
            "median": median_or_none(yaw_errors), "p95": percentile(yaw_errors, 0.95),
        },
        "absolute_pitch_error_deg": {
            "samples": len(pitch_errors), "mean": mean_or_none(pitch_errors),
            "median": median_or_none(pitch_errors), "p95": percentile(pitch_errors, 0.95),
        },
        "angular_error_deg": {
            "samples": len(errors), "mean": mean_or_none(errors),
            "median": median_or_none(errors), "p95": percentile(errors, 0.95),
        },
        "notes": [
            "Estatísticas descritivas; nenhum limiar ou veredito automático de cheat é aplicado.",
            "target_switches conta mudanças entre IDs não vazios consecutivos; lacunas em target_id não viram troca.",
            "velocidade, aceleração e jerk ignoram intervalos maiores que derivative_gap_limit_ms.",
        ],
    }
    return derived, summary


def json_safe(obj: Any) -> Any:
    if isinstance(obj, float) and not math.isfinite(obj):
        return None
    if isinstance(obj, dict):
        return {key: json_safe(value) for key, value in obj.items()}
    if isinstance(obj, list):
        return [json_safe(value) for value in obj]
    return obj


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Mede características de uma gravação de câmera/alvo para análise offline de anti-cheat."
    )
    parser.add_argument("input_csv", type=Path, help="CSV com timestamp_ms,yaw,pitch")
    parser.add_argument("--out", type=Path, default=Path("aim_trace_analysis"),
                        help="prefixo dos arquivos de saída (padrão: aim_trace_analysis)")
    parser.add_argument("--max-gap-ms", type=float, default=250.0,
                        help="não calcular derivadas entre amostras separadas por mais que este intervalo")
    args = parser.parse_args()

    if args.max_gap_ms <= 0 or not math.isfinite(args.max_gap_ms):
        parser.error("--max-gap-ms precisa ser maior que zero")
    try:
        rows = load_rows(args.input_csv)
        derived, summary = analyze(rows, args.max_gap_ms)
        summary = json_safe(summary)
        detail_path = args.out.with_name(args.out.name + "_frames.csv")
        summary_path = args.out.with_name(args.out.name + "_summary.json")
        detail_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        columns = [
            "timestamp_ms", "yaw", "pitch", "target_id", "target_yaw", "target_pitch", "source", "session",
            "dt_ms", "yaw_delta_deg", "pitch_delta_deg", "yaw_speed_dps", "pitch_speed_dps",
            "angular_speed_dps", "angular_accel_dps2", "angular_jerk_dps3", "yaw_error_deg",
            "pitch_error_deg", "angular_error_deg",
        ]
        with detail_path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(derived)
        with summary_path.open("w", encoding="utf-8") as handle:
            json.dump(summary, handle, ensure_ascii=False, indent=2, allow_nan=False)
            handle.write("\n")
    except (OSError, ValueError) as exc:
        print(f"erro: {exc}", file=sys.stderr)
        return 2

    print(f"Amostras analisadas: {summary['input_samples']}")
    print(f"Duração: {summary['duration_ms']:.1f} ms")
    print(f"Trocas de alvo observadas: {summary['target_switches']}")
    print(f"CSV detalhado: {detail_path}")
    print(f"Resumo JSON: {summary_path}")
    print("Nota: os números descrevem a gravação; não classificam automaticamente um jogador.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
