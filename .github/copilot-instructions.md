# GitHub Copilot instructions

Read and follow the repository-root `AGENTS.md` before planning or editing. It is the shared source of truth for Copilot, Codex, and other agents.

Priorities:
- Inspect current source, Git history, CI, diagnostics, and related issues before changing code.
- Work only on a dedicated branch; never commit to or merge into `main`.
- Do not invent native APIs, offsets, hook targets, test results, or build success.
- Preserve unrelated modules and the current working baseline; do not resurrect retired AutoHit/AutoTotem code without an explicit task.
- Run relevant tests/builds where available and distinguish compile-time evidence from in-game validation.
- Update `CHANGELOG.md` for material changes and leave a complete handoff in `docs/agent-collaboration/`.
- Never expose secrets or make destructive repository/settings changes.
