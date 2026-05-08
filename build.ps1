#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Build orchestration script for collab_media_controller.
    
.DESCRIPTION
    Builds Windows and Android CMake targets and packages installable artifacts.
    - Windows: native CMake build
    - Android: NDK cross-compile via CMake toolchain

.PARAMETER Target
    Build target: 'all', 'desktop', 'android', 'test', 'dist', 'clean'
    Default: 'dist'

.PARAMETER Config
    Build configuration: 'Debug' or 'Release'
    Default: 'Release'

.PARAMETER Install
    For Android target, run CMake install after build.
    Default: $false

.PARAMETER DistDir
    Output directory for installable desktop artifacts (headers/libs/cmake config).
    Default: ../collab_media_controller_dist

.PARAMETER DistBuildDir
    Temporary CMake build directory used for dist packaging.
    Default: build_artifact

.PARAMETER DistConfigs
    One or more desktop configurations to include in the dist package.
    Default: Debug, Release

.PARAMETER DistZipPath
    Zip file path for packaged dist artifacts.
    Default: ../collab_media_controller_dist.zip

.PARAMETER AndroidABIs
    ABI list used by Target=android.

.PARAMETER DistAndroidABIs
    ABI list used by Target=dist and Target=dist-android.

.PARAMETER AndroidApiLevel
    Android API level for NDK builds.

.PARAMETER CleanDist
    If set, remove existing dist output/build directories before creating artifacts.
    Default: $false

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Target all -Config Release
    .\build.ps1 -Target desktop -Config Release
    .\build.ps1 -Target android -AndroidABIs arm64-v8a,x86_64
    .\build.ps1 -Target test
    .\build.ps1 -Target dist
    .\build.ps1 -Target dist -CleanDist
    .\build.ps1 -Target dist -DistConfigs Debug,Release -DistAndroidABIs arm64-v8a,armeabi-v7a,x86_64
    .\build.ps1 -Target dist -DistDir C:\artifacts\collab_media_controller_dist
    .\build.ps1 -Target dist -DistZipPath C:\artifacts\collab_media_controller_dist.zip
    .\build.ps1 -Target clean
#>

param(
    [ValidateSet('all', 'desktop', 'android', 'test', 'dist', 'dist-android', 'clean')]
    [string]$Target = 'dist',
    
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    
    [switch]$Install,

    [string]$DistDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'collab_media_controller_dist'),

    [string]$DistBuildDir = (Join-Path $PSScriptRoot 'build_artifact'),

    [ValidateSet('Debug', 'Release')]
    [string[]]$DistConfigs = @('Debug', 'Release'),

    [string[]]$AndroidABIs = @('arm64-v8a'),

    [string[]]$DistAndroidABIs = @('arm64-v8a', 'armeabi-v7a', 'x86_64'),

    [int]$AndroidApiLevel = 26,

    [string]$DistZipPath = (Join-Path (Split-Path -Parent $PSScriptRoot) "collab_media_controller_dist.zip"),

    [switch]$CleanDist
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DesktopBuildDir = Join-Path $ScriptDir 'build_windows'
$AndroidBuildRoot = Join-Path $ScriptDir 'build_android'
$Timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'

function Write-Header([string]$Text) {
    Write-Host "`n$('=' * 80)" -ForegroundColor Cyan
    Write-Host "  $Text" -ForegroundColor Cyan
    Write-Host "$('=' * 80)" -ForegroundColor Cyan
}

function Write-Info([string]$Text) {
    Write-Host "  [*] $Text" -ForegroundColor Blue
}

function Write-Success([string]$Text) {
    Write-Host "  [+] $Text" -ForegroundColor Green
}

function Write-Failure([string]$Text) {
    Write-Host "  [-] $Text" -ForegroundColor Red
}

function Resolve-NdkHome {
    $NdkHome = $env:ANDROID_NDK_HOME
    if ($NdkHome -and (Test-Path $NdkHome)) {
        return $NdkHome
    }

    $AndroidHome = $env:ANDROID_HOME
    if (-not $AndroidHome) {
        throw "ANDROID_NDK_HOME is not set and ANDROID_HOME is unavailable. Source env.ps1 or set one of these variables."
    }

    $NdkDir = Join-Path $AndroidHome 'ndk'
    if (-not (Test-Path $NdkDir)) {
        throw "ANDROID_NDK_HOME is not set and no NDK folder exists under: $NdkDir"
    }

    $Latest = Get-ChildItem $NdkDir -Directory -ErrorAction Stop | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $Latest) {
        throw "No NDK versions found under: $NdkDir"
    }

    return $Latest.FullName
}

