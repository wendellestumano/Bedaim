package dev.bedaim.bridge.service

import android.app.*
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.Handler
import android.os.Looper
import android.content.pm.ServiceInfo
import androidx.core.app.NotificationCompat
import dev.bedaim.bridge.R
import dev.bedaim.bridge.core.*
import dev.bedaim.bridge.mcp.McpServerFactory
import dev.bedaim.bridge.security.BridgePreferences
import dev.bedaim.bridge.security.BridgeOAuth
import io.ktor.server.cio.CIO
import io.ktor.server.engine.*
import io.ktor.server.application.*
import io.ktor.server.request.*
import io.ktor.server.response.*
import io.ktor.http.*
import io.modelcontextprotocol.kotlin.sdk.server.mcpStreamableHttp
import java.security.MessageDigest
import java.util.concurrent.atomic.AtomicBoolean

class BridgeService : Service() {
    private var server: EmbeddedServer<*, *>? = null
    @Volatile private var serviceStopping = false
    private val tunnelCheckInFlight = AtomicBoolean(false)
    private val tunnelMonitorHandler = Handler(Looper.getMainLooper())
    private val tunnelMonitor = object : Runnable {
        override fun run() {
            if (server == null || serviceStopping) return
            if (BridgePreferences.isTermuxAutoReconnectEnabled(this@BridgeService)) {
                if (tunnelCheckInFlight.compareAndSet(false, true)) {
                    Thread({
                        try {
                            if (!serviceStopping) {
                                val publicUrl = TunnelController.readPublicUrl(this@BridgeService)
                                if (!serviceStopping && publicUrl == null) {
                                    BridgeStatus.recordEvent("Reconexão automática: túnel ausente; solicitando restauração pelo Termux.")
                                    if (!serviceStopping && !TunnelController.start(this@BridgeService)) {
                                        BridgeStatus.recordEvent("Reconexão automática: Termux não aceitou RUN_COMMAND.")
                                    }
                                } else if (!serviceStopping && publicUrl != null) {
                                    BridgePreferences.saveTunnelUrl(this@BridgeService, publicUrl)
                                }
                            }
                        } catch (error: Throwable) {
                            BridgeStatus.recordEvent("Reconexão automática falhou: ${error.javaClass.simpleName}")
                        } finally {
                            tunnelCheckInFlight.set(false)
                        }
                    }, "BridgeTunnelMonitor").start()
                } else {
                    BridgeStatus.recordEvent("Reconexão automática: verificação anterior ainda está em andamento.")
                }
            }
            tunnelMonitorHandler.postDelayed(this, TUNNEL_MONITOR_INTERVAL_MS)
        }
    }

    private fun scheduleTunnelMonitor() {
        tunnelMonitorHandler.removeCallbacks(tunnelMonitor)
        tunnelMonitorHandler.postDelayed(tunnelMonitor, TUNNEL_MONITOR_INITIAL_DELAY_MS)
    }

