package dev.bedaim.bridge.mcp
import android.content.Context
import android.os.Build
import dev.bedaim.bridge.core.SafePathPolicy
import io.modelcontextprotocol.kotlin.sdk.server.Server
import io.modelcontextprotocol.kotlin.sdk.server.ServerOptions
import io.modelcontextprotocol.kotlin.sdk.types.*
import kotlinx.serialization.json.*
import java.io.File
object McpServerFactory {
    private const val MAX_READ = 1_048_576L
    private const val MAX_WRITE = 262_144
    fun create(context: Context): Server {
        val app = context.applicationContext
        val workspace = File(app.filesDir, "workspace").apply { mkdirs() }.canonicalFile
        val server = Server(Implementation(name="termux-mcp-bridge",version="0.1.0-alpha01"),
            ServerOptions(capabilities=ServerCapabilities(tools=ServerCapabilities.Tools(listChanged=true))))
        server.addTool("bridge_status","Reports Android and bridge capabilities without exposing device identifiers.",
            ToolSchema(properties=buildJsonObject {})) {
            val shizuku = runCatching { rikka.shizuku.Shizuku.pingBinder() }.getOrDefault(false)
            val granted = shizuku && runCatching { rikka.shizuku.Shizuku.checkSelfPermission() == android.content.pm.PackageManager.PERMISSION_GRANTED }.getOrDefault(false)
            val root = listOf("/system/bin/su","/system/xbin/su","/sbin/su").any { File(it).canExecute() }
            CallToolResult(content=listOf(TextContent("Bridge online\nDevice: ${Build.MANUFACTURER} ${Build.MODEL}\nAndroid API: ${Build.VERSION.SDK_INT}\nShizuku: ${if(shizuku) "available" else "not detected"} / ${if(granted) "authorized" else "not authorized"}\nRoot binary: ${if(root) "detected (not validated)" else "not detected"}\nTermux execution: not implemented in this alpha")))
        }
        server.addTool("workspace_list","Lists files inside the private workspace. Optional relative path.",
            ToolSchema(properties=buildJsonObject { putJsonObject("path"){put("type","string")} })) { req ->
            val rel=req.arguments?.get("path")?.jsonPrimitive?.contentOrNull.orEmpty()
            val dir=if(rel.isBlank()) workspace else File(SafePathPolicy.resolve(workspace.path,rel))
            require(dir.isDirectory) { "Directory not found" }
            val entries=dir.listFiles().orEmpty().filter { runCatching { it.canonicalPath.startsWith(workspace.path+File.separator) }.getOrDefault(false) }.sortedBy{it.name}.take(200)
            CallToolResult(content=listOf(TextContent(if(entries.isEmpty()) "(empty)" else entries.joinToString("\n"){"${if(it.isDirectory) "DIR" else "FILE"} ${it.name} ${if(it.isFile) it.length() else 0} bytes"})))
        }
        server.addTool("workspace_read","Reads a UTF-8 file in the private workspace (max 1 MiB).",
            ToolSchema(properties=buildJsonObject{putJsonObject("path"){put("type","string")}},required=listOf("path"))) { req ->
            val rel=req.arguments?.get("path")?.jsonPrimitive?.contentOrNull ?: error("path required")
            val f=File(SafePathPolicy.resolve(workspace.path,rel))
            require(f.isFile && f.length()<=MAX_READ) { "File missing or exceeds 1 MiB" }
            CallToolResult(content=listOf(TextContent(f.readText(Charsets.UTF_8))))
        }
        server.addTool("workspace_write","Creates or replaces a UTF-8 file in the private workspace (max 256 KiB).",
            ToolSchema(properties=buildJsonObject{putJsonObject("path"){put("type","string")};putJsonObject("content"){put("type","string")}},required=listOf("path","content"))) { req ->
            val rel=req.arguments?.get("path")?.jsonPrimitive?.contentOrNull ?: error("path required")
            val content=req.arguments?.get("content")?.jsonPrimitive?.contentOrNull ?: error("content required")
            require(content.length<=MAX_WRITE) { "Content exceeds 256 KiB" }
            val f=File(SafePathPolicy.resolve(workspace.path,rel)); f.parentFile?.mkdirs()
            val safe=File(SafePathPolicy.resolve(workspace.path,rel))
            safe.writeText(content,Charsets.UTF_8)
            CallToolResult(content=listOf(TextContent("Wrote ${content.length} characters to ${safe.name}")))
        }
        return server
    }
}
