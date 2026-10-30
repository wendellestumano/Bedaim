plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.serialization")
}
android {
    namespace = "dev.bedaim.bridge"
    compileSdk = 36
    defaultConfig {
        applicationId = "dev.bedaim.bridge"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0-alpha01"
    }
    buildTypes {
        debug { applicationIdSuffix = ".debug" }
        release { isMinifyEnabled = false }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
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
