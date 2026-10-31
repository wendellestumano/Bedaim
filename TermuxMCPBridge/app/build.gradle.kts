plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.serialization")
}

val persistentStoreFile = System.getenv("BRIDGE_SIGNING_STORE_FILE")
val persistentStorePassword = System.getenv("BRIDGE_SIGNING_STORE_PASSWORD")
val persistentKeyAlias = System.getenv("BRIDGE_SIGNING_KEY_ALIAS")
val persistentKeyPassword = System.getenv("BRIDGE_SIGNING_KEY_PASSWORD")
val hasPersistentSigning = !persistentStoreFile.isNullOrBlank() &&
    !persistentStorePassword.isNullOrBlank() &&
    !persistentKeyAlias.isNullOrBlank() &&
    !persistentKeyPassword.isNullOrBlank()

android {
    buildFeatures { aidl = true }
    namespace = "dev.bedaim.bridge"
    compileSdk = 36
    defaultConfig {
        applicationId = "dev.bedaim.bridge"
        minSdk = 26
        targetSdk = 35
        versionCode = 4
        versionName = "0.2.0-alpha04"
    }
    signingConfigs {
        if (hasPersistentSigning) {
            create("bridgePersistent") {
                storeFile = file(persistentStoreFile!!)
                storePassword = persistentStorePassword!!
                keyAlias = persistentKeyAlias!!
                keyPassword = persistentKeyPassword!!
            }
        }
    }
    buildTypes {
        debug {
            applicationIdSuffix = ".debug"
            if (hasPersistentSigning) {
                signingConfig = signingConfigs.getByName("bridgePersistent")
            }
        }
        release {
            isMinifyEnabled = false
            if (hasPersistentSigning) {
                signingConfig = signingConfigs.getByName("bridgePersistent")
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.17.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.11.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.11.0")
    implementation("io.ktor:ktor-server-core:3.5.2")
    implementation("io.ktor:ktor-server-cio:3.5.2")
    implementation("io.ktor:ktor-server-auth:3.5.2")
    implementation("io.modelcontextprotocol:kotlin-sdk-server:0.15.0")
    implementation("dev.rikka.shizuku:api:13.1.5")
    implementation("dev.rikka.shizuku:provider:13.1.5")
    testImplementation("junit:junit:4.13.2")
}
