# Bedaim Dev Shift - 2026-10-20

## Cycle baseline and safety
- Repository: wendellestumano/Bedaim.
- Experimental base: feature/cloudflare-screenbridge-relay-fix-20261020 at 3d5f6adc07ac5016e29036f86209db0ce5c78214.
- New isolated branch: experiment/relay-rate-limit-map-bounds-20261020.
- PR #42 remains open/draft and unmerged. No changes to main; no merge is planned.
- Existing Termux worktrees Bedaim-working-20261019 and Bedaim-validation-20261019 are preserved; the latter already contains staged diagnostics changes and is not used for this cycle.
- Latest open-issues query returned no open issues. Relevant open PRs include #42 (ScreenBridge relay), #41 (diagnostics log arity), and #40 (actor-vector capacity guards).

## Latest CI evidence before source edits
- Bedaim Build #905: https://github.com/wendellestumano/Bedaim/actions/runs/39363898886 - success on 3d5f6adc07ac5016e29036f86209db0ce5c78214; host regression tests, Android NDK setup, Android ARM64 compilation, and .levipack upload all succeeded. Artifact BedrockTools-levipack, 631,029 bytes, artifact ID 12238147833.
- ScreenBridge Android APK run #24: https://github.com/wendellestumano/Bedaim/actions/runs/39363894445 - success on the same commit; persistent signing bundle step, APK build, and upload succeeded. Artifact ScreenBridge-stable-signed-apk, 1,380,936 bytes, artifact ID 12239035780.
- Latest relay branch commits before this cycle were 3d5f6adc (relay persistence fixes), 73a49bd2 (pair-code renewal/reconnect), and e05b7a56 (protocol docs).

## Pre-edit hypothesis
ScreenBridge/relay-worker/worker.js keeps per-IP rate-limit entries in this.rateBuckets, but rateLimit() never deletes expired keys and has no size cap. Requests from many distinct client IPs can therefore grow this in-memory map for the lifetime of the Durable Object instance, even though each entry is only relevant for a 60-second window. This is a bounded-memory stability issue in the public pairing endpoints. The separate lastFrameAt map was also observed, but it is out of scope for this small first patch.

## Intended change
- Bound rateBuckets to a fixed maximum (2,048 entries).
- Expire stale buckets during a periodic sweep when the map is full, and evict least-recently-used entries only when necessary to enforce the cap.
- Preserve the existing per-IP request-count semantics and 60-second window.
- Add Node built-in tests for per-IP thresholds, stale-entry cleanup, and map bounds.
- Add a focused test step to the ScreenBridge APK workflow so future relay changes run the tests before packaging.
- Run those tests locally, then run the existing Bedaim regression/Android workflow and ScreenBridge APK workflow on the experimental branch. Inspect actual logs and artifact lists; a green workflow alone is not enough if APK/levipack upload is skipped.

## Intended test (written before source changes)
1. Verify repeated requests from one IP still pass up to the configured limit and reject the next request.
2. Insert substantially more than 2,048 distinct IPs and assert the map remains at or below the cap.
3. Seed stale buckets and verify the next full-map sweep removes them.
4. Run node --test ScreenBridge/relay-worker/worker.test.mjs.
5. Run Git whitespace/diff checks and inspect the new CI run(s), including the .levipack and stable-signed APK artifacts.

No source files have been edited yet in this cycle.
