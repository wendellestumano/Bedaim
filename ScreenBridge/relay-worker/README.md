# ScreenBridge Relay (Cloudflare Worker)

This directory contains the Worker deployed at `screenbridge-relay.screenbridge-wendell.workers.dev`.

The persistent-controller change keeps the 8-digit code single-use and short-lived, but after successful pairing stores a random controller credential in that browser's local storage. The credential remains valid until the user presses **Stop control** (revokes it) or pairs a new controller. On reopening the panel, the browser authenticates automatically and retries dropped WebSocket connections with backoff. Browser storage is a bearer credential: do not use the panel on a shared browser profile. Closing the tab still closes its WebSocket; reopening reconnects automatically. This does not make a closed browser tab or the ChatGPT model itself run a background connection.

## Deploy

Install Wrangler and authenticate to the intended Cloudflare account. From this directory:

```sh
npx wrangler deploy
```

The Worker uses a SQLite-backed Durable Object (`RelayHub`) for device registration, one-time pairing codes, role tokens, and WebSocket forwarding. Do not commit device tokens, controller tokens, account credentials, or other secrets.

## Endpoints

- `GET /api/health` — health check.
- `POST /api/device/pair/start` — Android registers `{ deviceId, deviceToken }`; returns a one-time 8-digit `pairCode` expiring after 120 seconds.
- `POST /api/controller/pair` — browser submits `{ pairCode }`; returns `deviceId` and a persistent, random `controllerToken` (revocable; never expires by time).
- `wss://<worker>/ws/device` — Android outbound WebSocket.
- `wss://<worker>/ws/controller` — browser controller WebSocket.

Both WebSocket clients send an initial JSON `auth` message containing `role`, `deviceId`, and their role token. The Android client expects a successful response with `{ "type": "authenticated", "role": "device" }`; the browser controller expects `{ "type": "auth-result", "ok": true }`. Keep these message contracts synchronized with `ScreenBridgeRelayClient.java` and the dashboard.

Binary JPEG frames are forwarded to the authenticated controller without writing them to persistent storage. Commands are restricted to tap, swipe, back, home, and stop-control. This is an early prototype; verify the complete Android WebSocket handshake, frame transfer, and accessibility commands on-device after each deployment.
