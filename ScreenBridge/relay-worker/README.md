# ScreenBridge Relay (Cloudflare Worker)

This directory contains the Worker deployed at `screenbridge-relay.screenbridge-wendell.workers.dev`.

## Deploy

Install Wrangler and authenticate to the intended Cloudflare account. From this directory:

```sh
npx wrangler deploy
```

The Worker uses a SQLite-backed Durable Object (`RelayHub`) for device registration, one-time pairing codes, role tokens, and WebSocket forwarding. Do not commit device tokens, controller tokens, account credentials, or other secrets.

## Endpoints

- `GET /api/health` — health check.
- `POST /api/device/pair/start` — Android registers `{ deviceId, deviceToken }`; returns a one-time 8-digit `pairCode` expiring after 120 seconds.
- `POST /api/controller/pair` — browser submits `{ pairCode }`; returns `deviceId` and a time-limited `controllerToken`.
- `wss://<worker>/ws/device` — Android outbound WebSocket.
- `wss://<worker>/ws/controller` — browser controller WebSocket.

Both WebSocket clients send an initial JSON `auth` message containing `role`, `deviceId`, and their role token. The Android client expects a successful response with `{ "type": "authenticated", "role": "device" }`; the browser controller expects `{ "type": "auth-result", "ok": true }`. Keep these message contracts synchronized with `ScreenBridgeRelayClient.java` and the dashboard.

Binary JPEG frames are forwarded to the authenticated controller without writing them to persistent storage. Commands are restricted to tap, swipe, back, home, and stop-control. This is an early prototype; verify the complete Android WebSocket handshake, frame transfer, and accessibility commands on-device after each deployment.
