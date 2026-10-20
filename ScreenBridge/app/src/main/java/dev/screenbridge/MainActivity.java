package dev.screenbridge;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.net.Uri;
import android.content.ComponentName;
import android.text.TextUtils;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.text.InputType;
import android.os.Handler;
import android.os.Looper;
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
    private Button overlayButton;
    private EditText relayUrlEdit;
    private TextView relayStatus;
    private String token;
    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final Runnable relayStatusRefresh = new Runnable() {
        @Override public void run() {
            updateRelayStatus();
            if (!isFinishing()) uiHandler.postDelayed(this, 700L);
        }
    };

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

        overlayButton = button("Mostrar indicador flutuante");
        overlayButton.setOnClickListener(v -> {
            if (!Settings.canDrawOverlays(this)) {
                startActivity(new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
                Toast.makeText(this, "Permita a sobreposição e toque no botão novamente.", Toast.LENGTH_LONG).show();
                return;
            }
            if (!ScreenBridgeService.isRunning()) {
                Toast.makeText(this, "Inicie a captura do ScreenBridge primeiro.", Toast.LENGTH_SHORT).show();
                return;
            }
            Intent toggle = new Intent(this, ScreenBridgeService.class);
            toggle.setAction(ScreenBridgeService.ACTION_TOGGLE_OVERLAY);
            startService(toggle);
            overlayButton.setText(ScreenBridgeService.isOverlayShown()
                    ? "Mostrar indicador flutuante" : "Ocultar indicador flutuante");
        });
        content.addView(overlayButton);

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

        Button recordStart = button("Começar a gravar sessão");
        recordStart.setOnClickListener(v -> {
            if (!ScreenBridgeService.isRunning()) {
                Toast.makeText(this, "Autorize a captura e inicie o ScreenBridge primeiro.", Toast.LENGTH_LONG).show();
                return;
            }
            Intent action = new Intent(this, ScreenBridgeService.class);
            action.setAction(ScreenBridgeService.ACTION_START_RECORDING);
            startService(action);
            Toast.makeText(this, "Solicitação de gravação enviada.", Toast.LENGTH_SHORT).show();
        });
        content.addView(recordStart);

        Button recordStop = button("Parar gravação e finalizar MP4");
        recordStop.setOnClickListener(v -> {
            Intent action = new Intent(this, ScreenBridgeService.class);
            action.setAction(ScreenBridgeService.ACTION_STOP_RECORDING);
            startService(action);
            Toast.makeText(this, "Finalizando vídeo; aguarde alguns segundos antes de compartilhar.", Toast.LENGTH_LONG).show();
        });
        content.addView(recordStop);

        Button shareRecording = button("Compartilhar última gravação");
        shareRecording.setOnClickListener(v -> {
            String value = ScreenBridgeService.getLastRecordingUri();
            if (value == null || value.isEmpty()) {
                Toast.makeText(this, "Nenhum vídeo finalizado disponível ainda.", Toast.LENGTH_LONG).show();
                return;
            }
            Uri video = Uri.parse(value);
            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType("video/mp4");
            share.putExtra(Intent.EXTRA_STREAM, video);
            share.setClipData(ClipData.newUri(getContentResolver(), "Gravação ScreenBridge", video));
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, "Enviar gravação para análise"));
        });
        content.addView(shareRecording);

        TextView relayTitle = new TextView(this);
        relayTitle.setText("Relay remoto — controle ao vivo");
        relayTitle.setTextSize(19);
        relayTitle.setTypeface(null, 1);
        relayTitle.setTextColor(0xFF241A32);
        relayTitle.setPadding(0, dp(22), 0, dp(4));
        content.addView(relayTitle);

        TextView relayHint = new TextView(this);
        relayHint.setText("Cole a origem HTTPS do ScreenBridge Relay. Ao conectar, um código temporário será exibido para parear o painel de controle. A conexão remota só fica ativa enquanto este serviço estiver autorizado.");
        relayHint.setTextSize(13);
        relayHint.setTextColor(0xFF665C70);
        content.addView(relayHint);

        relayUrlEdit = new EditText(this);
        relayUrlEdit.setSingleLine(true);
        relayUrlEdit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        relayUrlEdit.setHint("https://teu-relay.example");
        relayUrlEdit.setText(getSharedPreferences("bridge", MODE_PRIVATE).getString("relay_base_url", ""));
        content.addView(relayUrlEdit, new LinearLayout.LayoutParams(-1, -2));

        Button relayConnect = button("Conectar ao relay seguro");
        relayConnect.setOnClickListener(v -> {
            if (!ScreenBridgeService.isRunning()) {
                Toast.makeText(this, "Autorize a captura e inicie o ScreenBridge antes de conectar.", Toast.LENGTH_LONG).show();
                return;
            }
            String url = relayUrlEdit.getText().toString().trim();
            if (url.isEmpty()) {
                Toast.makeText(this, "Insira a URL HTTPS do relay.", Toast.LENGTH_LONG).show();
                return;
            }
            Intent action = new Intent(this, ScreenBridgeService.class);
            action.setAction(ScreenBridgeService.ACTION_RELAY_CONNECT);
            action.putExtra(ScreenBridgeService.EXTRA_RELAY_URL, url);
            startService(action);
            getSharedPreferences("bridge", MODE_PRIVATE).edit().putString("relay_base_url", url).apply();
            updateRelayStatus();
        });
        content.addView(relayConnect);

        Button relayDisconnect = button("PARAR CONTROLE REMOTO (emergência)");
        relayDisconnect.setOnClickListener(v -> {
            Intent action = new Intent(this, ScreenBridgeService.class);
            action.setAction(ScreenBridgeService.ACTION_RELAY_DISCONNECT);
            startService(action);
            Toast.makeText(this, "Controle remoto desconectado neste aparelho.", Toast.LENGTH_SHORT).show();
            updateRelayStatus();
        });
        content.addView(relayDisconnect);

        relayStatus = new TextView(this);
        relayStatus.setTextSize(14);
        relayStatus.setTextColor(0xFF241A32);
        relayStatus.setPadding(dp(12), dp(12), dp(12), dp(12));
        relayStatus.setBackgroundColor(0xFFEAE4F4);
        content.addView(relayStatus);
        updateRelayStatus();

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

    @Override protected void onResume() {
        super.onResume();
        updateStatus();
        uiHandler.removeCallbacks(relayStatusRefresh);
        uiHandler.post(relayStatusRefresh);
    }

    @Override protected void onPause() {
        uiHandler.removeCallbacks(relayStatusRefresh);
        super.onPause();
    }

    private void updateRelayStatus() {
        if (relayStatus == null) return;
        android.content.SharedPreferences prefs = getSharedPreferences("bridge", MODE_PRIVATE);
        String state = prefs.getString("relay_state", "Desconectado");
        String code = prefs.getString("relay_pair_code", "");
        String error = prefs.getString("relay_error", "");
        StringBuilder info = new StringBuilder("Estado: ").append(state);
        if (!code.isEmpty()) info.append("\n\nCÓDIGO DE PAREAMENTO: ").append(code)
                .append("\nDigite esse código no painel web. Ele expira em até 2 minutos.");
        if (!error.isEmpty()) info.append("\n\nDetalhes: ").append(error);
        relayStatus.setText(info.toString());
    }

    private void updateStatus() {
        if (status == null) return;
        boolean running = ScreenBridgeService.isRunning();
        if (overlayButton != null) {
            overlayButton.setText(ScreenBridgeService.isOverlayShown()
                    ? "Ocultar indicador flutuante" : "Mostrar indicador flutuante");
        }
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
