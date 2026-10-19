package dev.screenbridge;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.graphics.Path;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.util.DisplayMetrics;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public class ScreenControlAccessibilityService extends AccessibilityService {
    private static final int MAX_SNAPSHOT_NODES = 260;
    private static final int MAX_SNAPSHOT_DEPTH = 18;
    private static final int MAX_TEXT_CHARS = 180;
    private static volatile ScreenControlAccessibilityService instance;

    private final Handler snapshotHandler = new Handler(Looper.getMainLooper());
    private volatile String cachedUiSnapshot = "{\"ok\":false,\"error\":\"snapshot_not_ready\",\"nodes\":[]}";
    private volatile long uiSnapshotAtMs;
    private final Runnable refreshSnapshotTask = this::refreshUiSnapshot;

    @Override protected void onServiceConnected() {
        super.onServiceConnected();
        instance = this;
        scheduleSnapshotRefresh(0);
    }

    @Override public void onAccessibilityEvent(AccessibilityEvent event) {
        if (event == null) return;
        int type = event.getEventType();
        if (type == AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED ||
            type == AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED ||
            type == AccessibilityEvent.TYPE_WINDOWS_CHANGED ||
            type == AccessibilityEvent.TYPE_VIEW_SCROLLED ||
            type == AccessibilityEvent.TYPE_VIEW_TEXT_CHANGED ||
            type == AccessibilityEvent.TYPE_VIEW_CLICKED) {
            // Coalesce event bursts; the local endpoint reads this cache directly.
            scheduleSnapshotRefresh(65);
        }
    }

    @Override public void onInterrupt() { }

    @Override public void onDestroy() {
        snapshotHandler.removeCallbacks(refreshSnapshotTask);
        if (instance == this) instance = null;
        super.onDestroy();
    }

    public static boolean isConnected() { return instance != null; }

    public static int[] screenSize() {
        ScreenControlAccessibilityService s = instance;
        if (s == null) return new int[]{0, 0};
        DisplayMetrics m = new DisplayMetrics();
        WindowManager wm = (WindowManager)s.getSystemService(WINDOW_SERVICE);
        if (wm != null) wm.getDefaultDisplay().getRealMetrics(m);
        return new int[]{m.widthPixels, m.heightPixels};
    }

    /** Reads the event-updated cache. refreshNow queues one refresh on the service thread. */
    public static String uiSnapshotJson(boolean refreshNow) {
        ScreenControlAccessibilityService s = instance;
        if (s == null) {
            return "{\"ok\":false,\"error\":\"accessibility_disconnected\",\"nodes\":[]}";
        }
        if (refreshNow) {
            if (Looper.myLooper() == Looper.getMainLooper()) {
                s.refreshUiSnapshot();
            } else {
                CountDownLatch done = new CountDownLatch(1);
                s.snapshotHandler.post(() -> {
                    try { s.refreshUiSnapshot(); } finally { done.countDown(); }
                });
                try { done.await(220, TimeUnit.MILLISECONDS); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            }
        } else if (s.uiSnapshotAtMs == 0) {
            s.scheduleSnapshotRefresh(0);
        }
        return s.cachedUiSnapshot;
    }

    private void scheduleSnapshotRefresh(long delayMs) {
        snapshotHandler.removeCallbacks(refreshSnapshotTask);
        snapshotHandler.postDelayed(refreshSnapshotTask, Math.max(0, delayMs));
    }

    private void refreshUiSnapshot() {
        AccessibilityNodeInfo root = null;
        try {
            root = getRootInActiveWindow();
            JSONObject snapshot = new JSONObject();
            JSONArray nodes = new JSONArray();
            int[] count = new int[]{0};
            String packageName = "";
            boolean activeWindow = root != null;
            if (root != null) {
                CharSequence p = root.getPackageName();
                packageName = p == null ? "" : p.toString();
                collectVisibleNodes(root, -1, 0, nodes, count);
            }
            long now = System.currentTimeMillis();
            snapshot.put("ok", activeWindow);
            snapshot.put("activeWindow", activeWindow);
            snapshot.put("error", activeWindow ? "" : "no_active_window");
            snapshot.put("package", packageName);
            snapshot.put("updatedAtMs", now);
            int[] size = screenSize();
            snapshot.put("screenWidth", size[0]);
            snapshot.put("screenHeight", size[1]);
            snapshot.put("nodeCount", nodes.length());
            snapshot.put("truncated", count[0] >= MAX_SNAPSHOT_NODES);
            snapshot.put("nodes", nodes);
            cachedUiSnapshot = snapshot.toString();
            uiSnapshotAtMs = now;
        } catch (Exception e) {
            try {
                JSONObject error = new JSONObject();
                error.put("ok", false);
                error.put("error", "snapshot_failed");
                error.put("detail", e.getClass().getSimpleName());
                error.put("updatedAtMs", System.currentTimeMillis());
                error.put("nodes", new JSONArray());
                cachedUiSnapshot = error.toString();
                uiSnapshotAtMs = System.currentTimeMillis();
            } catch (Exception ignored) {
                cachedUiSnapshot = "{\"ok\":false,\"error\":\"snapshot_failed\",\"nodes\":[]}";
            }
        } finally {
            if (root != null) {
                try { root.recycle(); } catch (Exception ignored) { }
            }
        }
    }

    private void collectVisibleNodes(AccessibilityNodeInfo node, int parentIndex, int depth,
                                     JSONArray nodes, int[] count) throws Exception {
        if (node == null || depth > MAX_SNAPSHOT_DEPTH || count[0] >= MAX_SNAPSHOT_NODES) return;
        int childParentIndex = parentIndex;
        CharSequence textValue = node.getText();
        CharSequence descValue = node.getContentDescription();
        String text = shorten(textValue);
        String desc = shorten(descValue);
        String viewId = "";
        try {
            String id = node.getViewIdResourceName();
            if (id != null) viewId = id;
        } catch (Exception ignored) { }

        boolean visible = node.isVisibleToUser();
        boolean useful = visible && (!text.isEmpty() || !desc.isEmpty() || !viewId.isEmpty() ||
                node.isClickable() || node.isFocusable() || node.isScrollable() || node.isCheckable());
        if (useful) {
            Rect bounds = new Rect();
            node.getBoundsInScreen(bounds);
            JSONObject item = new JSONObject();
            int index = nodes.length();
            item.put("i", index);
            item.put("parent", parentIndex);
            item.put("text", text);
            item.put("desc", desc);
            item.put("id", viewId);
            item.put("class", safe(node.getClassName()));
            item.put("bounds", new JSONArray().put(bounds.left).put(bounds.top).put(bounds.right).put(bounds.bottom));
            item.put("clickable", node.isClickable());
            item.put("enabled", node.isEnabled());
            item.put("focusable", node.isFocusable());
            item.put("scrollable", node.isScrollable());
            item.put("checkable", node.isCheckable());
            item.put("checked", node.isChecked());
            item.put("selected", node.isSelected());
            nodes.put(item);
            childParentIndex = index;
            count[0]++;
        }

        int children = Math.min(node.getChildCount(), MAX_SNAPSHOT_NODES);
        for (int i = 0; i < children && count[0] < MAX_SNAPSHOT_NODES; i++) {
            AccessibilityNodeInfo child = null;
            try {
                child = node.getChild(i);
                if (child != null) collectVisibleNodes(child, childParentIndex, depth + 1, nodes, count);
            } finally {
                if (child != null) {
                    try { child.recycle(); } catch (Exception ignored) { }
                }
            }
        }
    }

    private static String shorten(CharSequence value) {
        if (value == null) return "";
        String s = value.toString().replace('\n', ' ').replace('\r', ' ').trim();
        return s.length() > MAX_TEXT_CHARS ? s.substring(0, MAX_TEXT_CHARS) : s;
    }

    private static String safe(CharSequence value) {
        return value == null ? "" : shorten(value);
    }

    public static boolean tap(int x, int y) {
        ScreenControlAccessibilityService s = instance;
        if (s == null) return false;
        int[] size = screenSize();
        if (x < 0 || y < 0 || x >= size[0] || y >= size[1]) return false;
        Path p = new Path();
        p.moveTo(x, y);
        p.lineTo(Math.min(x + 1, size[0] - 1), Math.min(y + 1, size[1] - 1));
        GestureDescription g = new GestureDescription.Builder()
                .addStroke(new GestureDescription.StrokeDescription(p, 0, 55)).build();
        return s.dispatchGesture(g, null, new Handler(Looper.getMainLooper()));
    }

    public static boolean swipe(int x1, int y1, int x2, int y2, long durationMs) {
        ScreenControlAccessibilityService s = instance;
        if (s == null) return false;
        int[] size = screenSize();
        if (x1 < 0 || y1 < 0 || x2 < 0 || y2 < 0 ||
            x1 >= size[0] || x2 >= size[0] || y1 >= size[1] || y2 >= size[1] ||
            durationMs < 1 || durationMs > 10000) return false;
        Path p = new Path();
        p.moveTo(x1, y1);
        p.lineTo(x2, y2);
        GestureDescription g = new GestureDescription.Builder()
                .addStroke(new GestureDescription.StrokeDescription(p, 0, durationMs)).build();
        return s.dispatchGesture(g, null, new Handler(Looper.getMainLooper()));
    }

    public static boolean globalAction(int action) {
        ScreenControlAccessibilityService s = instance;
        return s != null && s.performGlobalAction(action);
    }
}
