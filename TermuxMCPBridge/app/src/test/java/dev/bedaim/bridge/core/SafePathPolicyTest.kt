package dev.bedaim.bridge.core
import org.junit.Assert.*
import org.junit.Test
import java.io.File
class SafePathPolicyTest {
    @Test fun allowsChildPath(){val root=createTempDir();val got=SafePathPolicy.resolve(root.path,"a/b.txt");assertTrue(got.startsWith(root.canonicalPath+File.separator))}
    @Test(expected=IllegalArgumentException::class) fun rejectsTraversal(){val root=createTempDir();SafePathPolicy.resolve(root.path,"../escape.txt")}
    @Test(expected=IllegalArgumentException::class) fun rejectsAbsolute(){val root=createTempDir();SafePathPolicy.resolve(root.path,File(root,"x").absolutePath)}
}
