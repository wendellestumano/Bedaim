package dev.bedaim.bridge.security

import android.content.Context
import android.util.Base64
import java.security.SecureRandom

object BridgePreferences {
    private const val PREFS = "bridge_private_settings"
    private const val TOKEN = "mcp_bearer_token"
    private const val TUNNEL_URL = "ngrok_public_url"
    private const val AUTO_START = "bridge_auto_start"

    @Synchronized
    fun getOrCreateToken(context: Context): String {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        prefs.getString(TOKEN, null)?.let { return it }
        return generateAndStore(prefs)
    }

    @Synchronized
    fun rotateToken(context: Context): String {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        return generateAndStore(prefs)
    }

    fun isAutoStartEnabled(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(AUTO_START, true)

    fun setAutoStartEnabled(context: Context, enabled: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(AUTO_START, enabled)
            .apply()
    }

    fun saveTunnelUrl(context: Context, url: String?) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putString(TUNNEL_URL, url?.trim()?.trimEnd('/'))
            .apply()
    }

    fun getTunnelUrl(context: Context): String? =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(TUNNEL_URL, null)

    private fun generateAndStore(prefs: android.content.SharedPreferences): String {
        val bytes = ByteArray(32).also { SecureRandom().nextBytes(it) }
        val value = Base64.encodeToString(
            bytes,
            Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING
        )
        check(prefs.edit().putString(TOKEN, value).commit()) {
            "Could not persist the MCP token"
        }
        return value
    }
}
