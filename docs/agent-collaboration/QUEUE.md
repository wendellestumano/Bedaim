# Agent task queue

Use this file as a lightweight index, not as a substitute for GitHub Issues or PRs. Keep one owner per task and update the row before starting or handing off work.

| Status | Task / Issue | Owner agent | Branch / PR | Scope / conflicts | Next action |
|---|---|---|---|---|---|
| Ready | End-to-end custom-agent smoke test | Coordinator → Implementer → Reviewer | [Smoke-test prompt](../../.github/prompts/bedaim-agent-smoke-test.prompt.md) / PR #66 | Documentation-only; use isolated branch `agent/smoke-test-docs`; no gameplay/build changes | Run the prompt from BedAim Coordinator in the active Codespaces Copilot Chat; record whether handoffs actually work |
| Done | Audit current build/CI commands and artifact outputs | Repository-connected assistant | [Audit report](reports/initial-read-only-audit-2026-10-21.md) | Build/CI evidence recorded for SHA `b05ccb5f6f93d295848033ff47019bc48301ab0b` | Use report links; runtime behavior remains unverified |
| Blocked | Configure server-side `main` protection | Human maintainer | GitHub repository settings | Rulesets list empty; classic protection read returned 403, so effective state unknown | Open Settings → Rules → Rulesets / Branches and configure PR + required checks |

## Status values
- **Backlog** — not ready to start.
- **Ready** — scope and acceptance criteria are clear.
- **In progress** — one named agent owns it.
- **Review** — implementation complete; independent review pending.
- **Blocked** — waiting for evidence, access, or human decision.
- **Done** — acceptance criteria and verification evidence are recorded.

When a task is added, include the problem statement, expected behavior, non-goals, target version/device, acceptance criteria, test plan, and files likely to overlap. Use a GitHub Issue for substantial work and link it here.
