package dev.bedaim.bridge.apklab

import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Build
import java.io.File
import java.security.MessageDigest
import java.util.Locale
import java.util.zip.ZipFile

data class ApkReport(
    val fileName: String,
    val fileSize: Long,
    val packageName: String,
    val versionName: String,
    val versionCode: Long,
    val minSdk: Int?,
    val targetSdk: Int?,
    val componentSummary: String,
    val requestedPermissions: List<String>,
    val sensitivePermissions: List<String>,
    val signerSha256: List<String>,
    val entryCount: Int,
    val dexCount: Int,
    val nativeLibraries: Int,
    val hasResources: Boolean,
    val hasAssets: Boolean,
    val notableEntries: List<String>
) {
    fun asText(): String = buildString {
        appendLine("APK: $fileName")
        appendLine("Tamanho: ${formatBytes(fileSize)}")
        appendLine("Pacote: $packageName")
        appendLine("Versão: $versionName ($versionCode)")
        appendLine("SDK mínimo: ${minSdk ?: "não identificado"}")
        appendLine("SDK alvo: ${targetSdk ?: "não identificado"}")
        appendLine("Componentes: $componentSummary")
        appendLine("Permissões solicitadas: ${requestedPermissions.size}")
        if (sensitivePermissions.isNotEmpty()) {
            appendLine("Permissões sensíveis para revisão:")
            sensitivePermissions.forEach { appendLine("  $it") }
        }
        appendLine("Entradas ZIP: $entryCount")
        appendLine("DEX: $dexCount")
        appendLine("Bibliotecas nativas: $nativeLibraries")
        appendLine("Resources: ${if (hasResources) "sim" else "não"}")
        appendLine("Assets: ${if (hasAssets) "sim" else "não"}")
        appendLine("SHA-256 do(s) certificado(s):")
        if (signerSha256.isEmpty()) appendLine("  assinatura não encontrada no parser do Android")
        else signerSha256.forEach { appendLine("  $it") }
        if (notableEntries.isNotEmpty()) {
            appendLine("Arquivos relevantes:")
            notableEntries.forEach { appendLine("  $it") }
        }
    }

    companion object {
        private fun formatBytes(bytes: Long): String {
            if (bytes < 1024) return "$bytes B"
            val units = arrayOf("KB", "MB", "GB")
            var value = bytes.toDouble()
            var index = -1
            do { value /= 1024.0; index++ } while (value >= 1024 && index < units.lastIndex)
            return String.format(Locale.US, "%.2f %s", value, units[index])
        }
    }
}

object ApkLab {
    private const val MAX_APK_SIZE = 1_500L * 1024L * 1024L
    private const val MAX_ENTRIES_TO_REPORT = 30
    private val SENSITIVE_PERMISSIONS = setOf(
        "android.permission.READ_SMS",
        "android.permission.RECEIVE_SMS",
        "android.permission.SEND_SMS",
        "android.permission.READ_CONTACTS",
        "android.permission.WRITE_CONTACTS",
        "android.permission.RECORD_AUDIO",
        "android.permission.CAMERA",
        "android.permission.ACCESS_FINE_LOCATION",
        "android.permission.ACCESS_COARSE_LOCATION",
        "android.permission.REQUEST_INSTALL_PACKAGES",
        "android.permission.SYSTEM_ALERT_WINDOW",
        "android.permission.MANAGE_EXTERNAL_STORAGE",
        "android.permission.QUERY_ALL_PACKAGES",
        "android.permission.READ_CALL_LOG",
        "android.permission.WRITE_CALL_LOG"
    )

