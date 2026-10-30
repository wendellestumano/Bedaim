package dev.bedaim.bridge.core

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

enum class BridgeState { STOPPED, STARTING, RUNNING, STOPPING, ERROR }

object BridgeStatus {
    private const val MAX_EVENTS = 100
    private val events = ArrayDeque<String>()

    @Volatile var state = BridgeState.STOPPED
        private set
    @Volatile var detail = "Stopped"
        private set

    @Synchronized
    fun update(next: BridgeState, message: String) {
        state = next
        detail = message
        val stamp = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        events.addFirst("[$stamp] $next — $message")
        while (events.size > MAX_EVENTS) events.removeLast()
    }

    @Synchronized
    fun history(): List<String> = events.toList()
}
