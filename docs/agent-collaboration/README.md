# BedAim multi-agent workflow

This directory is the durable coordination layer for GitHub Copilot, OpenAI Codex, and human review.

## What this setup does
- Shares common project rules through root `AGENTS.md` and `.github/copilot-instructions.md`.
- Adds workspace custom agents under `.github/agents/`: Coordinator, Implementer, and read-only Reviewer.
- Defines a durable queue and handoff format so another agent can resume from an issue, branch, PR, or report.
- Keeps all code changes on feature branches and sends them through PR review.

## What it does not do by itself
Repository files cannot force two separate chat sessions or the ChatGPT mobile app to message each other continuously. The GitHub connection inside ChatGPT is for reading/searching repository content, not directly editing a Codespace filesystem. The VS Code agent workflow can hand off between compatible agents in the same supported session; availability depends on the VS Code client, selected harness, and account.

For actual automatic cross-session orchestration, a separate runner must watch GitHub issues/labels or workflow events, call each provider through an authorized API, store outputs, and open/update PRs. This requires deliberate credential setup, cost controls, and least-privilege permissions. Do not store API keys in this repository. Do not give an automation permission to merge into `main`.

## Suggested roles
1. **Coordinator** — read-only reconnaissance, task decomposition, conflict detection, and handoff.
2. **Implementer** — one scoped task on one isolated branch; tests and report.
3. **Reviewer** — independent read-only diff review; does not trust implementer claims without evidence.
4. **Human maintainer** — approves merge and validates in-game behavior.

## Android + Codespaces setup
1. Open the BedAim repository in GitHub Codespaces from Brave.
2. Create a new Codespace or open the existing one, then make sure its working tree is clean.
3. Switch to `chore/ai-agent-collaboration-bootstrap` and pull the branch.
4. Open the Chat/Agents view in the Codespace and refresh/reload the window if needed.
5. Check whether workspace agents appear: **BedAim Coordinator**, **BedAim Implementer**, **BedAim Reviewer**. Browser-based Codespaces may not expose every harness/feature available in desktop VS Code; if Codex is not offered there, the shared instructions still work with Copilot, and Codex can review the same branch/PR from a supported ChatGPT/Codex surface.
6. Start with Coordinator and ask it to inspect current state and propose a task list. Use the handoff buttons to move to implementation and review when supported.
7. Keep the branch/PR as the shared communication record. Put diagnostic JSON/logs and exact build output in the issue/PR or an appropriately sanitized report.

## Safe branch and PR policy
- Branch naming: `agent/<issue>-<short-topic>` or `fix/<short-topic>`.
- One task per branch. Avoid two agents editing the same files simultaneously.
- No direct pushes to `main`; no agent may merge or enable auto-merge.
- Require CI checks and a human review before merging. Configure branch protection/rulesets in GitHub repository settings; the config files in this branch do not enable server-side protection automatically.
- Never upload secrets, proprietary game binaries, private data, or raw logs containing credentials.

## Current repository build notes
README documents Android NDK r28c, xmake, and Python 3. The documented commands are:

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

Replace the path with the actual NDK location. Check current CI and project scripts before assuming the commands work unchanged. A successful host test or compile does not replace Android ARM64 package verification and in-game reproduction.

## First smoke test
- Confirm the three custom agents load.
- Ask Coordinator for a read-only audit of current branch/PR/CI/diagnostics.
- Have Implementer make a documentation-only change on a test branch.
- Have Reviewer inspect that diff without editing.
- Verify both handoff reports include branch/commit, changed files, checks, evidence, and remaining risks.
