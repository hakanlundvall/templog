plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

/* Release signing material comes from the environment (GitHub Actions secrets,
 * or a local export) rather than from a file in the repository, which is
 * public. When it is absent the release build still succeeds and simply
 * produces an unsigned APK. */
val releaseStore: String? = System.getenv("ANDROID_KEYSTORE_PATH")
val releaseStorePassword: String? = System.getenv("ANDROID_KEYSTORE_PASSWORD")
val releaseKeyAlias: String? = System.getenv("ANDROID_KEY_ALIAS")
val releaseKeyPassword: String? = System.getenv("ANDROID_KEY_PASSWORD")
val canSignRelease = !releaseStore.isNullOrBlank() &&
    !releaseStorePassword.isNullOrBlank() &&
    !releaseKeyAlias.isNullOrBlank() &&
    !releaseKeyPassword.isNullOrBlank()

/* Release builds in CI are tagged, e.g. "v0.1.0"; anything else keeps the
 * development version. The version code has to grow with each release, or
 * Android refuses to install the new APK over the old one, so it is derived
 * from the numbers in the tag: v1.2.3 -> 10203. */
val tagVersion: String? = System.getenv("APP_VERSION_NAME")?.trim()?.removePrefix("v")
    ?.takeIf { it.isNotEmpty() }

fun versionCodeFrom(version: String?): Int {
    val parts = version?.substringBefore('-')?.split('.').orEmpty()
        .mapNotNull { it.toIntOrNull() }
    if (parts.isEmpty()) return 1
    val (major, minor, patch) = List(3) { parts.getOrElse(it) { 0 } }
    return major * 10000 + minor * 100 + patch
}

android {
    namespace = "io.github.hakanlundvall.templog"
    compileSdk = 35

    defaultConfig {
        applicationId = "io.github.hakanlundvall.templog"
        minSdk = 26
        targetSdk = 35
        versionCode = versionCodeFrom(tagVersion)
        versionName = tagVersion ?: "dev"
    }

    signingConfigs {
        if (canSignRelease) {
            create("release") {
                storeFile = file(releaseStore!!)
                storePassword = releaseStorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            if (canSignRelease) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2024.10.01"))
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.material3:material3")
    debugImplementation("androidx.compose.ui:ui-tooling")
    implementation("androidx.compose.ui:ui-tooling-preview")
}
