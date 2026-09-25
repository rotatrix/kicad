# Run with Windows PowerShell 5.1 (the upstream builder uses System.Drawing).
[CmdletBinding()]
param(
    [ValidateSet('Configure', 'Build', 'Package', 'All')][string]$Stage = 'All',
    [string]$BuilderPath = '',
    [string]$SdkPath = '',
    [ValidateRange(1, 32)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$oaSource = Split-Path $PSScriptRoot -Parent
$oaStage = $Stage
$oaSdk = $SdkPath
$oaJobs = $Jobs
if (!$BuilderPath) { $BuilderPath = Join-Path $oaSource 'build-tools/win-builder' }
$oaBuilder = [IO.Path]::GetFullPath($BuilderPath)
$oaBuilderCommit = '81d736abb12384f85d4453553246d43002d0ff45'
if (!(Test-Path "$oaBuilder/build.ps1")) {
    git clone https://gitlab.com/kicad/packaging/kicad-win-builder.git $oaBuilder
    if ($LASTEXITCODE) { throw 'Builder clone failed' }
    git -C $oaBuilder checkout $oaBuilderCommit
    if ($LASTEXITCODE) { throw 'Builder checkout failed' }
}
if ((git -C $oaBuilder rev-parse HEAD) -ne $oaBuilderCommit) { throw 'Unexpected builder revision' }
if (!(Test-Path "$oaBuilder/.support/nsis-3.11/makensis.exe")) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$oaBuilder/build.ps1" -Init
    if ($LASTEXITCODE) { throw 'Builder initialization failed' }
}
if (!(Test-Path "$oaBuilder/vcpkg/vcpkg.exe")) {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$oaBuilder/build.ps1" -Vcpkg -BuildConfigName kicad-10.0.6 -Arch x64
    if ($LASTEXITCODE) { throw 'vcpkg initialization failed' }
}
# Import the official environment and packaging functions, without Start-Build's
# source resets or multi-gigabyte library clones. Lite installers use online libraries.
. "$oaBuilder/build.ps1" -Env -Arch x64
# GitHub windows-2022 and the local VS 2022 installation provide v143.
$env:VCPKG_PLATFORM_TOOLSET = 'v143'
$env:VCPKG_BINARY_SOURCES = 'clear;default,readwrite;nuget,https://gitlab.com/api/v4/projects/27426693/packages/nuget/index.json,read'
$env:VCPKG_MAX_CONCURRENCY = "$oaJobs"
$buildConfig = @{ custom_python = $true }
(Get-Content "$oaBuilder/build-configs/kicad-10.0.6.json" -Raw | ConvertFrom-Json).psobject.properties |
    ForEach-Object { $buildConfig[$_.Name] = $_.Value }
function Get-Source-Path([string]$subfolder) {
    if ($subfolder -eq 'kicad') { return $oaSource }
    return Join-Path $BuilderPaths.BuildRoot $subfolder
}
$oaBuild = Join-Path $oaSource 'build/x64-windows-Release'
if ($oaStage -in @('Configure', 'All')) {
    $oaArgs = @('-S', $oaSource, '-B', $oaBuild, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_TOOLCHAIN_FILE=$oaBuilder/vcpkg/scripts/buildsystems/vcpkg.cmake",
        "-DCMAKE_INSTALL_PREFIX=$oaBuilder/.out/x64-windows-Release",
        "-DCMAKE_MAKE_PROGRAM=$env:NINJA_PATH", '-DKICAD_OPENAXIS=ON',
        '-DKICAD_BUILD_QA_TESTS=OFF', '-DKICAD_BUILD_I18N=ON',
        '-DKICAD_WIN32_DPI_AWARE=ON', '-DKICAD_SCRIPTING_WXPYTHON=ON')
    if ($oaSdk) { $oaArgs += "-DOPENAXIS_SOURCE_DIR=$oaSdk" }
    & cmake @oaArgs
    if ($LASTEXITCODE) { throw 'CMake configuration failed' }
}
if ($oaStage -in @('Build', 'All')) {
    & cmake --build $oaBuild --parallel $oaJobs
    if ($LASTEXITCODE) { throw 'KiCad build failed' }
    $env:PATH = "$oaBuild/vcpkg_installed/x64-windows/bin;" + $env:PATH
    $oaCtest = Join-Path (Split-Path (Get-Command cmake).Definition -Parent) 'ctest.exe'
    & $oaCtest --test-dir $oaBuild -R 'openaxis_(native_camera|wx_scheduler)' --output-on-failure --no-tests=error
    if ($LASTEXITCODE) { throw 'OpenAxis camera regression checks failed' }
    & cmake --install $oaBuild
    if ($LASTEXITCODE) { throw 'KiCad install failed' }
}
if ($oaStage -in @('Package', 'All')) {
    # Upstream preparation replaces this generated tree. Validate it before deletion.
    $oaOutput = [IO.Path]::GetFullPath((Join-Path $oaBuilder '.out/x64-windows-Release'))
    if (!$oaOutput.StartsWith($oaBuilder.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Package output must stay within the dedicated builder checkout'
    }
    foreach ($oaDirectory in @((Join-Path $oaBuilder '.out'), $oaOutput)) {
        if ((Test-Path $oaDirectory) -and ((Get-Item $oaDirectory).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Package output cannot be a junction or symbolic link'
        }
    }
    # Prefix distinguishes this GPLv3 OpenAxis fork from official KiCad builds.
    $buildConfig.output_prefix = 'rotatrix-kicad-openaxis-'
    Start-Prepare-Package -arch x64 -buildType Release -lite $true
    Start-Package-Nsis -arch x64 -buildType Release -lite $true -postCleanup $false -sign $false
    & "$oaOutput/bin/kicad-cli.exe" version
    if ($LASTEXITCODE) { throw 'Packaged KiCad runtime smoke test failed' }
}
