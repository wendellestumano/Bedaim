package dev.bedaim.bridge.shizuku

import androidx.annotation.Keep
import dev.bedaim.bridge.IBridgeUserService
import java.io.File
import kotlin.system.exitProcess

/** Shizuku shell-identity service. Filesystem access is confined to Download/Testing2. */
@Keep
class BridgeUserService : IBridgeUserService.Stub() {
    private val root = File("/sdcard/Download/Testing2").canonicalFile

    override fun listFiles(relativePath: String?): Array<String> {
        val dir = resolve(relativePath.orEmpty())
        require(dir.isDirectory) { "Directory not found" }
        return dir.listFiles().orEmpty().sortedBy { it.name }.take(MAX_ENTRIES).map { entry ->
            "${if (entry.isDirectory) "DIR" else "FILE"} ${entry.name} ${if (entry.isFile) entry.length() else 0} bytes"
        }.toTypedArray()
    }

    override fun readFile(relativePath: String, maxBytes: Int): ByteArray {
        require(maxBytes in 1..MAX_READ) { "Invalid read limit" }
        val file = resolve(relativePath)
        require(file.isFile && file.length() <= maxBytes) { "File missing or exceeds 1 MiB" }
        return file.readBytes()
    }

    override fun writeFile(relativePath: String, content: ByteArray, overwrite: Boolean): String {
        require(content.size <= MAX_WRITE) { "Content exceeds 256 KiB" }
        val file = resolve(relativePath)
        require(!file.exists() || overwrite) { "File already exists; set overwrite=true to replace it" }
        file.parentFile?.mkdirs()
        val safeFile = resolve(relativePath)
        safeFile.writeBytes(content)
        return "Wrote ${content.size} bytes to ${safeFile.name} inside Download/Testing2"
    }

    private fun resolve(relativePath: String): File {
        require(relativePath.isNotBlank()) { "Path must not be blank" }
        require(!File(relativePath).isAbsolute) { "Absolute paths are not allowed" }
        val candidate = File(root, relativePath).canonicalFile
        val prefix = root.path.trimEnd(File.separatorChar) + File.separator
        require(candidate.path.startsWith(prefix)) { "Path escapes Download/Testing2" }
        return candidate
    }

    override fun destroy() { exitProcess(0) }

    companion object {
        private const val MAX_ENTRIES = 200
        private const val MAX_READ = 1_048_576
        private const val MAX_WRITE = 262_144
    }
}
