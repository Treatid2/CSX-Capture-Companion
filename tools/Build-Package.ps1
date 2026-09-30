[CmdletBinding()]
param(
    [string] $BuildDirectory = 'build',
    [string[]] $PapyrusImportPath = @(),
    [string] $CommonLibSseSource = 'L:\Codex\projects\CSX-Capture-API\extern\CommonLibSSE-NG',
    [string] $CommonLibSsePrebuilt,
    [string] $VcpkgToolchain = 'L:\Codex\shared\tools\vcpkg\scripts\buildsystems\vcpkg.cmake',
    [string] $VcpkgInstalledDirectory = 'L:\Codex\projects\CSX-Capture-API\build\ALL-Prebuilt\vcpkg_installed'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$dependencyMetadata = Import-PowerShellDataFile -LiteralPath (Join-Path $PSScriptRoot 'DependencyVersions.psd1')
if (-not $CommonLibSsePrebuilt) {
    $CommonLibSsePrebuilt = $dependencyMetadata.CommonLibSseNg.PrebuiltDirectory
}
$dependencyProvenance = & (Join-Path $PSScriptRoot 'Test-DependencyProvenance.ps1') `
    -CommonLibSseSource $CommonLibSseSource `
    -CommonLibSsePrebuilt $CommonLibSsePrebuilt
$buildRoot = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDirectory))
}
$dataRoot = Join-Path $projectRoot 'Data'
$pluginPath = Join-Path $dataRoot 'CSXCaptureCompanion.esp'
$stageRoot = Join-Path $buildRoot 'package'
$distRoot = Join-Path $projectRoot 'dist'
$archivePath = Join-Path $distRoot 'CSXCaptureCompanion-0.1.1.zip'

& (Join-Path $PSScriptRoot 'Build-Papyrus.ps1') `
    -ImportPath $PapyrusImportPath `
    -OutputDirectory (Join-Path $buildRoot 'papyrus\Release')
if ($LASTEXITCODE -ne 0) {
    throw "Papyrus build failed with exit code $LASTEXITCODE."
}

& dotnet run --project (Join-Path $PSScriptRoot 'BuildPlugin\BuildPlugin.csproj') -c Release -- $pluginPath
if ($LASTEXITCODE -ne 0) {
    throw "Plugin-record build failed with exit code $LASTEXITCODE."
}

$configureArguments = @(
    '-S', $projectRoot,
    '-B', $buildRoot,
    '-G', 'Visual Studio 18 2026',
    '-A', 'x64',
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain",
    "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDirectory",
    '-DVCPKG_TARGET_TRIPLET=x64-windows-static-md',
    "-DCOMMONLIBSSE_SOURCE_DIR=$CommonLibSseSource",
    "-DCOMMONLIB_PREBUILT_DIR=$CommonLibSsePrebuilt",
    '-DCOMMONLIB_PREBUILT_MULTICONFIG=ON'
)
& cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}
& cmake --build $buildRoot --config Release
if ($LASTEXITCODE -ne 0) {
    throw "Native build failed with exit code $LASTEXITCODE."
}
& ctest --test-dir $buildRoot -C Release --output-on-failure `
    -R '^(composer-smoke|capture-session|capture-controller|package-provenance|package-provenance-mismatch)$'
if ($LASTEXITCODE -ne 0) {
    throw "Native tests failed with exit code $LASTEXITCODE."
}

$normalizedBuild = [System.IO.Path]::GetFullPath($buildRoot).TrimEnd('\') + '\'
$normalizedStage = [System.IO.Path]::GetFullPath($stageRoot)
if (-not $normalizedStage.StartsWith($normalizedBuild, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace package staging outside the build directory: $normalizedStage"
}
if (Test-Path -LiteralPath $normalizedStage) {
    Remove-Item -LiteralPath $normalizedStage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $normalizedStage, $distRoot | Out-Null

& cmake --install $buildRoot --config Release --prefix $normalizedStage
if ($LASTEXITCODE -ne 0) {
    throw "CMake install failed with exit code $LASTEXITCODE."
}

$provenanceDirectory = Join-Path $normalizedStage 'Docs\CSX Capture Companion'
[System.IO.Directory]::CreateDirectory($provenanceDirectory) | Out-Null
$provenancePath = Join-Path $provenanceDirectory 'dependency-provenance.json'
$provenanceJson = $dependencyProvenance | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText(
    $provenancePath,
    $provenanceJson + "`n",
    [System.Text.UTF8Encoding]::new($false))

Compress-Archive -Path (Join-Path $normalizedStage '*') -DestinationPath $archivePath -Force
$archive = Get-Item -LiteralPath $archivePath
[pscustomobject]@{
    Path = $archive.FullName
    Bytes = $archive.Length
    SHA256 = (Get-FileHash -LiteralPath $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
