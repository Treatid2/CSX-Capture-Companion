[CmdletBinding()]
param(
    [string] $MetadataPath = (Join-Path $PSScriptRoot 'DependencyVersions.psd1'),
    [string] $NoticePath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'docs\legal\THIRD_PARTY_NOTICES.md'),
    [string] $CommonLibSsePrebuilt
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$metadata = Import-PowerShellDataFile -LiteralPath $MetadataPath
$dependency = $metadata.CommonLibSseNg
if (-not $dependency.Version -or -not $dependency.SourceUrl -or -not $dependency.PrebuiltDirectory) {
    throw 'CommonLibSSE-NG dependency metadata is incomplete.'
}
if (-not $CommonLibSsePrebuilt) {
    $CommonLibSsePrebuilt = $dependency.PrebuiltDirectory
}

$notice = Get-Content -LiteralPath $NoticePath -Raw
$declaredVersions = @(
    [regex]::Matches($notice, 'CommonLibSSE-NG\s+(?<version>\d+(?:\.\d+){2})') |
        ForEach-Object { $_.Groups['version'].Value } |
        Sort-Object -Unique
)
if ($declaredVersions.Count -ne 1 -or $declaredVersions[0] -ne $dependency.Version) {
    throw "THIRD_PARTY_NOTICES.md does not exclusively declare CommonLibSSE-NG $($dependency.Version)."
}
if (-not $notice.Contains("Source: <$($dependency.SourceUrl)>")) {
    throw 'THIRD_PARTY_NOTICES.md does not use the configured CommonLibSSE-NG source URL.'
}

$versionSegment = [regex]::Escape("v$($dependency.Version)")
$normalizedPrebuilt = [System.IO.Path]::GetFullPath($CommonLibSsePrebuilt)
if ($normalizedPrebuilt -notmatch "(?i)(^|[\\/])$versionSegment([\\/]|$)") {
    throw "Selected CommonLibSSE-NG prebuilt does not match configured version $($dependency.Version): $normalizedPrebuilt"
}

[pscustomobject]@{
    Dependency = 'CommonLibSSE-NG'
    Version = $dependency.Version
    SourceUrl = $dependency.SourceUrl
    PrebuiltDirectory = $normalizedPrebuilt
}
