package dev.bedaim.bridge.service

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import dev.bedaim.bridge.core.BridgeStatus
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import java.io.File
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap

/**
 * Narrow, read-only Termux tool runner. No shell, interpreter, package manager,
 * VCS mutation, network client, or arbitrary executable is exposed to MCP.
 */
object ToolchainRunner {
    private const val TERMUX = "com.termux"
    private const val RUN_PERMISSION = "com.termux.permission.RUN_COMMAND"
    private const val PREFIX = "/data/data/com.termux/files/usr/bin/"
    private const val SHARED_ROOT = "/storage/emulated/0/Download/Testing2"
    private const val HOME = "/data/data/com.termux/files/home"
    private const val MAX_ARGS = 48
    private const val MAX_ARG_LENGTH = 2048
    private const val MAX_OUTPUT = 48_000
    private const val TIMEOUT_MS = 45_000L
    private val pending = ConcurrentHashMap<String, CompletableFuture<String>>()

    private val allowed = setOf("readelf", "llvm-objdump", "strings", "sha256sum", "file", "unzip")

    fun complete(id: String?, report: String) {
        if (id.isNullOrBlank()) return
        pending.remove(id)?.complete(report.take(MAX_OUTPUT))
    }

    suspend fun check(context: Context): String = execute(
        context,
        "sh",
        listOf("-c", "command -v readelf || echo readelf:MISSING; command -v llvm-objdump || echo llvm-objdump:MISSING; command -v strings || echo strings:MISSING; command -v sha256sum || echo sha256sum:MISSING; command -v file || echo file:MISSING; command -v unzip || echo unzip:MISSING; command -v git || echo git:MISSING; command -v python || echo python:MISSING; command -v jadx || echo jadx:MISSING; command -v radare2 || echo radare2:MISSING"),
        SHARED_ROOT
    )

    suspend fun run(context: Context, tool: String, args: List<String>, cwd: String): String {
        require(tool in allowed) { "Ferramenta não permitida. Permitidas: ${allowed.sorted().joinToString()}" }
        require(args.size <= MAX_ARGS) { "Máximo de $MAX_ARGS argumentos." }
        require(args.all { it.length <= MAX_ARG_LENGTH && '\u0000' !in it && '\n' !in it && '\r' !in it }) {
            "Argumento inválido ou muito longo."
        }
        require(args.none { it == "--output" || it == "-o" || it.startsWith("--output=") }) {
            "Opções que gravam arquivos não são permitidas."
        }
        val canonicalRoot = File(SHARED_ROOT).canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == canonicalRoot.path || directory.path.startsWith(canonicalRoot.path + File.separator)) {
            "cwd deve permanecer dentro de $SHARED_ROOT"
        }
        require(directory.isDirectory) { "Diretório de trabalho não encontrado." }
        args.filter { !it.startsWith("-") }.forEach { value ->
            val candidate = File(value).let { if (it.isAbsolute) it else File(directory, it.path) }.canonicalFile
            require(candidate.path == canonicalRoot.path || candidate.path.startsWith(canonicalRoot.path + File.separator)) {
                "Arquivos de entrada devem permanecer dentro de $SHARED_ROOT"
            }
        }
        if (tool == "unzip") require(args.firstOrNull() == "-l") { "unzip aceita somente -l (listagem)." }
        return execute(context, tool, args, directory.path)
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String): String {
        require(context.packageManager.getLaunchIntentForPackage(TERMUX) != null) { "Termux não está instalado." }
        require(context.checkSelfPermission(RUN_PERMISSION) == PackageManager.PERMISSION_GRANTED) {
            "Permissão Termux RUN_COMMAND não concedida."
        }
        val id = UUID.randomUUID().toString()
        val future = CompletableFuture<String>()
        pending[id] = future
        val callback = Intent(context, TermuxCommandResultService::class.java).putExtra("bridge_request_id", id)
        val flags = PendingIntent.FLAG_ONE_SHOT or
            (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0)
        val resultPending = PendingIntent.getService(context, id.hashCode(), callback, flags)
        val command = Intent().apply {
            setClassName(TERMUX, "com.termux.app.RunCommandService")
            action = "com.termux.RUN_COMMAND"
            putExtra("com.termux.RUN_COMMAND_PATH", PREFIX + tool)
            putExtra("com.termux.RUN_COMMAND_ARGUMENTS", args.toTypedArray())
            putExtra("com.termux.RUN_COMMAND_WORKDIR", cwd)
            putExtra("com.termux.RUN_COMMAND_BACKGROUND", true)
            putExtra("com.termux.RUN_COMMAND_COMMAND_LABEL", "Bridge tool: $tool")
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Read-only allowlisted analysis command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    withTimeout(TIMEOUT_MS) { future.get() }
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}
