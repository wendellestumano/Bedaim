package dev.bedaim.bridge.service

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import dev.bedaim.bridge.core.BridgeStatus
import dev.bedaim.bridge.security.BridgePreferences
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

/**
 * Owns one ngrok process for the local MCP server.
 *
 * When ngrok is installed inside Debian/PRoot, the PRoot process itself is
 * launched in the Termux background. The ngrok child remains in the
 * foreground of that PRoot session, so PRoot does not kill it on exit.
 */
object TunnelController {
    private const val TERMUX_PACKAGE = "com.termux"
    private const val TERMUX_RUN_PERMISSION = "com.termux.permission.RUN_COMMAND"
    private const val TERMUX_HOME = "/data/data/com.termux/files/home"
    private const val BASH = "/data/data/com.termux/files/usr/bin/bash"

    internal fun buildStartScript(domain: String): String = startScript(domain)

    private fun startScript(domain: String) = """
        set -eu
        BASE_DIR="${'$'}HOME/.bedaim-bridge"
        PID_FILE="${'$'}BASE_DIR/ngrok.pid"
        LOCK_DIR="${'$'}BASE_DIR/ngrok-start.lock"
        LOG_FILE="${'$'}BASE_DIR/ngrok.log"
        mkdir -p "${'$'}BASE_DIR"

        # Serialize starts requested by the initial service launch and the
        # periodic reconnect monitor.
        if ! mkdir "${'$'}LOCK_DIR" 2>/dev/null; then
          sleep 1
          LOCK_PID="\$(cat "${'$'}LOCK_DIR/pid" 2>/dev/null || true)"
          case "${'$'}LOCK_PID" in
            ''|*[!0-9]*) ;;
            *)
              if kill -0 "${'$'}LOCK_PID" 2>/dev/null; then
                echo "BRIDGE_TUNNEL_START_IN_PROGRESS"
                exit 0
              fi
              ;;
          esac
          rm -rf "${'$'}LOCK_DIR"
          if ! mkdir "${'$'}LOCK_DIR" 2>/dev/null; then
            echo "BRIDGE_TUNNEL_START_IN_PROGRESS"
            exit 0
          fi
        fi
        printf '%s\n' "${'$'}${'$'}" > "${'$'}LOCK_DIR/pid"
        cleanup_lock() {
          LOCK_OWNER="\$(cat "${'$'}LOCK_DIR/pid" 2>/dev/null || true)"
          if [ "${'$'}LOCK_OWNER" = "${'$'}${'$'}" ]; then
            rm -rf "${'$'}LOCK_DIR"
          fi
        }
        trap cleanup_lock EXIT
        trap 'exit 143' TERM
        trap 'exit 130' INT

        AGENT_TUNNELS="\$(curl -fsS --max-time 2 http://127.0.0.1:4040/api/tunnels 2>/dev/null || true)"
        if printf '%s' "${'$'}AGENT_TUNNELS" | grep -Fq '${domain}'; then
          if printf '%s' "${'$'}AGENT_TUNNELS" | grep -q '8765'; then
            echo "BRIDGE_TUNNEL_ALREADY_RUNNING=LOCAL_AGENT"
            exit 0
          fi
          echo "BRIDGE_ERROR=NGROK_DOMAIN_ACTIVE_WITH_WRONG_UPSTREAM"
          exit 2
        elif printf '%s' "${'$'}AGENT_TUNNELS" | grep -q '8765'; then
          echo "BRIDGE_ERROR=OTHER_NGROK_DOMAIN_ALREADY_ACTIVE"
          exit 2
        fi

        # Remove only a process previously recorded by this controller.
        if [ -s "${'$'}PID_FILE" ]; then
          PID="\$(cat "${'$'}PID_FILE" 2>/dev/null || true)"
          case "${'$'}PID" in
            ''|*[!0-9]*) rm -f "${'$'}PID_FILE" ;;
            *)
              if kill -0 "${'$'}PID" 2>/dev/null; then
                CMD="\$(ps -p "${'$'}PID" -o args= 2>/dev/null || true)"
                case "${'$'}CMD" in
                  *ngrok*|*proot*)
                    kill "${'$'}PID" 2>/dev/null || true
                    sleep 1
                    ;;
                  *)
                    echo "BRIDGE_ERROR=PID_OWNERSHIP_MISMATCH"
                    rm -f "${'$'}PID_FILE"
                    exit 3
                    ;;
                esac
              fi
              rm -f "${'$'}PID_FILE"
              ;;
          esac
        fi

        if command -v ngrok >/dev/null 2>&1; then
          nohup ngrok http 8765 --url '${domain}' --log=stdout --log-format=logfmt > "${'$'}LOG_FILE" 2>&1 < /dev/null &
          PID="${'$'}!"
          OWNER=TERMUX
        elif command -v proot-distro >/dev/null 2>&1 &&
          proot-distro login debian -- test -x /usr/local/bin/ngrok; then
          # IMPORTANT: background PRoot from Termux, not ngrok inside a
          # short-lived PRoot shell. PRoot's --kill-on-exit would kill a
          # background child when that shell returns.
          nohup proot-distro login debian -- /usr/local/bin/ngrok http 8765 --url '${domain}' --log=stdout --log-format=logfmt > "${'$'}LOG_FILE" 2>&1 < /dev/null &
          PID="${'$'}!"
          OWNER=DEBIAN_PROOT
        else
          echo "BRIDGE_ERROR=NGROK_NOT_FOUND"
          exit 127
        fi

        printf '%s\n' "${'$'}PID" > "${'$'}PID_FILE"
        echo "BRIDGE_TUNNEL_PROCESS_STARTED=${'$'}OWNER PID=${'$'}PID"
        sleep 2
        if kill -0 "${'$'}PID" 2>/dev/null; then
          echo "BRIDGE_TUNNEL_PROCESS_ALIVE"
          AGENT_TUNNELS="\$(curl -fsS --max-time 2 http://127.0.0.1:4040/api/tunnels 2>/dev/null || true)"
          if printf '%s' "${'$'}AGENT_TUNNELS" | grep -Fq '${domain}' &&
            printf '%s' "${'$'}AGENT_TUNNELS" | grep -q '8765'; then
            echo "BRIDGE_TUNNEL_READY"
          else
            echo "BRIDGE_TUNNEL_STARTING_OR_WAITING_FOR_NGROK"
          fi
        else
          rm -f "${'$'}PID_FILE"
          echo "BRIDGE_ERROR=NGROK_PROCESS_EXITED"
          tail -40 "${'$'}LOG_FILE" 2>/dev/null || true
          exit 1
        fi
    """.trimIndent()

