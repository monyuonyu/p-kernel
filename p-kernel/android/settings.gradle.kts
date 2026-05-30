/*
 * UMP — Android settings.gradle.kts
 *
 * Single-module project for now (`:app`). If a separate :wire module
 * for the relay protocol appears later, add it here.
 */

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "ump"
include(":app")
