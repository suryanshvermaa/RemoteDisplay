plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// CI passes the release version: gradlew assembleRelease -PversionName=1.2.0 -PversionCode=10200
val appVersionName = (findProperty("versionName") as String?) ?: "1.0.0"
val appVersionCode = ((findProperty("versionCode") as String?) ?: "1").toInt()

// Release signing comes from the environment, never from the repository. Without it, the release build is
// left unsigned (CI then publishes the debug-signed APK, which installs fine for sideloading).
val keystorePath: String? = System.getenv("ANDROID_KEYSTORE_FILE")

android {
    namespace = "com.remotedisplay.viewer"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.remotedisplay.viewer"
        minSdk = 26
        targetSdk = 35
        versionCode = appVersionCode
        versionName = appVersionName
    }

    signingConfigs {
        if (keystorePath != null) {
            create("release") {
                storeFile = file(keystorePath)
                storePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("ANDROID_KEY_ALIAS")
                keyPassword = System.getenv("ANDROID_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release")
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
        viewBinding = false
    }
    testOptions {
        unitTests.isReturnDefaultValues = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    testImplementation("junit:junit:4.13.2")
}

// The interop test talks to a real Windows host when given one:
//   gradlew testDebugUnitTest -Drd.host=192.168.1.10:47800 -Drd.key=XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-X
tasks.withType<Test> {
    systemProperty("rd.host", System.getProperty("rd.host") ?: "")
    systemProperty("rd.key", System.getProperty("rd.key") ?: "")
    testLogging {
        events("passed", "skipped", "failed", "standardOut")
        showStandardStreams = true
    }
}
