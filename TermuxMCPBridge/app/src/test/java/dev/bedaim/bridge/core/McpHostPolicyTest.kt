package dev.bedaim.bridge.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class McpHostPolicyTest {
    @Test
    fun allowsConfiguredNgrokHostAndLoopbackHosts() {
        assertEquals(
            listOf("volatile-basil-quail.ngrok-free.dev", "localhost", "127.0.0.1", "[::1]"),
            McpHostPolicy.allowedHosts("https://volatile-basil-quail.ngrok-free.dev")
        )
    }

    @Test
    fun trimsWhitespaceAndTrailingSlash() {
        assertEquals(
            "volatile-basil-quail.ngrok-free.dev",
            McpHostPolicy.allowedHosts("  https://volatile-basil-quail.ngrok-free.dev/  ").first()
        )
    }

    @Test
    fun rejectsNonHttpsDomain() {
        assertThrows(IllegalArgumentException::class.java) {
            McpHostPolicy.allowedHosts("http://volatile-basil-quail.ngrok-free.dev")
        }
    }

    @Test
    fun rejectsUrlWithPathOrCredentials() {
        assertThrows(IllegalArgumentException::class.java) {
            McpHostPolicy.allowedHosts("https://user@example.ngrok-free.dev/mcp")
        }
    }
}
