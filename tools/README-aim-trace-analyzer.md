# Aim trace analyzer (offline)

Small, dependency-free Python utility for inspecting camera traces collected in a private test world. It reports descriptive measurements to help compare legitimate play sessions, scripted test inputs, and anti-cheat telemetry. It does **not** classify players or produce a cheat verdict.

## CSV format

Required columns:

```csv
timestamp_ms,yaw,pitch
0,179,8
50,-179,8.5
100,-176,9
```

Optional columns:

- `target_id`: server-side test entity/player identifier. Use pseudonymous IDs in shared traces.
- `target_yaw,target_pitch`: target's angular direction from the observer for the same sample. Supply both columns and leave both empty when unavailable.
- `source`: a label such as `manual`, `scripted_test`, or `replay`.
- `session`: a pseudonymous session identifier.

Yaw wrap-around is normalized, so moving from `179` to `-179` degrees is treated as a 2-degree turn, not a 358-degree turn.

## Run

Requires Python 3.10+; no pip installs required.

```bash
python tools/aim_trace_analyzer.py traces/session.csv --out results/session
```

Creates `results/session_frames.csv` with per-sample deltas, speed, acceleration, jerk, and (when target angles are present) angular error, plus `results/session_summary.json` with aggregate statistics. `--max-gap-ms 250` (default) prevents derivatives from being computed across long gaps such as pauses or dropped packets.

## Reading results responsibly

Compare multiple labeled sessions and retain benign cases such as high-sensitivity mouse/touch input, rapid turns, lag, spectating, and target occlusion. These are descriptive statistics, not a standalone detection system; use server-side evidence, repeatable tests, and false-positive review before acting on a session.
