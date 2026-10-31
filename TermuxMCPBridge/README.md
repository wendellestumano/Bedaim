# Termux MCP Bridge — Alpha 0.2.0-alpha05

Native Android app that hosts an authenticated MCP Streamable HTTP endpoint at `127.0.0.1:8765/mcp` and can expose it through an existing ngrok installation.

## Alpha05: automatic connection

When the app opens, it starts the local MCP foreground service. The service then asks Termux to start or reuse an ngrok tunnel targeting port `8765`. It supports ngrok installed directly in Termux or inside the `debian` proot-distro environment. The app reads ngrok's local agent API at `127.0.0.1:4040` to discover the HTTPS URL and displays the remote MCP endpoint.

While the MCP service is running, a lightweight watchdog checks the tunnel every 60 seconds and requests a restart if the ngrok agent is no longer reachable. The service uses Android `START_STICKY` so Android may recreate it after process reclamation. A boot receiver also requests startup after device reboot and after app updates; this is enabled by default and can be disabled in the dashboard. Explicitly stopping the service disables reboot autostart until the app is opened again. The dashboard includes a one-tap **Connect everything / repair connection** action, remote endpoint copy, and a combined endpoint + Bearer configuration copy action.

The watchdog only stops a tunnel whose PID file was created by this Bridge. It does not kill an unrelated ngrok process. Stopping the MCP service also stops the Bridge-owned tunnel.

## One-time requirements

1. Install Termux and ngrok, and authenticate ngrok in either the normal Termux environment or the `debian` proot-distro environment.
2. In Termux, enable external app commands by setting `allow-external-apps=true` in `~/.termux/termux.properties`, then restart Termux.
3. Grant **RUN_COMMAND** permission to Termux MCP Bridge in Android app permissions.
4. Open Termux MCP Bridge. It will start the local server and tunnel automatically.

No ngrok authtoken is requested by the Bridge or sent to ChatGPT. The app reuses ngrok's existing local configuration. If ngrok is not installed or authenticated, the local MCP server can still run; the dashboard logs explain why the public tunnel did not start.

The ngrok Free plan assigns an account development domain that is retained when the agent restarts. The app discovers the active URL rather than hardcoding a previous tunnel address. See [ngrok Free Plan Limits](https://ngrok.com/docs/pricing-limits/free-plan-limits/).

## One-time ChatGPT/client registration

The app cannot silently register a remote MCP server inside ChatGPT. Copy the remote endpoint from the dashboard and configure it once in the MCP client, with the Bearer token copied by the app. After that registration, reopening the Bridge should bring the same ngrok development domain online again, so the endpoint does not need to be changed on every restart.

The public endpoint remains protected by the per-install 256-bit Bearer token. Treat the combined configuration copied by the app as a password and do not share it. Rotating the token invalidates existing MCP client credentials.

## Implemented

- Dark Android dashboard with device diagnostics and event log.
- Foreground MCP server bound to loopback only.
- Per-install 256-bit Bearer token with copy and rotation controls.
- Automatic MCP startup when the app opens.
- ngrok startup/reuse through fixed Termux RUN_COMMAND scripts; no arbitrary shell command tool is exposed.
- ngrok URL discovery through the local agent API and persistent display in the dashboard.
- 60-second tunnel watchdog while the MCP foreground service is active.
- Automatic ngrok lookup in Termux first, then Debian proot-distro.
- Shizuku UserService-backed shared-storage MCP tools rooted at `/sdcard`: list, read, write, mkdir, delete, copy, and move. Transfers are chunked.
- MCP workspace tools: `bridge_status`, `workspace_list`, `workspace_read`, and `workspace_write`.
- APK Lab for APK metadata/signature inspection, protected raw ZIP extraction, backup, export, and selected Apktool decode through Termux.

## Limitations

- The Android app does not install ngrok or configure its authtoken. ngrok must already be installed and authenticated in Termux or Debian proot-distro.
- ChatGPT's MCP server registration and Bearer credential must be configured once by the user; the Android app cannot modify the ChatGPT account configuration.
- Android battery management, force-stop, reboot policy, and OEM background restrictions can still stop services. Boot recovery is best-effort; if Android or the device manufacturer blocks it, open the Bridge once. Shizuku may separately need to be started/authorized again after reboot.
- Shizuku must be running and authorized for shared-storage file operations. Shizuku is not root; private data belonging to other apps and restricted Android/data or obb paths may remain inaccessible.
- Remote access is available only while the local MCP service, ngrok agent, and network connection are online.
- No arbitrary shell execution, root command execution, process manager, or build orchestration is exposed.

## Build

JDK 17, Android SDK platform 36 and build-tools 36.0.0.

```bash
gradle :app:testDebugUnitTest :app:assembleDebug
```

## Persistent signing

GitHub Actions uses the configured persistent signing key to keep APK updates installable over the previous alpha. Required repository secrets:

- `BRIDGE_SIGNING_KEYSTORE_BASE64`
- `BRIDGE_SIGNING_STORE_PASSWORD`
- `BRIDGE_SIGNING_KEY_ALIAS`
- `BRIDGE_SIGNING_KEY_PASSWORD`

Keep an offline backup of the keystore and credentials. Never commit them to the repository.

## Security

The MCP listener remains bound to `127.0.0.1`; ngrok is the only public ingress. Every MCP request requires the Bearer token. The tunnel manager invokes a fixed, app-authored script and never accepts arbitrary shell input from MCP callers.
