package dev.bedaim.bridge.core

import java.net.URI

/**
 * Builds the narrow Host allowlist for the remote MCP endpoint.
 *
 * The MCP SDK keeps DNS-rebinding protection enabled and validates the HTTP Host header
 * against this list. Only the configured HTTPS ngrok hostname and loopback development
 * hosts are accepted.
 */
object McpHostPolicy {
    fun allowedHosts(configuredNgrokDomain: String): List<String> {
        val uri = runCatching { URI(configuredNgrokDomain.trim().trimEnd('/')) }
            .getOrElse { throw IllegalArgumentException("Invalid configured ngrok domain", it) }
        require(uri.scheme.equals("https", ignoreCase = true)) {
            "Configured ngrok domain must use HTTPS"
        }
        require(!uri.host.isNullOrBlank()) {
            "Configured ngrok domain must contain a valid hostname"
        }
        require(uri.rawUserInfo == null && uri.rawPath.isNullOrEmpty() &&
            uri.rawQuery == null && uri.rawFragment == null) {
            "Configured ngrok value must be a domain URL without path, credentials, query, or fragment"
        }
        return listOf(uri.host, "localhost", "127.0.0.1", "[::1]").distinct()
    }
}
