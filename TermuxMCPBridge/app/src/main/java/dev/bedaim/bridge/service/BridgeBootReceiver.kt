package dev.bedaim.bridge.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat
import dev.bedaim.bridge.core.BridgeStatus
import dev.bedaim.bridge.security.BridgePreferences

/**
 * Best-effort recovery after device reboot or an app update.
 * Auto-start can be disabled from the dashboard or by explicitly stopping the service.
 */
class BridgeBootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        when (intent?.action) {
            Intent.ACTION_BOOT_COMPLETED,
            Intent.ACTION_MY_PACKAGE_REPLACED -> {
                if (!BridgePreferences.isAutoStartEnabled(context)) {
                    BridgeStatus.recordEvent("Auto-start: desativado pelo usuário; ignorando boot/update.")
                    return
                }
                try {
                    ContextCompat.startForegroundService(
                        context,
                        Intent(context, BridgeService::class.java).setAction(BridgeService.ACTION_START)
                    )
                    BridgeStatus.recordEvent("Auto-start: recuperação do Bridge solicitada após boot/update.")
                } catch (error: Throwable) {
                    BridgeStatus.recordEvent("Auto-start: Android bloqueou a partida — " + error.javaClass.simpleName)
                }
            }
        }
    }
}
