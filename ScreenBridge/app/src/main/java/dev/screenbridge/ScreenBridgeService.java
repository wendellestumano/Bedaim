package dev.screenbridge;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
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
import android.util.DisplayMetrics;
import android.view.WindowManager;

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
    private static final String CHANNEL = "screenbridge_capture";
    private static final int PORT = 18766;
    private static volatile boolean running = false;

    private final AtomicBoolean stopped = new AtomicBoolean(false);
    private final ExecutorService clients = Executors.newFixedThreadPool(4);
    private final Handler main = new Handler(Looper.getMainLooper());
    private HandlerThread captureThread;
    private Handler captureHandler;
    private ServerSocket server;
    private Thread acceptThread;
    private volatile MediaProjection projection;
    private VirtualDisplay virtualDisplay;
    private ImageReader imageReader;
    private DisplayManager displayManager;
    private String token = "";
    private volatile int width, height, density;
    private final Object captureLock = new Object();
    private final Object frameLock = new Object();
    private volatile byte[] latestJpeg;
    private volatile long latestFrameAt;
    private volatile long frameCount;
    private volatile String captureError = "";
    private volatile boolean projectionActive;
    private volatile long startedAt;

    public static boolean isRunning() { return running; }

    @Override public void onCreate() {
        super.onCreate();
        running = true;
        startedAt = System.currentTimeMillis();
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
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && intent.hasExtra(EXTRA_RESULT_DATA)) {
            int resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0);
            Intent resultData = getIntentExtra(intent, EXTRA_RESULT_DATA);
            if (resultData != null && projection == null) startProjection(resultCode, resultData);
        }
        startHttpServer();
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
            captureError = "";
            newProjection.registerCallback(new MediaProjection.Callback() {
                @Override public void onStop() {
                    projectionActive = false;
                    projection = null;
                    captureError = "Screen capture permission was revoked; reopen ScreenBridge and authorize again";
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
            captureError = "";
        }
    }

    private void onImageAvailable(ImageReader reader) {
        Image image = null;
        Bitmap padded = null;
        Bitmap cropped = null;
        try {
            image = reader.acquireLatestImage();
            if (image == null) return;
            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer buffer = plane.getBuffer();
            int pixelStride = plane.getPixelStride();
            int rowStride = plane.getRowStride();
            int paddedWidth = rowStride / pixelStride;
            padded = Bitmap.createBitmap(paddedWidth, image.getHeight(), Bitmap.Config.ARGB_8888);
            buffer.rewind();
            padded.copyPixelsFromBuffer(buffer);
            cropped = Bitmap.createBitmap(padded, 0, 0, image.getWidth(), image.getHeight());
            ByteArrayOutputStream out = new ByteArrayOutputStream(Math.max(16 * 1024, width * height / 8));
            cropped.compress(Bitmap.CompressFormat.JPEG, 72, out);
            byte[] jpeg = out.toByteArray();
            if (jpeg.length > 4) {
                synchronized (frameLock) {
                    latestJpeg = jpeg;
                    latestFrameAt = System.currentTimeMillis();
                    frameCount++;
                }
                captureError = "";
            }
        } catch (Exception e) {
            captureError = "Frame encoding failed: " + e.getClass().getSimpleName();
        } finally {
            if (image != null) image.close();
            if (cropped != null && cropped != padded) cropped.recycle();
            if (padded != null) padded.recycle();
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
            if ("/health".equals(path)) {
                JSONObject j = new JSONObject();
                j.put("ok", true);
                j.put("running", running);
                j.put("projection", projectionActive && projection != null);
                j.put("accessibility", ScreenControlAccessibilityService.isConnected());
                j.put("captureReady", latestJpeg != null);
                j.put("frameAgeMs", latestFrameAt == 0 ? -1 : Math.max(0, System.currentTimeMillis() - latestFrameAt));
                j.put("frameCount", frameCount);
                j.put("captureError", captureError);
                j.put("serverUptimeMs", Math.max(0, System.currentTimeMillis() - startedAt));
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
            if ("screen.jpg".equals(route)) {
                byte[] jpg = awaitFrame(350);
                if (jpg == null) {
                    respond(s, 503, "application/json", ("{\"ok\":false,\"error\":\"No frame available\",\"detail\":\"" + jsonEscape(captureError) + "\"}").getBytes(StandardCharsets.UTF_8));
                    return;
                }
                respond(s, 200, "image/jpeg", jpg);
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
                    respond(s, 409, "application/json", "{\"ok\":false,\"error\":\"accessibility unavailable or coordinates invalid\"}".getBytes(StandardCharsets.UTF_8));
                    return;
                }
                long oldFrame = latestFrameAt;
                byte[] jpg = awaitNewFrame(oldFrame, 450);
                if (jpg != null) {
                    respond(s, 200, "image/jpeg", jpg);
                } else {
                    JSONObject j = new JSONObject();
                    j.put("ok", true);
                    j.put("accepted", true);
                    j.put("frameUpdated", false);
                    j.put("frameAgeMs", latestFrameAt == 0 ? -1 : Math.max(0, System.currentTimeMillis() - latestFrameAt));
                    j.put("detail", captureError);
                    respond(s, 200, "application/json", j.toString().getBytes(StandardCharsets.UTF_8));
                }
            } else {
                respond(s, 404, "text/plain", "Not found".getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception ignored) { }
    }

    private byte[] awaitFrame(long timeoutMs) {
        long until = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < until) {
            byte[] frame = latestJpeg;
            if (frame != null) return frame.clone();
            try { Thread.sleep(15); } catch (InterruptedException e) { Thread.currentThread().interrupt(); return null; }
        }
        return null;
    }

    private byte[] awaitNewFrame(long previousFrameAt, long timeoutMs) {
        long until = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < until) {
            byte[] frame = latestJpeg;
            if (frame != null && latestFrameAt > previousFrameAt) return frame.clone();
            try { Thread.sleep(15); } catch (InterruptedException e) { Thread.currentThread().interrupt(); return null; }
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
