package dev.bedaim.bridge.mcp
import android.content.Context
import android.os.Build
import dev.bedaim.bridge.core.SafePathPolicy
import dev.bedaim.bridge.shizuku.ShizukuStorageAccess
import dev.bedaim.bridge.service.ToolchainRunner
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
        val server = Server(Implementation(name="termux-mcp-bridge",version="0.2.0-alpha10"),
            ServerOptions(capabilities=ServerCapabilities(tools=ServerCapabilities.Tools(listChanged=true))))
        server.addTool("bridge_status","Reports Android and bridge capabilities without exposing device identifiers.",
            ToolSchema(properties=buildJsonObject {})) {
            val shizuku = runCatching { rikka.shizuku.Shizuku.pingBinder() }.getOrDefault(false)
            val granted = shizuku && runCatching { rikka.shizuku.Shizuku.checkSelfPermission() == android.content.pm.PackageManager.PERMISSION_GRANTED }.getOrDefault(false)
            val root = listOf("/system/bin/su","/system/xbin/su","/sbin/su").any { File(it).canExecute() }
            CallToolResult(content=listOf(TextContent("Bridge online\nDevice: ${Build.MANUFACTURER} ${Build.MODEL}\nAndroid API: ${Build.VERSION.SDK_INT}\nShizuku: ${if(shizuku) "available" else "not detected"} / ${if(granted) "authorized" else "not authorized"}\nRoot binary: ${if(root) "detected (not validated)" else "not detected"}\nAPK Lab: metadata, signer SHA-256 and ZIP inventory available in the Android UI\nTermux toolchain: expanded diagnostic includes Git, Python, JADX, Radare2, tmux, compilers and build tools\nTermux shell: local opt-in; arbitrary commands and persistent interactive tmux sessions available")))
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

        server.addTool("termux_exec","Executes a command in the Termux shell. Requires explicit local opt-in in the Android Bridge app. Runs as the Termux app user (no root).",ToolSchema(properties=buildJsonObject {
            putJsonObject("command") { put("type","string"); put("maxLength",12000) }
            putJsonObject("cwd") { put("type","string") }
            putJsonObject("timeout_seconds") { put("type","integer"); put("minimum",1); put("maximum",300) }
        },required=listOf("command","cwd"))) { req ->
            val command=req.arguments?.get("command")?.jsonPrimitive?.contentOrNull ?: error("command required")
            val cwd=req.arguments?.get("cwd")?.jsonPrimitive?.contentOrNull ?: error("cwd required")
            val seconds=req.arguments?.get("timeout_seconds")?.jsonPrimitive?.intOrNull ?: 60
            CallToolResult(content=listOf(TextContent(ToolchainRunner.shell(app,command,cwd,seconds*1000L))))
        }


        server.addTool("termux_session_start","Starts a persistent interactive Bash session in Termux using tmux. Requires local remote-shell opt-in; runs as the Termux app user, not root.",ToolSchema(properties=buildJsonObject {
            putJsonObject("command") { put("type","string"); put("maxLength",12000) }
            putJsonObject("cwd") { put("type","string") }
        },required=listOf("command","cwd"))) { req ->
            val command=req.arguments?.get("command")?.jsonPrimitive?.contentOrNull ?: error("command required")
            val cwd=req.arguments?.get("cwd")?.jsonPrimitive?.contentOrNull ?: error("cwd required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.sessionStart(app,command,cwd))))
        }
        server.addTool("termux_session_send","Sends input to an active Termux tmux session. press_enter submits the line. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("session_id") { put("type","string") }
            putJsonObject("input") { put("type","string"); put("maxLength",8000) }
            putJsonObject("press_enter") { put("type","boolean") }
        },required=listOf("session_id","input","press_enter"))) { req ->
            val id=req.arguments?.get("session_id")?.jsonPrimitive?.contentOrNull ?: error("session_id required")
            val input=req.arguments?.get("input")?.jsonPrimitive?.contentOrNull ?: error("input required")
            val enter=req.arguments?.get("press_enter")?.jsonPrimitive?.booleanOrNull ?: true
            CallToolResult(content=listOf(TextContent(ToolchainRunner.sessionSend(app,id,input,enter))))
        }
        server.addTool("termux_session_output","Captures recent output from a persistent Termux tmux session. start_line ranges from -2000 to 0. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("session_id") { put("type","string") }
            putJsonObject("start_line") { put("type","integer"); put("minimum",-2000); put("maximum",0) }
        },required=listOf("session_id","start_line"))) { req ->
            val id=req.arguments?.get("session_id")?.jsonPrimitive?.contentOrNull ?: error("session_id required")
            val start=req.arguments?.get("start_line")?.jsonPrimitive?.intOrNull ?: -200
            CallToolResult(content=listOf(TextContent(ToolchainRunner.sessionOutput(app,id,start))))
        }
        server.addTool("termux_session_list","Lists active persistent Termux tmux sessions. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {})) {
            CallToolResult(content=listOf(TextContent(ToolchainRunner.sessionList(app))))
        }
        server.addTool("termux_session_stop","Stops a persistent Termux tmux session by its Bridge-generated session id. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("session_id") { put("type","string") }
        },required=listOf("session_id"))) { req ->
            val id=req.arguments?.get("session_id")?.jsonPrimitive?.contentOrNull ?: error("session_id required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.sessionStop(app,id))))
        }

        server.addTool("toolchain_check","Checks installed Termux analysis tools using a fixed diagnostic. No arbitrary shell input.",ToolSchema(properties=buildJsonObject {})) {
            CallToolResult(content=listOf(TextContent(ToolchainRunner.check(app))))
        }
        server.addTool("toolchain_run","Runs one read-only allowlisted binary in /sdcard/Download/Testing2 or a subdirectory. Allowed: readelf, llvm-objdump, strings, sha256sum, file, unzip (-l only). No shell, Python, Git, package installation, network tools, or write operations.",ToolSchema(properties=buildJsonObject {
            putJsonObject("tool") { put("type","string"); put("enum", buildJsonArray { listOf("readelf","llvm-objdump","strings","sha256sum","file","unzip").forEach { add(it) } }) }
            putJsonObject("args") { put("type","array"); putJsonObject("items") { put("type","string") }; put("maxItems",48) }
            putJsonObject("cwd") { put("type","string") }
        },required=listOf("tool","args","cwd"))) { req ->
            val tool=req.arguments?.get("tool")?.jsonPrimitive?.contentOrNull ?: error("tool required")
            val args=req.arguments?.get("args")?.jsonArray?.map { it.jsonPrimitive.content } ?: error("args array required")
            val cwd=req.arguments?.get("cwd")?.jsonPrimitive?.contentOrNull ?: error("cwd required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.run(app,tool,args,cwd))))
        }

        server.addTool("termux_process_list","Lists processes owned by the Termux app UID. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {})) {
            CallToolResult(content=listOf(TextContent(ToolchainRunner.processList(app))))
        }
        server.addTool("termux_process_inspect","Inspects one process only when it belongs to the Termux app UID. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("pid") { put("type","string"); put("pattern","^[0-9]{1,10}$") }
        },required=listOf("pid"))) { req ->
            val pid=req.arguments?.get("pid")?.jsonPrimitive?.contentOrNull ?: error("pid required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.processInspect(app,pid))))
        }
        server.addTool("termux_process_stop","Sends SIGTERM only to a process owned by the Termux app UID. PID 1 and invalid PIDs are rejected. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("pid") { put("type","string"); put("pattern","^[0-9]{1,10}$") }
        },required=listOf("pid"))) { req ->
            val pid=req.arguments?.get("pid")?.jsonPrimitive?.contentOrNull ?: error("pid required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.processStop(app,pid))))
        }
        server.addTool("termux_job_start","Starts a predefined Gradle/CMake/Ninja task in a persistent tmux session. Returns a job/session UUID. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("task") { put("type","string"); put("enum",buildJsonArray { listOf("gradle_tests","gradle_debug_apk","gradle_clean_debug","cmake_configure","cmake_build","ninja_build").forEach { add(it) } }) }
            putJsonObject("cwd") { put("type","string") }
        },required=listOf("task","cwd"))) { req ->
            val task=req.arguments?.get("task")?.jsonPrimitive?.contentOrNull ?: error("task required")
            val cwd=req.arguments?.get("cwd")?.jsonPrimitive?.contentOrNull ?: error("cwd required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.jobStart(app,task,cwd))))
        }
        server.addTool("termux_job_status","Returns recent output for a persistent build job by UUID. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("job_id") { put("type","string") }
        },required=listOf("job_id"))) { req ->
            val id=req.arguments?.get("job_id")?.jsonPrimitive?.contentOrNull ?: error("job_id required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.jobStatus(app,id))))
        }
        server.addTool("termux_job_list","Lists active tmux sessions/jobs. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {})) {
            CallToolResult(content=listOf(TextContent(ToolchainRunner.jobList(app))))
        }
        server.addTool("termux_job_stop","Stops a persistent build job by its Bridge session UUID. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("job_id") { put("type","string") }
        },required=listOf("job_id"))) { req ->
            val id=req.arguments?.get("job_id")?.jsonPrimitive?.contentOrNull ?: error("job_id required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.jobStop(app,id))))
        }
        server.addTool("project_git_inspect","Read-only Git status, diff, log or branch inspection inside Termux HOME/shared storage. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("operation") { put("type","string"); put("enum",buildJsonArray { listOf("status","diff","log","branches").forEach { add(it) } }) }
            putJsonObject("cwd") { put("type","string") }
        },required=listOf("operation","cwd"))) { req ->
            val operation=req.arguments?.get("operation")?.jsonPrimitive?.contentOrNull ?: error("operation required")
            val cwd=req.arguments?.get("cwd")?.jsonPrimitive?.contentOrNull ?: error("cwd required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.gitInspect(app,operation,cwd))))
        }
        server.addTool("android_project_diagnose","Checks Android/NDK project files, installed build tools, SDK/NDK environment and Git state without modifying the project. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("cwd") { put("type","string") }
        },required=listOf("cwd"))) { req ->
            val cwd=req.arguments?.get("cwd")?.jsonPrimitive?.contentOrNull ?: error("cwd required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.diagnoseAndroidProject(app,cwd))))
        }
        server.addTool("native_inspect","Read-only APK/ELF inspection: summary, ELF headers, sections, symbols, disassembly, strings or APK ZIP inventory. File must be under Termux HOME or shared storage. Requires local remote-shell opt-in.",ToolSchema(properties=buildJsonObject {
            putJsonObject("path") { put("type","string") }
            putJsonObject("mode") { put("type","string"); put("enum",buildJsonArray { listOf("summary","elf_headers","sections","symbols","disassembly","strings","apk_inventory").forEach { add(it) } }) }
        },required=listOf("path","mode"))) { req ->
            val path=req.arguments?.get("path")?.jsonPrimitive?.contentOrNull ?: error("path required")
            val mode=req.arguments?.get("mode")?.jsonPrimitive?.contentOrNull ?: error("mode required")
            CallToolResult(content=listOf(TextContent(ToolchainRunner.inspectNative(app,path,mode))))
        }

        return server
    }
}