function Get-AndroidToolchainFile([string]$NdkHome) {
    $toolchain = Join-Path $NdkHome 'build' 'cmake' 'android.toolchain.cmake'
    if (-not (Test-Path $toolchain)) {
        throw "Android toolchain file not found: $toolchain"
    }
    return $toolchain
}

function Configure-BuildAndroidLibrary {
    param(
        [string]$Abi,
        [string]$BuildDir,
        [string]$NdkHome,
        [string]$ToolchainFile,
        [string]$BuildType,
        [string]$InstallPrefix,
        [switch]$DisableTesting,
        [switch]$MinimalHeaders
    )

    $configureArgs = @(
        '-S', '.',
        '-B', $BuildDir,
        '-G', 'Ninja',
        "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile",
        "-DANDROID_ABI=$Abi",
        "-DANDROID_PLATFORM=android-$AndroidApiLevel",
        "-DCMAKE_ANDROID_NDK=$NdkHome",
        "-DCMAKE_BUILD_TYPE=$BuildType"
    )

    if ($DisableTesting) {
        $configureArgs += '-DBUILD_TESTING=OFF'
    }

    if ($MinimalHeaders) {
        $configureArgs += '-DCOLLAB_media_controller_INSTALL_MINIMAL_HEADERS=ON'
    }

    if ($InstallPrefix) {
        $configureArgs += "-DCMAKE_INSTALL_PREFIX=$InstallPrefix"
    }

    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed for ABI $Abi"
    }
}

function Build-Desktop {
    Write-Header "WINDOWS BUILD"
    Write-Info "Configuration: $Config"

    $cmakeCacheFile = Join-Path $DesktopBuildDir 'CMakeCache.txt'
    if (-not (Test-Path $cmakeCacheFile)) {
        Write-Info "CMake cache not found. Running CMake configure..."
        pushd $ScriptDir
        cmake -S . -B "$DesktopBuildDir" -DBUILD_TESTING=OFF
        if ($LASTEXITCODE -ne 0) {
            Write-Failure "Windows CMake configure failed!"
            popd
            exit 1
        }
        popd
    }

    Write-Info "Building..."
    pushd $ScriptDir
    cmake --build "$DesktopBuildDir" --config $Config --parallel 4
    if ($LASTEXITCODE -ne 0) {
        Write-Failure "Windows build failed!"
        popd
        exit 1
    }
    Write-Success "Windows build complete"
    popd
}

function Build-DesktopWithTests {
    Write-Header "WINDOWS BUILD (WITH TESTS)"
    Write-Info "Configuration: $Config"

    $testBuildDir = Join-Path $ScriptDir 'build_windows_tests'
    
    if (Test-Path $testBuildDir) {
        Remove-Item -Recurse -Force $testBuildDir
    }

    Write-Info "CMake configure (BUILD_TESTING=ON)..."
    pushd $ScriptDir
    cmake -S . -B "$testBuildDir" -DCMAKE_BUILD_TYPE=$Config -DBUILD_TESTING=ON
    if ($LASTEXITCODE -ne 0) {
        Write-Failure "Windows CMake configure failed!"
        popd
        exit 1
    }

    Write-Info "Building tests..."
    cmake --build "$testBuildDir" --config $Config --parallel 4
    if ($LASTEXITCODE -ne 0) {
        Write-Failure "Windows build with tests failed!"
        popd
        exit 1
    }
    Write-Success "Windows build with tests complete"
    popd
}

function Run-Tests {
    Write-Header "RUNNING UNIT TESTS (Windows)"
    $testBuildDir = Join-Path $ScriptDir 'build_windows_tests'
    Write-Info "Test directory: $testBuildDir"

    pushd $ScriptDir
    ctest --test-dir "$testBuildDir" -C $Config --output-on-failure --verbose
    if ($LASTEXITCODE -ne 0) {
        Write-Failure "Tests failed!"
        popd
        exit 1
    }
    Write-Success "All tests passed"
    popd
}

