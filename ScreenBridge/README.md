# ScreenBridge Android (prototype)

ScreenBridge is an Android foreground service for authenticated screen capture and accessibility gestures. These updates live on the isolated `feature/screenbridge-status-overlay-20261019` branch; Bedaim's `main` branch is not changed.

## Setup and permission model
1. Install the stable-signed APK produced by the GitHub Actions workflow.
2. Enable **ScreenBridge Remote Control** in Android Accessibility settings.
3. Optional: tap **Mostrar indicador flutuante** and grant ScreenBridge permission to display over other apps. Tap the button again to show or hide the small, non-interactive status pill in the top-right corner.
4. Open ScreenBridge and tap **Authorize screen and start service**. Confirm Android's screen-capture dialog.
5. Keep the foreground-service notification visible while you want control enabled.

Screen capture requires explicit Android consent. If Android revokes the session or kills the service, open ScreenBridge and authorize again. Android's system consent cannot be silently persisted across new MediaProjection sessions.

## Keeping permissions across updates
- CI expects the repository Actions secret `SCREENBRIDGE_SIGNING_BUNDLE_B64`. The value is a Base64-encoded ZIP containing `screenbridge-signing.p12` and `signing.properties`.
- Keep that signing bundle private and make a secure backup. **Never commit the keystore, signing bundle, passwords, or the Base64 value to this public repository or paste them into chat.**
- The bundle must remain unchanged for every future build. A different signing key can prevent an in-place update and require uninstalling/reinstalling the app, which loses app data and usually requires enabling Accessibility again.
- The first stable-key migration from an earlier CI debug APK may still require one clean reinstall because previous GitHub-hosted runners generated temporary debug keys. After migration, all workflow-built APKs reuse the same certificate and have monotonically increasing CI version codes.
- A missing signing secret deliberately produces no APK. Do not install locally built/default-debug-signed APKs as replacements for the stable-signed app.

The overlay never consumes touch input (`FLAG_NOT_TOUCHABLE`). It shows capture status (`SB · …`), accessibility/control status (`CTRL · …`), and the independent GPT status (`GPT · …`). Capture values distinguish `SEM PROJEÇÃO`, `SEM FRAME`, `FRAME ATRASADO`, and `ERRO`; control values distinguish `PRONTO`, `EXECUTANDO`, `OFF`, and `ERRO`. The pill is optional and can be hidden from the app at any time.

## Local endpoints
- `GET /health` or `/diagnostics` — detailed status including projection, accessibility readiness, image-reader/virtual-display availability, frame age and count, callback count, empty-image count, JPEG encoding failures, automatic-recovery attempts/successes/failures, action result, last errors, and GPT-state age/TTL.
- `GET /<token>/screen.jpg` — returns a JPEG only if it is fresh (≤1.8 seconds old). If no fresh frame arrives within 900 ms, it returns HTTP 503 with JSON diagnostics rather than silently returning an old screenshot.
- `GET /<token>/ui.json` — cached active-window accessibility tree with text, descriptions, IDs, clickability and display bounds. Add `?refresh=1` to queue a fresh snapshot on the accessibility-service thread; normal reads use the event-updated cache and avoid spawning `uiautomator dump`.
- `GET /<token>/action?op=tap&x=120&y=240` — dispatch a tap and return the next available frame.
- `GET /<token>/action?op=swipe&x1=100&y1=300&x2=400&y2=300&ms=180` — dispatch a swipe and return the next frame.
- `GET /<token>/action?op=back` or `op=home` — global navigation and the next frame.

Coordinates use full-display pixel dimensions from `/health`. An accepted gesture is not proof that the intended action succeeded; inspect a fresh screenshot and check `lastActionAccepted`. `frameAgeMs` uses monotonic elapsed time. `/screen.jpg` never labels an old cache entry as current; when capture stalls, inspect `imageCallbackCount`, `lastImageCallbackAgeMs`, `frameCount`, `virtualDisplayActive`, `imageReaderActive`, and `captureError`. A watchdog detects a stalled frame stream after about 5 seconds and safely attempts to rebind the `ImageReader` surface on the same `MediaProjection` session, with a 15-second cooldown and at most three attempts per two-minute window. It does not silently create another projection or a second `VirtualDisplay`. The health endpoint distinguishes an active projection, fresh capture, and available accessibility control.

## Architecture and limits
- **ScreenBridge is the intended UI-control path.** Termux/ADB-based input is not required for ordinary tap, swipe, back, or home operations.
- Termux or a remote-device tool may still be needed for diagnostics, file transfer, logs, installation, and recovery.
- This build still exposes only a loopback endpoint. ChatGPT cannot directly reach the phone's `127.0.0.1` from its remote execution environment without an explicitly configured, authenticated relay/connector. Do not expose the HTTP server publicly or forward the token to an untrusted endpoint as a workaround.
- ScreenBridge is a prototype; verify on the target Android/HyperOS build before relying on it for time-sensitive interactions.

## Release verification
Every distributed APK must be built with the persistent signing bundle. Verify the artifact's certificate before installation, and do not publish an APK if the signing secret is missing.


## ChatGPT activity indicator and test recording

The floating pill now shows two independent states: `SB · …` for the phone-side ScreenBridge state and `GPT · …` for the ChatGPT state. The GPT state is **not automatically detectable from Android**. A trusted local controller can set it through the token-protected loopback endpoint:

- `GET /<token>/gpt-state?state=thinking`
- `GET /<token>/gpt-state?state=working`
- `GET /<token>/gpt-state?state=done`
- `GET /<token>/gpt-state?state=unknown`

The endpoint accepts only the app's existing random token and only the listed state values. A set state expires to `unknown` after 90 seconds unless a trusted controller refreshes it; an active controller should refresh the state periodically (for example every 30 seconds). `done` means the controller explicitly marked the work complete; it is not a claim that Android inferred ChatGPT's internal state. Android cannot reliably inspect ChatGPT's internal reasoning/activity itself, so fully automatic GPT status still requires a trusted controller that knows when the work starts and finishes. No public relay or token sharing is needed or recommended.

### Recording a reproducible test

For this build, start Android's built-in **Screen record** tile in Quick Settings before running the test, select device audio/microphone only if needed, and stop recording afterward. Save the MP4 under `Download/ScreenBridgeTests/` if the recorder offers a location option. The system recorder is intentionally kept separate from MediaProjection capture: Android 14+ restricts reuse of a MediaProjection session for multiple virtual displays, so this prototype does not start a second recording pipeline that could interrupt live screenshots. Verify that an MP4 exists and plays before relying on the recording.
