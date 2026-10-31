# Termux MCP Bridge — Alpha 0.2.0-alpha05

Native Android app that hosts an authenticated MCP Streamable HTTP endpoint at `127.0.0.1:8765/mcp` and can expose it through an existing ngrok installation.

## Alpha05: automatic connection

When the app opens, it starts the local MCP foreground service. The service then asks Termux to start or reuse an ngrok tunnel targeting port `8765`. It supports ngrok installed directly in Termux or inside the `debian` proot-distro environment. The app launches ngrok with the configured account-assigned static dev domain (`--url`) and reads ngrok's local agent API at `127.0.0.1:4040` to verify that the expected HTTPS URL is active. The dashboard includes a one-time domain editor; the initial value is the ngrok dev URL used in earlier Bridge testing.

While the MCP service is running, a lightweight watchdog checks the tunnel every 60 seconds and requests a restart if the ngrok agent is no longer reachable. The service uses Android `START_STICKY` so Android may recreate it after process reclamation. A boot receiver also requests startup after device reboot and after app updates; this is enabled by default and can be disabled in the dashboard. Explicitly stopping the service disables reboot autostart until the app is opened again. The dashboard includes a one-tap **Connect everything / repair connection** action, remote endpoint copy, and a combined endpoint + Bearer configuration copy action. The remote endpoint copy now includes a high-entropy `access_token` query parameter for MCP clients that cannot configure a static Authorization header.

The watchdog only stops a tunnel whose PID file was created by this Bridge. It does not kill an unrelated ngrok process. Stopping the MCP service also stops the Bridge-owned tunnel.

## One-time requirements

1. Install Termux and ngrok, and authenticate ngrok in either the normal Termux environment or the `debian` proot-distro environment. The configured static dev domain must belong to that same ngrok account.
2. In Termux, enable external app commands by setting `allow-external-apps=true` in `~/.termux/termux.properties`, then restart Termux.
3. Grant **RUN_COMMAND** permission to Termux MCP Bridge in Android app permissions.
4. Open Termux MCP Bridge. It will start the local server and tunnel automatically.

No ngrok authtoken is requested by the Bridge or sent to ChatGPT. The app reuses ngrok's existing local configuration. If ngrok is not installed or authenticated, the local MCP server can still run; the dashboard logs explain why the public tunnel did not start.

The ngrok Free plan assigns an account development domain that remains assigned to the account. The Bridge explicitly passes that domain to ngrok so the URL remains stable across agent restarts. If the default domain shown in the dashboard is not owned by the configured ngrok account, replace it once in **Configure fixed ngrok domain**. See [ngrok's static dev domain explanation](https://ngrok.com/blog/free-static-domains-ngrok-users).

## OAuth 2.1 for ChatGPT MCP

The Bridge now exposes OAuth discovery and authorization endpoints on the configured HTTPS ngrok domain:

- `/.well-known/oauth-protected-resource` and `/.well-known/oauth-protected-resource/mcp`
- `/.well-known/oauth-authorization-server`
- `/register` (Dynamic Client Registration)
- `/authorize` (user consent page)
- `/token` (authorization-code + PKCE S256 and refresh-token grants)
- `/revoke`

The MCP endpoint returns a Bearer challenge with the protected-resource metadata URL when no valid token is supplied. OAuth access tokens are scoped to the Bridge MCP resource, expire after one hour, and refresh tokens rotate when used. Authorization codes are single-use, expire after five minutes, and are bound to the registered redirect URI and PKCE S256 challenge. Only HTTPS redirect URIs and loopback HTTP redirect URIs are accepted.

In ChatGPT, add the custom MCP server using the public HTTPS URL ending in `/mcp` and select OAuth authentication when prompted. Do not paste the legacy capability URL as an OAuth endpoint.

**Alpha limitation:** the current OAuth client/code/token registry is held in process memory. Restarting the Bridge invalidates existing OAuth sessions and may require reconnecting/authorizing again. The legacy per-install token remains accepted temporarily for backwards compatibility. Do not treat this alpha implementation as a hardened multi-user authorization service.

## One-time ChatGPT/client registration

The app cannot silently register a remote MCP server inside ChatGPT. Copy **COPIAR ENDPOINT MCP REMOTO** from the dashboard. In ChatGPT's custom MCP connector choose **Sem autenticação** and paste this complete URL. The URL includes a random 256-bit `access_token` capability, which the Bridge validates in constant time; it is not an open unauthenticated endpoint. Keep the URL private. Clients that support custom Authorization headers can continue using the plain `/mcp` endpoint with `Authorization: Bearer <token>` from **COPIAR CONFIGURAÇÃO DO CLIENTE MCP**. After registration, reopening the Bridge should bring the same ngrok development domain online again.

The public endpoint remains protected by the per-install 256-bit secret, accepted either as a Bearer token or as the `access_token` query parameter for clients without header-auth configuration. Treat the complete capability URL as a password: anyone possessing it can access the MCP tools. Query credentials can appear in URL history or infrastructure logs, so use only over HTTPS and do not share it. Rotating the Bridge token invalidates both Bearer and capability-URL credentials.

## Implemented

- Dark Android dashboard with device diagnostics and event log.
- Foreground MCP server bound to loopback only.
- OAuth 2.1 authorization-code flow with PKCE S256, DCR, consent UI, refresh-token rotation, resource metadata discovery, and Bearer validation; legacy per-install token remains temporarily available for compatibility.
- Automatic MCP startup when the app opens.
- ngrok startup/reuse through fixed Termux RUN_COMMAND scripts with an explicit stable account dev domain; stale Bridge-owned tunnels are replaced when the configured domain changes. No arbitrary shell command tool is exposed.
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

The MCP listener remains bound to `127.0.0.1`; ngrok is the only public ingress. Every MCP request requires either a valid OAuth Bearer access token or the legacy per-install secret (Bearer or `access_token` query parameter) during the compatibility period. The tunnel manager invokes a fixed, app-authored script and never accepts arbitrary shell input from MCP callers.
