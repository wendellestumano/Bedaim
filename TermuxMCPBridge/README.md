# Termux MCP Bridge — Alpha 0.1
Native Android app hosting an authenticated MCP Streamable HTTP endpoint on 127.0.0.1:8765/mcp.

## Current scope
- User-started foreground service and notification.
- Per-install 256-bit bearer token stored in private preferences.
- MCP tools: bridge_status, workspace_list, workspace_read, workspace_write.
- File access is restricted to app-private workspace with path traversal checks and file size limits.
- Shizuku status/permission request and explicit root capability probe.
- Termux detection only; shell/process execution and Cloudflare Tunnel are not yet implemented.

## Build
JDK 17, Android SDK platform 36 and build-tools 36.0.0. Run ./gradlew :app:testDebugUnitTest :app:assembleDebug.

## Security
No arbitrary shell is exposed. Do not share the bearer token. Remote tunnel is intentionally disabled until its lifecycle and authentication are tested.
