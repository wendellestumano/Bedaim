# ScreenBridge Android (prototype)

ScreenBridge is an Android foreground service that offers an authenticated, loopback-only HTTP bridge for screen capture and accessibility gestures. It is being developed on a separate branch; it does not modify Bedaim's main branch.

## Permissions and setup
1. Install the debug APK built by GitHub Actions.
2. Open ScreenBridge and enable **ScreenBridge Remote Control** in Android Accessibility settings.
3. Return to the app and tap **Authorize screen and start service**. Android will show the system screen-capture consent dialog.
4. Keep the ongoing notification visible while remote access is needed.

The HTTP listener binds only to `127.0.0.1:18766`; it is not exposed to Wi-Fi or the public internet. Each install generates a random access token. Never share the token publicly. The user can stop capture from the app or Android's active screen-capture controls.

## Local endpoints
- `GET /health` — status and current dimensions (local diagnostic endpoint).
- `GET /<token>/screen.jpg` — current JPEG frame.
- `GET /<token>/action?op=tap&x=120&y=240` — tap, then returns a JPEG frame.
- `GET /<token>/action?op=swipe&x1=100&y1=300&x2=400&y2=300&ms=180` — swipe and return a frame.
- `GET /<token>/action?op=back` or `op=home` — global navigation, then frame.

Coordinates use the current full-display pixel dimensions reported by `/health`. Accessibility gestures are asynchronous: an HTTP success means the gesture was accepted, not proof that the intended app action succeeded; inspect the returned frame.

## Current limitations
- Android requires explicit screen-capture consent. If the OS terminates the service or revokes projection, the user must authorize a new capture session.
- Gesture control requires the user to enable the accessibility service manually.
- This bridge is local only. ChatGPT still needs an authorized tool/relay that can reach the device's loopback endpoint; installing the APK alone does not make the chat a continuous live-video receiver.
- Prototype; test on the intended Android/HyperOS build before relying on it.
