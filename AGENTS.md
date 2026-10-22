# BedAim — shared agent instructions

These instructions apply to OpenAI Codex, GitHub Copilot, and other compatible coding agents working in this repository.

## Mission
Improve the repository through evidence-driven investigation, small reviewable changes, reproducible builds, regression tests, and clear documentation. Treat repository history, current source, CI logs, diagnostic exports, and reproducible test results as the source of truth—not assumptions or remembered snippets.

## Mandatory safety rules
- Never commit directly to `main`, force-push, rewrite shared history, or merge a PR autonomously.
- Start each task from the latest `main` and work on a dedicated `agent/<topic>` or `fix/<topic>` branch. If another agent owns the same files, coordinate before editing; use separate worktrees/branches where possible.
- Do not remove, disable, or rewrite unrelated modules to make a build pass. Preserve working behavior and keep changes tightly scoped.
- Never reintroduce retired/removed AutoHit or AutoTotem code merely because an old branch contains it. Verify the current tree and task scope first.
- Never invent game classes, method signatures, offsets, vtable indices, signatures, hook targets, or build results. Mark unverified hypotheses clearly and trace every native call to actual declarations, symbols, disassembly, or reproducible evidence.
- Treat Minecraft/LeviLauncher internals as version-sensitive. Record target game version, ABI, build toolchain, evidence source, and confidence for native offsets/signatures.
- Do not claim a runtime bug is fixed solely because code compiles or a host test passes. Distinguish static analysis, host tests, Android ARM64 build, and in-game validation.
- Do not download, commit, or redistribute proprietary game binaries/source, credentials, tokens, private user data, or unlicensed artifacts. Use public SDKs, official documentation, symbols and legally available samples.
- Never expose secrets in logs, commits, artifacts, prompts, or issue comments. Do not run destructive commands or alter GitHub settings without explicit human approval.
- Do not merge or enable auto-merge. A human owns the final merge decision.

## Required task loop
1. **Recon:** inspect `git status`, current branch, recent commits, related files, history, open issues/PRs, CI results, and relevant diagnostics before editing.
2. **Scope:** state the observed problem, expected behavior, non-goals, suspected cause, evidence, and files likely to change.
3. **Plan:** identify the smallest safe fix and a test that would fail before the fix. If evidence is insufficient, instrument or investigate instead of guessing.
4. **Implement:** one concern per change; preserve public interfaces unless change is required and documented.
5. **Verify:** run the narrowest relevant tests first, then host regression tests and the supported Android ARM64 package build when feasible. Record exact commands and results; never fabricate skipped tests.
6. **Independent review:** use the BedAim Reviewer agent or a second agent/model to inspect the diff for correctness, regressions, unsafe native memory access, concurrency/lifetime issues, missing tests, and unsupported claims.
7. **Report:** update `CHANGELOG.md` for user-visible or material engineering changes; add/update diagnostics or docs as needed. Provide changed files, commits, tests, build output, remaining risks, and runtime validation status.
8. **Handoff:** save a concise report in `docs/agent-collaboration/reports/` or the linked PR/issue. Include branch and commit SHA so another agent can continue without repeating research.

## Repository-specific development facts
- This is a native C++20 Android/LeviLauncher project; README documents Android NDK r28c, xmake, and Python 3.
- Documented release build commands:
  ```sh
  xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
  xmake -y
  ```
  Replace the NDK path with the actual installed path. Confirm current build scripts and CI before relying on these commands.
- Native Minecraft signatures and offsets are version-specific. Prefer existing wrappers, event APIs, ownership conventions, and established hook infrastructure over new guessed offsets.
- Diagnostic JSON/logs are evidence, but generic health checks do not prove gameplay behavior. Correlate diagnostics with the exact build/commit and a repeatable in-game scenario.
- Keep the working tree clean before changing branches. Never discard user changes.

## Cross-agent coordination protocol
Use GitHub as the durable shared mailbox:
- Issues describe tasks and acceptance criteria.
- Branches isolate changes.
- Pull requests carry diffs, test evidence, and review findings.
- `docs/agent-collaboration/QUEUE.md` lists queued/in-progress tasks.
- `docs/agent-collaboration/HANDOFF_TEMPLATE.md` defines the minimum handoff record.
- Each agent must read the latest handoff/PR comments before continuing and write back results before stopping.
- Avoid simultaneous edits to the same files. If a task overlaps another branch, stop and coordinate rather than resolving conflicts by guessing.

## Definition of done
A task is done only when the change is scoped, reviewed, test results are honestly reported, relevant documentation is updated, and remaining runtime limitations are explicit. A green build is not proof of in-game correctness.
