# ChatC++ Core V0.1

ChatC++ is a model-agnostic engineering intelligence core for the BedAim repository. V0.1 establishes persistent project memory, a task ledger, configurable priorities, experiment history, bounded inspection cycles, full-state snapshots, and a conservative read-only repository scanner.

IMPORTANT: V0.1 does not autonomously edit or merge BedAim source code and does not call an AI API. The model provider is disabled by default. This is intentional: establish reliable state, evidence, tests, and safety boundaries before enabling autonomous code changes.

## Hybrid deployment

- Android / Termux: control surface, CLI, configuration, local fallback database, and status inspection.
- Cloud worker: bounded GitHub Actions jobs. A scheduled job runs a read-only scan every 30 minutes by default.
- Memory: SQLite is the local source of truth. Cloud jobs restore and save versioned JSON snapshots on the dedicated chatcpp-memory branch.
- AI providers: disabled in V0.1. Future adapters must enforce a hard daily call limit and monthly BRL budget cap. No API key is stored in the repository.

GitHub Actions standard hosted runners are free for public repositories. Scheduled jobs are not real-time, run from the default branch, and public-repository schedules can be disabled after 60 days without repository activity. A workflow run is a bounded job, not a permanent server process.

## Install on Termux

Install Python and Git, then enter the ChatC++ directory:

    pkg update
    pkg install python git
    cd ~/BedAim/tools/chatcpp
    python -m pip install --user --no-deps .

Copy config.example.json to config.local.json and set workspace to the absolute path of the BedAim checkout. Keep config.local.json, API keys, and the SQLite database out of Git.

## Commands

    chatcpp --config config.local.json doctor
    chatcpp --config config.local.json scan
    chatcpp --config config.local.json status
    chatcpp --config config.local.json cycle
    chatcpp --config config.local.json cycle --loop --interval 30
    chatcpp --config config.local.json cycle --if-due
    chatcpp --config config.local.json memory add "AimAssist baseline" "Record verified baseline and test evidence" --kind decision --tag aimassist
    chatcpp --config config.local.json memory search "AimAssist baseline"
    chatcpp --config config.local.json memory export ./memory.jsonl
    chatcpp --config config.local.json export-state ./chatcpp-state.json
    chatcpp --config config.local.json import-state ./chatcpp-state.json
    chatcpp --config config.local.json task add "Investigate BlockESP chunk discovery" --area blockesp --priority 8
    chatcpp --config config.local.json task list
    chatcpp --config config.local.json findings
    chatcpp --config config.local.json priority list
    chatcpp --config config.local.json priority set aimassist 10
    chatcpp --config config.local.json experiment add "FOV causes drift" "Compare target smoothing with FOV changes" "inconclusive" --evidence "test notes" --learned "Do not assume the camera update is frame invariant"

cycle --loop is opt-in and stops on Ctrl+C or SIGTERM. Do not run an unbounded loop in GitHub Actions. Cloud jobs execute one bounded cycle and let the scheduler invoke the next run.

## Scanner scope and limitations

The scanner inventories supported text/source files and records TODO/FIXME/HACK/XXX markers with path, line, and a stable fingerprint. Duplicate findings are suppressed. A marker is not necessarily a defect. This scanner cannot prove the codebase is bug-free and does not modify target files.

## Memory model

SQLite stores tasks, findings, memory entries, experiments, cycles, priorities, and audit events. FTS5 is used when available; a LIKE-based fallback is used when the system SQLite lacks FTS5. Full-state snapshots are versioned JSON and can be imported into a fresh worker.

The initial cloud worker stores technical snapshots on a dedicated public chatcpp-memory branch. Anyone who can view the public repository can read that branch. Never put secrets, tokens, signing keys, private user data, or credentials in ChatC++ memory. Supabase/Postgres is a later option if Git-based snapshots become too large.

## Safety policy

1. Never commit directly to main from an autonomous agent.
2. All future code-writing work must happen in a dedicated task branch or worktree.
3. Build and test evidence must accompany every proposed code change.
4. A reviewer must inspect the diff before merge.
5. Secrets and private data must never enter memory exports or logs.
6. Failed experiments must record hypothesis, attempted change, outcome, evidence, and lesson; failures must not be stored as successful knowledge.
7. If validation fails repeatedly, stop the task and report rather than looping indefinitely.

## Roadmap

- V0.1: SQLite memory, task ledger, priorities, experiment journal, read-only scanner, bounded scheduler, snapshots, CLI, tests, CI, scheduled cloud scan.
- V0.2: Android/cloud sync hardening, remote memory adapter, and model-provider adapters.
- V0.3: budget meter, research ingestion, and tool sandbox.
- V0.4: isolated branch/worktree executor, build/test orchestration, patch evidence, reviewer gate.
- V1.0: supervised continuous engineering loop with per-module priorities, recovery, and dashboard.
