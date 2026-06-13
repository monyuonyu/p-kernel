/*
 * UMP — app module build.gradle.kts
 *
 * Builds:
 *   app-debug.apk        — installable on aarch64 (arm64-v8a) phones,
 *                          API 26+ (Oreo+; matches Foreground Service
 *                          requirements).
 *
 * Native side: CMakeLists.txt in src/main/cpp/ pulls the existing
 * arch/linux/aarch64 + arch/common + kernel/common sources plus the
 * Phase B v2 relay client (net_relay.c, net_dispatch.c, relay/sha256.c)
 * — single canonical source shared with boot/linux/Makefile.
 */

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace  = "io.pkernel"
    compileSdk = 34
    ndkVersion = "26.3.11579264"

    defaultConfig {
        applicationId = "io.pkernel.ump"
        minSdk        = 26   // Oreo — required by foreground services
        targetSdk     = 34
        // Bumped from 1 (wave-36 D4 build) so an installed wave-36 APK
        // upgrades cleanly. This build carries the living mind (LM-7..11),
        // real words (r3_vocab), Path W/W2 merge, Ed25519 signing
        // (ed25519/sign/sign_entropy), and the 32-language manifesto.
        versionCode   = 8
        versionName   = "0.4.4"

        ndk {
            // Phones are aarch64; the kernel's cpu_support.S is aarch64.
            // armeabi-v7a / x86 / x86_64 would need separate ports.
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=gnu11")
                arguments += listOf("-DANDROID_STL=none")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    externalNativeBuild {
        cmake {
            path    = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Keep the .so close to the JNI bridge — no compression so the kernel
    // can mmap directly (Android 6+ extracts uncompressed .so anyway).
    packaging {
        jniLibs.useLegacyPackaging = false
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
}