    override fun onCreate() {
        super.onCreate()
        createChannel()
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification("Starting"), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        } else {
            startForeground(NOTIFICATION_ID, notification("Starting"))
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                serviceStopping = true
                BridgePreferences.setAutoStartEnabled(this, false)
                tunnelMonitorHandler.removeCallbacks(tunnelMonitor)
                TunnelController.stop(this)
                BridgePreferences.saveTunnelUrl(this, null)
                BridgeStatus.update(BridgeState.STOPPING, "Stopping")
                server?.stop(250, 750)
                server = null
                BridgeStatus.update(BridgeState.STOPPED, "Stopped")
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_ROTATE_TOKEN -> {
                BridgeStatus.update(BridgeState.STOPPING, "Rotating MCP token")
                server?.stop(250, 750)
                server = null
                return try {
                    BridgePreferences.rotateToken(this)
                    startServer()
                    START_STICKY
                } catch (t: Throwable) {
                    BridgeStatus.update(BridgeState.ERROR, "Token rotation failed: ${t.javaClass.simpleName}")
                    stopForeground(STOP_FOREGROUND_REMOVE)
                    stopSelf()
                    START_NOT_STICKY
                }
            }
            else -> startServer()
        }
        return START_STICKY
    }

    private fun startServer() {
        serviceStopping = false
        if (server != null) {
            TunnelController.start(this)
            scheduleTunnelMonitor()
            return
        }
        BridgeStatus.update(BridgeState.STARTING, "Starting local MCP server")
        val token = BridgePreferences.getOrCreateToken(this)
        BridgeOAuth.initialize(applicationContext)
        try {
            // ngrok preserves the public Host header when proxying to the loopback server.
            // Allow only the configured HTTPS ngrok hostname plus the local development hosts.
            // Keep the SDK's DNS-rebinding protection enabled; do not use enableDnsRebindingProtection=false.
            val allowedMcpHosts = McpHostPolicy.allowedHosts(
                BridgePreferences.getConfiguredNgrokDomain(this)
            )

            server = embeddedServer(CIO, host = "127.0.0.1", port = PORT) {
                intercept(ApplicationCallPipeline.Call) {
                    val issuer = BridgePreferences.getConfiguredNgrokDomain(this@BridgeService).trimEnd('/')
                    if (BridgeOAuth.handle(call, issuer)) finish()
                    if (call.request.path().startsWith("/mcp")) {
                        val header = call.request.headers[HttpHeaders.Authorization].orEmpty()
                        val bearerSupplied = header.takeIf { it.startsWith("Bearer ", ignoreCase = true) }
                            ?.substringAfter(' ')
                        val legacyQueryToken = call.request.queryParameters["access_token"]
                        val oauthValid = bearerSupplied?.let { BridgeOAuth.isValidAccessToken(it, issuer) } == true
                        val legacyValid = bearerSupplied?.let {
                            MessageDigest.isEqual(token.toByteArray(), it.toByteArray())
                        } == true || legacyQueryToken?.let {
                            MessageDigest.isEqual(token.toByteArray(), it.toByteArray())
                        } == true
                        if (!oauthValid && !legacyValid) {
                            call.response.headers.append(
                                HttpHeaders.WWWAuthenticate,
                                """Bearer resource_metadata="$issuer/.well-known/oauth-protected-resource", scope="mcp""""
                            )
                            call.respond(HttpStatusCode.Unauthorized)
                            finish()
                        }
                    }
                }
                mcpStreamableHttp(allowedHosts = allowedMcpHosts) {
                    McpServerFactory.create(this@BridgeService)
                }
            }.start(wait = false)
            BridgeStatus.update(BridgeState.RUNNING, "Listening on 127.0.0.1:$PORT")
            getSystemService(NotificationManager::class.java)
                .notify(NOTIFICATION_ID, notification("MCP server running on port $PORT"))
            TunnelController.start(this)
            scheduleTunnelMonitor()
        } catch (t: Throwable) {
            server = null
            BridgeStatus.update(BridgeState.ERROR, "MCP startup failed: ${t.javaClass.simpleName}")
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    override fun onDestroy() {
        serviceStopping = true
        tunnelMonitorHandler.removeCallbacks(tunnelMonitor)
        server?.stop(250, 750)
        server = null
        BridgeStatus.update(BridgeState.STOPPED, "Stopped")
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            getSystemService(NotificationManager::class.java)
                .createNotificationChannel(NotificationChannel(CHANNEL, "Bridge service", NotificationManager.IMPORTANCE_LOW))
        }
    }

    private fun notification(message: String) = NotificationCompat.Builder(this, CHANNEL)
        .setSmallIcon(android.R.drawable.stat_sys_upload_done)
        .setContentTitle(getString(R.string.app_name))
        .setContentText(message)
        .setOngoing(true)
        .setCategory(NotificationCompat.CATEGORY_SERVICE)
        .build()

    companion object {
        const val PORT = 8765
        const val ACTION_START = "dev.bedaim.bridge.action.START"
        const val ACTION_STOP = "dev.bedaim.bridge.action.STOP"
        const val ACTION_ROTATE_TOKEN = "dev.bedaim.bridge.action.ROTATE_TOKEN"
        private const val CHANNEL = "bridge_service"
        private const val NOTIFICATION_ID = 8765
        private const val TUNNEL_MONITOR_INITIAL_DELAY_MS = 20_000L
        private const val TUNNEL_MONITOR_INTERVAL_MS = 60_000L
    }
}
