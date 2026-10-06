plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "jp.azukimap.m5f9p"
    compileSdk = 35

    defaultConfig {
        applicationId = "jp.azukimap.m5f9p"
        minSdk = 31     // Android 12。Bluetoothの権限が新しい方式だけで済む
        targetSdk = 35
        versionCode = 2
        versionName = "0.2.0"
    }

    // 画面は、Windowsアプリと共用の web/ をビルドしたもの(mise run web-build)を取り込む
    sourceSets["main"].assets.srcDir("../../web/dist")

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.webkit:webkit:1.12.1")
}
