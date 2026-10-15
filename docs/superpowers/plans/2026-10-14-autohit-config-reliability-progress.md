# SDD ledger — plan: docs/superpowers/plans/2026-10-14-autohit-config-reliability.md

## Setup

- Execution is scoped to branch work/autohit-maceswapper-first-build-20261014; pull request #1 remains draft and targets main.
- The available session does not expose a local shell or native worktree manager. Changes are being made through the GitHub branch API; no commit targets main.
- Plan: docs/superpowers/plans/2026-10-14-autohit-config-reliability.md
- Architecture spec: docs/superpowers/specs/2026-10-14-bedaim-module-reliability-program-design.md

## Pre-flight interfaces

- Task 1 to Task 2: Task 1 produces bedaim::menuconfig::applyValue(json&, key, rawValue) to parse typed menu values without mutating unrelated keys; Task 2 can reuse the parser for a temporary-file round-trip test. Interface is compatible with the plan.
- Task 3 to Task 4: Task 3 observes the real configured attack-input path; Task 4 may gate dispatch only with a verified semantic input/action identifier. No numeric or touch-region guess is allowed.

## Task 1 progress

- Added menu parser regression cases covering boolean spellings, malformed boolean no-mutation, signed/overflow integer handling, decimal/non-finite float handling, string/radio payloads, missing keys, null values, and unrelated-key preservation.
- Added a standalone xmake test project under tests/ and a dedicated host-test step to CI.
- Initial CI run 38656593481 failed before compiling the regression: in tests/, xmake selected the repository-root project and attempted Android-only preloader on Linux (preloader @default unsupported on linux/x86_64, exit code 255). This is an environment/project-selection error, not a parser test result.
- Ruling: pass -P . to every xmake command executed from the tests/ directory — the observed diagnostic says the parent root project was being selected — cost if wrong: the host run still selects the root project, requiring another small command correction.
- Current state: re-run the host job with the project directory pinned, then verify the failure is caused by the planned MenuConfigValue API not existing. Only after observing that red state should parser production code be added.

## Task 1 update — parser implementation and CI diagnosis

- Added `src/launcher/MenuConfigValue.hpp/.cpp`; `ModuleMenu.cpp` now delegates typed parsing to this helper, leaving module-owned normalization and serialization in the existing path.
- Added `src/launcher/MenuConfigValue.cpp` to the host test target.
- CI run 38659023769 reached the host-test step but failed before compiling tests: xmake requested package confirmation for `nlohmann_json v3.11.3` despite `-y` appearing after `-P .`. The parser test result is therefore unknown from that run.
- Ruling: move xmake's `-y` flag before `-P .` in the host-test commands — the recorded package prompt is consistent with the flag not being applied; cost if wrong: another CI-only command correction.
- Corrected the C++ float parser's null-terminator comparison during source review.
- Current status: host workflow updated; wait for a fresh Actions run and inspect its logs before declaring Task 1 complete.
