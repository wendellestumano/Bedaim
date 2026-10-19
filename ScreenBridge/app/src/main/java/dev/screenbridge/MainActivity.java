package dev.screenbridge;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.content.ComponentName;
import android.text.TextUtils;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import android.content.ClipData;
import android.content.ClipboardManager;

import java.security.SecureRandom;

public class MainActivity extends Activity {
    private static final int REQUEST_CAPTURE = 701;
    private TextView status;
    private TextView endpoint;
    private String token;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(0xFF17121F);
        getWindow().setNavigationBarColor(0xFF17121F);
        token = getSharedPreferences("bridge", MODE_PRIVATE).getString("token", null);
        if (token == null) {
            byte[] b = new byte[18];
            new SecureRandom().nextBytes(b);
            StringBuilder s = new StringBuilder();
            for (byte v : b) s.append(String.format(java.util.Locale.US, "%02x", v));
            token = s.toString();
            getSharedPreferences("bridge", MODE_PRIVATE).edit().putString("token", token).apply();
        }

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(22), dp(20), dp(24));
        content.setBackgroundColor(0xFFF7F5FA);

        TextView title = new TextView(this);
        title.setText("ScreenBridge");
        title.setTextSize(30);
        title.setTextColor(0xFF241A32);
        title.setTypeface(null, 1);
        content.addView(title);

        TextView intro = new TextView(this);
        intro.setText("Conexão local para capturar a tela e executar toques/gestos. O serviço não abre uma porta para a rede Wi-Fi: ele escuta apenas neste aparelho.");
        intro.setTextSize(15);
        intro.setTextColor(0xFF51485B);
        intro.setPadding(0, dp(8), 0, dp(16));
        content.addView(intro);

        status = new TextView(this);
        status.setTextSize(15);
        status.setTextColor(0xFF241A32);
        status.setPadding(dp(12), dp(12), dp(12), dp(12));
        status.setBackgroundColor(0xFFEAE4F4);
        content.addView(status);

        Button accessibility = button("1. Ativar controle por acessibilidade");
        accessibility.setOnClickListener(v -> startActivity(new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)));
        content.addView(accessibility);

        Button start = button("2. Autorizar tela e iniciar serviço");
        start.setOnClickListener(v -> requestCapture());
        content.addView(start);

        endpoint = new TextView(this);
        endpoint.setTextSize(13);
        endpoint.setTextColor(0xFF332A3D);
        endpoint.setPadding(dp(12), dp(12), dp(12), dp(12));
        endpoint.setTextIsSelectable(true);
        endpoint.setBackgroundColor(0xFFFFFFFF);
        content.addView(endpoint);

        Button copy = button("Copiar endereço de captura");
        copy.setOnClickListener(v -> {
            ClipboardManager cm = (ClipboardManager)getSystemService(CLIPBOARD_SERVICE);
            cm.setPrimaryClip(ClipData.newPlainText("ScreenBridge URL", baseUrl() + "/" + token + "/screen.jpg"));
            Toast.makeText(this, "Endereço copiado", Toast.LENGTH_SHORT).show();
        });
        content.addView(copy);

        Button health = button("Verificar conexão");
        health.setOnClickListener(v -> {
            endpoint.setText("Base local: " + baseUrl() + "\nCaptura: " + baseUrl() + "/" + token + "/screen.jpg\nEstado: consulte /" + token + "/health");
            updateStatus();
        });
        content.addView(health);

        Button stop = button("Parar serviço");
        stop.setOnClickListener(v -> {
            stopService(new Intent(this, ScreenBridgeService.class));
            updateStatus();
        });
        content.addView(stop);

        TextView note = new TextView(this);
        note.setText("Privacidade: só aceite a captura quando quiser compartilhar sua tela. O serviço mostra uma notificação enquanto estiver ativo. O acesso HTTP exige o token aleatório exibido acima.");
        note.setTextColor(0xFF665C70);
        note.setTextSize(13);
        note.setPadding(0, dp(18), 0, 0);
        content.addView(note);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        setContentView(scroll);
        endpoint.setText("Captura: " + baseUrl() + "/" + token + "/screen.jpg\nControle: " + baseUrl() + "/" + token + "/action?op=tap&x=100&y=100");
        updateStatus();
    }

    private void requestCapture() {
        MediaProjectionManager manager = (MediaProjectionManager)getSystemService(Context.MEDIA_PROJECTION_SERVICE);
        startActivityForResult(manager.createScreenCaptureIntent(), REQUEST_CAPTURE);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_CAPTURE) {
            if (resultCode != RESULT_OK || data == null) {
                Toast.makeText(this, "A captura não foi autorizada.", Toast.LENGTH_LONG).show();
                return;
            }
            Intent service = new Intent(this, ScreenBridgeService.class);
            service.putExtra(ScreenBridgeService.EXTRA_RESULT_CODE, resultCode);
            service.putExtra(ScreenBridgeService.EXTRA_RESULT_DATA, data);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(service); else startService(service);
            Toast.makeText(this, "Iniciando ScreenBridge…", Toast.LENGTH_SHORT).show();
            updateStatus();
        }
    }

    @Override protected void onResume() { super.onResume(); updateStatus(); }

    private void updateStatus() {
        if (status == null) return;
        boolean running = ScreenBridgeService.isRunning();
        boolean accessibilityOn = ScreenControlAccessibilityService.isConnected();
        status.setText("Serviço: " + (running ? "ativo" : "parado") + "\nControle de gestos: " + (accessibilityOn ? "conectado" : "precisa ser ativado nas configurações") + "\nEndereço: 127.0.0.1:18766");
    }

    private String baseUrl() { return "http://127.0.0.1:18766"; }
    private int dp(int value) { return (int)(value * getResources().getDisplayMetrics().density + 0.5f); }
    private Button button(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = dp(8);
        b.setLayoutParams(lp);
        return b;
    }
}
