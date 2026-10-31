package dev.bedaim.bridge.ui

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.app.ActivityManager
import android.content.ClipboardManager
import android.app.PendingIntent
import android.content.ClipData
import android.content.Context
import android.content.ContentValues
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
import android.os.Environment
import android.provider.MediaStore
import android.provider.Settings
import android.net.Uri
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.content.ContextCompat
import dev.bedaim.bridge.core.BridgeStatus
import dev.bedaim.bridge.apklab.ApkLab
import dev.bedaim.bridge.core.RootAdapter
import dev.bedaim.bridge.security.BridgePreferences
import dev.bedaim.bridge.service.BridgeService
import dev.bedaim.bridge.service.TunnelController
import rikka.shizuku.Shizuku
import java.io.File
import java.io.FileOutputStream
import java.util.Locale

class MainActivity : Activity() {
    private lateinit var statusValue: TextView
    private lateinit var statusDot: View
    private lateinit var deviceValue: TextView
    private lateinit var accessValue: TextView
    private lateinit var tokenValue: TextView
    private lateinit var apkReportValue: TextView
    private lateinit var logsValue: TextView
    private lateinit var workspaceValue: TextView
    private lateinit var tunnelValue: TextView
    private lateinit var autoStartValue: TextView
    private lateinit var ngrokDomainValue: TextView
    private var selectedApk: File? = null

