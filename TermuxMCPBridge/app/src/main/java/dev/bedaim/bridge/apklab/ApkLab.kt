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

    fun inspect(file: File, packageManager: PackageManager): ApkReport {
        require(file.isFile) { "Selecione um arquivo APK válido." }
        require(file.length() in 1..MAX_APK_SIZE) { "APK vazio ou maior que 1,5 GiB." }

        val flags = if (Build.VERSION.SDK_INT >= 28) {
            PackageManager.GET_SIGNING_CERTIFICATES
        } else {
            @Suppress("DEPRECATION")
            PackageManager.GET_SIGNATURES
        }
        val info = packageManager.getPackageArchiveInfo(file.absolutePath, flags)
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
