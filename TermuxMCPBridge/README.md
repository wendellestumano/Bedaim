# Termux MCP Bridge — Alpha 0.2

Native Android app hosting an authenticated MCP Streamable HTTP endpoint on `127.0.0.1:8765/mcp`.

## Current scope
- Black/dark device dashboard with status cards and quick actions.
- Live local diagnostics: Android/device model, chipset, battery percentage, RAM, app-private storage usage and active network transport.
- Quick launch for Termux and Shizuku.
- Shizuku status/permission request and explicit root capability probe.
- User-started foreground MCP service and notification.
- Per-install 256-bit bearer token stored in private preferences.
- Copy endpoint and token actions; token is masked in the UI and can be rotated from the dashboard. Rotation restarts the local MCP listener with the new credential.
- MCP tools: `bridge_status`, `workspace_list`, `workspace_read`, `workspace_write`.
- File access is restricted to app-private workspace with path traversal checks and file size limits.

## Not implemented yet
- Termux shell/process execution and build orchestration.
- Shizuku-backed device operations beyond permission/status detection.
- Cloudflare Tunnel and remote ChatGPT connectivity.
- Root command execution.

## Build
JDK 17, Android SDK platform 36 and build-tools 36.0.0. Run `./gradlew :app:testDebugUnitTest :app:assembleDebug`.

## Security
No arbitrary shell is exposed. The bearer token is never displayed by default; copy it only when needed and do not share it. Token rotation invalidates clients using the previous token. The listener remains bound to loopback; remote tunnel is intentionally disabled until its lifecycle and authentication are tested.