    private val bgColor = Color.rgb(7, 8, 11)
    private val surface = Color.rgb(17, 19, 24)
    private val surfaceAlt = Color.rgb(25, 28, 35)
    private val accent = Color.rgb(0, 220, 160)
    private val primaryText = Color.rgb(241, 244, 248)
    private val secondaryText = Color.rgb(145, 153, 166)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.statusBarColor = bgColor
        window.navigationBarColor = bgColor
        window.decorView.systemUiVisibility = 0
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 9)
        }
        render()
        window.decorView.post { connectAll(silent = true) }
    }

    override fun onResume() {
        super.onResume()
        if (::statusValue.isInitialized) refresh()
    }

    private fun render() {
        val scroll = ScrollView(this).apply {
            setBackgroundColor(bgColor)
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
        statusCard.addView(label("MCP local em 127.0.0.1:8765 • túnel remoto gerenciado pelo Bridge", 12f, secondaryText))
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

        val tunnelCard = card(root)
        sectionTitle(tunnelCard, "CONEXÃO REMOTA • NGROK")
        tunnelValue = label("Preparando conexão automática…", 13f, primaryText)
        tunnelValue.setLineSpacing(dp(4).toFloat(), 1f)
        tunnelCard.addView(tunnelValue)
        autoStartValue = label("", 12f, secondaryText)
        autoStartValue.setPadding(0, dp(8), 0, dp(2))
        tunnelCard.addView(autoStartValue)
        ngrokDomainValue = label("", 12f, secondaryText)
        ngrokDomainValue.setPadding(0, dp(4), 0, dp(2))
        tunnelCard.addView(ngrokDomainValue)
        updateNgrokDomainLabel()
        tunnelCard.addView(label(
            "Ao abrir este app, o servidor MCP e o túnel são iniciados automaticamente. O ngrok usa a configuração já existente no Termux ou no Debian do proot-distro.",
            12f, secondaryText
        ).apply { setPadding(0, dp(8), 0, dp(2)) })
        actionButton(tunnelCard, "CONECTAR TUDO / REPARAR CONEXÃO", true) { connectAll() }
        actionButton(tunnelCard, "CONFIGURAR DOMÍNIO FIXO DO NGROK", false) {
            showNgrokDomainDialog()
        }
        actionButton(tunnelCard, "ALTERNAR INICIALIZAÇÃO APÓS REINICIAR", false) {
            val enabled = !BridgePreferences.isAutoStartEnabled(this)
            BridgePreferences.setAutoStartEnabled(this, enabled)
            updateAutoStartLabel()
            Toast.makeText(
                this,
                if (enabled) "Inicialização automática após reiniciar ativada." else "Inicialização automática desativada.",
                Toast.LENGTH_LONG
            ).show()
        }
        actionButton(tunnelCard, "COPIAR ENDPOINT MCP REMOTO", false) {
            val base = BridgePreferences.getTunnelUrl(this)
            if (base.isNullOrBlank()) {
                Toast.makeText(this, "Túnel ainda não detectado. Aguarde ou toque em Conectar tudo.", Toast.LENGTH_LONG).show()
                refreshTunnelStatusAsync()
            } else {
                copyText("Remote MCP endpoint", "$base/mcp")
            }
        }
        actionButton(tunnelCard, "COPIAR CONFIGURAÇÃO DO CLIENTE MCP", false) {
            val base = BridgePreferences.getTunnelUrl(this)
            if (base.isNullOrBlank()) {
                Toast.makeText(this, "Túnel ainda não detectado.", Toast.LENGTH_LONG).show()
            } else {
                val value = "Endpoint: $base/mcp\nAuthorization: Bearer ${BridgePreferences.getOrCreateToken(this)}"
                copyText("Remote MCP configuration", value)
                Toast.makeText(this, "Endpoint e bearer token copiados. Trate esse texto como senha.", Toast.LENGTH_LONG).show()
            }
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

        val termuxCard = card(root)
        sectionTitle(termuxCard, "TERMUX TOOLCHAIN")
        termuxCard.addView(label(
            "Verifica ferramentas instaladas sem aceitar comandos arbitrários. Para Apktool, conceda RUN_COMMAND, defina allow-external-apps=true e execute termux-setup-storage no Termux.",
            12f, secondaryText
        ))
        actionButton(termuxCard, "VERIFICAR APKTOOL / AAPT / GIT / PYTHON / CLANG", true) {
            runTermuxToolchainCheck()
        }
        actionButton(termuxCard, "INSTALAR APKTOOL NO TERMUX", false) { confirmInstallApktool() }
        actionButton(termuxCard, "INSTALAR CLOUDFLARED NO TERMUX", true) { confirmInstallCloudflared() }
        actionButton(termuxCard, "CONFIGURAR PERMISSÃO RUN_COMMAND", false) {
            openPackageSettings("com.termux")
        }
        actionButton(termuxCard, "ABRIR TERMUX", false) { openPackage("com.termux") }

        val settingsCard = card(root)
        sectionTitle(settingsCard, "ATALHOS DO ANDROID")
        actionButton(settingsCard, "CONFIGURAÇÕES DE WI-FI E REDE", false) {
            openSystemSettings(Settings.ACTION_WIFI_SETTINGS)
        }
        actionButton(settingsCard, "CONFIGURAÇÕES DE BATERIA", false) {
            openSystemSettings(Settings.ACTION_BATTERY_SAVER_SETTINGS)
        }
        actionButton(settingsCard, "INFORMAÇÕES DO APLICATIVO", false) {
            val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null))
            runCatching { startActivity(intent) }.onFailure {
                Toast.makeText(this, "Não foi possível abrir as configurações.", Toast.LENGTH_SHORT).show()
            }
        }

        val workspaceCard = card(root)
        sectionTitle(workspaceCard, "WORKSPACE LOCAL")
        workspaceValue = label("Preparando espaço de trabalho…", 12f, primaryText)
        workspaceCard.addView(workspaceValue)
        actionButton(workspaceCard, "ATUALIZAR ARQUIVOS", false) { refreshWorkspace() }
        actionButton(workspaceCard, "IMPORTAR ARQUIVO PARA WORKSPACE", true) { chooseWorkspaceImport() }
        actionButton(workspaceCard, "ABRIR PASTA NO GERENCIADOR", false) {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
            runCatching { startActivity(intent) }.onFailure {
                Toast.makeText(this, "Gerenciador de arquivos indisponível.", Toast.LENGTH_SHORT).show()
            }
        }

        val logsCard = card(root)
        sectionTitle(logsCard, "LOGS DO BRIDGE")
        logsValue = label("Nenhum evento registrado.", 12f, primaryText)
        logsValue.setLineSpacing(dp(3).toFloat(), 1f)
        logsCard.addView(logsValue)
        actionButton(logsCard, "ATUALIZAR LOGS", false) { refreshLogs() }
        actionButton(logsCard, "COPIAR LOGS", false) {
            copyText("Bridge logs", BridgeStatus.history().joinToString("\\n"))
        }

        val apkCard = card(root)
        sectionTitle(apkCard, "APK LAB • INSPEÇÃO E BACKUP")
        apkReportValue = label("Nenhum APK selecionado. Escolha um arquivo para analisar metadados, assinatura e estrutura ZIP.", 12f, primaryText)
        apkReportValue.setLineSpacing(dp(4).toFloat(), 1f)
        apkCard.addView(apkReportValue)
        actionButton(apkCard, "SELECIONAR APK", true) { chooseApk() }
        actionButton(apkCard, "ANALISAR NOVAMENTE", false) { selectedApk?.let { analyzeApk(it) } }
        actionButton(apkCard, "DUPLICAR APK COMO BACKUP", false) { duplicateSelectedApk() }
        actionButton(apkCard, "EXTRAIR CONTEÚDO ZIP", false) { extractSelectedApk() }
        actionButton(apkCard, "DECOMPILAR RECURSOS COM APKTOOL", true) { decodeSelectedApkWithApktool() }
        actionButton(apkCard, "EXPORTAR APK PARA DOWNLOADS", false) { exportSelectedApk() }
        actionButton(apkCard, "COPIAR RELATÓRIO", false) {
            if (selectedApk == null) {
                Toast.makeText(this, "Selecione um APK primeiro.", Toast.LENGTH_SHORT).show()
            } else copyText("APK report", apkReportValue.text.toString())
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

        root.addView(label("O endpoint remoto exige cadastro único no cliente MCP e autenticação Bearer. Não compartilhe o token.", 11f, secondaryText).apply {
            setPadding(dp(2), dp(10), dp(2), dp(12))
        })
        refresh()
    }

    private fun refreshLogs() {
        if (!::logsValue.isInitialized) return
        logsValue.text = BridgeStatus.history().take(25).ifEmpty { listOf("Nenhum evento registrado.") }.joinToString("\\n")
    }

    private fun refreshWorkspace() {
        if (!::workspaceValue.isInitialized) return
        val root = File(filesDir, "workspace").apply { mkdirs() }
        val files = root.walkTopDown().maxDepth(4).filter { it.isFile }.take(80).toList()
        workspaceValue.text = if (files.isEmpty()) {
            "Workspace vazio. Arquivos criados pelas ferramentas MCP aparecerão aqui."
        } else {
            files.joinToString("\\n") {
                val relative = it.relativeTo(root).path
                "$relative  •  ${formatBytes(it.length())}"
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun chooseApk() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/vnd.android.package-archive"
        }
        runCatching { startActivityForResult(intent, REQUEST_APK) }.onFailure {
            Toast.makeText(this, "Não foi possível abrir o seletor de arquivos.", Toast.LENGTH_LONG).show()
        }
    }

    @Suppress("DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        if (requestCode == REQUEST_WORKSPACE_IMPORT) {
            importWorkspaceFile(uri)
            return
        }
        if (requestCode == REQUEST_APK_EXPORT) {
            exportApkToUri(uri)
            return
        }
        if (requestCode != REQUEST_APK) return
        apkReportValue.text = "Importando APK selecionado…"
        Thread {
            try {
                val directory = File(cacheDir, "apk-lab/imported").apply { mkdirs() }
                val imported = File(directory, "selected_${System.currentTimeMillis()}.apk")
                val input = contentResolver.openInputStream(uri) ?: error("Não foi possível abrir o arquivo.")
                input.use { source ->
                    FileOutputStream(imported).use { output ->
                        val buffer = ByteArray(64 * 1024)
                        var total = 0L
                        while (true) {
                            val count = source.read(buffer)
                            if (count < 0) break
                            total += count
                            require(total <= 1_500L * 1024L * 1024L) { "APK excede o limite de 1,5 GiB." }
                            output.write(buffer, 0, count)
                        }
                    }
                }
                val report = ApkLab.inspect(imported, packageManager).asText()
                runOnUiThread {
                    selectedApk = imported
                    apkReportValue.text = report
                    Toast.makeText(this, "APK analisado.", Toast.LENGTH_SHORT).show()
                }
            } catch (error: Throwable) {
                runOnUiThread {
                    apkReportValue.text = "Falha na análise: ${error.message ?: error.javaClass.simpleName}"
                    Toast.makeText(this, "Não foi possível analisar o APK.", Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    private fun chooseWorkspaceImport() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        @Suppress("DEPRECATION")
        runCatching { startActivityForResult(intent, REQUEST_WORKSPACE_IMPORT) }.onFailure {
            Toast.makeText(this, "Não foi possível abrir o seletor de arquivos.", Toast.LENGTH_LONG).show()
        }
    }

    private fun importWorkspaceFile(uri: Uri) {
        Thread {
            try {
                val directory = File(filesDir, "workspace/imports").apply { mkdirs() }.canonicalFile
                val destination = File(directory, "import_${System.currentTimeMillis()}.bin").canonicalFile
                require(destination.parentFile == directory) { "Destino inválido." }
                val input = contentResolver.openInputStream(uri) ?: error("Não foi possível abrir o arquivo.")
                input.use { source ->
                    FileOutputStream(destination).use { output ->
                        val buffer = ByteArray(32 * 1024)
                        var total = 0L
                        while (true) {
                            val count = source.read(buffer)
                            if (count < 0) break
                            total += count
                            require(total <= 32L * 1024L * 1024L) { "Limite de importação: 32 MiB." }
                            output.write(buffer, 0, count)
                        }
                    }
                }
                runOnUiThread {
                    refreshWorkspace()
                    Toast.makeText(this, "Arquivo importado para workspace/imports.", Toast.LENGTH_LONG).show()
                }
            } catch (error: Throwable) {
                runOnUiThread {
                    Toast.makeText(this, "Falha na importação: ${error.message}", Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    private fun exportSelectedApk() {
        val source = selectedApk
        if (source == null || !source.isFile) {
            Toast.makeText(this, "Selecione um APK primeiro.", Toast.LENGTH_SHORT).show()
            return
        }
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/vnd.android.package-archive"
            putExtra(Intent.EXTRA_TITLE, source.name)
        }
        @Suppress("DEPRECATION")
        runCatching { startActivityForResult(intent, REQUEST_APK_EXPORT) }.onFailure {
            Toast.makeText(this, "Não foi possível abrir o exportador de arquivos.", Toast.LENGTH_LONG).show()
        }
    }

    private fun exportApkToUri(uri: Uri) {
        val source = selectedApk ?: return
        Thread {
            try {
                val output = contentResolver.openOutputStream(uri) ?: error("Não foi possível criar o destino.")
                source.inputStream().use { input -> output.use { input.copyTo(it) } }
                runOnUiThread {
                    Toast.makeText(this, "APK exportado com sucesso.", Toast.LENGTH_LONG).show()
                }
            } catch (error: Throwable) {
                runOnUiThread {
                    Toast.makeText(this, "Falha na exportação: ${error.message}", Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    private fun analyzeApk(file: File) {
        apkReportValue.text = "Analisando APK…"
        Thread {
            try {
                val report = ApkLab.inspect(file, packageManager).asText()
                runOnUiThread { apkReportValue.text = report }
            } catch (error: Throwable) {
                runOnUiThread { apkReportValue.text = "Falha na análise: ${error.message}" }
            }
        }.start()
    }

    private fun decodeSelectedApkWithApktool() {
        val source = selectedApk
        if (source == null || !source.isFile) {
            Toast.makeText(this, "Selecione um APK primeiro.", Toast.LENGTH_SHORT).show()
            return
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            Toast.makeText(this, "A integração automática exige Android 10 ou superior.", Toast.LENGTH_LONG).show()
            return
        }
        if (packageManager.getLaunchIntentForPackage("com.termux") == null) {
            Toast.makeText(this, "Instale o Termux primeiro.", Toast.LENGTH_LONG).show()
            return
        }
        if (checkSelfPermission("com.termux.permission.RUN_COMMAND") != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, "Conceda RUN_COMMAND nas permissões adicionais do Bridge.", Toast.LENGTH_LONG).show()
            openPackageSettings(packageName)
            return
        }

        apkReportValue.append("\n\nPreparando APK para o Termux…")
        Thread {
            var exportedUri: Uri? = null
            try {
                val safeBase = source.nameWithoutExtension.replace(Regex("[^A-Za-z0-9_-]"), "_")
                val exportName = "${safeBase}_${System.currentTimeMillis()}.apk"
                val relativePath = Environment.DIRECTORY_DOWNLOADS + "/BedAimBridge/"
                val values = ContentValues().apply {
                    put(MediaStore.MediaColumns.DISPLAY_NAME, exportName)
                    put(MediaStore.MediaColumns.MIME_TYPE, "application/vnd.android.package-archive")
                    put(MediaStore.MediaColumns.RELATIVE_PATH, relativePath)
                    put(MediaStore.MediaColumns.IS_PENDING, 1)
                }
                val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                    ?: error("Não foi possível criar o APK em Downloads.")
                exportedUri = uri
                val output = contentResolver.openOutputStream(uri) ?: error("Não foi possível gravar o APK exportado.")
                source.inputStream().use { input -> output.use { input.copyTo(it) } }
                contentResolver.update(uri, ContentValues().apply {
                    put(MediaStore.MediaColumns.IS_PENDING, 0)
                }, null, null)

                val inputPath = "/storage/emulated/0/Download/BedAimBridge/$exportName"
                val outputPath = "/storage/emulated/0/Download/BedAimBridge/decoded_$safeBase"
                runOnUiThread {
                    apkReportValue.append("\nAPK copiado para: $inputPath\nSaída esperada: $outputPath")
                    val started = runTermuxCommand(
                        "/data/data/com.termux/files/usr/bin/apktool",
                        arrayOf("d", "-f", inputPath, "-o", outputPath),
                        "APKTool decode: $exportName",
                        "Decompiles a user-selected APK into an isolated Downloads folder."
                    )
                    if (started) {
                        Toast.makeText(this, "APKTool iniciado pelo Termux. Confira os logs.", Toast.LENGTH_LONG).show()
                    } else {
                        Toast.makeText(this, "APK copiado, mas o Termux não iniciou Apktool. Verifique a permissão.", Toast.LENGTH_LONG).show()
                    }
                }
            } catch (error: Throwable) {
                exportedUri?.let { runCatching { contentResolver.delete(it, null, null) } }
                runOnUiThread {
                    apkReportValue.append("\nFalha ao iniciar Apktool: ${error.message}")
                    Toast.makeText(this, "Falha na preparação do APK.", Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    private fun runTermuxCommand(
        commandPath: String,
        arguments: Array<String>,
        label: String,
        description: String
    ): Boolean {
        if (checkSelfPermission("com.termux.permission.RUN_COMMAND") != PackageManager.PERMISSION_GRANTED) {
            BridgeStatus.recordEvent("Termux: permissão RUN_COMMAND não concedida.")
            return false
        }
        val resultIntent = Intent(this, dev.bedaim.bridge.service.TermuxCommandResultService::class.java)
        val pendingFlags = PendingIntent.FLAG_ONE_SHOT or
            (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0)
        val pending = PendingIntent.getService(
            this,
            (System.currentTimeMillis() and 0x7fffffff).toInt(),
            resultIntent,
            pendingFlags
        )
        val command = Intent().apply {
            setClassName("com.termux", "com.termux.app.RunCommandService")
            action = "com.termux.RUN_COMMAND"
            putExtra("com.termux.RUN_COMMAND_PATH", commandPath)
            putExtra("com.termux.RUN_COMMAND_ARGUMENTS", arguments)
            putExtra("com.termux.RUN_COMMAND_WORKDIR", "/data/data/com.termux/files/home")
            putExtra("com.termux.RUN_COMMAND_BACKGROUND", true)
            putExtra("com.termux.RUN_COMMAND_COMMAND_LABEL", label)
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", description)
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", pending)
        }
        return try {
            startService(command)
            BridgeStatus.recordEvent("Termux: iniciado — $label")
            refreshLogs()
            true
        } catch (error: Throwable) {
            BridgeStatus.recordEvent("Termux: falha ao iniciar $label — ${error.javaClass.simpleName}")
            refreshLogs()
            false
        }
    }

    private fun extractSelectedApk() {
        val source = selectedApk
        if (source == null || !source.isFile) {
            Toast.makeText(this, "Selecione um APK primeiro.", Toast.LENGTH_SHORT).show()
            return
        }
        apkReportValue.append("\n\nExtraindo estrutura ZIP…")
        Thread {
            try {
                val destination = File(
                    filesDir,
                    "workspace/apk-extracted/${source.nameWithoutExtension}_${System.currentTimeMillis()}"
                )
                val result = ApkLab.extractArchive(source, destination)
                runOnUiThread {
                    apkReportValue.append(
                        "\nExtraídos: ${result.filesExtracted} arquivos • ${formatBytes(result.bytesExtracted)}" +
                            "\nPasta: ${result.directory.absolutePath}"
                    )
                    refreshWorkspace()
                    Toast.makeText(this, "Estrutura extraída para o workspace.", Toast.LENGTH_LONG).show()
                }
            } catch (error: Throwable) {
                runOnUiThread {
                    apkReportValue.append("\nFalha na extração: ${error.message}")
                    Toast.makeText(this, "Extração interrompida.", Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    private fun duplicateSelectedApk() {
        val source = selectedApk
        if (source == null) {
            Toast.makeText(this, "Selecione um APK primeiro.", Toast.LENGTH_SHORT).show()
            return
        }
        Thread {
            try {
                val destination = ApkLab.duplicateAsBackup(source, File(filesDir, "workspace/apk-backups"))
                runOnUiThread {
                    Toast.makeText(this, "Backup salvo: ${destination.name}", Toast.LENGTH_LONG).show()
                    apkReportValue.append("\n\nBackup: ${destination.absolutePath}")
                }
            } catch (error: Throwable) {
                runOnUiThread {
                    Toast.makeText(this, "Falha no backup: ${error.message}", Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    private fun updateNgrokDomainLabel() {
        if (!::ngrokDomainValue.isInitialized) return
        ngrokDomainValue.text = "Domínio ngrok fixo: ${BridgePreferences.getConfiguredNgrokDomain(this)}"
    }

    private fun showNgrokDomainDialog() {
        val editor = EditText(this).apply {
            setSingleLine(true)
            inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_URI
            setText(BridgePreferences.getConfiguredNgrokDomain(this@MainActivity))
            hint = "https://seu-dominio.ngrok-free.dev"
        }
        AlertDialog.Builder(this)
            .setTitle("Domínio fixo do ngrok")
            .setMessage("Use o domínio gratuito atribuído à sua conta ngrok. Ele é guardado localmente e usado ao iniciar o túnel. Não cole seu authtoken aqui.")
            .setView(editor)
            .setNegativeButton("Cancelar", null)
            .setPositiveButton("Salvar") { _, _ ->
                val accepted = BridgePreferences.setConfiguredNgrokDomain(this, editor.text.toString())
                if (!accepted) {
                    Toast.makeText(this, "Domínio inválido. Use https://nome.ngrok-free.dev", Toast.LENGTH_LONG).show()
                } else {
                    BridgePreferences.saveTunnelUrl(this, null)
                    updateNgrokDomainLabel()
                    Toast.makeText(this, "Domínio salvo. Reconectando o túnel…", Toast.LENGTH_LONG).show()
                    connectAll(silent = true)
                }
            }
            .show()
    }

    private fun connectAll(silent: Boolean = false) {
        try {
            BridgePreferences.setAutoStartEnabled(this, true)
            updateAutoStartLabel()
            ContextCompat.startForegroundService(
                this,
                Intent(this, BridgeService::class.java).setAction(BridgeService.ACTION_START)
            )
            BridgeStatus.recordEvent("Conexão automática: servidor MCP e túnel solicitados.")
            if (::tunnelValue.isInitialized) tunnelValue.text = "Iniciando servidor MCP e verificando ngrok…"
            refreshTunnelStatusAsync()
            if (!silent) Toast.makeText(this, "Conexão solicitada. O Bridge está verificando o túnel.", Toast.LENGTH_LONG).show()
        } catch (error: Throwable) {
            BridgeStatus.recordEvent("Conexão automática falhou: ${error.javaClass.simpleName}")
            if (::tunnelValue.isInitialized) tunnelValue.text = "Falha ao iniciar: ${error.message}"
            if (!silent) Toast.makeText(this, "Não foi possível iniciar o serviço Bridge.", Toast.LENGTH_LONG).show()
        }
    }

    private fun refreshTunnelStatusAsync() {
        Thread {
            var found: String? = null
            for (attempt in 0 until 18) {
                found = TunnelController.readPublicUrl(this@MainActivity)
                if (found != null) break
                Thread.sleep(900)
            }
            if (found != null) BridgePreferences.saveTunnelUrl(this, found)
            runOnUiThread {
                if (!::tunnelValue.isInitialized) return@runOnUiThread
                tunnelValue.text = if (found != null) {
                    "Conectado • túnel HTTPS ativo\n$found/mcp"
                } else {
                    val saved = BridgePreferences.getTunnelUrl(this)
                    if (saved.isNullOrBlank()) {
                        "Servidor local solicitado. Ngrok ainda não respondeu. Confira Termux + RUN_COMMAND, a configuração de autenticação do ngrok e os logs."
                    } else {
                        "Último endpoint salvo: $saved/mcp\nVerificando se o agente está online…"
                    }
                }
                refreshLogs()
            }
        }.start()
    }

    private fun updateAutoStartLabel() {
        if (!::autoStartValue.isInitialized) return
        autoStartValue.text = if (BridgePreferences.isAutoStartEnabled(this)) {
            "Autostart após reiniciar: ATIVADO"
        } else {
            "Autostart após reiniciar: DESATIVADO (o serviço não voltará sozinho)"
        }
    }

    private fun refresh() {
        if (!::statusValue.isInitialized) return
        updateAutoStartLabel()
        updateNgrokDomainLabel()
        val running = BridgeStatus.state.name == "RUNNING"
        if (::tunnelValue.isInitialized) {
            val savedUrl = BridgePreferences.getTunnelUrl(this)
            tunnelValue.text = if (savedUrl.isNullOrBlank()) {
                "Túnel ngrok: aguardando conexão. O Bridge tentará iniciá-lo automaticamente."
            } else {
                "Túnel ngrok conectado\n$savedUrl/mcp"
            }
        }
        statusDot.background = circle(if (running) accent else Color.rgb(220, 170, 75))
        statusValue.text = "Estado: ${BridgeStatus.state}\n${BridgeStatus.detail}"
        deviceValue.text = deviceDiagnostics()
        accessValue.text = accessDiagnostics()
        refreshLogs()
        refreshWorkspace()
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
            "CPU lógica: ${Runtime.getRuntime().availableProcessors()} núcleos\n" +
            "ABIs: ${Build.SUPPORTED_ABIS.take(4).joinToString()}\n" +
            "PID do Bridge: ${android.os.Process.myPid()}\n" +
            "Conexão: $network"
    }

    private fun accessDiagnostics(): String {
        val shizukuActive = runCatching { Shizuku.pingBinder() }.getOrDefault(false)
        val shizukuAllowed = shizukuActive && runCatching {
            Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED
        }.getOrDefault(false)
        val rootFound = listOf("/system/bin/su", "/system/xbin/su", "/sbin/su").any { File(it).canExecute() }
        val termux = packageManager.getLaunchIntentForPackage("com.termux") != null
        val termuxRun = checkSelfPermission("com.termux.permission.RUN_COMMAND") == PackageManager.PERMISSION_GRANTED
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

    private fun confirmInstallCloudflared() {
        AlertDialog.Builder(this)
            .setTitle("Instalar cloudflared no Termux?")
            .setMessage("O Termux executará pkg install cloudflared. Isso instala o cliente do Cloudflare Tunnel. Nenhum túnel será criado ou iniciado automaticamente.")
            .setNegativeButton("Cancelar", null)
            .setPositiveButton("Instalar") { _, _ ->
                if (packageManager.getLaunchIntentForPackage("com.termux") == null) {
                    Toast.makeText(this, "Termux não está instalado.", Toast.LENGTH_SHORT).show()
                } else if (checkSelfPermission("com.termux.permission.RUN_COMMAND") != PackageManager.PERMISSION_GRANTED) {
                    Toast.makeText(this, "Conceda RUN_COMMAND primeiro.", Toast.LENGTH_LONG).show()
                    openPackageSettings(packageName)
                } else {
                    val started = runTermuxCommand(
                        "/data/data/com.termux/files/usr/bin/pkg",
                        arrayOf("install", "-y", "cloudflared"),
                        "Install Cloudflared",
                        "Installs the Cloudflare Tunnel client from the configured Termux package repositories."
                    )
                    Toast.makeText(
                        this,
                        if (started) "Instalação enviada ao Termux. Confira os logs." else "Não foi possível iniciar a instalação.",
                        Toast.LENGTH_LONG
                    ).show()
                }
            }
            .show()
    }

    private fun confirmInstallApktool() {
        AlertDialog.Builder(this)
            .setTitle("Instalar Apktool no Termux?")
            .setMessage("O Termux executará pkg install apktool. Isso instalará o decodificador no ambiente Termux e pode baixar dependências.")
            .setNegativeButton("Cancelar", null)
            .setPositiveButton("Instalar") { _, _ ->
                if (packageManager.getLaunchIntentForPackage("com.termux") == null) {
                    Toast.makeText(this, "Termux não está instalado.", Toast.LENGTH_SHORT).show()
                } else if (checkSelfPermission("com.termux.permission.RUN_COMMAND") != PackageManager.PERMISSION_GRANTED) {
                    Toast.makeText(this, "Conceda RUN_COMMAND primeiro.", Toast.LENGTH_LONG).show()
                    openPackageSettings(packageName)
                } else {
                    runTermuxCommand(
                        "/data/data/com.termux/files/usr/bin/pkg",
                        arrayOf("install", "-y", "apktool"),
                        "Install Apktool",
                        "Installs the Apktool package in the user's Termux environment."
                    )
                    Toast.makeText(this, "Instalação enviada ao Termux. Confira os logs.", Toast.LENGTH_LONG).show()
                }
            }
            .show()
    }

    private fun runTermuxToolchainCheck() {
        if (packageManager.getLaunchIntentForPackage("com.termux") == null) {
            Toast.makeText(this, "Termux não está instalado.", Toast.LENGTH_LONG).show()
            return
        }
        if (checkSelfPermission("com.termux.permission.RUN_COMMAND") != PackageManager.PERMISSION_GRANTED) {
            Toast.makeText(this, "Conceda RUN_COMMAND nas permissões adicionais do Termux Bridge.", Toast.LENGTH_LONG).show()
            openPackageSettings(packageName)
            return
        }

        val script = """
            printf 'BEDAIM BRIDGE TOOLCHAIN\\n'
            for tool in apktool aapt apksigner zipalign git python python3 clang cmake ninja gradle cloudflared; do
              if command -v "${'$'}tool" >/dev/null 2>&1; then
                printf '%-12s %s\\n' "${'$'}tool" "${'$'}(command -v "${'$'}tool")"
              else
                printf '%-12s missing\\n' "${'$'}tool"
              fi
            done
            if [ -d "${'$'}HOME/storage/shared" ]; then
              printf 'shared-storage %s\\n' 'available'
            else
              printf 'shared-storage %s\\n' 'not linked'
            fi
        """.trimIndent()

        val resultIntent = Intent(this, dev.bedaim.bridge.service.TermuxCommandResultService::class.java)
        val pendingFlags = PendingIntent.FLAG_ONE_SHOT or
            (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0)
        val pending = PendingIntent.getService(
            this,
            (System.currentTimeMillis() and 0x7fffffff).toInt(),
            resultIntent,
            pendingFlags
        )
        val command = Intent().apply {
            setClassName("com.termux", "com.termux.app.RunCommandService")
            action = "com.termux.RUN_COMMAND"
            putExtra("com.termux.RUN_COMMAND_PATH", "/data/data/com.termux/files/usr/bin/bash")
            putExtra("com.termux.RUN_COMMAND_ARGUMENTS", arrayOf("-lc", script))
            putExtra("com.termux.RUN_COMMAND_WORKDIR", "/data/data/com.termux/files/home")
            putExtra("com.termux.RUN_COMMAND_BACKGROUND", true)
            putExtra("com.termux.RUN_COMMAND_COMMAND_LABEL", "BedAim Bridge toolchain check")
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Checks installed development tools. No arbitrary command input is accepted.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", pending)
        }
        try {
            startService(command)
            BridgeStatus.recordEvent("Termux: diagnóstico de toolchain solicitado.")
            refreshLogs()
            Toast.makeText(this, "Diagnóstico enviado ao Termux. Atualize os logs em instantes.", Toast.LENGTH_LONG).show()
        } catch (error: Throwable) {
            BridgeStatus.recordEvent("Termux: falha ao iniciar comando — ${error.javaClass.simpleName}")
            Toast.makeText(this, "Termux recusou o comando. Verifique RUN_COMMAND e allow-external-apps.", Toast.LENGTH_LONG).show()
            refreshLogs()
        }
    }

    private fun openPackageSettings(packageName: String) {
        val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null))
        runCatching { startActivity(intent) }.onFailure {
            Toast.makeText(this, "Não foi possível abrir as permissões do aplicativo.", Toast.LENGTH_SHORT).show()
        }
    }

    private fun openSystemSettings(action: String) {
        runCatching { startActivity(Intent(action)) }.onFailure {
            Toast.makeText(this, "Configuração não disponível neste Android.", Toast.LENGTH_SHORT).show()
        }
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
            setTextColor(if (primary) bgColor else primaryText)
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

    companion object {
        private const val REQUEST_APK = 4102
        private const val REQUEST_WORKSPACE_IMPORT = 4103
        private const val REQUEST_APK_EXPORT = 4104
    }
}
