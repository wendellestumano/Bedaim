package dev.bedaim.bridge.service

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import dev.bedaim.bridge.core.BridgeStatus
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import dev.bedaim.bridge.security.BridgePreferences

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
    private const val TIMEOUT_MS = 300_000L
    private val pending = ConcurrentHashMap<String, CompletableFuture<String>>()

    private val allowed = setOf("readelf", "llvm-objdump", "strings", "sha256sum", "file", "unzip")

    fun complete(id: String?, report: String) {
        if (id.isNullOrBlank()) return
        pending.remove(id)?.complete(report.take(MAX_OUTPUT))
    }

    suspend fun check(context: Context): String = execute(
        context,
        "sh",
        listOf("-c", "command -v readelf || echo readelf:MISSING; command -v llvm-objdump || echo llvm-objdump:MISSING; command -v strings || echo strings:MISSING; command -v sha256sum || echo sha256sum:MISSING; command -v file || echo file:MISSING; command -v unzip || echo unzip:MISSING; command -v git || echo git:MISSING; command -v python || echo python:MISSING; command -v jadx || echo jadx:MISSING; command -v radare2 || echo radare2:MISSING; command -v tmux || echo tmux:MISSING; command -v clang || echo clang:MISSING; command -v cmake || echo cmake:MISSING; command -v ninja || echo ninja:MISSING; command -v gradle || echo gradle:MISSING; command -v java || echo java:MISSING; command -v node || echo node:MISSING; command -v npm || echo npm:MISSING; command -v apktool || echo apktool:MISSING; command -v aapt || echo aapt:MISSING; command -v proot-distro || echo proot-distro:MISSING; command -v ps || echo ps:MISSING; command -v id || echo id:MISSING; command -v kill || echo kill:MISSING"),
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
        // This process belongs to the Bridge app, not Termux. Android scoped
        // storage can deny stat/list access to Download even though Termux can
        // access it. Validate the path boundary here; RUN_COMMAND, executing
        // under Termux's UID, reports a missing or inaccessible working dir.
        args.filter { !it.startsWith("-") }.forEach { value ->
            val candidate = File(value).let { if (it.isAbsolute) it else File(directory, it.path) }.canonicalFile
            require(candidate.path == canonicalRoot.path || candidate.path.startsWith(canonicalRoot.path + File.separator)) {
                "Arquivos de entrada devem permanecer dentro de $SHARED_ROOT"
            }
        }
        if (tool == "unzip") require(args.firstOrNull() == "-l") { "unzip aceita somente -l (listagem)." }
        return execute(context, tool, args, directory.path)
    }

    suspend fun shell(context: Context, command: String, cwd: String, timeoutMs: Long): String {
        requireRemoteShell(context)
        require(command.isNotBlank() && command.length <= 12_000) { "Comando vazio ou acima de 12.000 caracteres." }
        require(timeoutMs in 1_000L..300_000L) { "Timeout deve ficar entre 1 e 300 segundos." }
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        // The Bridge runs under its own Android UID and cannot stat Termux's
        // private HOME. Do not call File.isDirectory here: that check always
        // fails across app sandboxes even when Termux itself can access cwd.
        // RUN_COMMAND executes as Termux and will report a genuine bad cwd.
        return execute(context, "bash", listOf("-lc", command), directory.path, timeoutMs)
    }


    /** Starts a persistent interactive Termux shell in a tmux session. */
    suspend fun sessionStart(context: Context, command: String, cwd: String): String {
        requireRemoteShell(context)
        require(command.isNotBlank() && command.length <= 12_000 && '\u0000' !in command) {
            "Comando vazio, inválido ou acima de 12.000 caracteres."
        }
        val directory = validateShellDirectory(cwd)
        val id = UUID.randomUUID().toString()
        execute(context, "tmux", listOf("new-session", "-d", "-s", id, "-c", directory.path),
            File("/data/data/com.termux/files/home").path, 15_000L)
        try {
            execute(context, "tmux", listOf("send-keys", "-t", id, "-l", command),
                File("/data/data/com.termux/files/home").path, 10_000L)
            execute(context, "tmux", listOf("send-keys", "-t", id, "Enter"),
                File("/data/data/com.termux/files/home").path, 10_000L)
        } catch (error: Throwable) {
            runCatching { execute(context, "tmux", listOf("kill-session", "-t", id),
                File("/data/data/com.termux/files/home").path, 5_000L) }
            throw error
        }
        BridgeStatus.recordEvent("Termux: sessão tmux criada ($id).")
        return "SESSION_ID=$id\nCWD=" + directory.path + "\nStarted persistent interactive Bash session."
    }

    /** Sends text to a session; pressEnter=true submits it as a command/input line. */
    suspend fun sessionSend(context: Context, sessionId: String, input: String, pressEnter: Boolean): String {
        requireRemoteShell(context)
        val id = validateSessionId(sessionId)
        require(input.length <= 8_000 && '\u0000' !in input && '\n' !in input && '\r' !in input) {
            "Entrada inválida ou acima de 8.000 caracteres; envie uma linha por chamada."
        }
        execute(context, "tmux", listOf("send-keys", "-t", id, "-l", input),
            File("/data/data/com.termux/files/home").path, 10_000L)
        if (pressEnter) {
            execute(context, "tmux", listOf("send-keys", "-t", id, "Enter"),
                File("/data/data/com.termux/files/home").path, 10_000L)
        }
        return "Input sent to session " + id + if (pressEnter) " and submitted." else "."
    }

    /** Captures the current tmux pane, including recent output. */
    suspend fun sessionOutput(context: Context, sessionId: String, startLine: Int): String {
        requireRemoteShell(context)
        val id = validateSessionId(sessionId)
        require(startLine in -2_000..0) { "start_line must be between -2000 and 0." }
        return execute(context, "tmux", listOf("capture-pane", "-p", "-S", startLine.toString(), "-t", id),
            File("/data/data/com.termux/files/home").path, 10_000L)
    }

    suspend fun sessionStop(context: Context, sessionId: String): String {
        requireRemoteShell(context)
        val id = validateSessionId(sessionId)
        val result = execute(context, "tmux", listOf("kill-session", "-t", id),
            File("/data/data/com.termux/files/home").path, 10_000L)
        BridgeStatus.recordEvent("Termux: sessão tmux encerrada ($id).")
        return result.ifBlank { "Stopped session $id." }
    }

    suspend fun sessionList(context: Context): String {
        requireRemoteShell(context)
        return execute(context, "bash", listOf("-lc",
            "tmux list-sessions -F '#{session_name}\\t#{session_windows}\\t#{session_attached}' 2>/dev/null || true"),
            File("/data/data/com.termux/files/home").path, 10_000L).ifBlank { "(no active tmux sessions)" }
    }


    // Process manager: every operation is scoped to the Termux app UID.
    suspend fun processList(context: Context): String {
        requireRemoteShell(context)
        val uid = execute(context, "id", listOf("-u"), HOME).trim()
        return execute(context, "ps", listOf("-u", uid, "-o", "pid,ppid,stat,etime,args"), HOME, 15_000L)
            .ifBlank { "(no visible Termux processes)" }
    }

    suspend fun processInspect(context: Context, pidValue: String): String {
        requireRemoteShell(context)
        val pid = validatePid(pidValue)
        val owner = execute(context, "ps", listOf("-o", "uid=", "-p", pid), HOME, 10_000L).trim()
        val uid = execute(context, "id", listOf("-u"), HOME, 10_000L).trim()
        require(owner == uid) { "PID não pertence ao usuário do Termux ou não está visível." }
        return execute(context, "ps", listOf("-o", "pid,ppid,stat,etime,args", "-p", pid), HOME, 10_000L)
    }

    suspend fun processStop(context: Context, pidValue: String): String {
        requireRemoteShell(context)
        val pid = validatePid(pidValue)
        val owner = execute(context, "ps", listOf("-o", "uid=", "-p", pid), HOME, 10_000L).trim()
        val uid = execute(context, "id", listOf("-u"), HOME, 10_000L).trim()
        require(owner == uid) { "Recusado: PID não pertence ao usuário do Termux." }
        execute(context, "kill", listOf("-TERM", pid), HOME, 10_000L)
        BridgeStatus.recordEvent("Termux: SIGTERM enviado ao PID $pid.")
        return "SIGTERM enviado ao PID $pid (UID Termux $uid)."
    }

    private fun validatePid(value: String): String {
        require(Regex("^[0-9]{1,10}$").matches(value)) { "PID inválido." }
        require(value.toLongOrNull() != null && value.toLong() > 1L) { "PID protegido ou inválido." }
        return value
    }

    // Structured build jobs use fixed commands; arbitrary shell remains separately opt-in.
    suspend fun jobStart(context: Context, task: String, cwd: String): String {
        requireRemoteShell(context)
        val command = when (task) {
            "gradle_tests" -> "if [ -x ./gradlew ]; then ./gradlew --no-daemon :app:testDebugUnitTest; else gradle --no-daemon :app:testDebugUnitTest; fi"
            "gradle_debug_apk" -> "if [ -x ./gradlew ]; then ./gradlew --no-daemon :app:assembleDebug; else gradle --no-daemon :app:assembleDebug; fi"
            "gradle_clean_debug" -> "if [ -x ./gradlew ]; then ./gradlew --no-daemon clean :app:testDebugUnitTest :app:assembleDebug; else gradle --no-daemon clean :app:testDebugUnitTest :app:assembleDebug; fi"
            "cmake_configure" -> "cmake -S . -B build"
            "cmake_build" -> "cmake --build build --parallel 2"
            "ninja_build" -> "ninja -C build"
            else -> error("Tarefa não permitida. Use gradle_tests, gradle_debug_apk, gradle_clean_debug, cmake_configure, cmake_build ou ninja_build.")
        }
        val directory = validateShellDirectory(cwd)
        return "JOB_TASK=$task\n" + sessionStart(context, command, directory.path)
    }

    suspend fun jobStatus(context: Context, jobId: String): String =
        sessionOutput(context, jobId, -200)

    suspend fun jobList(context: Context): String = sessionList(context)

    suspend fun jobStop(context: Context, jobId: String): String = sessionStop(context, jobId)

    // Read-only Git project inspection; mutations remain available only through the opted-in shell.
    suspend fun gitInspect(context: Context, operation: String, cwd: String): String {
        requireRemoteShell(context)
        val directory = validateShellDirectory(cwd)
        val args = when (operation) {
            "status" -> listOf("--no-pager", "status", "--short", "--branch")
            "diff" -> listOf("--no-pager", "diff", "--no-ext-diff", "--unified=3")
            "log" -> listOf("--no-pager", "log", "-12", "--oneline", "--decorate")
            "branches" -> listOf("--no-pager", "branch", "--list", "--no-color")
            else -> error("Operação Git não permitida.")
        }
        return execute(context, "git", args, directory.path, 30_000L)
            .ifBlank { "(sem saída; verifique se o diretório é um repositório Git)" }
    }

    suspend fun diagnoseAndroidProject(context: Context, cwd: String): String {
        requireRemoteShell(context)
        val directory = validateShellDirectory(cwd)
        val script = """
            echo '== PROJECT FILES =='
            for f in settings.gradle settings.gradle.kts build.gradle build.gradle.kts gradlew gradle.properties local.properties CMakeLists.txt Android.mk; do
              [ ! -e "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "\${'
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "\${'
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}x" 2>/dev/null || echo "\${'
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}ANDROID_HOME" "\${'
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}x" 2>/dev/null || echo "\${'
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}ANDROID_SDK_ROOT" "\${'
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}x" 2>/dev/null || echo "\${'
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}x" 2>/dev/null || echo "\${'
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f" ] || ls -ld "\${'
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
                } finally {
                    pending.remove(id)
                }
            }
        } catch (error: Throwable) {
            pending.remove(id)
            throw error
        }
    }
}}f"
            done
            echo '== TOOLCHAIN =='
            for x in java javac gradle adb sdkmanager cmake ninja clang ndk-build aapt apkanalyzer jadx apktool radare2; do
              command -v "$x" 2>/dev/null || echo "$x: MISSING"
            done
            echo '== ANDROID ENV =='
            printf 'ANDROID_HOME=%s\nANDROID_SDK_ROOT=%s\nANDROID_NDK_HOME=%s\n' "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$ANDROID_NDK_HOME"
            echo '== GIT =='
            git status --short --branch 2>/dev/null || echo 'Not a Git worktree'
        """.trimIndent()
        return execute(context, "bash", listOf("-lc", script), directory.path, 30_000L)
    }

    suspend fun inspectNative(context: Context, pathValue: String, mode: String): String {
        requireRemoteShell(context)
        require(pathValue.isNotBlank() && pathValue.length <= 2048 && '\u0000' !in pathValue) { "Caminho inválido." }
        val home = File(HOME).canonicalFile.path
        val shared = File("/storage/emulated/0").canonicalFile.path
        val candidate = File(pathValue).canonicalFile
        require(candidate.path == home || candidate.path.startsWith(home + File.separator) ||
            candidate.path == shared || candidate.path.startsWith(shared + File.separator)) {
            "Arquivo deve estar no HOME do Termux ou armazenamento compartilhado."
        }
        val args = when (mode) {
            "summary" -> listOf(candidate.path)
            "elf_headers" -> listOf("-h", "-l", candidate.path)
            "sections" -> listOf("-S", candidate.path)
            "symbols" -> listOf("-Ws", candidate.path)
            "disassembly" -> listOf("-d", "--no-show-raw-insn", candidate.path)
            "strings" -> listOf("-n", "8", candidate.path)
            "apk_inventory" -> listOf("-l", candidate.path)
            else -> error("Modo inválido: summary, elf_headers, sections, symbols, disassembly, strings ou apk_inventory.")
        }
        val tool = when (mode) {
            "summary" -> "file"
            "elf_headers", "sections", "symbols" -> "readelf"
            "disassembly" -> "llvm-objdump"
            "strings" -> "strings"
            "apk_inventory" -> "unzip"
            else -> error("Modo inválido.")
        }
        return execute(context, tool, args, candidate.parentFile?.path ?: HOME, 60_000L)
    }

    private fun validateShellDirectory(cwd: String): File {
        val home = File("/data/data/com.termux/files/home").canonicalFile
        val shared = File("/storage/emulated/0").canonicalFile
        val directory = File(cwd).canonicalFile
        require(directory.path == home.path || directory.path.startsWith(home.path + File.separator) ||
            directory.path == shared.path || directory.path.startsWith(shared.path + File.separator)) {
            "cwd deve estar no HOME do Termux ou no armazenamento compartilhado."
        }
        return directory
    }

    private fun requireRemoteShell(context: Context) {
        require(BridgePreferences.isTermuxRemoteShellEnabled(context)) {
            "Acesso remoto ao shell do Termux está desativado. Ative-o no painel do Bridge."
        }
    }

    private fun validateSessionId(value: String): String {
        require(Regex("^[0-9a-fA-F-]{36}$").matches(value)) { "Invalid session id." }
        return value
    }

    private suspend fun execute(context: Context, tool: String, args: List<String>, cwd: String, timeoutMs: Long = TIMEOUT_MS): String {
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
            putExtra("com.termux.RUN_COMMAND_COMMAND_DESCRIPTION", "Bridge-managed Termux command.")
            putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", resultPending)
        }
        try {
            context.startService(command)
            BridgeStatus.recordEvent("Toolchain: iniciado $tool (id=$id).")
            return withContext(Dispatchers.IO) {
                try {
                    future.get(timeoutMs, TimeUnit.MILLISECONDS)
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