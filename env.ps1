#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Sets environment variables required by build.ps1 for Android builds.

.DESCRIPTION
    Exports ANDROID_HOME and ANDROID_NDK_HOME for the current session.
    Source this script before running: .\build.ps1 -Target dist-android

.EXAMPLE
    . .\env.ps1
    .\build.ps1 -Target dist-android
#>

$env:ANDROID_HOME = "C:\Users\JSuppe\AppData\Local\Android\Sdk"

# Auto-detect latest NDK version under SDK
$NdkDir = Join-Path $env:ANDROID_HOME 'ndk'
if (Test-Path $NdkDir) {
    $Latest = Get-ChildItem $NdkDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($Latest) {
        $env:ANDROID_NDK_HOME = $Latest.FullName
    } else {
        Write-Warning "No NDK versions found under $NdkDir. Install one via Android Studio SDK Manager."
    }
} else {
    Write-Warning "NDK directory not found: $NdkDir. Install NDK via Android Studio SDK Manager."
}

# Add SDK's CMake bin (includes Ninja) to PATH
$SdkCmakeDir = Join-Path $env:ANDROID_HOME 'cmake'
if (Test-Path $SdkCmakeDir) {
    $LatestCmake = Get-ChildItem $SdkCmakeDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($LatestCmake) {
        $CmakeBin = Join-Path $LatestCmake.FullName 'bin'
        if (Test-Path $CmakeBin) {
            $env:PATH = "$CmakeBin;$env:PATH"
        }
    }
}

Write-Host "ANDROID_HOME     = $env:ANDROID_HOME"
Write-Host "ANDROID_NDK_HOME = $env:ANDROID_NDK_HOME"
