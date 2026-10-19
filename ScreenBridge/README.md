# ScreenBridge Android (prototype)

ScreenBridge is an Android foreground service for authenticated screen capture and accessibility gestures. It lives on the separate `feature/screenbridge-android-app-20261019` branch; Bedaim's `main` branch is not changed.

## Setup and permission model
1. Install the stable-signed APK produced by the GitHub Actions workflow.
2. Enable **ScreenBridge Remote Control** in Android Accessibility settings.
3. Open ScreenBridge and tap **Authorize screen and start service**. Confirm Android's screen-capture dialog.
4. Keep the foreground-service notification visible while you want control enabled.

Screen capture requires explicit Android consent. If Android revokes the session or kills the service, open ScreenBridge and authorize again. Android's system consent cannot be silently persisted across new MediaProjection sessions.

## Keeping permissions across updates
- CI expects the repository Actions secret `SCREENBRIDGE_SIGNING_BUNDLE_B64`. The value is a Base64-encoded ZIP containing `screenbridge-signing.p12` and `signing.properties`.
- Keep that signing bundle private and make a secure backup. **Never commit the keystore, signing bundle, passwords, or the Base64 value to this public repository or paste them into chat.**
- The bundle must remain unchanged for every future build. A different signing key can prevent an in-place update and require uninstalling/reinstalling the app, which loses app data and usually requires enabling Accessibility again.
- The first stable-key migration from an earlier CI debug APK may still require one clean reinstall because previous GitHub-hosted runners generated temporary debug keys. After migration, all workflow-built APKs reuse the same certificate and have monotonically increasing CI version codes.
- A missing signing secret deliberately produces no APK. Do not install locally built/default-debug-signed APKs as replacements for the stable-signed app.

## Local endpoints
- `GET /health` — service, projection, accessibility, cached-frame freshness, frame count, and capture error diagnostics.
- `GET /<token>/screen.jpg` — most recently captured JPEG frame.
- `GET /<token>/ui.json` — cached active-window accessibility tree with text, descriptions, IDs, clickability and display bounds. Add `?refresh=1` to queue a fresh snapshot on the accessibility-service thread; normal reads use the event-updated cache and avoid spawning `uiautomator dump`.
- `GET /<token>/action?op=tap&x=120&y=240` — dispatch a tap and return the next available frame.
- `GET /<token>/action?op=swipe&x1=100&y1=300&x2=400&y2=300&ms=180` — dispatch a swipe and return the next frame.
- `GET /<token>/action?op=back` or `op=home` — global navigation and the next frame.

Coordinates use full-display pixel dimensions from `/health`. An accepted gesture is not proof that the intended action succeeded; inspect the returned frame. Frame encoding is continuous and cached, so reading `screen.jpg` does not depend on catching the exact moment that a request arrives. The health endpoint distinguishes an active projection from a usable frame.

## Architecture and limits
- **ScreenBridge is the intended UI-control path.** Termux/ADB-based input is not required for ordinary tap, swipe, back, or home operations.
- Termux or a remote-device tool may still be needed for diagnostics, file transfer, logs, installation, and recovery.
- This build still exposes only a loopback endpoint. ChatGPT cannot directly reach the phone's `127.0.0.1` from its remote execution environment without an explicitly configured, authenticated relay/connector. Do not expose the HTTP server publicly or forward the token to an untrusted endpoint as a workaround.
- ScreenBridge is a prototype; verify on the target Android/HyperOS build before relying on it for time-sensitive interactions.

## Release verification
Every distributed APK must be built with the persistent signing bundle. Verify the artifact's certificate before installation, and do not publish an APK if the signing secret is missing.
