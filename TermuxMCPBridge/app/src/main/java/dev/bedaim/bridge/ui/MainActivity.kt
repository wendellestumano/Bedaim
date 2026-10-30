package dev.bedaim.bridge.ui

import android.Manifest
import android.app.Activity
import android.app.ActivityManager
import android.content.ClipboardManager
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.StatFs
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.content.ContextCompat
import dev.bedaim.bridge.core.BridgeStatus
import dev.bedaim.bridge.core.RootAdapter
import dev.bedaim.bridge.security.BridgePreferences
import dev.bedaim.bridge.service.BridgeService
import rikka.shizuku.Shizuku
import java.io.File
import java.util.Locale

class MainActivity : Activity() {
    private lateinit var statusValue: TextView
    private lateinit var statusDot: View
    private lateinit var deviceValue: TextView
    private lateinit var accessValue: TextView
    private lateinit var tokenValue: TextView

    private val background = Color.rgb(7, 8, 11)
    private val surface = Color.rgb(17, 19, 24)
    private val surfaceAlt = Color.rgb(25, 28, 35)
    private val accent = Color.rgb(0, 220, 160)
    private val primaryText = Color.rgb(241, 244, 248)
    private val secondaryText = Color.rgb(145, 153, 166)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = background
        window.navigationBarColor = background
        window.decorView.systemUiVisibility = 0
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 9)
        }
        render()
    }

    override fun onResume() {
        super.onResume()
        if (::statusValue.isInitialized) refresh()
    }

    private fun render() {
        val scroll = ScrollView(this).apply {
            setBackgroundColor(background)
            clipToPadding = false
            setPadding(dp(18), dp(14), dp(18), dp(28))
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, 0, 0, dp(20))
        }
        scroll.addView(root)
        setContentView(scroll)

        root.addView(label("BEDAIM • DEVICE BRIDGE", 11f, accent, true).apply {
            letterSpacing = 0.16f
            setPadding(0, dp(12), 0, dp(8))
        })
        root.addView(label("Termux MCP Bridge", 29f, primaryText, true))
        root.addView(label("Painel local de desenvolvimento Android", 13f, secondaryText).apply {
            setPadding(0, dp(4), 0, dp(20))
        })

        val statusCard = card(root)
        val statusHeader = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        statusDot = View(this).apply {
            background = circle(accent)
            layoutParams = LinearLayout.LayoutParams(dp(9), dp(9)).apply { rightMargin = dp(10) }
        }
        statusHeader.addView(statusDot)
        statusHeader.addView(label("SERVIDOR MCP", 11f, secondaryText, true))
        statusCard.addView(statusHeader)
        statusValue = label("Verificando estado…", 17f, primaryText, true).apply {
            setPadding(0, dp(10), 0, dp(4))
        }
        statusCard.addView(statusValue)
        statusCard.addView(label("127.0.0.1:8765  •  somente neste dispositivo", 12f, secondaryText))
        actionButton(statusCard, "INICIAR SERVIDOR", true) {
            ContextCompat.startForegroundService(
                this,
                Intent(this, BridgeService::class.java).setAction(BridgeService.ACTION_START)
            )
            statusValue.postDelayed({ refresh() }, 500)
        }
        actionButton(statusCard, "PARAR SERVIDOR", false) {
            startService(Intent(this, BridgeService::class.java).setAction(BridgeService.ACTION_STOP))
            statusValue.postDelayed({ refresh() }, 350)
        }

        val deviceCard = card(root)
        sectionTitle(deviceCard, "DISPOSITIVO")
        deviceValue = label("Carregando diagnóstico…", 13f, primaryText)
        deviceValue.setLineSpacing(dp(5).toFloat(), 1f)
        deviceCard.addView(deviceValue)
        actionButton(deviceCard, "ATUALIZAR DIAGNÓSTICO", false) { refresh() }

        val accessCard = card(root)
        sectionTitle(accessCard, "ACESSO E INTEGRAÇÕES")
        accessValue = label("", 13f, primaryText)
        accessValue.setLineSpacing(dp(5).toFloat(), 1f)
        accessCard.addView(accessValue)
        actionButton(accessCard, "ABRIR TERMUX", true) { openPackage("com.termux") }
        actionButton(accessCard, "SOLICITAR PERMISSÃO SHIZUKU", false) { requestShizuku() }
        actionButton(accessCard, "ABRIR SHIZUKU", false) { openPackage("moe.shizuku.privileged.api") }
        actionButton(accessCard, "VERIFICAR ROOT", false) {
            Thread {
                val result = RootAdapter.probeExplicitly()
                runOnUiThread {
                    Toast.makeText(this, result.detail, Toast.LENGTH_LONG).show()
                    refresh()
                }
            }.start()
        }

        val securityCard = card(root)
        sectionTitle(securityCard, "SEGURANÇA MCP")
        securityCard.addView(label("Endpoint local", 11f, secondaryText, true))
        securityCard.addView(label("http://127.0.0.1:${BridgeService.PORT}/mcp", 13f, primaryText).apply {
            setPadding(0, dp(5), 0, dp(12))
        })
        tokenValue = label("Token protegido • não exibido na tela", 12f, secondaryText)
        securityCard.addView(tokenValue)
        actionButton(securityCard, "COPIAR ENDPOINT", false) {
            copyText("MCP endpoint", "http://127.0.0.1:${BridgeService.PORT}/mcp")
        }
        actionButton(securityCard, "COPIAR TOKEN CONFIDENCIAL", false) {
            copyText("MCP bearer token", BridgePreferences.getOrCreateToken(this))
            Toast.makeText(this, "Token copiado. Não compartilhe esse valor.", Toast.LENGTH_LONG).show()
        }
        actionButton(securityCard, "GERAR NOVO TOKEN", false) {
            startService(Intent(this, BridgeService::class.java).setAction(BridgeService.ACTION_ROTATE_TOKEN))
            tokenValue.text = "Token renovado • reinicie o cliente MCP local"
            Toast.makeText(this, "Token renovado. Clientes antigos perderão acesso.", Toast.LENGTH_LONG).show()
            statusValue.postDelayed({ refresh() }, 700)
        }

        root.addView(label("Acesso remoto e execução de comandos ainda estão desativados nesta versão.", 11f, secondaryText).apply {
            setPadding(dp(2), dp(10), dp(2), dp(12))
        })
        refresh()
    }

    private fun refresh() {
        if (!::statusValue.isInitialized) return
        val running = BridgeStatus.state.name == "RUNNING"
        statusDot.background = circle(if (running) accent else Color.rgb(220, 170, 75))
        statusValue.text = "Estado: ${BridgeStatus.state}\n${BridgeStatus.detail}"
        deviceValue.text = deviceDiagnostics()
        accessValue.text = accessDiagnostics()
    }

    private fun deviceDiagnostics(): String {
        val batteryManager = getSystemService(BATTERY_SERVICE) as BatteryManager
        val battery = batteryManager.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY)
        val memory = ActivityManager.MemoryInfo()
        (getSystemService(ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(memory)
        val stat = StatFs(filesDir.absolutePath)
        val total = stat.totalBytes
        val free = stat.availableBytes
        val used = total - free
        val network = networkLabel()
        val model = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) Build.SOC_MODEL else Build.HARDWARE
        return "Modelo: ${Build.MANUFACTURER} ${Build.MODEL}\n" +
            "Android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n" +
            "Chipset: ${model.ifBlank { "não informado" }}\n" +
            "Bateria: ${if (battery >= 0) "$battery%" else "indisponível"}\n" +
            "RAM disponível: ${formatBytes(memory.availMem)} / ${formatBytes(memory.totalMem)}\n" +
            "Armazenamento interno: ${formatBytes(used)} usados / ${formatBytes(total)} total\n" +
            "Conexão: $network"
    }

    private fun accessDiagnostics(): String {
        val shizukuActive = runCatching { Shizuku.pingBinder() }.getOrDefault(false)
        val shizukuAllowed = shizukuActive && runCatching {
            Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED
        }.getOrDefault(false)
        val rootFound = listOf("/system/bin/su", "/system/xbin/su", "/sbin/su").any { File(it).canExecute() }
        val termux = packageManager.getLaunchIntentForPackage("com.termux") != null
        return "Shizuku: ${if (!shizukuActive) "inativo" else if (shizukuAllowed) "autorizado" else "sem permissão"}\n" +
            "Termux: ${if (termux) "instalado" else "não detectado"}\n" +
            "Root: ${if (rootFound) "binário su encontrado (não validado)" else "não detectado"}"
    }

    private fun networkLabel(): String {
        val manager = getSystemService(CONNECTIVITY_SERVICE) as ConnectivityManager
        val network = manager.activeNetwork ?: return "offline"
        val caps = manager.getNetworkCapabilities(network) ?: return "desconhecida"
        return when {
            caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) -> "Wi-Fi"
            caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR) -> "dados móveis"
            caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) -> "Ethernet"
            else -> "conectada"
        }
    }

    private fun requestShizuku() {
        if (!runCatching { Shizuku.pingBinder() }.getOrDefault(false)) {
            Toast.makeText(this, "Abra o Shizuku e inicie o serviço primeiro.", Toast.LENGTH_LONG).show()
            return
        }
        if (runCatching { Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED }.getOrDefault(false)) {
            Toast.makeText(this, "Shizuku já está autorizado.", Toast.LENGTH_SHORT).show()
            return
        }
        runCatching { Shizuku.requestPermission(9021) }
            .onFailure { Toast.makeText(this, "Erro: ${it.message}", Toast.LENGTH_LONG).show() }
    }

    private fun openPackage(packageName: String) {
        val launch = packageManager.getLaunchIntentForPackage(packageName)
        if (launch == null) {
            Toast.makeText(this, "Aplicativo não encontrado.", Toast.LENGTH_SHORT).show()
        } else startActivity(launch)
    }

    private fun copyText(label: String, value: String) {
        val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        clipboard.setPrimaryClip(ClipData.newPlainText(label, value))
        Toast.makeText(this, "$label copiado.", Toast.LENGTH_SHORT).show()
    }

    private fun card(parent: LinearLayout): LinearLayout {
        val view = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(16))
            background = rounded(surface, 18)
        }
        parent.addView(view, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { bottomMargin = dp(14) })
        return view
    }

    private fun sectionTitle(parent: LinearLayout, title: String) {
        parent.addView(label(title, 11f, accent, true).apply {
            letterSpacing = 0.12f
            setPadding(0, 0, 0, dp(12))
        })
    }

    private fun actionButton(parent: LinearLayout, title: String, primary: Boolean, action: () -> Unit) {
        val button = Button(this).apply {
            text = title
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
            isAllCaps = false
            setTextColor(if (primary) background else primaryText)
            background = rounded(if (primary) accent else surfaceAlt, 12)
            stateListAnimator = null
            setPadding(dp(12), dp(7), dp(12), dp(7))
            minHeight = dp(46)
            setOnClickListener { action() }
        }
        parent.addView(button, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = dp(9) })
    }

    private fun label(text: String, size: Float, color: Int, bold: Boolean = false): TextView =
        TextView(this).apply {
            this.text = text
            textSize = size
            setTextColor(color)
            if (bold) typeface = Typeface.DEFAULT_BOLD
        }

    private fun rounded(color: Int, radius: Int) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(radius).toFloat()
    }

    private fun circle(color: Int) = GradientDrawable().apply {
        shape = GradientDrawable.OVAL
        setColor(color)
    }

    private fun formatBytes(bytes: Long): String {
        if (bytes < 1024) return "$bytes B"
        val units = arrayOf("KB", "MB", "GB", "TB")
        var value = bytes.toDouble()
        var index = -1
        do { value /= 1024.0; index++ } while (value >= 1024 && index < units.lastIndex)
        return String.format(Locale.US, "%.1f %s", value, units[index])
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).toInt()
}
