# BedAim custom-agent smoke test

Run the first end-to-end test of the repository's custom-agent workflow in this Codespaces workspace.

## Coordinator instructions
1. Read `AGENTS.md`, `docs/agent-collaboration/README.md`, and the current `docs/agent-collaboration/QUEUE.md`.
2. Verify the three files under `.github/agents/` and inspect their frontmatter, declared tools, and handoff targets.
3. Do not claim you can see the user's agent picker. Report that discovery requires confirmation from the active VS Code session.
4. Create a tiny documentation-only task on a dedicated branch named `agent/smoke-test-docs`. Do not edit gameplay code, build configuration, or existing unrelated work. If that branch already exists or the working tree is dirty, stop and choose a unique safe branch or ask for clarification; never overwrite work.
5. Use the handoff to **BedAim Implementer** to make one small, useful documentation improvement in `docs/agent-collaboration/README.md`: add a concise "Agent permissions" section describing Coordinator as read/search, Implementer as read/search/edit/execute, and Reviewer as read/search only. Confirm the values against the actual agent files rather than blindly copying this prompt. Do not change the agent definitions in this task.
6. Require Implementer to report the actual branch, commit, diff, and checks. Do not claim it completed if you only produced a plan.
7. Hand off the actual diff to **BedAim Reviewer**. Reviewer must independently inspect the diff and confirm no gameplay files changed. Reviewer must not edit.
8. Update `docs/agent-collaboration/QUEUE.md` with the actual status and evidence only after each stage is really completed. If handoffs are unavailable in this harness, stop at the first unsupported step and provide the exact manual next action instead of pretending the other agent ran.

## Acceptance criteria
- The active session visibly runs Coordinator, then Implementer, then Reviewer, or clearly records the precise point where the environment prevents the handoff.
- Any implementation is docs-only, isolated to a feature branch, and not merged.
- Reviewer reports findings from the real diff.
- Every test/build claim includes evidence; no in-game or CI success is inferred from documentation edits.
- Never push to `main`, merge, enable auto-merge, or reintroduce AutoHit/AutoTotem.
