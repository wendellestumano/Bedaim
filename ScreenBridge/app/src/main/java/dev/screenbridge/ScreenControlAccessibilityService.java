package dev.screenbridge;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.graphics.Path;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.WindowManager;
import android.util.DisplayMetrics;
import android.view.accessibility.AccessibilityEvent;

public class ScreenControlAccessibilityService extends AccessibilityService {
    private static volatile ScreenControlAccessibilityService instance;

    @Override protected void onServiceConnected() {
        super.onServiceConnected();
        instance = this;
    }

    @Override public void onAccessibilityEvent(AccessibilityEvent event) { }
    @Override public void onInterrupt() { }
    @Override public void onDestroy() {
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
