# Agent task queue

Use this file as a lightweight index, not as a substitute for GitHub Issues or PRs. Keep one owner per task and update the row before starting or handing off work.

| Status | Task / Issue | Owner agent | Branch / PR | Scope / conflicts | Next action |
|---|---|---|---|---|---|
| Ready | Initial collaboration smoke test | Coordinator | `chore/ai-agent-collaboration-bootstrap` | Documentation/config only; do not change gameplay code | Verify agent discovery and handoffs in Codespaces |
| Backlog | Audit current build/CI commands and artifact outputs | Coordinator | Not assigned | Inspect current workflows before adding CI assumptions | Record verified commands and environment limitations |
| Backlog | Configure server-side `main` protection | Human maintainer | GitHub repository settings | Requires admin settings; do not infer that files enable protection | Require PRs, review, and successful checks |

## Status values
- **Backlog** — not ready to start.
- **Ready** — scope and acceptance criteria are clear.
- **In progress** — one named agent owns it.
- **Review** — implementation complete; independent review pending.
- **Blocked** — waiting for evidence, access, or human decision.
- **Done** — acceptance criteria and verification evidence are recorded.

When a task is added, include the problem statement, expected behavior, non-goals, target version/device, acceptance criteria, test plan, and files likely to overlap. Use a GitHub Issue for substantial work and link it here.
