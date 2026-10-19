# AutoHit engineering memory

This directory is the durable engineering log for AutoHit work. Update it after every source change and test so experiments do not repeat old mistakes.

## Rules

- Work only on an explicit experimental branch; never push AutoHit experiments to `main` without a separately reviewed decision.
- Keep hypotheses, observations, and conclusions separate.
- Build success means source integration/package generation passed. It does **not** mean attacks cause server-confirmed damage.
- Record the exact branch, commit SHA, CI run, artifact id/name, game APK/build hash, device/runtime, test setup, and observed result.
- Keep useful failed hypotheses. Do not describe a hypothesis as a fix until the runtime observation supports it.
- Before changing native ARM64 dispatch, classify the exact live vtable entry's ABI and target. Never infer identity from a signature match or a string alone.
- Separate visual swing/particles, native wrapper return value, client-side hurt time, server-side damage, and target/camera tracking in logs.

## Current focus

See [SESSION-2026-10-19.md](SESSION-2026-10-19.md) for the latest evidence, risks, and prioritized next steps.
