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
import android.os.IBinder;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.view.Display;
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
    private final ExecutorService clients = Executors.newCachedThreadPool();
    private final Handler main = new Handler(Looper.getMainLooper());
    private ServerSocket server;
    private Thread acceptThread;
    private MediaProjection projection;
    private VirtualDisplay virtualDisplay;
    private ImageReader imageReader;
    private DisplayManager displayManager;
    private String token = "";
    private int width, height, density;
    private final Object captureLock = new Object();

    public static boolean isRunning() { return running; }

    @Override public void onCreate() {
        super.onCreate();
        running = true;
        token = getSharedPreferences("bridge", MODE_PRIVATE).getString("token", "");
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
        displayManager.registerDisplayListener(displayListener, main);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && intent.hasExtra(EXTRA_RESULT_DATA)) {
            int resultCode = intent.getIntExtra(EXTRA_RESULT_CODE, 0);
            Intent resultData = getIntentExtra(intent, EXTRA_RESULT_DATA);
            if (resultData != null) startProjection(resultCode, resultData);
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
            if (projection != null) {
                try { projection.stop(); } catch (Exception ignored) { }
                projection = null;
            }
            MediaProjectionManager m = (MediaProjectionManager)getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            projection = m.getMediaProjection(resultCode, data);
            if (projection == null) return;
            projection.registerCallback(new MediaProjection.Callback() {
                @Override public void onStop() {
                    synchronized (captureLock) { releaseCapture(); }
                }
            }, main);
            configureCapture();
        } catch (SecurityException e) {
            stopSelf();
        }
    }

    private void configureCapture() {
        if (projection == null) return;
        DisplayMetrics metrics = new DisplayMetrics();
        WindowManager wm = (WindowManager)getSystemService(WINDOW_SERVICE);
        if (wm != null) wm.getDefaultDisplay().getRealMetrics(metrics);
        int newW = Math.max(1, metrics.widthPixels);
        int newH = Math.max(1, metrics.heightPixels);
        int newD = Math.max(1, metrics.densityDpi);
        synchronized (captureLock) {
            if (virtualDisplay != null && imageReader != null && width == newW && height == newH) return;
            releaseCapture();
            width = newW; height = newH; density = newD;
            imageReader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3);
            virtualDisplay = projection.createVirtualDisplay("ScreenBridgeCapture", width, height, density,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, imageReader.getSurface(), null, main);
        }
    }

    private void releaseCapture() {
        if (virtualDisplay != null) { virtualDisplay.release(); virtualDisplay = null; }
        if (imageReader != null) { imageReader.close(); imageReader = null; }
    }

    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int id) { }
        @Override public void onDisplayRemoved(int id) { }
        @Override public void onDisplayChanged(int id) {
            if (id == Display.DEFAULT_DISPLAY) main.postDelayed(() -> {
                try { configureCapture(); } catch (Exception ignored) { }
            }, 180);
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
                if (!stopped.get()) main.post(() -> android.widget.Toast.makeText(this,
                        "ScreenBridge: " + e.getMessage(), android.widget.Toast.LENGTH_LONG).show());
            }
        }, "ScreenBridge-HTTP");
        acceptThread.setDaemon(true);
        acceptThread.start();
    }

    private void handle(Socket socket) {
        try (Socket s = socket) {
            s.setSoTimeout(2500);
            BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream(), StandardCharsets.US_ASCII));
            String request = in.readLine();
            if (request == null || !request.startsWith("GET ")) { respond(s, 405, "text/plain", "GET only".getBytes(StandardCharsets.UTF_8)); return; }
            String target = request.split(" ", 3)[1];
            while (true) { String line = in.readLine(); if (line == null || line.isEmpty()) break; }
            URI uri = URI.create(target);
            String path = uri.getPath();
            if ("/health".equals(path)) {
                JSONObject j = new JSONObject();
                j.put("ok", true); j.put("running", running); j.put("projection", projection != null);
                j.put("accessibility", ScreenControlAccessibilityService.isConnected());
                int[] size = ScreenControlAccessibilityService.screenSize();
                j.put("controlWidth", size[0] > 0 ? size[0] : width);
                j.put("controlHeight", size[1] > 0 ? size[1] : height);
                respond(s, 200, "application/json; charset=utf-8", j.toString().getBytes(StandardCharsets.UTF_8));
                return;
            }
            String prefix = "/" + token + "/";
            if (token.isEmpty() || !path.startsWith(prefix)) { respond(s, 403, "text/plain", "Forbidden".getBytes(StandardCharsets.UTF_8)); return; }
            String route = path.substring(prefix.length());
            if ("screen.jpg".equals(route)) {
                byte[] jpg = captureJpeg();
                if (jpg == null) { respond(s, 503, "application/json", "{\"ok\":false,\"error\":\"No frame yet\"}".getBytes(StandardCharsets.UTF_8)); return; }
                respond(s, 200, "image/jpeg", jpg);
            } else if ("action".equals(route)) {
                Map<String,String> q = query(uri.getRawQuery());
                String op = q.getOrDefault("op", "");
                boolean accepted = false;
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
                    respond(s, 400, "application/json", "{\"ok\":false,\"error\":\"unsupported action\"}".getBytes(StandardCharsets.UTF_8)); return;
                }
                if (!accepted) {
                    respond(s, 409, "application/json", "{\"ok\":false,\"error\":\"accessibility unavailable or coordinates invalid\"}".getBytes(StandardCharsets.UTF_8)); return;
                }
                Thread.sleep(220);
                byte[] jpg = captureJpeg();
                if (jpg == null) jpg = "{\"ok\":true,\"accepted\":true}".getBytes(StandardCharsets.UTF_8);
                respond(s, 200, jpg.length > 2 && (jpg[0] & 255) == 255 && (jpg[1] & 255) == 216 ? "image/jpeg" : "application/json", jpg);
            } else {
                respond(s, 404, "text/plain", "Not found".getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception ignored) { }
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

    private byte[] captureJpeg() {
        synchronized (captureLock) {
            if (imageReader == null) return null;
            Image image = null;
            try {
                image = imageReader.acquireLatestImage();
                if (image == null) {
                    Thread.sleep(80);
                    image = imageReader.acquireLatestImage();
                }
                if (image == null) return null;
                Image.Plane plane = image.getPlanes()[0];
                ByteBuffer buffer = plane.getBuffer();
                int pixelStride = plane.getPixelStride();
                int rowStride = plane.getRowStride();
                int paddedWidth = rowStride / pixelStride;
                Bitmap padded = Bitmap.createBitmap(paddedWidth, image.getHeight(), Bitmap.Config.ARGB_8888);
                buffer.rewind();
                padded.copyPixelsFromBuffer(buffer);
                Bitmap cropped = Bitmap.createBitmap(padded, 0, 0, image.getWidth(), image.getHeight());
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                cropped.compress(Bitmap.CompressFormat.JPEG, 72, out);
                padded.recycle();
                if (cropped != padded) cropped.recycle();
                return out.toByteArray();
            } catch (Exception e) {
                return null;
            } finally {
                if (image != null) image.close();
            }
        }
    }

    private void respond(Socket s, int status, String type, byte[] body) throws Exception {
        String reason = status == 200 ? "OK" : status == 403 ? "Forbidden" : status == 404 ? "Not Found" :
                status == 405 ? "Method Not Allowed" : status == 409 ? "Conflict" : "Service Unavailable";
        OutputStream out = s.getOutputStream();
        String header = "HTTP/1.1 " + status + " " + reason + "\r\n" +
                "Content-Type: " + type + "\r\nContent-Length: " + body.length +
                "\r\nCache-Control: no-store\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
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
        if (displayManager != null) displayManager.unregisterDisplayListener(displayListener);
        synchronized (captureLock) { releaseCapture(); }
        if (projection != null) { try { projection.stop(); } catch (Exception ignored) { } projection = null; }
        clients.shutdownNow();
        super.onDestroy();
    }
}
