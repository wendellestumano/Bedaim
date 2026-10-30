package dev.bedaim.bridge.security
import android.content.Context
import android.util.Base64
import java.security.SecureRandom
object BridgePreferences {
    private const val PREFS = "bridge_private_settings"
    private const val TOKEN = "mcp_bearer_token"
    @Synchronized fun getOrCreateToken(context: Context): String {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        prefs.getString(TOKEN, null)?.let { return it }
        val bytes = ByteArray(32).also { SecureRandom().nextBytes(it) }
        val value = Base64.encodeToString(bytes, Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)
        prefs.edit().putString(TOKEN, value).apply()
        return value
    }
}
