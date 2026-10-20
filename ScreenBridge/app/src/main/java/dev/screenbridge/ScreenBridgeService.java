package dev.screenbridge;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.WindowManager;
import android.view.Gravity;
import android.provider.Settings;
import android.view.View;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URI;
import java.net.URLDecoder;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public class ScreenBridgeService extends Service {
    public static final String EXTRA_RESULT_CODE = "projection_result_code";
    public static final String EXTRA_RESULT_DATA = "projection_result_data";
    public static final String ACTION_TOGGLE_OVERLAY = "dev.screenbridge.action.TOGGLE_OVERLAY";
    public static final String ACTION_START_RECORDING = "dev.screenbridge.action.START_RECORDING";
    public static final String ACTION_STOP_RECORDING = "dev.screenbridge.action.STOP_RECORDING";
    private static volatile String lastRecordingUri = "";
    private static final String CHANNEL = "screenbridge_capture";
    private static final int PORT = 18766;
    private static final long FRESH_FRAME_MAX_AGE_MS = 1800L;
    private static final long FRAME_WAIT_TIMEOUT_MS = 900L;
    private static final long GPT_STATE_TTL_MS = 90000L;
    private static final long CAPTURE_STALL_THRESHOLD_MS = 5000L;
    private static final long CAPTURE_RECOVERY_COOLDOWN_MS = 15000L;
    private static final long CAPTURE_RECOVERY_WINDOW_MS = 120000L;
    private static final int CAPTURE_RECOVERY_MAX_ATTEMPTS = 3;
    private static volatile boolean running = false;

    private final AtomicBoolean stopped = new AtomicBoolean(false);
    private final ExecutorService clients = Executors.newFixedThreadPool(4);
    private final Handler main = new Handler(Looper.getMainLooper());
    private HandlerThread captureThread;
    private Handler captureHandler;
    private ServerSocket server;
    private Thread acceptThread;
    private volatile MediaProjection projection;
    private volatile VirtualDisplay virtualDisplay;
    private volatile ImageReader imageReader;
    private DisplayManager displayManager;
    private String token = "";
    private volatile int width, height, density;
    private final Object captureLock = new Object();
    private final Object frameLock = new Object();
    private volatile byte[] latestJpeg;
    private volatile ScreenBridgeRecorder recorder;
    // Frame timestamps and state TTLs use monotonic elapsed time, not wall clock.
    private volatile long latestFrameAt;
    private volatile long lastImageCallbackAt;
    private volatile long frameCount;
    private volatile long imageCallbackCount;
    private volatile long nullImageCount;
    private volatile long encodingFailureCount;
    private volatile long captureConfigCount;
    private volatile long projectionStartedAtMs;
    private volatile long lastCaptureRecoveryAtMs;
    private volatile long captureRecoveryWindowStartedAtMs;
    private volatile int captureRecoveryAttemptsInWindow;
    private volatile long captureRecoveryAttemptCount;
    private volatile long captureRecoverySuccessCount;
    private volatile long captureRecoveryFailureCount;
    private volatile boolean recoveryAwaitingFreshFrame;
    private volatile String captureError = "";
    private volatile String lastControlError = "";
    private volatile String lastAction = "none";
    private volatile boolean lastActionAccepted;
    private volatile long chatGptStateUpdatedAtMs;
    private volatile boolean projectionActive;
    private volatile long startedAt;
    private volatile long lastActionAtMs;
    private volatile String lastOverlayState = "";
    private volatile String chatGptState = "unknown";
    private static volatile boolean overlayShown = false;
    private WindowManager overlayWindowManager;
    private TextView statusOverlay;
    private WindowManager.LayoutParams overlayParams;
    private final Handler overlayHandler = new Handler(Looper.getMainLooper());
    private final Runnable captureWatchdog = new Runnable() {
        @Override public void run() {
            runCaptureWatchdog();
            if (running) main.postDelayed(this, 2000L);
        }
    };
    private final Runnable overlayTicker = new Runnable() {
        @Override public void run() {
            updateStatusOverlay();
            if (statusOverlay != null) overlayHandler.postDelayed(this, 350);
        }
    };

    public static boolean isRunning() { return running; }
    public static boolean isOverlayShown() { return overlayShown; }

    @Override public void onCreate() {
        super.onCreate();
        running = true;
        startedAt = SystemClock.elapsedRealtime();
        token = getSharedPreferences("bridge", MODE_PRIVATE).getString("token", "");
        captureThread = new HandlerThread("ScreenBridge-FrameEncoder");
        captureThread.start();
        captureHandler = new Handler(captureThread.getLooper());
        createNotificationChannel();
        Notification notification = new Notification.Builder(this, CHANNEL)
                .setContentTitle("ScreenBridge ativo")
                .setContentText("Captura local autorizada; toque para abrir o app.")
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(11, notification, android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        } else startForeground(11, notification);
        displayManager = (DisplayManager)getSystemService(DISPLAY_SERVICE);
        if (displayManager != null) displayManager.registerDisplayListener(displayListener, main);
        main.removeCallbacks(captureWatchdog);
        main.postDelayed(captureWatchdog, 2000L);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_TOGGLE_OVERLAY.equals(intent.getAction())) {
            main.post(this::toggleStatusOverlay);
        } else if (intent != null && ACTION_START_RECORDING.equals(intent.getAction())) {
            startRecording();
        } else if (intent != null && ACTION_STOP_RECORDING.equals(intent.getAction())) {
            new Thread(this::stopRecording, "ScreenBridge-StopRecorder").start();
        }
        if (intent != null && intent.hasExtra(EXTRA_RESULT_DATA)) {
            int resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0);
            Intent resultData = getIntentExtra(intent, EXTRA_RESULT_DATA);
            if (resultData != null && projection == null) startProjection(resultCode, resultData);
        }
        startHttpServer();
        if ((intent == null || !ACTION_TOGGLE_OVERLAY.equals(intent.getAction()))
                && getSharedPreferences("bridge", MODE_PRIVATE).getBoolean("overlay_enabled", false)) {
            main.postDelayed(this::ensureStatusOverlay, 250);
        }
        return START_STICKY;
    }

    @SuppressWarnings("deprecation")
    private Intent getIntentExtra(Intent i, String key) {
        if (Build.VERSION.SDK_INT >= 33) return i.getParcelableExtra(key, Intent.class);
        return i.getParcelableExtra(key);
    }

    private void startProjection(int resultCode, Intent data) {
        try {
            MediaProjectionManager manager = (MediaProjectionManager)getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            MediaProjection newProjection = manager.getMediaProjection(resultCode, data);
            if (newProjection == null) {
                captureError = "MediaProjection unavailable";
                return;
            }
            projection = newProjection;
            projectionActive = true;
            projectionStartedAtMs = SystemClock.elapsedRealtime();
            recoveryAwaitingFreshFrame = false;
            captureError = "";
            newProjection.registerCallback(new MediaProjection.Callback() {
                @Override public void onStop() {
                    projectionActive = false;
                    projection = null;
                    projectionStartedAtMs = 0;
                    recoveryAwaitingFreshFrame = false;
                    captureError = "Screen capture permission was revoked; reopen ScreenBridge and authorize again";
                    stopRecording();
        synchronized (captureLock) { releaseCapture(); }
                }
            }, main);
            configureCapture();
        } catch (SecurityException e) {
            projectionActive = false;
            captureError = "Capture permission expired; authorize again";
            stopSelf();
        } catch (Exception e) {
            projectionActive = false;
            captureError = "Capture setup failed: " + e.getClass().getSimpleName();
        }
    }

    private void configureCapture() {
        MediaProjection p = projection;
        if (p == null || !projectionActive) return;
        DisplayMetrics metrics = new DisplayMetrics();
        WindowManager wm = (WindowManager)getSystemService(WINDOW_SERVICE);
        if (wm != null) wm.getDefaultDisplay().getRealMetrics(metrics);
        int newW = Math.max(1, metrics.widthPixels);
        int newH = Math.max(1, metrics.heightPixels);
        int newD = Math.max(1, metrics.densityDpi);
        synchronized (captureLock) {
            if (virtualDisplay != null && imageReader != null && width == newW && height == newH && density == newD) return;
            width = newW;
            height = newH;
            density = newD;
            captureConfigCount++;
            ImageReader newReader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3);
            newReader.setOnImageAvailableListener(this::onImageAvailable, captureHandler);
            if (virtualDisplay == null) {
                // Android 14+ expects one VirtualDisplay per MediaProjection session.
                virtualDisplay = p.createVirtualDisplay("ScreenBridgeCapture", width, height, density,
                        DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, newReader.getSurface(), null, main);
            } else {
                virtualDisplay.resize(width, height, density);
                virtualDisplay.setSurface(newReader.getSurface());
            }
            ImageReader oldReader = imageReader;
            imageReader = newReader;
            if (oldReader != null) oldReader.close();
            if (!recoveryAwaitingFreshFrame) captureError = "";
        }
    }

    private void onImageAvailable(ImageReader reader) {
        // Ignore callbacks already queued by a reader that the watchdog replaced.
        if (reader != imageReader) return;
        lastImageCallbackAt = SystemClock.elapsedRealtime();
        imageCallbackCount++;
        Image image = null;
        Bitmap padded = null;
        Bitmap cropped = null;
        try {
            image = reader.acquireLatestImage();
            if (image == null) {
                nullImageCount++;
                return;
            }
            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer buffer = plane.getBuffer();
            int pixelStride = plane.getPixelStride();
            int rowStride = plane.getRowStride();
            int paddedWidth = rowStride / pixelStride;
            padded = Bitmap.createBitmap(paddedWidth, image.getHeight(), Bitmap.Config.ARGB_8888);
            buffer.rewind();
            padded.copyPixelsFromBuffer(buffer);
            cropped = Bitmap.createBitmap(padded, 0, 0, image.getWidth(), image.getHeight());
            ScreenBridgeRecorder activeRecorder = recorder;
            if (activeRecorder != null) activeRecorder.submitFrame(cropped);
            ByteArrayOutputStream out = new ByteArrayOutputStream(Math.max(16 * 1024, width * height / 8));
            cropped.compress(Bitmap.CompressFormat.JPEG, 72, out);
            byte[] jpeg = out.toByteArray();
            if (jpeg.length > 4) {
                synchronized (frameLock) {
                    latestJpeg = jpeg;
                    latestFrameAt = SystemClock.elapsedRealtime();
                    frameCount++;
                }
                // Clear transient capture errors only after a frame is successfully encoded.
                captureError = "";
                if (recoveryAwaitingFreshFrame) {
                    captureRecoverySuccessCount++;
                    recoveryAwaitingFreshFrame = false;
                }
            } else {
                encodingFailureCount++;
                captureError = "Frame encoding returned an empty JPEG";
            }
        } catch (Exception e) {
            encodingFailureCount++;
            captureError = "Frame encoding failed: " + e.getClass().getSimpleName();
            Log.w("ScreenBridge", captureError, e);
        } finally {
            if (image != null) image.close();
            if (cropped != null && cropped != padded) cropped.recycle();
            if (padded != null) padded.recycle();
        }
    }

    private void runCaptureWatchdog() {
        long now = SystemClock.elapsedRealtime();
        if (!running || !projectionActive || projection == null) return;

        long frameAt = latestFrameAt;
        if (frameAt > 0 && now >= frameAt && now - frameAt <= CAPTURE_STALL_THRESHOLD_MS) return;
        if (frameAt == 0 && projectionStartedAtMs > 0
                && now - projectionStartedAtMs <= CAPTURE_STALL_THRESHOLD_MS) return;
        if (lastCaptureRecoveryAtMs > 0
                && now - lastCaptureRecoveryAtMs < CAPTURE_RECOVERY_COOLDOWN_MS) return;

        if (captureRecoveryWindowStartedAtMs == 0
                || now - captureRecoveryWindowStartedAtMs >= CAPTURE_RECOVERY_WINDOW_MS) {
            captureRecoveryWindowStartedAtMs = now;
            captureRecoveryAttemptsInWindow = 0;
        }
        if (captureRecoveryAttemptsInWindow >= CAPTURE_RECOVERY_MAX_ATTEMPTS) {
            captureError = "Capture stalled; automatic recovery limit reached";
            recoveryAwaitingFreshFrame = false;
            return;
        }

        lastCaptureRecoveryAtMs = now;
        captureRecoveryAttemptsInWindow++;
        captureRecoveryAttemptCount++;
        recoveryAwaitingFreshFrame = true;
        captureError = "Capture stalled; automatic surface recovery in progress";
        try {
            synchronized (captureLock) {
                if (!projectionActive || projection == null) return;
                VirtualDisplay display = virtualDisplay;
                ImageReader oldReader = imageReader;
                if (display == null || oldReader == null) {
                    // Reuse the current MediaProjection; never start a second projection.
                    configureCapture();
                } else {
                    ImageReader replacement = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3);
                    try {
                        replacement.setOnImageAvailableListener(this::onImageAvailable, captureHandler);
                        display.setSurface(replacement.getSurface());
                        imageReader = replacement;
                        replacement = null;
                    } finally {
                        if (replacement != null) {
                            replacement.setOnImageAvailableListener(null, null);
                            replacement.close();
                        }
                    }
                    oldReader.setOnImageAvailableListener(null, null);
                    oldReader.close();
                }
            }
            if (recoveryAwaitingFreshFrame) captureError = "Capture stalled; automatic surface recovery attempted";
            Log.w("ScreenBridge", "Capture stalled; automatic surface recovery attempted ("
                    + captureRecoveryAttemptsInWindow + "/" + CAPTURE_RECOVERY_MAX_ATTEMPTS + ")");
        } catch (Exception e) {
            captureRecoveryFailureCount++;
            recoveryAwaitingFreshFrame = false;
            captureError = "Automatic capture recovery failed: " + e.getClass().getSimpleName();
            Log.e("ScreenBridge", captureError, e);
        }
    }

    private void releaseCapture() {
        if (virtualDisplay != null) {
            try { virtualDisplay.setSurface(null); } catch (Exception ignored) { }
            virtualDisplay.release();
            virtualDisplay = null;
        }
        if (imageReader != null) {
            imageReader.setOnImageAvailableListener(null, null);
            imageReader.close();
            imageReader = null;
        }
        latestJpeg = null;
        latestFrameAt = 0;
        lastImageCallbackAt = 0;
        recoveryAwaitingFreshFrame = false;
    }

    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int id) { }
        @Override public void onDisplayRemoved(int id) { }
        @Override public void onDisplayChanged(int id) {
            if (id == android.view.Display.DEFAULT_DISPLAY) main.postDelayed(() -> {
                try { configureCapture(); } catch (Exception e) {
                    captureError = "Display reconfiguration failed: " + e.getClass().getSimpleName();
                }
            }, 250);
        }
    };

    private int dp(int value) {
        return (int)(value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private void toggleStatusOverlay() {
        if (statusOverlay != null) {
            removeStatusOverlay();
            getSharedPreferences("bridge", MODE_PRIVATE).edit().putBoolean("overlay_enabled", false).apply();
        } else {
            if (!Settings.canDrawOverlays(this)) return;
            getSharedPreferences("bridge", MODE_PRIVATE).edit().putBoolean("overlay_enabled", true).apply();
            ensureStatusOverlay();
        }
    }

    private void ensureStatusOverlay() {
        if (statusOverlay != null || !Settings.canDrawOverlays(this)) return;
        try {
            overlayWindowManager = (WindowManager)getSystemService(WINDOW_SERVICE);
            TextView pill = new TextView(this);
            pill.setText("SB · INICIANDO | CTRL · … | GPT · DESCONHECIDO");
            pill.setTextSize(10.5f);
            pill.setTextColor(0xFFFFFFFF);
            pill.setTypeface(null, 1);
            pill.setPadding(dp(9), dp(5), dp(9), dp(5));
            pill.setElevation(dp(4));
            pill.setBackground(statusBackground(0xFF34313A));
            pill.setAlpha(0.94f);
            pill.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
            int flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                    | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN;
            overlayParams = new WindowManager.LayoutParams(
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.WRAP_CONTENT,
                    WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                    flags, PixelFormat.TRANSLUCENT);
            overlayParams.gravity = Gravity.TOP | Gravity.RIGHT;
            overlayParams.x = dp(8);
            overlayParams.y = dp(58);
            overlayWindowManager.addView(pill, overlayParams);
            statusOverlay = pill;
            overlayShown = true;
            lastOverlayState = "";
            updateStatusOverlay();
            overlayHandler.removeCallbacks(overlayTicker);
            overlayHandler.postDelayed(overlayTicker, 350);
        } catch (Exception e) {
            statusOverlay = null;
            overlayShown = false;
            Log.w("ScreenBridge", "Status overlay unavailable", e);
        }
    }

    private GradientDrawable statusBackground(int color) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(dp(18));
        drawable.setStroke(dp(1), 0x55FFFFFF);
        return drawable;
    }

    private void updateStatusOverlay() {
        TextView pill = statusOverlay;
        if (pill == null) return;
        long now = SystemClock.elapsedRealtime();
        long frameAge = latestFrameAt == 0 ? Long.MAX_VALUE : Math.max(0, now - latestFrameAt);
        String captureLabel;
        String captureState;
        int color;

        if (!running) {
            captureState = "stopped"; captureLabel = "SB · INATIVO"; color = 0xFF34313A;
        } else if (!projectionActive || projection == null) {
            captureState = "no-projection"; captureLabel = "SB · SEM PROJEÇÃO"; color = 0xFF8B2D22;
        } else if (!captureError.isEmpty()) {
            captureState = "error"; captureLabel = "SB · ERRO"; color = 0xFFB3261E;
        } else if (latestJpeg == null || frameAge > 3000L) {
            captureState = "no-frame"; captureLabel = "SB · SEM FRAME"; color = 0xFF8B2D22;
        } else if (frameAge > FRESH_FRAME_MAX_AGE_MS) {
            captureState = "delayed"; captureLabel = "SB · FRAME ATRASADO"; color = 0xFF8A4F00;
        } else if (lastActionAtMs > 0 && now - lastActionAtMs < 900L) {
            captureState = "working"; captureLabel = "SB · EXECUTANDO"; color = 0xFF8A4F00;
        } else if (lastActionAtMs > 0 && now - lastActionAtMs < 8000L) {
            captureState = "active"; captureLabel = "SB · ATIVO"; color = 0xFF146C43;
        } else {
            captureState = "idle"; captureLabel = "SB · OCIOSO"; color = 0xFF34313A;
        }

        String controlLabel;
        String controlState;
        if (!ScreenControlAccessibilityService.isConnected()) {
            controlState = "offline"; controlLabel = "CTRL · OFF";
        } else if (!lastControlError.isEmpty()) {
            controlState = "error"; controlLabel = "CTRL · ERRO";
        } else if (lastActionAtMs > 0 && now - lastActionAtMs < 900L) {
            controlState = "working"; controlLabel = "CTRL · EXECUTANDO";
        } else {
            controlState = "ready"; controlLabel = "CTRL · PRONTO";
        }

        String gpt = getEffectiveChatGptState();
        String gptLabel;
        if ("thinking".equals(gpt)) gptLabel = "GPT · PENSANDO";
        else if ("working".equals(gpt)) gptLabel = "GPT · TRABALHANDO";
        else if ("done".equals(gpt)) gptLabel = "GPT · CONCLUÍDO";
        else gptLabel = "GPT · DESCONHECIDO";

        String combinedState = captureState + "|" + controlState + "|" + gpt;
        if (!combinedState.equals(lastOverlayState)) {
            pill.setText(captureLabel + "  |  " + controlLabel + "  |  " + gptLabel);
            pill.setBackground(statusBackground(color));
            pill.setContentDescription("Captura: " + captureLabel + ". Controle: " + controlLabel + ". ChatGPT: " + gptLabel);
            lastOverlayState = combinedState;
        }
    }

    private void removeStatusOverlay() {
        overlayHandler.removeCallbacks(overlayTicker);
        TextView pill = statusOverlay;
        statusOverlay = null;
        overlayShown = false;
        lastOverlayState = "";
        if (pill != null && overlayWindowManager != null) {
            try { overlayWindowManager.removeView(pill); } catch (Exception ignored) { }
        }
        overlayParams = null;
        overlayWindowManager = null;
    }

    private void startHttpServer() {
        if (acceptThread != null && acceptThread.isAlive()) return;
        stopped.set(false);
        acceptThread = new Thread(() -> {
            try (ServerSocket ss = new ServerSocket(PORT, 16, InetAddress.getByName("127.0.0.1"))) {
                server = ss;
                while (!stopped.get()) {
                    Socket socket = ss.accept();
                    clients.execute(() -> handle(socket));
                }
            } catch (Exception e) {
                if (!stopped.get()) {
                    captureError = "Local HTTP server failed: " + e.getClass().getSimpleName();
                    main.post(() -> android.widget.Toast.makeText(this, "ScreenBridge: " + e.getMessage(), android.widget.Toast.LENGTH_LONG).show());
                }
            }
        }, "ScreenBridge-HTTP");
        acceptThread.setDaemon(true);
        acceptThread.start();
    }

    private void handle(Socket socket) {
        try (Socket s = socket) {
            s.setSoTimeout(3000);
            BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.US_ASCII));
            String request = in.readLine();
            if (request == null || !request.startsWith("GET ")) {
                respond(s, 405, "text/plain", "GET only".getBytes(StandardCharsets.UTF_8));
                return;
            }
            String target = request.split(" ", 3)[1];
            while (true) { String line = in.readLine(); if (line == null || line.isEmpty()) break; }
            URI uri = URI.create(target);
            String path = uri.getPath();
            if ("/health".equals(path) || "/diagnostics".equals(path)) {
                JSONObject j = new JSONObject();
                long now = SystemClock.elapsedRealtime();
                long frameAge = latestFrameAt == 0 ? -1 : Math.max(0, now - latestFrameAt);
                long callbackAge = lastImageCallbackAt == 0 ? -1 : Math.max(0, now - lastImageCallbackAt);
                long actionAge = lastActionAtMs == 0 ? -1 : Math.max(0, now - lastActionAtMs);
                long gptAge = chatGptStateUpdatedAtMs == 0 ? -1 : Math.max(0, now - chatGptStateUpdatedAtMs);
                boolean controlReady = ScreenControlAccessibilityService.isConnected();
                boolean freshFrame = latestJpeg != null && latestFrameAt > 0 && frameAge >= 0 && frameAge <= FRESH_FRAME_MAX_AGE_MS;
                j.put("ok", true);
                j.put("running", running);
                j.put("projection", projectionActive && projection != null);
                j.put("accessibility", controlReady);
                j.put("controlReady", controlReady);
                j.put("captureReady", freshFrame);
                j.put("recording", recorder != null && recorder.isAccepting());
                j.put("lastRecordingUriAvailable", !lastRecordingUri.isEmpty());
                j.put("cachedFrameAvailable", latestJpeg != null);
                j.put("frameAgeMs", frameAge);
                j.put("frameFreshnessLimitMs", FRESH_FRAME_MAX_AGE_MS);
                j.put("frameCount", frameCount);
                j.put("imageCallbackCount", imageCallbackCount);
                j.put("nullImageCount", nullImageCount);
                j.put("encodingFailureCount", encodingFailureCount);
                j.put("captureConfigCount", captureConfigCount);
                j.put("captureRecoveryAttemptCount", captureRecoveryAttemptCount);
                j.put("captureRecoverySuccessCount", captureRecoverySuccessCount);
                j.put("captureRecoveryFailureCount", captureRecoveryFailureCount);
                j.put("captureRecoveryAttemptsInWindow", captureRecoveryAttemptsInWindow);
                j.put("lastCaptureRecoveryAgeMs", lastCaptureRecoveryAtMs == 0 ? -1 : Math.max(0, now - lastCaptureRecoveryAtMs));
                j.put("recoveryAwaitingFreshFrame", recoveryAwaitingFreshFrame);
                j.put("lastImageCallbackAgeMs", callbackAge);
                j.put("virtualDisplayActive", virtualDisplay != null);
                j.put("imageReaderActive", imageReader != null);
                j.put("captureError", captureError);
                j.put("lastAction", lastAction);
                j.put("lastActionAccepted", lastActionAccepted);
                j.put("lastActionAgeMs", actionAge);
                j.put("lastControlError", lastControlError);
                j.put("chatGptState", getEffectiveChatGptState());
                j.put("chatGptStateAgeMs", gptAge);
                j.put("chatGptStateTtlMs", GPT_STATE_TTL_MS);
                j.put("serverUptimeMs", Math.max(0, SystemClock.elapsedRealtime() - startedAt));
                int[] size = ScreenControlAccessibilityService.screenSize();
                j.put("controlWidth", size[0] > 0 ? size[0] : width);
                j.put("controlHeight", size[1] > 0 ? size[1] : height);
                respond(s, 200, "application/json; charset=utf-8", j.toString().getBytes(StandardCharsets.UTF_8));
                return;
            }
            String prefix = "/" + token + "/";
            if (token.isEmpty() || !path.startsWith(prefix)) {
                respond(s, 403, "text/plain", "Forbidden".getBytes(StandardCharsets.UTF_8));
                return;
            }
            String route = path.substring(prefix.length());
            if ("gpt-state".equals(route)) {
                Map<String,String> q = query(uri.getRawQuery());
                String next = q.getOrDefault("state", "");
                if (!("thinking".equals(next) || "working".equals(next) || "done".equals(next) || "unknown".equals(next))) {
                    respond(s, 400, "application/json", "{\"ok\":false,\"error\":\"invalid state\"}".getBytes(StandardCharsets.UTF_8));
                    return;
                }
                chatGptState = next;
                chatGptStateUpdatedAtMs = SystemClock.elapsedRealtime();
                main.post(this::updateStatusOverlay);
                JSONObject j = new JSONObject();
                j.put("ok", true);
                j.put("chatGptState", getEffectiveChatGptState());
                j.put("chatGptStateAgeMs", 0);
                j.put("chatGptStateTtlMs", GPT_STATE_TTL_MS);
                respond(s, 200, "application/json; charset=utf-8", j.toString().getBytes(StandardCharsets.UTF_8));
                return;
            }
            if ("recording".equals(route)) {
                Map<String,String> q = query(uri.getRawQuery());
                String op = q.getOrDefault("op", "status");
                JSONObject j = new JSONObject();
                if ("start".equals(op)) {
                    boolean ok = startRecording();
                    j.put("ok", ok);
                    j.put("recording", recorder != null && recorder.isAccepting());
                    j.put("error", ok ? "" : captureError);
                    respond(s, ok ? 200 : 409, "application/json; charset=utf-8", j.toString().getBytes(StandardCharsets.UTF_8));
                } else if ("stop".equals(op)) {
                    stopRecording();
                    j.put("ok", !lastRecordingUri.isEmpty());
                    j.put("recording", recorder != null && recorder.isAccepting());
                    j.put("uri", lastRecordingUri);
                    j.put("error", captureError);
                    respond(s, lastRecordingUri.isEmpty() ? 409 : 200, "application/json; charset=utf-8", j.toString().getBytes(StandardCharsets.UTF_8));
                } else {
                    j.put("ok", true);
                    j.put("recording", recorder != null && recorder.isAccepting());
                    j.put("uri", lastRecordingUri);
                    respond(s, 200, "application/json; charset=utf-8", j.toString().getBytes(StandardCharsets.UTF_8));
                }
                return;
            }
            if ("screen.jpg".equals(route)) {
                byte[] jpg = awaitFreshFrame(FRAME_WAIT_TIMEOUT_MS);
                if (jpg == null) {
                    JSONObject j = new JSONObject();
                    long now = SystemClock.elapsedRealtime();
                    long age = latestFrameAt == 0 ? -1 : Math.max(0, now - latestFrameAt);
                    long callbackAge = lastImageCallbackAt == 0 ? -1 : Math.max(0, now - lastImageCallbackAt);
                    j.put("ok", false);
                    j.put("error", "No fresh frame available");
                    j.put("frameAgeMs", age);
                    j.put("frameFreshnessLimitMs", FRESH_FRAME_MAX_AGE_MS);
                    j.put("frameCount", frameCount);
                    j.put("lastImageCallbackAgeMs", callbackAge);
                    j.put("projection", projectionActive && projection != null);
                    j.put("virtualDisplayActive", virtualDisplay != null);
                    j.put("imageReaderActive", imageReader != null);
                    j.put("captureError", captureError);
                    respond(s, 503, "application/json; charset=utf-8", j.toString().getBytes(StandardCharsets.UTF_8));
                    return;
                }
                respond(s, 200, "image/jpeg", jpg);
            } else if ("ui.json".equals(route)) {
                Map<String,String> q = query(uri.getRawQuery());
                boolean refresh = "1".equals(q.get("refresh"));
                String snapshot = ScreenControlAccessibilityService.uiSnapshotJson(refresh);
                respond(s, 200, "application/json; charset=utf-8", snapshot.getBytes(StandardCharsets.UTF_8));
            } else if ("action".equals(route)) {
                Map<String,String> q = query(uri.getRawQuery());
                String op = q.getOrDefault("op", "");
                boolean accepted;
                try {
                    if ("tap".equals(op)) {
                        accepted = ScreenControlAccessibilityService.tap(number(q, "x"), number(q, "y"));
                    } else if ("swipe".equals(op)) {
                        accepted = ScreenControlAccessibilityService.swipe(number(q, "x1"), number(q, "y1"),
                                number(q, "x2"), number(q, "y2"), number(q, "ms"));
                    } else if ("back".equals(op)) {
                        accepted = ScreenControlAccessibilityService.globalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK);
                    } else if ("home".equals(op)) {
                        accepted = ScreenControlAccessibilityService.globalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_HOME);
                    } else {
                        respond(s, 400, "application/json", "{\"ok\":false,\"error\":\"unsupported action\"}".getBytes(StandardCharsets.UTF_8));
                        return;
                    }
                } catch (IllegalArgumentException e) {
                    respond(s, 400, "application/json", "{\"ok\":false,\"error\":\"invalid parameters\"}".getBytes(StandardCharsets.UTF_8));
                    return;
                }
                if (!accepted) {
                    lastAction = op;
                    lastActionAccepted = false;
                    lastActionAtMs = SystemClock.elapsedRealtime();
                    lastControlError = "Accessibility unavailable or coordinates invalid";
                    respond(s, 409, "application/json", "{\"ok\":false,\"error\":\"accessibility unavailable or coordinates invalid\"}".getBytes(StandardCharsets.UTF_8));
                    return;
                }
                lastActionAtMs = SystemClock.elapsedRealtime();
                lastAction = op;
                lastActionAccepted = accepted;
                if (accepted) lastControlError = "";
                else lastControlError = "Accessibility unavailable or coordinates invalid";
                // Fast-control mode: acknowledge the gesture immediately instead of
                // blocking the HTTP client while waiting for a new screen frame.
                // Add async=0 (or waitFrame=1) to preserve the legacy image-after-action response.
                if ("0".equals(q.get("async")) || "1".equals(q.get("waitFrame"))) {
                    long oldFrame = latestFrameAt;
                    byte[] jpg = awaitNewFrame(oldFrame, 450);
                    if (jpg != null) {
                        respond(s, 200, "image/jpeg", jpg);
                    } else {
                        JSONObject j = new JSONObject();
                        j.put("ok", true);
                        j.put("accepted", true);
                        j.put("frameUpdated", false);
                        j.put("frameAgeMs", latestFrameAt == 0 ? -1 : Math.max(0, SystemClock.elapsedRealtime() - latestFrameAt));
                        j.put("detail", captureError);
                        respond(s, 200, "application/json", j.toString().getBytes(StandardCharsets.UTF_8));
                    }
                } else {
                    JSONObject j = new JSONObject();
                    j.put("ok", true);
                    j.put("accepted", true);
                    j.put("frameUpdated", false);
                    j.put("frameAgeMs", latestFrameAt == 0 ? -1 : Math.max(0, SystemClock.elapsedRealtime() - latestFrameAt));
                    j.put("detail", captureError);
                    respond(s, 200, "application/json", j.toString().getBytes(StandardCharsets.UTF_8));
                }
            } else {
                respond(s, 404, "text/plain", "Not found".getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception ignored) { }
    }

    public static String getLastRecordingUri() { return lastRecordingUri; }

    private synchronized boolean startRecording() {
        if (recorder != null && recorder.isAccepting()) return true;
        if (!projectionActive || projection == null || width < 2 || height < 2) {
            captureError = "Authorize screen capture before recording";
            return false;
        }
        try {
            lastRecordingUri = "";
            recorder = new ScreenBridgeRecorder(this, width, height);
            captureError = "";
            return true;
        } catch (Exception e) {
            recorder = null;
            captureError = "Recording could not start: " + e.getClass().getSimpleName() + (e.getMessage() == null ? "" : " (" + e.getMessage() + ")");
            Log.e("ScreenBridge", captureError, e);
            return false;
        }
    }

    private synchronized void stopRecording() {
        ScreenBridgeRecorder current = recorder;
        if (current == null) return;
        recorder = null;
        current.stopAndFinalize();
        android.net.Uri completed = ScreenBridgeRecorder.lastCompletedUri();
        if (completed != null) {
            lastRecordingUri = completed.toString();
            captureError = "";
        } else if (!current.error().isEmpty()) {
            captureError = current.error();
        } else {
            captureError = "Recording ended without a finalized MP4";
        }
        main.post(this::updateStatusOverlay);
    }

    private long ageMs(long timestamp) {
        if (timestamp <= 0) return -1;
        return Math.max(0, SystemClock.elapsedRealtime() - timestamp);
    }

    private String getEffectiveChatGptState() {
        String state = chatGptState;
        long updated = chatGptStateUpdatedAtMs;
        if ("unknown".equals(state) || updated <= 0) return "unknown";
        long age = Math.max(0, SystemClock.elapsedRealtime() - updated);
        return age <= GPT_STATE_TTL_MS ? state : "unknown";
    }

    // Return only a recent cached frame. When stale, wait for a genuinely newer
    // frame; never silently relabel an old JPEG as a current screenshot.
    private byte[] awaitFreshFrame(long timeoutMs) {
        long now = SystemClock.elapsedRealtime();
        byte[] frame = latestJpeg;
        long frameAt = latestFrameAt;
        if (frame != null && frameAt > 0 && now >= frameAt && now - frameAt <= FRESH_FRAME_MAX_AGE_MS) {
            return frame.clone();
        }

        long previousFrameAt = frameAt;
        long until = now + timeoutMs;
        while (SystemClock.elapsedRealtime() < until) {
            frame = latestJpeg;
            frameAt = latestFrameAt;
            now = SystemClock.elapsedRealtime();
            if (frame != null && frameAt > previousFrameAt && now >= frameAt
                    && now - frameAt <= FRESH_FRAME_MAX_AGE_MS) {
                return frame.clone();
            }
            try { Thread.sleep(15); } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return null;
            }
        }
        return null;
    }

    private byte[] awaitNewFrame(long previousFrameAt, long timeoutMs) {
        long until = SystemClock.elapsedRealtime() + timeoutMs;
        while (SystemClock.elapsedRealtime() < until) {
            byte[] frame = latestJpeg;
            long frameAt = latestFrameAt;
            long now = SystemClock.elapsedRealtime();
            if (frame != null && frameAt > previousFrameAt && now >= frameAt
                    && now - frameAt <= FRESH_FRAME_MAX_AGE_MS) return frame.clone();
            try { Thread.sleep(15); } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return null;
            }
        }
        return null;
    }

    private String jsonEscape(String value) {
        if (value == null) return "";
        return value.replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", " ").replace("\r", " ");
    }

    private int number(Map<String,String> q, String key) {
        String v = q.get(key);
        if (v == null || !v.matches("-?[0-9]{1,5}")) throw new IllegalArgumentException("bad " + key);
        return Integer.parseInt(v);
    }

    private Map<String,String> query(String raw) throws Exception {
        Map<String,String> out = new HashMap<>();
        if (raw == null) return out;
        for (String pair : raw.split("&")) {
            String[] kv = pair.split("=", 2);
            out.put(URLDecoder.decode(kv[0], "UTF-8"), kv.length > 1 ? URLDecoder.decode(kv[1], "UTF-8") : "");
        }
        return out;
    }

    private void respond(Socket s, int status, String type, byte[] body) throws Exception {
        String reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 403 ? "Forbidden" :
                status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" :
                status == 409 ? "Conflict" : status == 503 ? "Service Unavailable" : "Error";
        OutputStream out = s.getOutputStream();
        String header = "HTTP/1.1 " + status + " " + reason + "\r\n" +
                "Content-Type: " + type + "\r\nContent-Length: " + body.length +
                "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
        out.write(header.getBytes(StandardCharsets.US_ASCII));
        out.write(body);
        out.flush();
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel c = new NotificationChannel(CHANNEL, "ScreenBridge screen access", NotificationManager.IMPORTANCE_LOW);
            c.setDescription("Persistent notification while screen capture is active.");
            ((NotificationManager)getSystemService(NOTIFICATION_SERVICE)).createNotificationChannel(c);
        }
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public void onDestroy() {
        removeStatusOverlay();
        main.removeCallbacks(captureWatchdog);
        running = false;
        stopped.set(true);
        try { if (server != null) server.close(); } catch (Exception ignored) { }
        if (displayManager != null) {
            try { displayManager.unregisterDisplayListener(displayListener); } catch (Exception ignored) { }
        }
        synchronized (captureLock) { releaseCapture(); }
        MediaProjection old = projection;
        projection = null;
        projectionActive = false;
        if (old != null) { try { old.stop(); } catch (Exception ignored) { } }
        clients.shutdownNow();
        if (captureThread != null) captureThread.quitSafely();
        super.onDestroy();
    }
}
