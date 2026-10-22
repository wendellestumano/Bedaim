# BedAim multi-agent workflow

This directory is the durable coordination layer for GitHub Copilot, OpenAI Codex, and human review.

## What this setup does
- Shares common project rules through root `AGENTS.md` and `.github/copilot-instructions.md`.
- Adds workspace custom agents under `.github/agents/`: Coordinator, Implementer, and read-only Reviewer.
- Defines a durable queue and handoff format so another agent can resume from an issue, branch, PR, or report.
- Keeps all code changes on feature branches and sends them through PR review.

## What it does not do by itself
Repository files cannot force two separate chat sessions or the ChatGPT mobile app to message each other continuously. The GitHub connection inside ChatGPT is for reading/searching repository content, not directly editing a Codespace filesystem. The VS Code agent workflow can hand off between compatible agents in the same supported session; availability depends on the VS Code client, selected harness, and account.

The repository now includes a GitHub Actions runner for a bounded Copilot CLI loop and a separate read-only AI reviewer workflow. These are configured on the collaboration branch in PR #66; they are not active on `main` until the PR is reviewed and merged. This is a sequential Copilot-based workflow, not an always-on bridge that makes Copilot, Codex, and ChatGPT chat with one another. Never store tokens in repository files or prompts, and never give an AI workflow merge permission.

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

## Automated GitHub Actions agent loop

The collaboration branch adds two workflows:

- `.github/workflows/bedaim-ai-agent.yml` — manually triggered **BedAim AI Agent Runner**. The Coordinator creates an evidence-based plan. In `implement` mode, the Implementer may make a scoped change in a fresh Actions checkout on a new `agent/copilot-<run-id>` branch. A draft PR is opened only if the Implementer step succeeds. The existing `Build` workflow is then dispatched against that branch.
- `.github/workflows/bedaim-ai-review.yml` — after a completed `Build` run, finds only an open same-repository PR from an `agent/copilot-` branch that matches the exact built SHA, runs Copilot with read/search tools only, and posts or updates an advisory comment. It does not approve, request changes through GitHub, merge, or run fork PR code.

### One-time authentication setup

The Copilot CLI workflow requires an Actions repository secret named `COPILOT_GITHUB_TOKEN`. Create a fine-grained personal access token with the **Copilot Requests** permission and save it at **Settings → Secrets and variables → Actions → New repository secret**. Never paste that token into chat or commit it to the repository. See the [official Copilot CLI Actions guide](https://docs.github.com/en/copilot/how-tos/copilot-cli/automate-copilot-cli/automate-with-actions).

Because this is a public personal repository, the workflow uses the PAT-based authentication route. These workflows are not active on `main` until PR #66 is merged. Adding the secret is a maintainer action; this repository integration cannot create or read secret values.

### Run the agent loop

After PR #66 is merged and the secret is configured:

1. Open **Actions → BedAim AI Agent Runner → Run workflow**.
2. Choose `plan-only` to inspect a task first, or `implement` to permit the scoped Implementer to edit files in its isolated runner.
3. Enter one concrete task with success criteria and a valid base branch (normally `main`).
4. Inspect the run summary. In `implement` mode, review the draft PR and the matching Build result; the automatic AI review is an advisory comment on that PR.

The workflow only accepts manual runs triggered by the repository owner. AI output remains untrusted until reviewed. No agent can merge. CI can prove tests/compilation only for the exact SHA; an in-game Minecraft test on the target device is still needed for runtime claims. Failed/partial implementation runs are not automatically packaged into a PR.

### Scope limitations

- This automates the Copilot CLI Coordinator → Implementer sequence and a follow-up read-only Reviewer; it does **not** automatically invoke the separate Codex service or the mobile ChatGPT app.
- The reviewer workflow becomes event-active only after it is on the default branch. Until then, its YAML is merely part of PR #66.
- Configure and verify server-side protection on `main` separately. Files in a PR do not enforce branch rules by themselves.