function Build-Android {
    param([switch]$DoInstall)

    Write-Header "ANDROID BUILD (NDK CMake)"

    try {
        $NdkHome = Resolve-NdkHome
        $ToolchainFile = Get-AndroidToolchainFile -NdkHome $NdkHome
    }
    catch {
        Write-Failure $_.Exception.Message
        exit 1
    }

    Write-Info "NDK: $NdkHome"
    Write-Info "ABIs: $($AndroidABIs -join ', ')"
    Write-Info "API Level: $AndroidApiLevel"

    pushd $ScriptDir
    foreach ($abi in $AndroidABIs) {
        $abiBuildDir = Join-Path $AndroidBuildRoot $abi
        $installPrefix = $null
        if ($DoInstall) {
            $installPrefix = Join-Path (Join-Path $DistDir 'android') $abi
        }

        Write-Info "Configuring Android ($abi)..."
        try {
            Configure-BuildAndroidLibrary -Abi $abi -BuildDir $abiBuildDir -NdkHome $NdkHome -ToolchainFile $ToolchainFile -BuildType $Config -InstallPrefix $installPrefix -DisableTesting
        }
        catch {
            Write-Failure $_.Exception.Message
            popd
            exit 1
        }

        Write-Info "Building Android ($abi)..."
        cmake --build "$abiBuildDir" --parallel 4
        if ($LASTEXITCODE -ne 0) {
            Write-Failure "Android build failed for ABI $abi!"
            popd
            exit 1
        }

        if ($DoInstall) {
            Write-Info "Installing Android artifacts ($abi)..."
            cmake --install "$abiBuildDir"
            if ($LASTEXITCODE -ne 0) {
                Write-Failure "Android install failed for ABI $abi!"
                popd
                exit 1
            }
        }

        Write-Success "Android build complete for ABI: $abi"
    }
    popd
}

function Clean-All {
    Write-Header "CLEANING BUILD ARTIFACTS"

    Write-Info "Cleaning Windows build..."
    if (Test-Path $DesktopBuildDir) {
        Remove-Item -Recurse -Force $DesktopBuildDir
        Write-Success "Windows build cleaned"
    }

    Write-Info "Cleaning Android build directories..."
    if (Test-Path $AndroidBuildRoot) {
        Remove-Item -Recurse -Force $AndroidBuildRoot
        Write-Success "Android build directories cleaned"
    }

    $legacyAndroidBuilds = Get-ChildItem -Path $ScriptDir -Directory -Filter 'build_android_*' -ErrorAction SilentlyContinue
    foreach ($legacyDir in $legacyAndroidBuilds) {
        Remove-Item -Recurse -Force $legacyDir.FullName
    }

    Write-Success "All artifacts cleaned"
}

function Build-DistWindows {
    Write-Header "WINDOWS DIST BUILD"
    $WindowsDistDir = Join-Path $DistDir 'windows'
    Write-Info "Configurations: $($DistConfigs -join ', ')"
    Write-Info "Install prefix: $WindowsDistDir"

    if ($CleanDist) {
        if (Test-Path $WindowsDistDir) { Remove-Item -Recurse -Force $WindowsDistDir }
        if (Test-Path $DistBuildDir) { Remove-Item -Recurse -Force $DistBuildDir }
    }

    pushd $ScriptDir

    Write-Info "Configuring CMake (BUILD_TESTING=OFF)..."
    cmake -S . -B "$DistBuildDir" -DBUILD_TESTING=OFF -DCOLLAB_media_controller_INSTALL_MINIMAL_HEADERS=ON -DCMAKE_INSTALL_PREFIX="$DistDir"
    if ($LASTEXITCODE -ne 0) {
        Write-Failure "Windows dist configure failed!"
        popd
        exit 1
    }

    foreach ($distConfig in $DistConfigs) {
        Write-Info "Building ($distConfig)..."
        cmake --build "$DistBuildDir" --config $distConfig --parallel 4
        if ($LASTEXITCODE -ne 0) {
            Write-Failure "Windows dist build failed for $distConfig!"
            popd
            exit 1
        }

        Write-Info "Installing libraries ($distConfig)..."
        cmake --install "$DistBuildDir" --config $distConfig --component libraries --prefix "$WindowsDistDir"
        if ($LASTEXITCODE -ne 0) {
            Write-Failure "Windows dist install failed for $distConfig!"
            popd
            exit 1
        }
    }

    # Install headers once at dist root
    Write-Info "Installing shared headers..."
    cmake --install "$DistBuildDir" --component headers --prefix "$DistDir"
    if ($LASTEXITCODE -ne 0) {
        Write-Failure "Headers install failed!"
        popd
        exit 1
    }

    # Install CMake config at dist root
    cmake --install "$DistBuildDir" --component cmake-config --prefix "$DistDir"

    Write-Success "Windows dist complete: $WindowsDistDir"
    popd
}

