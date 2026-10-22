# BedAim custom-agent smoke test

Run the first end-to-end test of the repository's custom-agent workflow in this Codespaces workspace.

## Coordinator instructions
1. Read `AGENTS.md`, `docs/agent-collaboration/README.md`, and the current `docs/agent-collaboration/QUEUE.md`.
2. Verify the three files under `.github/agents/` and inspect their frontmatter, declared tools, and handoff targets.
3. Do not claim you can see the user's agent picker. Report that discovery requires confirmation from the active VS Code session.
4. Preserve the current workspace exactly. First inspect `git status --short --branch` and confirm the repository root. If the working tree is dirty (as it may be), do **not** stop merely because of that and do not stash, reset, clean, checkout another branch, or edit files in the existing worktree. Instead create a separate sibling Git worktree, e.g. `../BedAim-agent-smoke-test`, from the current remote setup branch, using a new branch `agent/smoke-test-docs` (or a unique suffix if that branch already exists). Verify the new worktree is clean before editing. If creating a sibling worktree is unsupported or unsafe, stop and report the exact blocker; never touch the existing dirty worktree.
5. Use the handoff to **BedAim Implementer** to work only inside the isolated smoke-test worktree and make one small, useful documentation improvement in `docs/agent-collaboration/README.md`: add a concise "Agent permissions" section describing Coordinator as read/search, Implementer as read/search/edit/execute, and Reviewer as read/search only. Confirm the values against the actual agent files rather than blindly copying this prompt. Do not change the agent definitions in this task.
6. Require Implementer to report the isolated worktree path, actual branch, commit, diff, and checks. Do not claim it completed if you only produced a plan. If a sibling worktree is used, ensure all subsequent handoffs explicitly point to that worktree so the original dirty workspace stays untouched.
7. Hand off the actual diff to **BedAim Reviewer**. Reviewer must independently inspect the diff and confirm no gameplay files changed. Reviewer must not edit.
8. Update `docs/agent-collaboration/QUEUE.md` with the actual status and evidence only after each stage is really completed. If handoffs are unavailable in this harness, stop at the first unsupported step and provide the exact manual next action instead of pretending the other agent ran.

## Acceptance criteria
- The active session visibly runs Coordinator, then Implementer, then Reviewer, or clearly records the precise point where the environment prevents the handoff.
- Any implementation is docs-only, isolated to a feature branch, and not merged.
- Reviewer reports findings from the real diff.
- Every test/build claim includes evidence; no in-game or CI success is inferred from documentation edits.
- Never push to `main`, merge, enable auto-merge, or reintroduce AutoHit/AutoTotem.
