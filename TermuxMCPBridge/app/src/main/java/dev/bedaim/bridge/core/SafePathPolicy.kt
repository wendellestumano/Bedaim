package dev.bedaim.bridge.core
import java.io.File
object SafePathPolicy {
    fun resolve(rootPath: String, relativePath: String): String {
        require(relativePath.isNotBlank()) { "Path must not be blank" }
        val root = File(rootPath).canonicalFile
        require(!File(relativePath).isAbsolute) { "Absolute paths are not allowed" }
        val resolved = File(root, relativePath).canonicalFile
        val prefix = root.path.trimEnd(File.separatorChar) + File.separator
        require(resolved.path.startsWith(prefix)) { "Path escapes the approved root" }
        return resolved.path
    }
}
