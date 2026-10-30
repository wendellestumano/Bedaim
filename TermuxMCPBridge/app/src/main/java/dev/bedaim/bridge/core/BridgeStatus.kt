package dev.bedaim.bridge.core
enum class BridgeState { STOPPED, STARTING, RUNNING, STOPPING, ERROR }
object BridgeStatus {
    @Volatile var state = BridgeState.STOPPED
        private set
    @Volatile var detail = "Stopped"
        private set
    @Synchronized fun update(next: BridgeState, message: String) { state = next; detail = message }
}
