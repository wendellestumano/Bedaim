# BedrockAim — Apollon input-route test build

This branch is an isolated experiment based on public descriptions of Apollon's attack path. It does not replace the regular Bedaim build or modify `main`.

## What is being tested

The public Apollon source indicates an alternative to calling `GameMode::_attack` directly: request the native ClientInstance attack-button handling path on the client update thread. This branch probes the current `libminecraftpe.so` for exact exported `ClientInstance` wrapper symbols and, if one resolves inside that library, queues the click for the next ClientInstance update.

The test deliberately does **not** use Apollon's old hard-coded offset `0x6462B60`. That offset came from an older Bedrock binary and has not been validated for Bedrock 26.52. The current symbol lookup may fail on a stripped build; in that case the log reports that the experiment was unavailable and AutoHit falls back to Bedaim's existing validated public GameMode route.

No Apollon source files or offsets are copied wholesale. The public repository does not provide an explicit license file, so this is a small independent adapter of the observed call flow rather than a source-code transplant.

## Package identity

The build metadata and artifact name are distinct from the normal package:

- Launcher package name: `BedrockAim Apollon Test`
- Artifact: `BedrockAim-ApollonTest.levipack`
- Runtime version label: `1.5.7-apollon-test`

Although the artifact is named separately, it contains the same native library filename as the normal mod. For a controlled test, preserve the regular package as a backup and load only one BedrockTools/BedrockAim runtime at a time; do not assume two copies can safely coexist in one process.

## What to test and report

1. Enable AutoHit in a private test world with a stationary mob in view.
2. Verify whether the player attacks and whether manual attack still works.
3. Capture the Bedaim log lines containing `Apollon test route`, `Synthetic attack`, `Native attack detour`, and `First native attack trampoline result`.
4. Note whether the test build crashes, does nothing, or attacks reliably; include Minecraft Bedrock 26.52 and Levi Launcher 1.5.25 versions.

Important log states:

- `Apollon test route resolved a ClientInstance attack-button handler by symbol`: the native input route was found.
- `Apollon test route is dispatching through the native attack-button handler`: a queued input call was issued. This is not proof of a landed hit.
- `no exported ClientInstance attack-button wrapper was found`: this test build could not activate the Apollon-style route on the current binary and used the existing validated GameMode route instead.

The host regression suite and Android arm64 build must pass before interpreting in-game results. A successful build alone does not validate the native function ABI or prove damage was applied.
