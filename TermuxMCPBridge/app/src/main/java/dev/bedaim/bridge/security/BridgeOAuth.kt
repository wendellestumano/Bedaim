package dev.bedaim.bridge.security

import io.ktor.server.application.*
import io.ktor.server.request.*
import io.ktor.server.response.*
import io.ktor.http.*
import kotlinx.serialization.json.*
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64
import java.util.concurrent.ConcurrentHashMap

/** OAuth 2.1 authorization-code server. Codes are single-use and PKCE S256-bound. */
object BridgeOAuth {
    private const val RESOURCE_PATH = "/mcp"
    private const val ACCESS_TTL = 3600L
    private const val CODE_TTL = 300L
    private val random = SecureRandom()
    private data class Client(val redirects: Set<String>)
    private data class Code(val clientId: String, val redirect: String, val challenge: String,
        val state: String?, val resource: String, val expires: Long)
    private data class Access(val clientId: String, val resource: String, val scope: String, val expires: Long)
    private data class Refresh(val clientId: String, val resource: String, val scope: String, val expires: Long)
    private val clients = ConcurrentHashMap<String, Client>()
    private val codes = ConcurrentHashMap<String, Code>()
    private val access = ConcurrentHashMap<String, Access>()
    private val refresh = ConcurrentHashMap<String, Refresh>()

    private fun token(bytes: Int = 32): String {
        val b = ByteArray(bytes); random.nextBytes(b)
        return Base64.getUrlEncoder().withoutPadding().encodeToString(b)
    }
    private fun now() = System.currentTimeMillis() / 1000
    private fun equal(a: String, b: String) = MessageDigest.isEqual(a.toByteArray(), b.toByteArray())
    private fun validRedirect(value: String) = runCatching {
        val u = java.net.URI(value)
        (u.scheme == "https" && !u.host.isNullOrBlank()) ||
            (u.scheme == "http" && u.host in setOf("localhost", "127.0.0.1", "::1"))
    }.getOrDefault(false)

