package dev.bedaim.bridge.service

import android.app.Service
import android.content.Intent
import android.os.IBinder
import dev.bedaim.bridge.core.BridgeStatus

class TermuxCommandResultService : Service() {
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val result = intent?.getBundleExtra(TERMUX_RESULT_BUNDLE)
        if (result == null) {
            BridgeStatus.recordEvent("Termux: resposta vazia ou incompatível; confirme a versão do Termux.")
        } else {
            val stdout = result.getString(TERMUX_STDOUT).orEmpty().take(MAX_OUTPUT)
            val stderr = result.getString(TERMUX_STDERR).orEmpty().take(MAX_OUTPUT)
            val exitCode = result.getInt(TERMUX_EXIT_CODE, -1)
            val internalError = result.getInt(TERMUX_ERROR, 0)
            val errorMessage = result.getString(TERMUX_ERROR_MESSAGE).orEmpty().take(2_000)
            val report = buildString {
                append("Termux toolchain check • exit=$exitCode • internalError=$internalError")
                if (stdout.isNotBlank()) append("\nOUT:\n$stdout")
                if (stderr.isNotBlank()) append("\nERR:\n$stderr")
                if (errorMessage.isNotBlank()) append("\nTermux: $errorMessage")
            }
            BridgeStatus.recordEvent(report)
        }
        stopSelf(startId)
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val MAX_OUTPUT = 6_000
        private const val TERMUX_RESULT_BUNDLE = "result"
        private const val TERMUX_STDOUT = "stdout"
        private const val TERMUX_STDERR = "stderr"
        private const val TERMUX_EXIT_CODE = "exitCode"
        private const val TERMUX_ERROR = "err"
        private const val TERMUX_ERROR_MESSAGE = "errmsg"
    }
}
