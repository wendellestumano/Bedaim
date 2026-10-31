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
 * Manages only the configured ngrok tunnel to the local MCP server.
 * It intentionally does not expose arbitrary Termux shell execution.
 */
object TunnelController {
    private const val TERMUX_PACKAGE = "com.termux"
    private const val TERMUX_RUN_PERMISSION = "com.termux.permission.RUN_COMMAND"
    private const val TERMUX_HOME = "/data/data/com.termux/files/home"
    private const val BASH = "/data/data/com.termux/files/usr/bin/bash"

    private fun startScript(domain: String) = """
        set -u
        mkdir -p "${'$'}HOME/.bedaim-bridge"

        if command -v ngrok >/dev/null 2>&1; then
          PID_FILE="${'$'}HOME/.bedaim-bridge/ngrok.pid"
          if [ -s "${'$'}PID_FILE" ]; then
            PID="$(cat "${'$'}PID_FILE" 2>/dev/null)"
            case "${'$'}PID" in
              ''|*[!0-9]*) ;;
              *)
                if kill -0 "${'$'}PID" 2>/dev/null; then
                  if command -v curl >/dev/null 2>&1 && curl -fsS --max-time 1 http://127.0.0.1:4040/api/tunnels 2>/dev/null | grep -Fq '${domain}'; then
                    echo "BRIDGE_TUNNEL_ALREADY_RUNNING=TERMUX"
                    exit 0
                  fi
                  kill "${'$'}PID" 2>/dev/null || true
                fi
                ;;
            esac
            rm -f "${'$'}PID_FILE"
          fi
          if command -v curl >/dev/null 2>&1; then
            AGENT_TUNNELS="$(curl -fsS --max-time 1 http://127.0.0.1:4040/api/tunnels 2>/dev/null || true)"
            if printf '%s' "${'$'}AGENT_TUNNELS" | grep -Fq '${domain}'; then
              echo "BRIDGE_TUNNEL_ALREADY_RUNNING=LOCAL_AGENT"
              exit 0
            elif printf '%s' "${'$'}AGENT_TUNNELS" | grep -q '8765'; then
              echo "BRIDGE_ERROR=OTHER_NGROK_DOMAIN_ALREADY_ACTIVE"
              exit 2
            fi
          fi
          nohup ngrok http 8765 --url ${domain} --log=stdout --log-format=logfmt > "${'$'}HOME/.bedaim-bridge/ngrok.log" 2>&1 < /dev/null &
          echo "${'$'}!" > "${'$'}PID_FILE"
          echo "BRIDGE_TUNNEL_STARTING=TERMUX"
          exit 0
        fi

        if command -v proot-distro >/dev/null 2>&1 && proot-distro login debian -- bash -lc 'command -v ngrok >/dev/null 2>&1'; then
          proot-distro login debian -- env BRIDGE_NGROK_DOMAIN=${domain} bash -lc '
            mkdir -p "${'$'}HOME/.bedaim-bridge"
            PID_FILE="${'$'}HOME/.bedaim-bridge/ngrok.pid"
            if [ -s "${'$'}PID_FILE" ]; then
              PID="$(cat "${'$'}PID_FILE" 2>/dev/null)"
              case "${'$'}PID" in
                ""|*[!0-9]*) ;;
                *)
                  if kill -0 "${'$'}PID" 2>/dev/null; then
                    if command -v curl >/dev/null 2>&1 && curl -fsS --max-time 1 http://127.0.0.1:4040/api/tunnels 2>/dev/null | grep -Fq "${'$'}BRIDGE_NGROK_DOMAIN"; then
                      echo "BRIDGE_TUNNEL_ALREADY_RUNNING=DEBIAN"
                      exit 0
                    fi
                    kill "${'$'}PID" 2>/dev/null || true
                  fi
                  ;;
              esac
              rm -f "${'$'}PID_FILE"
            fi
            if command -v curl >/dev/null 2>&1; then
              AGENT_TUNNELS="$(curl -fsS --max-time 1 http://127.0.0.1:4040/api/tunnels 2>/dev/null || true)"
              if printf '%s' "${'$'}AGENT_TUNNELS" | grep -Fq "${'$'}BRIDGE_NGROK_DOMAIN"; then
                echo "BRIDGE_TUNNEL_ALREADY_RUNNING=LOCAL_AGENT"
                exit 0
              elif printf '%s' "${'$'}AGENT_TUNNELS" | grep -q "8765"; then
                echo "BRIDGE_ERROR=OTHER_NGROK_DOMAIN_ALREADY_ACTIVE"
                exit 2
              fi
            fi
            nohup ngrok http 8765 --url "${'$'}BRIDGE_NGROK_DOMAIN" --log=stdout --log-format=logfmt > "${'$'}HOME/.bedaim-bridge/ngrok.log" 2>&1 < /dev/null &
            echo "${'$'}!" > "${'$'}PID_FILE"
            echo "BRIDGE_TUNNEL_STARTING=DEBIAN"
          '
          exit ${'$'}?
        fi

        echo "BRIDGE_ERROR=NGROK_NOT_FOUND"
        exit 127
    """.trimIndent()

    private val stopScript = """
        set +e
        PID_FILE="${'$'}HOME/.bedaim-bridge/ngrok.pid"
        if [ -s "${'$'}PID_FILE" ]; then
          PID="$(cat "${'$'}PID_FILE" 2>/dev/null)"
          case "${'$'}PID" in
            ''|*[!0-9]*) ;;
            *) kill "${'$'}PID" 2>/dev/null && echo "BRIDGE_TUNNEL_STOPPED_TERMUX" || true ;;
          esac
          rm -f "${'$'}PID_FILE"
        fi

        if command -v proot-distro >/dev/null 2>&1 &&
          proot-distro login debian -- bash -lc 'test -s "${'$'}HOME/.bedaim-bridge/ngrok.pid"' >/dev/null 2>&1; then
          proot-distro login debian -- bash -lc '
            PID_FILE="${'$'}HOME/.bedaim-bridge/ngrok.pid"
            if [ -s "${'$'}PID_FILE" ]; then
              PID="$(cat "${'$'}PID_FILE" 2>/dev/null)"
              case "${'$'}PID" in
                ""|*[!0-9]*) ;;
                *) kill "${'$'}PID" 2>/dev/null && echo "BRIDGE_TUNNEL_STOPPED_DEBIAN" || true ;;
              esac
              rm -f "${'$'}PID_FILE"
            fi
          '
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

    /** Returns the public HTTPS URL only when it matches the configured account domain and port. */
    fun readPublicUrl(context: Context): String? {
        val connection = try {
            (URL("http://127.0.0.1:4040/api/tunnels").openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 1_500
                readTimeout = 1_500
                instanceFollowRedirects = false
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
