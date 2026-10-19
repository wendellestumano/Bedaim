# ScreenBridge Android (prototype)

ScreenBridge is an Android foreground service for authenticated screen capture and accessibility gestures. It lives on the separate `feature/screenbridge-android-app-20261019` branch; Bedaim's `main` branch is not changed.

## Setup and permission model
1. Install the debug APK built by GitHub Actions.
2. Enable **ScreenBridge Remote Control** in Android Accessibility settings.
3. Open ScreenBridge and tap **Authorize screen and start service**. Confirm Android's screen-capture dialog.
4. Keep the foreground-service notification visible while you want control enabled.

Screen capture requires explicit Android consent. If Android revokes the session or kills the service, open ScreenBridge and authorize again. The app must not silently bypass that OS permission. The local HTTP listener binds only to `127.0.0.1:18766`; it is not exposed to Wi-Fi or the public internet. A random per-install token protects control and image endpoints. Never share that token publicly.

## Local endpoints
- `GET /health` — service, projection, accessibility, cached-frame freshness, frame count, and capture error diagnostics.
- `GET /<token>/screen.jpg` — most recently captured JPEG frame.
- `GET /<token>/action?op=tap&x=120&y=240` — dispatch a tap and return the next available frame.
- `GET /<token>/action?op=swipe&x1=100&y1=300&x2=400&y2=300&ms=180` — dispatch a swipe and return the next frame.
- `GET /<token>/action?op=back` or `op=home` — global navigation and the next frame.

Coordinates use full-display pixel dimensions from `/health`. An accepted gesture is not proof that the intended action succeeded; inspect the returned frame. Frame encoding is continuous and cached, so reading `screen.jpg` does not depend on catching the exact moment that a request arrives. The health endpoint distinguishes an active projection from a usable frame.

## Architecture and limits
- **ScreenBridge is the intended UI-control path.** Termux/ADB-based input is not required for ordinary tap, swipe, back, or home operations.
- Termux or a remote-device tool may still be needed for diagnostics, file transfer, logs, installation, and recovery.
- This build still exposes only a loopback endpoint. ChatGPT cannot directly reach the phone's `127.0.0.1` from its remote execution environment without an explicitly configured, authenticated relay/connector. Do not expose the HTTP server publicly or forward the token to an untrusted endpoint as a workaround.
- ScreenBridge is a prototype; verify on the target Android/HyperOS build before relying on it for time-sensitive interactions.
