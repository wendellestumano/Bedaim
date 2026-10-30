package dev.bedaim.bridge.core

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import kotlin.io.path.createTempDirectory

class SafePathPolicyTest {
    private fun tempRoot(): File = createTempDirectory("safe-path-test").toFile()

    @Test
    fun allowsChildPath() {
        val root = tempRoot()
        val got = SafePathPolicy.resolve(root.path, "a/b.txt")
        assertTrue(got.startsWith(root.canonicalPath + File.separator))
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsTraversal() {
        val root = tempRoot()
        SafePathPolicy.resolve(root.path, "../escape.txt")
    }

    @Test(expected = IllegalArgumentException::class)
    fun rejectsAbsolute() {
        val root = tempRoot()
        SafePathPolicy.resolve(root.path, File(root, "x").absolutePath)
    }
}