    fun inspect(file: File, packageManager: PackageManager): ApkReport {
        require(file.isFile) { "Selecione um arquivo APK válido." }
        require(file.length() in 1..MAX_APK_SIZE) { "APK vazio ou maior que 1,5 GiB." }

        val flags = if (Build.VERSION.SDK_INT >= 28) {
            PackageManager.GET_SIGNING_CERTIFICATES or PackageManager.GET_ACTIVITIES or
                PackageManager.GET_SERVICES or PackageManager.GET_RECEIVERS or
                PackageManager.GET_PROVIDERS or PackageManager.GET_PERMISSIONS
        } else {
            @Suppress("DEPRECATION")
            PackageManager.GET_SIGNATURES or PackageManager.GET_ACTIVITIES or
                PackageManager.GET_SERVICES or PackageManager.GET_RECEIVERS or
                PackageManager.GET_PROVIDERS or PackageManager.GET_PERMISSIONS
        }
        val info = loadPackageInfo(packageManager, file, flags)
            ?: throw IllegalArgumentException("O Android não reconheceu este arquivo como APK válido.")
        val signers = signerDigests(info)
        return ZipFile(file).use { zip ->
            val all = zip.entries().asSequence().toList()
            val names = all.map { it.name }
            ApkReport(
                fileName = file.name,
                fileSize = file.length(),
                packageName = info.packageName ?: "desconhecido",
                versionName = info.versionName ?: "sem versão",
                versionCode = versionCode(info),
                minSdk = info.applicationInfo?.minSdkVersion,
                targetSdk = info.applicationInfo?.targetSdkVersion,
                componentSummary = "activities=${info.activities?.size ?: 0}, services=${info.services?.size ?: 0}, receivers=${info.receivers?.size ?: 0}, providers=${info.providers?.size ?: 0}",
                requestedPermissions = info.requestedPermissions?.toList().orEmpty().sorted(),
                sensitivePermissions = info.requestedPermissions?.filter { it in SENSITIVE_PERMISSIONS }?.sorted().orEmpty(),
                signerSha256 = signers,
                entryCount = all.size,
                dexCount = names.count { it.matches(Regex("""classes(\d*)\.dex""")) },
                nativeLibraries = names.count { it.startsWith("lib/") && it.endsWith(".so") },
                hasResources = names.contains("resources.arsc") || names.any { it.startsWith("res/") },
                hasAssets = names.any { it.startsWith("assets/") },
                notableEntries = names.filter {
                    it == "AndroidManifest.xml" || it == "resources.arsc" ||
                        it.matches(Regex("""classes(\d*)\.dex""")) ||
                        it.startsWith("lib/") || it.startsWith("META-INF/")
                }.take(MAX_ENTRIES_TO_REPORT)
            )
        }
    }

    @Suppress("DEPRECATION")
    private fun loadPackageInfo(
        packageManager: PackageManager,
        file: File,
        flags: Int
    ): PackageInfo? {
        return if (Build.VERSION.SDK_INT >= 33) {
            packageManager.getPackageArchiveInfo(
                file.absolutePath,
                PackageManager.PackageInfoFlags.of(flags.toLong())
            )
        } else {
            packageManager.getPackageArchiveInfo(file.absolutePath, flags)
        }
    }

    fun duplicateAsBackup(source: File, backupDirectory: File): File {
        require(source.isFile && source.length() in 1..MAX_APK_SIZE) { "APK de origem inválido." }
        if (!backupDirectory.exists() && !backupDirectory.mkdirs()) {
            throw IllegalStateException("Não foi possível criar a pasta de backups.")
        }
        val destination = File(
            backupDirectory,
            "${source.nameWithoutExtension}_copy_${System.currentTimeMillis()}.${source.extension.ifBlank { "apk" }}"
        ).canonicalFile
        require(destination.parentFile == backupDirectory.canonicalFile) { "Destino inválido." }
        source.copyTo(destination, overwrite = false)
        return destination
    }

    @Suppress("DEPRECATION")
    private fun versionCode(info: PackageInfo): Long =
        if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else info.versionCode.toLong()

    private fun signerDigests(info: PackageInfo): List<String> {
        val signatures = if (Build.VERSION.SDK_INT >= 28) {
            info.signingInfo?.apkContentsSigners?.toList().orEmpty()
        } else {
            @Suppress("DEPRECATION")
            info.signatures?.toList().orEmpty()
        }
        return signatures.map { signature ->
            MessageDigest.getInstance("SHA-256").digest(signature.toByteArray())
                .joinToString(":") { "%02X".format(it) }
        }
    }
}
