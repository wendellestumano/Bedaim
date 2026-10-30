package dev.bedaim.bridge.service

import android.app.*
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.content.pm.ServiceInfo
import androidx.core.app.NotificationCompat
import dev.bedaim.bridge.R
import dev.bedaim.bridge.core.*
import dev.bedaim.bridge.mcp.McpServerFactory
import dev.bedaim.bridge.security.BridgePreferences
import io.ktor.server.cio.CIO
import io.ktor.server.engine.*
import io.ktor.server.application.*
import io.ktor.server.request.*
import io.ktor.server.response.*
import io.ktor.http.*
import io.modelcontextprotocol.kotlin.sdk.server.mcpStreamableHttp
import java.security.MessageDigest

class BridgeService : Service() {
    private var server: EmbeddedServer<*, *>? = null

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
        if (server != null) return
        BridgeStatus.update(BridgeState.STARTING, "Starting local MCP server")
        val token = BridgePreferences.getOrCreateToken(this)
        try {
            server = embeddedServer(CIO, host = "127.0.0.1", port = PORT) {
                intercept(ApplicationCallPipeline.Call) {
                    if (call.request.path().startsWith("/mcp")) {
                        val header = call.request.headers[HttpHeaders.Authorization].orEmpty()
                        val supplied = header.removePrefix("Bearer ")
                        val ok = header.startsWith("Bearer ") &&
                            MessageDigest.isEqual(token.toByteArray(), supplied.toByteArray())
                        if (!ok) {
                            call.respond(HttpStatusCode.Unauthorized)
                            finish()
                        }
                    }
                }
                mcpStreamableHttp { McpServerFactory.create(this@BridgeService) }
            }.start(wait = false)
            BridgeStatus.update(BridgeState.RUNNING, "Listening on 127.0.0.1:$PORT")
            getSystemService(NotificationManager::class.java)
                .notify(NOTIFICATION_ID, notification("MCP server running on port $PORT"))
        } catch (t: Throwable) {
            server = null
            BridgeStatus.update(BridgeState.ERROR, "MCP startup failed: ${t.javaClass.simpleName}")
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    override fun onDestroy() {
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
    }
}
