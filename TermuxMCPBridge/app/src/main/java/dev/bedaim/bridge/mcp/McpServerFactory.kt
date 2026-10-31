package dev.bedaim.bridge.mcp
import android.content.Context
import android.os.Build
import dev.bedaim.bridge.core.SafePathPolicy
import dev.bedaim.bridge.shizuku.ShizukuStorageAccess
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
        val server = Server(Implementation(name="termux-mcp-bridge",version="0.2.0-alpha02"),
            ServerOptions(capabilities=ServerCapabilities(tools=ServerCapabilities.Tools(listChanged=true))))
        server.addTool("bridge_status","Reports Android and bridge capabilities without exposing device identifiers.",
            ToolSchema(properties=buildJsonObject {})) {
            val shizuku = runCatching { rikka.shizuku.Shizuku.pingBinder() }.getOrDefault(false)
            val granted = shizuku && runCatching { rikka.shizuku.Shizuku.checkSelfPermission() == android.content.pm.PackageManager.PERMISSION_GRANTED }.getOrDefault(false)
            val root = listOf("/system/bin/su","/system/xbin/su","/sbin/su").any { File(it).canExecute() }
            CallToolResult(content=listOf(TextContent("Bridge online\nDevice: ${Build.MANUFACTURER} ${Build.MODEL}\nAndroid API: ${Build.VERSION.SDK_INT}\nShizuku: ${if(shizuku) "available" else "not detected"} / ${if(granted) "authorized" else "not authorized"}\nRoot binary: ${if(root) "detected (not validated)" else "not detected"}\nAPK Lab: metadata, signer SHA-256 and ZIP inventory available in the Android UI\nTermux fixed toolchain diagnostic: available in Android UI\nArbitrary Termux command execution: disabled")))
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
        server.addTool("device_storage_list","Lists entries in any accessible directory of shared storage. Accepts /sdcard paths or relative paths; defaults to storage root.",ToolSchema(properties=buildJsonObject{putJsonObject("path"){put("type","string")}})){req->
            val path=req.arguments?.get("path")?.jsonPrimitive?.contentOrNull.orEmpty();val entries=ShizukuStorageAccess.listFiles(path)
            CallToolResult(content=listOf(TextContent(if(entries.isEmpty())"(empty)" else entries.joinToString("\\n"))))
        }
        server.addTool("device_storage_read","Reads a shared-storage file in chunks up to 256 KiB. Use offset for subsequent chunks.",ToolSchema(properties=buildJsonObject{putJsonObject("path"){put("type","string")};putJsonObject("offset"){put("type","integer")};putJsonObject("length"){put("type","integer")}},required=listOf("path","offset","length"))){req->
            val path=req.arguments?.get("path")?.jsonPrimitive?.contentOrNull?:error("path required");val offset=req.arguments?.get("offset")?.jsonPrimitive?.longOrNull?:0L;val length=req.arguments?.get("length")?.jsonPrimitive?.intOrNull?:262144
            val bytes=ShizukuStorageAccess.readFile(path,offset,length);CallToolResult(content=listOf(TextContent("Read ${bytes.size} bytes at offset $offset from $path; base64: "+android.util.Base64.encodeToString(bytes,android.util.Base64.NO_WRAP))))
        }
        server.addTool("device_storage_write","Writes a base64 chunk to shared storage. Set truncate=true only on the first chunk when replacing a file.",ToolSchema(properties=buildJsonObject{putJsonObject("path"){put("type","string")};putJsonObject("base64"){put("type","string")};putJsonObject("offset"){put("type","integer")};putJsonObject("truncate"){put("type","boolean")}},required=listOf("path","base64","offset","truncate"))){req->
            val path=req.arguments?.get("path")?.jsonPrimitive?.contentOrNull?:error("path required");val data=android.util.Base64.decode(req.arguments?.get("base64")?.jsonPrimitive?.contentOrNull?:error("base64 required"),android.util.Base64.DEFAULT);val offset=req.arguments?.get("offset")?.jsonPrimitive?.longOrNull?:0L;val truncate=req.arguments?.get("truncate")?.jsonPrimitive?.booleanOrNull?:false
            CallToolResult(content=listOf(TextContent(ShizukuStorageAccess.writeFile(path,data,offset,truncate))))
        }
        server.addTool("device_storage_mkdir","Creates a directory in shared storage.",ToolSchema(properties=buildJsonObject{putJsonObject("path"){put("type","string")}},required=listOf("path"))){req->val p=req.arguments?.get("path")?.jsonPrimitive?.contentOrNull?:error("path required");CallToolResult(content=listOf(TextContent(ShizukuStorageAccess.makeDirectory(p))))}
        server.addTool("device_storage_delete","Deletes a shared-storage file or directory. recursive=true permanently deletes directory contents.",ToolSchema(properties=buildJsonObject{putJsonObject("path"){put("type","string")};putJsonObject("recursive"){put("type","boolean")}},required=listOf("path","recursive"))){req->val p=req.arguments?.get("path")?.jsonPrimitive?.contentOrNull?:error("path required");val recursive=req.arguments?.get("recursive")?.jsonPrimitive?.booleanOrNull?:false;CallToolResult(content=listOf(TextContent(ShizukuStorageAccess.deletePath(p,recursive))))}
        server.addTool("device_storage_copy","Copies files or directories within shared storage.",ToolSchema(properties=buildJsonObject{putJsonObject("source"){put("type","string")};putJsonObject("destination"){put("type","string")};putJsonObject("overwrite"){put("type","boolean")}},required=listOf("source","destination","overwrite"))){req->val s=req.arguments?.get("source")?.jsonPrimitive?.contentOrNull?:error("source required");val d=req.arguments?.get("destination")?.jsonPrimitive?.contentOrNull?:error("destination required");val o=req.arguments?.get("overwrite")?.jsonPrimitive?.booleanOrNull?:false;CallToolResult(content=listOf(TextContent(ShizukuStorageAccess.copyPath(s,d,o))))}
        server.addTool("device_storage_move","Moves or renames files or directories within shared storage.",ToolSchema(properties=buildJsonObject{putJsonObject("source"){put("type","string")};putJsonObject("destination"){put("type","string")};putJsonObject("overwrite"){put("type","boolean")}},required=listOf("source","destination","overwrite"))){req->val s=req.arguments?.get("source")?.jsonPrimitive?.contentOrNull?:error("source required");val d=req.arguments?.get("destination")?.jsonPrimitive?.contentOrNull?:error("destination required");val o=req.arguments?.get("overwrite")?.jsonPrimitive?.booleanOrNull?:false;CallToolResult(content=listOf(TextContent(ShizukuStorageAccess.movePath(s,d,o))))}
        return server
    }
}