    private val stopScript = """
        set +e
        BASE_DIR="${'$'}HOME/.bedaim-bridge"
        PID_FILE="${'$'}BASE_DIR/ngrok.pid"
        if [ -s "${'$'}PID_FILE" ]; then
          PID="\$(cat "${'$'}PID_FILE" 2>/dev/null)"
          case "${'$'}PID" in
            ''|*[!0-9]*) echo "BRIDGE_ERROR=INVALID_PID_FILE" ;;
            *)
              if kill -0 "${'$'}PID" 2>/dev/null; then
                CMD="\$(ps -p "${'$'}PID" -o args= 2>/dev/null)"
                case "${'$'}CMD" in
                  *ngrok*|*proot*)
                    kill "${'$'}PID" 2>/dev/null && echo "BRIDGE_TUNNEL_STOPPED" || true
                    ;;
                  *)
                    echo "BRIDGE_ERROR=PID_OWNERSHIP_MISMATCH"
                    ;;
                esac
              fi
              ;;
          esac
          rm -f "${'$'}PID_FILE"
        else
          echo "BRIDGE_TUNNEL_NOT_OWNED_BY_CONTROLLER"
        fi
        exit 0
    """.trimIndent()

    fun start(context: Context): Boolean = runFixedTermuxCommand(
        context,
        startScript(BridgePreferences.getConfiguredNgrokDomain(context)),
        "BedAim Bridge • ensure ngrok tunnel",
        "Starts or reuses the configured ngrok tunnel for the local MCP server."
    )

    fun stop(context: Context): Boolean = runFixedTermuxCommand(
        context,
        stopScript,
        "BedAim Bridge • stop owned ngrok tunnel",
        "Stops only an ngrok process previously launched by this Bridge."
    )

    private fun runFixedTermuxCommand(
        context: Context,
        script: String,
        label: String,
        description: String
    ): Boolean {
        if (context.packageManager.getLaunchIntentForPackage(TERMUX_PACKAGE) == null) {
            BridgeStatus.recordEvent("Tunnel: Termux não está instalado.")
            return false
        }
        if (context.checkSelfPermission(TERMUX_RUN_PERMISSION) != PackageManager.PERMISSION_GRANTED) {
            BridgeStatus.recordEvent("Tunnel: permissão Termux RUN_COMMAND ainda não concedida.")
            return false
        }

        val callback = Intent(context, TermuxCommandResultService::class.java)
        val pendingFlags = PendingIntent.FLAG_ONE_SHOT or
            (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0)
        val pending = PendingIntent.getService(
            context,
            (System.currentTimeMillis() and 0x7fffffff).toInt(),
            callback,
            pendingFlags
        )
        val command = Intent().apply {
            setClassName(TERMUX_PACKAGE, "com.termux.app.RunCommandService")
            action = "com.termux.RUN_COMMAND"
            putExtra("com.termux.RUN_COMMAND_PATH", BASH)
            putExtra("com.termux.RUN_COMMAND_ARGUMENTS", arrayOf("-lc", script))
            putExtra("com.termux.RUN_COMMAND_WORKDIR", TERMUX_HOME)
            putExtra("com.termux.RUN_COMMAND_BACKGROUND", true)
            putExtra("com.termux.RUN_COMMAND_COMMAND_LABEL", label)
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", description)
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", pending)
        }
        return try {
            context.startService(command)
            BridgeStatus.recordEvent("Tunnel: solicitação enviada ao Termux.")
            true
        } catch (error: Throwable) {
            BridgeStatus.recordEvent("Tunnel: falha no RUN_COMMAND — " + error.javaClass.simpleName)
            false
        }
    }

    /** Returns the public HTTPS URL only when it matches the configured domain and local MCP port. */
    fun readPublicUrl(context: Context): String? {
        val connection = try {
            (URL("http://127.0.0.1:4040/api/tunnels").openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 1_500
                readTimeout = 1_500
                instanceFollowRedirects = false
                useCaches = false
            }
        } catch (_: Throwable) {
            return null
        }
        return try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) return null
            val body = connection.inputStream.bufferedReader().use { it.readText() }
            val tunnels = JSONObject(body).optJSONArray("tunnels") ?: return null
            for (index in 0 until tunnels.length()) {
                val tunnel = tunnels.optJSONObject(index) ?: continue
                val publicUrl = tunnel.optString("public_url")
                val address = tunnel.optJSONObject("config")?.optString("addr").orEmpty()
                if (publicUrl.startsWith("https://") && address.contains("8765")) {
                    val normalized = publicUrl.trimEnd('/')
                    if (normalized.equals(BridgePreferences.getConfiguredNgrokDomain(context), ignoreCase = true)) {
                        return normalized
                    }
                }
            }
            null
        } catch (_: Throwable) {
            null
        } finally {
            connection.disconnect()
        }
    }
}
