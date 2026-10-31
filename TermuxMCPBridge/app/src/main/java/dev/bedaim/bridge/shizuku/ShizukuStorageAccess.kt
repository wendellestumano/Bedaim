package dev.bedaim.bridge.shizuku

import android.content.ComponentName
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.IBinder
import dev.bedaim.bridge.IBridgeUserService
import dev.bedaim.bridge.BuildConfig
import rikka.shizuku.Shizuku
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Lazy Shizuku UserService connection used by the MCP storage tools. */
object ShizukuStorageAccess {
    private val lock = Any()
    @Volatile private var remote: IBridgeUserService? = null
    private var binding = false
    private var connected = CountDownLatch(1)
    private var args: Shizuku.UserServiceArgs? = null

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            synchronized(lock) {
                remote = IBridgeUserService.Stub.asInterface(binder)
                binding = false
                connected.countDown()
            }
        }
        override fun onServiceDisconnected(name: ComponentName) {
            synchronized(lock) {
                remote = null
                binding = false
                connected = CountDownLatch(1)
            }
        }
    }

    fun listFiles(path: String): Array<String> = service().listFiles(path)
    fun readFile(path: String, offset: Long, length: Int): ByteArray = service().readFile(path, offset, length)
    fun writeFile(path: String, content: ByteArray, offset: Long, truncate: Boolean): String = service().writeFile(path, content, offset, truncate)
    fun makeDirectory(path: String): String = service().makeDirectory(path)
    fun deletePath(path: String, recursive: Boolean): String = service().deletePath(path, recursive)
    fun copyPath(source: String, destination: String, overwrite: Boolean): String = service().copyPath(source, destination, overwrite)
    fun movePath(source: String, destination: String, overwrite: Boolean): String = service().movePath(source, destination, overwrite)

    private fun service(): IBridgeUserService {
        check(Shizuku.pingBinder()) { "Shizuku is not running" }
        check(Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED) {
            "Shizuku permission is not granted to Termux MCP Bridge"
        }
        val latch: CountDownLatch
        synchronized(lock) {
            remote?.let { return it }
            if (!binding) {
                binding = true
                connected = CountDownLatch(1)
                args = Shizuku.UserServiceArgs(
                    ComponentName(BuildConfig.APPLICATION_ID, BridgeUserService::class.java.name)
                ).daemon(false).processNameSuffix("storage").debuggable(false).version(1)
                try {
                    Shizuku.bindUserService(args!!, connection)
                } catch (error: Throwable) {
                    binding = false
                    throw IllegalStateException("Unable to bind Shizuku storage service: ${error.message}", error)
                }
            }
            latch = connected
        }
        check(latch.await(12, TimeUnit.SECONDS)) { "Timed out waiting for Shizuku storage service" }
        return remote ?: error("Shizuku storage service disconnected")
    }
}
