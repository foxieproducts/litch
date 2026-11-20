import java.util.Properties

plugins {
    id("com.android.application")
    id("kotlin-android")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

val effectiveStoreSigning = providers.gradleProperty("storeSigning").orNull
val environmentStoreSigning = providers.environmentVariable("ORG_GRADLE_PROJECT_storeSigning").orNull
require(environmentStoreSigning == null || environmentStoreSigning == effectiveStoreSigning) {
    "Conflicting storeSigning selectors; remove the environment or project-property override"
}
val storeSigning = when (effectiveStoreSigning) {
    null, "false" -> false
    "true" -> true
    else -> error("storeSigning must be true or false")
}
val storeSigningProperties = Properties()
if (storeSigning) {
    val propertiesFile = rootProject.file("key.properties")
    require(propertiesFile.isFile) {
        "Store signing requires android/key.properties"
    }
    propertiesFile.inputStream().use { storeSigningProperties.load(it) }
    for (name in listOf("storeFile", "storePassword", "keyAlias", "keyPassword")) {
        require(!storeSigningProperties.getProperty(name).isNullOrBlank()) {
            "Store signing requires $name in android/key.properties"
        }
    }
    require(rootProject.file(storeSigningProperties.getProperty("storeFile")).isFile) {
        "Store signing requires an existing keystore file"
    }
}

android {
    namespace = "com.foxieproducts.litch_mobile"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_17.toString()
    }

    defaultConfig {
        // TODO: Specify your own unique Application ID (https://developer.android.com/studio/build/application-id.html).
        applicationId = "com.foxieproducts.litch_mobile"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        minSdk = flutter.minSdkVersion
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName
    }

    signingConfigs {
        if (storeSigning) {
            create("store") {
                storeFile = rootProject.file(storeSigningProperties.getProperty("storeFile"))
                storePassword = storeSigningProperties.getProperty("storePassword")
                keyAlias = storeSigningProperties.getProperty("keyAlias")
                keyPassword = storeSigningProperties.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            // Preserve the signature used by local phone QA unless explicitly requested.
            signingConfig = signingConfigs.getByName(if (storeSigning) "store" else "debug")
        }
    }
}

flutter {
    source = "../.."
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
