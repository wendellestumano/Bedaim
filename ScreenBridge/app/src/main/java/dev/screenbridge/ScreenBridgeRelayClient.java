package dev.screenbridge;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import org.json.JSONObject;

import java.security.SecureRandom;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.TimeUnit;

import okhttp3.HttpUrl;
import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import okhttp3.WebSocket;
import okhttp3.WebSocketListener;
import okio.ByteString;

/**
 * Optional outbound-only connection to ScreenBridge Relay.
 * The existing local-only HTTP server remains bound to 127.0.0.1.
 */
final class ScreenBridgeRelayClient extends WebSocketListener {
    private static final String TAG = "ScreenBridgeRelay";
    private static final MediaType JSON = MediaType.get("application/json; charset=utf-8");
    private static final long FRAME_INTERVAL_MS = 120L;
    private static final long MAX_SOCKET_QUEUE_BYTES = 420_000L;

    private final Context context;
    private final String baseUrl;
    private final String deviceId;
    private final String deviceToken;
    private final OkHttpClient http;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile WebSocket socket;
    private volatile boolean stopped;
    private volatile boolean registered;
    private volatile boolean authenticated;
    private volatile boolean reconnectScheduled;
    private volatile long lastFrameAtMs;
    private volatile long lastFrameSentAtMs;
    private volatile int reconnectDelayMs = 1500;
    private final Runnable reconnectTask = () -> {
        reconnectScheduled = false;
        if (!stopped) connectOrRegister();
    };

    private ScreenBridgeRelayClient(Context context, String baseUrl) {
        this.context = context.getApplicationContext();
        this.baseUrl = normalizeBaseUrl(baseUrl);
        String[] identity = getOrCreateIdentity(this.context);
        this.deviceId = identity[0];
        this.deviceToken = identity[1];
        this.http = new OkHttpClient.Builder()
                .connectTimeout(6, TimeUnit.SECONDS)
                .readTimeout(8, TimeUnit.SECONDS)
                .writeTimeout(6, TimeUnit.SECONDS)
                .pingInterval(15, TimeUnit.SECONDS)
                .retryOnConnectionFailure(true)
                .build();
    }

    static ScreenBridgeRelayClient create(Context context, String baseUrl) {
        return new ScreenBridgeRelayClient(context, baseUrl);
    }

    static String normalizeBaseUrl(String value) {
        if (value == null) throw new IllegalArgumentException("Relay URL is empty");
        String s = value.trim();
        while (s.endsWith("/")) s = s.substring(0, s.length() - 1);
        HttpUrl url = HttpUrl.parse(s);
        if (url == null || !url.isHttps() || url.encodedPath().length() > 1
                || url.query() != null || url.fragment() != null) {
            throw new IllegalArgumentException("Use the relay's HTTPS origin, for example https://your-relay.example");
        }
        return url.toString().replaceAll("/+$", "");
    }

    static String[] getOrCreateIdentity(Context context) {
        android.content.SharedPreferences prefs = context.getSharedPreferences("bridge", Context.MODE_PRIVATE);
        String id = prefs.getString("relay_device_id", "");
        String secret = prefs.getString("relay_device_token", "");
        if (id.isEmpty() || secret.length() < 48) {
            id = UUID.randomUUID().toString();
            byte[] random = new byte[32];
            new SecureRandom().nextBytes(random);
            StringBuilder hex = new StringBuilder(64);
            for (byte b : random) hex.append(String.format(Locale.US, "%02x", b & 0xff));
            secret = hex.toString();
            prefs.edit().putString("relay_device_id", id).putString("relay_device_token", secret).apply();
        }
        return new String[]{id, secret};
    }

    void start() {
        stopped = false;
        registered = false;
        setStatus("Registrando dispositivo…", "", "");
        new Thread(this::connectOrRegister, "ScreenBridge-RelayConnect").start();
    }

    void stop() {
        stopped = true;
        authenticated = false;
        main.removeCallbacks(reconnectTask);
        reconnectScheduled = false;
        WebSocket ws = socket;
        socket = null;
        if (ws != null) ws.close(1000, "User disconnected");
        http.dispatcher().cancelAll();
        setStatus("Desconectado", "", "");
        http.connectionPool().evictAll();
    }

