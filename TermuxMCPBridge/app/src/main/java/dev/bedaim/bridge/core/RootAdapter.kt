package dev.bedaim.bridge.core
import java.io.File
import java.util.concurrent.TimeUnit
object RootAdapter {
    data class ProbeResult(val present: Boolean, val authorized: Boolean, val detail: String)
    fun probeExplicitly(): ProbeResult {
        val su = listOf("/system/bin/su","/system/xbin/su","/sbin/su","/vendor/bin/su").firstOrNull { File(it).canExecute() }
            ?: return ProbeResult(false, false, "No executable su binary found")
        return runCatching {
            val p = ProcessBuilder(su, "-c", "id -u").redirectErrorStream(true).start()
            if (!p.waitFor(2, TimeUnit.SECONDS)) { p.destroyForcibly(); ProbeResult(true,false,"Root probe timed out") }
            else {
                val out = p.inputStream.bufferedReader().use { it.readText().trim() }
                val ok = p.exitValue() == 0 && out.lineSequence().lastOrNull() == "0"
                ProbeResult(true,ok,if(ok) "Root authorized" else "su found but not authorized")
            }
        }.getOrElse { ProbeResult(true,false,"Root probe failed: ${it.javaClass.simpleName}") }
    }
}