function Build-DistAndroid {
    Write-Header "ANDROID DIST BUILD (NDK Cross-Compile)"

    try {
        $NdkHome = Resolve-NdkHome
        $ToolchainFile = Get-AndroidToolchainFile -NdkHome $NdkHome
    }
    catch {
        Write-Failure $_.Exception.Message
        exit 1
    }

    Write-Info "NDK: $NdkHome"

    $AndroidDistBase = Join-Path $DistDir 'android'

    Write-Info "ABIs: $($DistAndroidABIs -join ', ') | API Level: $AndroidApiLevel"
    Write-Info "Install prefix: $AndroidDistBase"

    if ($CleanDist) {
        if (Test-Path $AndroidDistBase) { Remove-Item -Recurse -Force $AndroidDistBase }
        foreach ($abi in $DistAndroidABIs) {
            $buildDir = Join-Path $AndroidBuildRoot $abi
            if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }
        }
    }

    pushd $ScriptDir

    foreach ($abi in $DistAndroidABIs) {
        $AndroidBuildDir = Join-Path $AndroidBuildRoot $abi
        $AbiInstallDir = Join-Path $AndroidDistBase $abi

        Write-Info "--- Building for ABI: $abi ---"

        Write-Info "Configuring CMake for Android ($abi)..."
        try {
            Configure-BuildAndroidLibrary -Abi $abi -BuildDir $AndroidBuildDir -NdkHome $NdkHome -ToolchainFile $ToolchainFile -BuildType 'Release' -InstallPrefix $AbiInstallDir -DisableTesting -MinimalHeaders
        }
        catch {
            Write-Failure $_.Exception.Message
            popd
            exit 1
        }

        Write-Info "Building ($abi)..."
        cmake --build "$AndroidBuildDir" --parallel 4
        if ($LASTEXITCODE -ne 0) {
            Write-Failure "Android dist build failed for $abi!"
            popd
            exit 1
        }

        Write-Info "Installing libraries ($abi)..."
        cmake --install "$AndroidBuildDir" --component libraries
        if ($LASTEXITCODE -ne 0) {
            Write-Failure "Android dist install failed for $abi!"
            popd
            exit 1
        }

        Write-Success "Completed ABI: $abi"
    }

    Write-Success "Android dist complete: $AndroidDistBase"
    popd
}

# Main orchestration
switch ($Target) {
    'all' {
        Build-Desktop
        Build-Android -DoInstall:$Install
    }
    'desktop' {
        Build-Desktop
    }
    'android' {
        Build-Android -DoInstall:$Install
    }
    'test' {
        Build-DesktopWithTests
        Run-Tests
    }
    'dist' {
        if (Test-Path $DistDir) {
            Write-Info "Removing existing dist output..."
            Remove-Item -Recurse -Force $DistDir
        }

        Build-DistWindows
        Build-DistAndroid

        Write-Header "PACKAGING UNIFIED DIST"
        Write-Info "Creating zip package..."
        $zipPathToUse = $DistZipPath
        if (Test-Path $zipPathToUse) {
            try {
                Remove-Item -Force $zipPathToUse
            }
            catch {
                $zipDir = Split-Path -Parent $DistZipPath
                $zipName = [System.IO.Path]::GetFileNameWithoutExtension($DistZipPath)
                $zipExt = [System.IO.Path]::GetExtension($DistZipPath)
                $fallbackName = "{0}_{1}{2}" -f $zipName, (Get-Date -Format 'yyyyMMdd_HHmmss'), $zipExt
                $zipPathToUse = Join-Path $zipDir $fallbackName
                Write-Info "Zip file is locked. Using fallback: $zipPathToUse"
            }
        }
        Compress-Archive -Path (Join-Path $DistDir '*') -DestinationPath $zipPathToUse -CompressionLevel Optimal
        if (-not (Test-Path $zipPathToUse)) {
            Write-Failure "Dist zip creation failed!"
            exit 1
        }

        Write-Success "Unified dist: $DistDir"
        Write-Success "Unified zip: $zipPathToUse"
    }
    'dist-android' {
        if ($CleanDist -and (Test-Path $DistDir)) {
            Remove-Item -Recurse -Force $DistDir
        }
        Build-DistAndroid
    }
    'clean' {
        Clean-All
    }
}

Write-Header "BUILD COMPLETE"
Write-Info "Time: $Timestamp"
Write-Success "All requested targets built successfully"