    boolean isAuthenticated() { return authenticated; }

    private void connectOrRegister() {
        if (stopped) return;
        if (!registered) {
            if (!registerDevice()) {
                scheduleReconnect();
                return;
            }
        }
        if (stopped) return;
        connectWebSocket();
    }

    private boolean registerDevice() {
        try {
            JSONObject payload = new JSONObject();
            payload.put("deviceId", deviceId);
            payload.put("deviceToken", deviceToken);
            RequestBody body = RequestBody.create(payload.toString(), JSON);
            Request request = new Request.Builder()
                    .url(baseUrl + "/api/device/pair/start")
                    .post(body)
                    .header("Accept", "application/json")
                    .build();
            try (Response response = http.newCall(request).execute()) {
                String text = response.body() == null ? "" : response.body().string();
                JSONObject result = new JSONObject(text);
                if (!response.isSuccessful() || !result.optBoolean("ok", false)) {
                    throw new IllegalStateException(result.optString("error", "Relay registration failed (HTTP " + response.code() + ")"));
                }
                registered = true;
                reconnectDelayMs = 1500;
                String code = result.optString("pairCode", "");
                setStatus(code.isEmpty() ? "Registrado; conectando…" : "Código de pareamento ativo", code, "");
                return true;
            }
        } catch (Exception e) {
            Log.w(TAG, "Device registration failed", e);
            setStatus("Relay indisponível", "", safeMessage(e));
            return false;
        }
    }

    private void connectWebSocket() {
        if (stopped || socket != null) return;
        try {
            Request request = new Request.Builder()
                    .url(baseUrl.replaceFirst("^https://", "wss://") + "/ws/device")
                    .build();
            setStatus("Conectando ao relay…", currentPairCode(), "");
            socket = http.newWebSocket(request, this);
        } catch (Exception e) {
            socket = null;
            authenticated = false;
            setStatus("Erro de conexão", currentPairCode(), safeMessage(e));
            scheduleReconnect();
        }
    }

    @Override public void onOpen(WebSocket webSocket, Response response) {
        socket = webSocket;
        try {
            JSONObject auth = new JSONObject();
            auth.put("type", "auth");
            auth.put("role", "device");
            auth.put("deviceId", deviceId);
            auth.put("token", deviceToken);
            webSocket.send(auth.toString());
            setStatus("Autenticando dispositivo…", currentPairCode(), "");
        } catch (Exception e) {
            setStatus("Erro de autenticação", currentPairCode(), safeMessage(e));
            webSocket.close(1008, "Authentication message failed");
        }
    }

    @Override public void onMessage(WebSocket webSocket, String text) {
        try {
            JSONObject message = new JSONObject(text);
            String type = message.optString("type", "");
            if ("authenticated".equals(type) && "device".equals(message.optString("role", "device"))) {
                authenticated = true;
                reconnectDelayMs = 1500;
                setStatus("Relay conectado — pronto para parear", currentPairCode(), "");
            } else if ("error".equals(type) || "auth-error".equals(type)) {
                authenticated = false;
                setStatus("Relay recusou a conexão", currentPairCode(), message.optString("error", "Erro de autenticação"));
            } else if ("command".equals(type)) {
                handleCommand(webSocket, message);
            } else if ("stop-control".equals(type)) {
                sendResult(webSocket, message.optString("id", ""), "stop-control", false, "Controle remoto encerrado pelo relay");
                stop();
            }
        } catch (Exception e) {
            Log.w(TAG, "Ignoring malformed relay message", e);
        }
    }

