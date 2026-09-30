[CmdletBinding()]
param(
    [string[]] $ImportPath = @(),
    [string] $CompilerWrapper = 'L:\Codex\shared\tools\papyrus-compiler\Invoke-PapyrusCompiler.ps1',
    [string] $OutputDirectory = 'build\papyrus\Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sourcePath = Join-Path $projectRoot 'Data\Scripts\Source'
$outputPath = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
    [System.IO.Path]::GetFullPath($OutputDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $projectRoot $OutputDirectory))
}
$dataPath = Join-Path $projectRoot 'Data\Scripts'

if (-not (Test-Path -LiteralPath $CompilerWrapper -PathType Leaf)) {
    throw "Papyrus compiler wrapper not found: $CompilerWrapper"
}

if ($ImportPath.Count -eq 0) {
    $codexHeaders = 'L:\Codex\projects\code\ThrowingStuffVR\deps\skyrim-papyrus-source'
    if (Test-Path -LiteralPath $codexHeaders -PathType Container) {
        $ImportPath = @($codexHeaders)
    }
}
if ($ImportPath.Count -eq 0) {
    throw 'Pass -ImportPath with Skyrim, SKSE, and SkyUI Papyrus source headers.'
}

New-Item -ItemType Directory -Force -Path $outputPath, $dataPath | Out-Null
$arguments = @(
    '-NoProfile',
    '-File', $CompilerWrapper,
    '-SourcePath', $sourcePath,
    '-OutputPath', $outputPath,
    '-ImportPath', ($ImportPath -join ';'),
    '-Optimize'
)
& pwsh @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Papyrus compilation failed with exit code $LASTEXITCODE."
}

$scripts = @('CSXCaptureNative', 'CSXCapturePowerEffect', 'CSXCaptureMCM')
foreach ($script in $scripts) {
    $compiled = Join-Path $outputPath "$script.pex"
    if (-not (Test-Path -LiteralPath $compiled -PathType Leaf)) {
        throw "Papyrus compilation did not produce $compiled"
    }
    Copy-Item -LiteralPath $compiled -Destination (Join-Path $dataPath "$script.pex") -Force
}

$scripts | ForEach-Object {
    $artifact = Get-Item -LiteralPath (Join-Path $dataPath "$_.pex")
    [pscustomobject]@{
        Path = $artifact.FullName
        Bytes = $artifact.Length
        SHA256 = (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
