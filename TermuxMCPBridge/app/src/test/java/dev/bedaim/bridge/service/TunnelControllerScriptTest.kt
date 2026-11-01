package dev.bedaim.bridge.service

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TunnelControllerScriptTest {
    @Test
    fun prootIsStartedAsThePersistentTermuxOwnedProcess() {
        val script = TunnelController.buildStartScript("https://example.ngrok-free.dev")

        assertTrue(script.contains("nohup proot-distro login debian -- /usr/local/bin/ngrok http 8765"))
        assertTrue(script.contains("PID_FILE=\"\$BASE_DIR/ngrok.pid\""))
        assertTrue(script.contains("PID=\"\$!\""))
        assertTrue(script.contains("OWNER=DEBIAN_PROOT"))
        assertFalse(script.contains("proot-distro login debian -- env BRIDGE_NGROK_DOMAIN"))
    }

    @Test
    fun startScriptSerializesRequestsAndChecksTheExpectedEndpoint() {
        val script = TunnelController.buildStartScript("https://example.ngrok-free.dev")

        assertTrue(script.contains("ngrok-start.lock"))
        assertTrue(script.contains("BRIDGE_TUNNEL_START_IN_PROGRESS"))
        assertTrue(script.contains("BRIDGE_TUNNEL_ALREADY_RUNNING=LOCAL_AGENT"))
        assertTrue(script.contains("BRIDGE_TUNNEL_ALREADY_RUNNING=REMOTE_ENDPOINT"))
        assertTrue(script.contains(".well-known/oauth-protected-resource/mcp"))
        assertTrue(script.contains("NGROK_DOMAIN_ACTIVE_WITH_WRONG_UPSTREAM"))
        assertTrue(script.contains("BRIDGE_TUNNEL_PROCESS_ALIVE"))
        assertTrue(script.contains("LOCK_PID=\"$(cat"))
        assertTrue(script.contains("AGENT_TUNNELS=\"$(curl"))
    }

    @Test
    fun startScriptDoesNotKillUnownedProcesses() {
        val script = TunnelController.buildStartScript("https://example.ngrok-free.dev")

        assertTrue(script.contains("PID_OWNERSHIP_MISMATCH"))
        assertTrue(script.contains("case \"\$CMD\" in"))
    }

    @Test
    fun remoteMetadataMustMatchTheExactMcpResource() {
        val domain = "https://example.ngrok-free.dev"
        val valid = """{"resource":"https://example.ngrok-free.dev/mcp"}"""
        val wrong = """{"resource":"https://other.ngrok-free.dev/mcp"}"""

        assertTrue(TunnelController.isExpectedRemoteResourceMetadata(domain, valid))
        assertFalse(TunnelController.isExpectedRemoteResourceMetadata(domain, wrong))
        assertFalse(TunnelController.isExpectedRemoteResourceMetadata(domain, "not-json"))
    }

    @Test
    fun endpointConflictUsesABoundedRetryCooldown() {
        val script = TunnelController.buildStartScript("https://example.ngrok-free.dev")

        assertTrue(script.contains("ERR_NGROK_334"))
        assertTrue(script.contains("ngrok-retry-after"))
        assertTrue(script.contains("BRIDGE_TUNNEL_RETRY_COOLDOWN"))
        assertTrue(script.contains("NGROK_ENDPOINT_ALREADY_ONLINE"))
    }
}