    private void handleCommand(WebSocket ws, JSONObject message) {
        String id = message.optString("id", "");
        String op = message.optString("op", "");
        boolean accepted = false;
        String detail = "";
        try {
            if (!authenticated || stopped) {
                detail = "Remote session is not active";
            } else if ("tap".equals(op)) {
                accepted = ScreenControlAccessibilityService.tap(message.getInt("x"), message.getInt("y"));
                if (!accepted) detail = "Accessibility unavailable or coordinates outside the display";
            } else if ("swipe".equals(op)) {
                int x1 = message.getInt("x1");
                int y1 = message.getInt("y1");
                int x2 = message.getInt("x2");
                int y2 = message.getInt("y2");
                long duration = message.optLong("ms", 180);
                accepted = ScreenControlAccessibilityService.swipe(x1, y1, x2, y2, duration);
                if (!accepted) detail = "Accessibility unavailable, coordinates invalid, or duration outside 1–10000 ms";
            } else if ("back".equals(op)) {
                accepted = ScreenControlAccessibilityService.globalAction(
                        android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK);
                if (!accepted) detail = "Accessibility service unavailable";
            } else if ("home".equals(op)) {
                accepted = ScreenControlAccessibilityService.globalAction(
                        android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_HOME);
                if (!accepted) detail = "Accessibility service unavailable";
            } else if ("stop-control".equals(op)) {
                sendResult(ws, id, op, true, "Remote control disconnected by request");
                stop();
                return;
            } else {
                detail = "Unsupported operation";
            }
        } catch (Exception e) {
            detail = safeMessage(e);
        }
        sendResult(ws, id, op, accepted, detail);
    }

    private void sendResult(WebSocket ws, String id, String op, boolean accepted, String detail) {
        try {
            JSONObject result = new JSONObject();
            result.put("type", "action-result");
            result.put("id", id);
            result.put("op", op);
            result.put("accepted", accepted);
            result.put("detail", detail == null ? "" : detail);
            result.put("elapsedRealtimeMs", SystemClock.elapsedRealtime());
            ws.send(result.toString());
        } catch (Exception ignored) { }
    }

    void offerFrame(byte[] jpeg, long capturedAtMs) {
        WebSocket ws = socket;
        if (jpeg == null || jpeg.length < 4 || !authenticated || stopped || ws == null) return;
        long now = SystemClock.elapsedRealtime();
        if (capturedAtMs <= 0 || now - capturedAtMs > 1800L) return;
        if (now - lastFrameSentAtMs < FRAME_INTERVAL_MS) return;
        if (ws.queueSize() > MAX_SOCKET_QUEUE_BYTES) return; // discard stale frames, never build a queue
        if (ws.send(ByteString.of(jpeg))) {
            lastFrameSentAtMs = now;
            lastFrameAtMs = capturedAtMs;
        }
    }

    @Override public void onMessage(WebSocket webSocket, ByteString bytes) {
        // The Android endpoint is a frame publisher; commands arrive as validated JSON.
    }

    @Override public void onClosing(WebSocket webSocket, int code, String reason) {
        webSocket.close(code, reason);
    }

    @Override public void onClosed(WebSocket webSocket, int code, String reason) {
        if (socket == webSocket) socket = null;
        authenticated = false;
        if (!stopped) {
            setStatus("Relay reconectando…", currentPairCode(), reason == null ? "" : reason);
            scheduleReconnect();
        }
    }

    @Override public void onFailure(WebSocket webSocket, Throwable t, Response response) {
        if (socket == webSocket) socket = null;
        authenticated = false;
        if (!stopped) {
            String detail = t == null ? "Connection lost" : safeMessage(t);
            setStatus("Relay reconectando…", currentPairCode(), detail);
            scheduleReconnect();
        }
    }

    private void scheduleReconnect() {
        if (stopped || reconnectScheduled) return;
        reconnectScheduled = true;
        int delay = reconnectDelayMs;
        reconnectDelayMs = Math.min(30000, reconnectDelayMs * 2);
        main.postDelayed(reconnectTask, delay);
    }

    private String currentPairCode() {
        return context.getSharedPreferences("bridge", Context.MODE_PRIVATE).getString("relay_pair_code", "");
    }

    private void setStatus(String status, String pairCode, String error) {
        android.content.SharedPreferences.Editor editor = context.getSharedPreferences("bridge", Context.MODE_PRIVATE)
                .edit().putString("relay_state", status == null ? "" : status)
                .putString("relay_error", error == null ? "" : error);
        if (pairCode != null) editor.putString("relay_pair_code", pairCode);
        editor.apply();
    }

    private static String safeMessage(Throwable e) {
        if (e == null) return "";
        String message = e.getMessage();
        return message == null || message.isEmpty() ? e.getClass().getSimpleName() : message;
    }

    private static String safeMessage(Exception e) {
        if (e == null) return "";
        String message = e.getMessage();
        return message == null || message.isEmpty() ? e.getClass().getSimpleName() : message;
    }
}