    suspend fun handle(call: ApplicationCall, issuerInput: String): Boolean {
        val issuer = issuerInput.trimEnd("/")
        val resource = "$issuer$RESOURCE_PATH"
        val path = call.request.path()
        when {
            path == "/.well-known/oauth-protected-resource" ||
                path == "/.well-known/oauth-protected-resource/mcp" -> {
                call.respondText(buildJsonObject {
                    put("resource", resource)
                    putJsonArray("authorization_servers") { add(issuer) }
                    putJsonArray("scopes_supported") { add("mcp") }
                    putJsonArray("bearer_methods_supported") { add("header") }
                }.toString(), ContentType.Application.Json); return true
            }
            path == "/.well-known/oauth-authorization-server" -> {
                call.respondText(buildJsonObject {
                    put("issuer", issuer)
                    put("authorization_endpoint", "$issuer/authorize")
                    put("token_endpoint", "$issuer/token")
                    put("registration_endpoint", "$issuer/register")
                    putJsonArray("response_types_supported") { add("code") }
                    putJsonArray("grant_types_supported") { add("authorization_code"); add("refresh_token") }
                    putJsonArray("code_challenge_methods_supported") { add("S256") }
                    putJsonArray("token_endpoint_auth_methods_supported") { add("none") }
                    putJsonArray("scopes_supported") { add("mcp") }
                }.toString(), ContentType.Application.Json); return true
            }
            path == "/register" && call.request.httpMethod == HttpMethod.Post -> {
                val body = runCatching { Json.parseToJsonElement(call.receiveText()).jsonObject }.getOrNull()
                val redirects = body?.get("redirect_uris")?.jsonArray?.mapNotNull {
                    it.jsonPrimitive.contentOrNull
                }?.toSet().orEmpty()
                if (redirects.isEmpty() || redirects.size > 10 || redirects.any { !validRedirect(it) }) {
                    call.respondText("""{"error":"invalid_redirect_uri"}""", ContentType.Application.Json, HttpStatusCode.BadRequest); return true
                }
                val id = token(24)
                clients[id] = Client(redirects)
                call.respondText(buildJsonObject {
                    put("client_id", id); put("client_id_issued_at", now())
                    putJsonArray("redirect_uris") { redirects.forEach { add(it) } }
                    put("token_endpoint_auth_method", "none")
                    putJsonArray("grant_types") { add("authorization_code"); add("refresh_token") }
                    putJsonArray("response_types") { add("code") }
                }.toString(), ContentType.Application.Json, HttpStatusCode.Created); return true
            }
            path == "/authorize" && call.request.httpMethod == HttpMethod.Get -> {
                val q = call.request.queryParameters
                val id = q["client_id"].orEmpty()
                val redirect = q["redirect_uri"].orEmpty()
                val challenge = q["code_challenge"].orEmpty()
                val requestedResource = q["resource"].orEmpty()
                val valid = clients[id]?.let { redirect in it.redirects } == true &&
                    validRedirect(redirect) && q["response_type"] == "code" &&
                    q["code_challenge_method"] == "S256" &&
                    challenge.matches(Regex("[A-Za-z0-9_-]{43,128}")) &&
                    requestedResource == resource && (q["state"]?.length ?: 0) <= 512
                if (!valid) {
                    call.respondText("Solicitação OAuth inválida. Volte ao ChatGPT e tente novamente.",
                        ContentType.Text.Plain, HttpStatusCode.BadRequest); return true
                }
                val hidden = q.entries().joinToString("") { entry ->
                    entry.value.joinToString("") { value ->
                        "<input type=\"hidden\" name=\"${entry.key.html()}\" value=\"${value.html()}\">"
                    }
                }
                call.respondText(
                    """<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1"><title>Autorizar Termux MCP Bridge</title><style>body{font:16px system-ui;background:#101318;color:#f2f4f8;max-width:520px;margin:12vh auto;padding:24px}section{background:#1b2028;padding:26px;border-radius:18px}button{background:#00dca0;border:0;border-radius:10px;padding:14px 22px;font-weight:700}p{color:#b5bdc9}</style><section><h2>Autorizar Termux MCP Bridge?</h2><p>O ChatGPT poderá usar as ferramentas MCP deste dispositivo. Autorize somente se você iniciou esta conexão.</p><form method="post" action="/authorize">$hidden<button name="decision" value="approve">Autorizar</button> <button name="decision" value="deny" style="background:#414955;color:white">Negar</button></form></section>""",
                    ContentType.Text.Html); return true
            }
            path == "/authorize" && call.request.httpMethod == HttpMethod.Post -> {
                val p = call.receiveParameters()
                val id = p["client_id"].orEmpty()
                val redirect = p["redirect_uri"].orEmpty()
                if (clients[id]?.let { redirect in it.redirects } != true || !validRedirect(redirect)) {
                    call.respond(HttpStatusCode.BadRequest); return true
                }
                val state = p["state"]
                val target = if (p["decision"] == "approve") {
                    val code = token()
                    codes[code] = Code(id, redirect, p["code_challenge"].orEmpty(), state,
                        p["resource"].orEmpty(), now() + CODE_TTL)
                    "$redirect?code=$code" + (state?.let { "&state=${it.enc()}" } ?: "")
                } else "$redirect?error=access_denied" + (state?.let { "&state=${it.enc()}" } ?: "")
                call.respondRedirect(target, false); return true
            }
            path == "/token" && call.request.httpMethod == HttpMethod.Post -> {
                val p = call.receiveParameters()
                when (p["grant_type"]) {
                    "authorization_code" -> {
                        val c = codes.remove(p["code"].orEmpty())
                        val verifier = p["code_verifier"].orEmpty()
                        val digest = Base64.getUrlEncoder().withoutPadding().encodeToString(
                            MessageDigest.getInstance("SHA-256").digest(verifier.toByteArray(Charsets.US_ASCII)))
                        if (c == null || c.expires < now() || c.clientId != p["client_id"] ||
                            c.redirect != p["redirect_uri"] || c.resource != resource ||
                            verifier.length !in 43..128 || !equal(c.challenge, digest)) {
                            call.respondText("""{"error":"invalid_grant"}""", ContentType.Application.Json, HttpStatusCode.BadRequest); return true
                        }
                        val at = token(); val rt = token()
                        access[at] = Access(c.clientId, resource, "mcp", now() + ACCESS_TTL)
                        refresh[rt] = Refresh(c.clientId, resource, "mcp", now() + 30L * 86400)
                        call.respondText(buildJsonObject {
                            put("access_token", at); put("token_type", "Bearer"); put("expires_in", ACCESS_TTL)
                            put("refresh_token", rt); put("scope", "mcp")
                        }.toString(), ContentType.Application.Json); return true
                    }
                    "refresh_token" -> {
                        val old = p["refresh_token"].orEmpty()
                        val prior = refresh.remove(old)
                        if (prior == null || prior.expires < now() || prior.clientId != p["client_id"]) {
                            call.respondText("""{"error":"invalid_grant"}""", ContentType.Application.Json, HttpStatusCode.BadRequest); return true
                        }
                        val at = token(); val rt = token()
                        access[at] = Access(prior.clientId, resource, prior.scope, now() + ACCESS_TTL)
                        refresh[rt] = prior.copy(expires = now() + 30L * 86400)
                        call.respondText(buildJsonObject {
                            put("access_token", at); put("token_type", "Bearer"); put("expires_in", ACCESS_TTL)
                            put("refresh_token", rt); put("scope", prior.scope)
                        }.toString(), ContentType.Application.Json); return true
                    }
                    else -> {
                        call.respondText("""{"error":"unsupported_grant_type"}""", ContentType.Application.Json, HttpStatusCode.BadRequest); return true
                    }
                }
            }
            path == "/revoke" && call.request.httpMethod == HttpMethod.Post -> {
                val p = call.receiveParameters()
                access.remove(p["token"].orEmpty()); refresh.remove(p["token"].orEmpty())
                call.respond(HttpStatusCode.OK); return true
            }
        }
        return false
    }

    fun isValidAccessToken(value: String, issuerInput: String): Boolean {
        val item = access[value] ?: return false
        if (item.expires < now() || item.resource != "${issuerInput.trimEnd("/")}/mcp" ||
            "mcp" !in item.scope.split(" ")) {
            access.remove(value); return false
        }
        return true
    }
    private fun String.html() = replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        .replace("\"", "&quot;").replace("'", "&#39;")
    private fun String.enc() = java.net.URLEncoder.encode(this, "UTF-8")
}
