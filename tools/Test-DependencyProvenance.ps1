[CmdletBinding()]
param(
    [string] $MetadataPath = (Join-Path $PSScriptRoot 'DependencyVersions.psd1'),
    [string] $NoticePath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'docs\legal\THIRD_PARTY_NOTICES.md'),
    [Parameter(Mandatory = $true)]
    [string] $CommonLibSseSource,
    [string] $CommonLibSsePrebuilt
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$metadata = Import-PowerShellDataFile -LiteralPath $MetadataPath
$dependency = $metadata.CommonLibSseNg
if (-not $dependency.Version -or -not $dependency.SourceTag -or
    $dependency.SourceCommit -notmatch '^[0-9a-f]{40}$' -or
    -not $dependency.SourceUrl -or -not $dependency.PrebuiltDirectory -or
    $dependency.PrebuiltManifestSha256 -notmatch '^[0-9a-f]{64}$' -or
    -not $dependency.ToolsetVersion) {
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
if (-not $notice.Contains("Release source commit: ``$($dependency.SourceCommit)``")) {
    throw 'THIRD_PARTY_NOTICES.md does not use the configured CommonLibSSE-NG source commit.'
}

$versionSegment = [regex]::Escape("v$($dependency.Version)")
$normalizedPrebuilt = [System.IO.Path]::GetFullPath($CommonLibSsePrebuilt)
if ($normalizedPrebuilt -notmatch "(?i)(^|[\\/])$versionSegment([\\/]|$)") {
    throw "Selected CommonLibSSE-NG prebuilt does not match configured version $($dependency.Version): $normalizedPrebuilt"
}
$prebuiltManifest = Join-Path $normalizedPrebuilt 'PREBUILT.md'
$toolsetVersionPath = Join-Path $normalizedPrebuilt 'TOOLSET_VERSION.txt'
if (-not (Test-Path -LiteralPath $prebuiltManifest -PathType Leaf) -or
    -not (Test-Path -LiteralPath $toolsetVersionPath -PathType Leaf)) {
    throw "Selected CommonLibSSE-NG prebuilt is missing its identity files: $normalizedPrebuilt"
}
$prebuiltManifestSha256 = (Get-FileHash -LiteralPath $prebuiltManifest -Algorithm SHA256).Hash.ToLowerInvariant()
if ($prebuiltManifestSha256 -ne $dependency.PrebuiltManifestSha256) {
    throw 'Selected CommonLibSSE-NG prebuilt manifest does not match the configured identity.'
}
$toolsetVersion = (Get-Content -LiteralPath $toolsetVersionPath -Raw).Trim()
if ($toolsetVersion -ne $dependency.ToolsetVersion) {
    throw 'Selected CommonLibSSE-NG prebuilt toolset does not match the configured identity.'
}

$normalizedSource = [System.IO.Path]::GetFullPath($CommonLibSseSource)
if (-not (Test-Path -LiteralPath $normalizedSource -PathType Container)) {
    throw "Selected CommonLibSSE-NG source checkout does not exist: $normalizedSource"
}
function Invoke-SourceGit {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]] $Arguments)
    $output = @(& git -C $normalizedSource @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not verify CommonLibSSE-NG source checkout: $($output -join [Environment]::NewLine)"
    }
    return ($output -join "`n").Trim()
}
$sourceHead = Invoke-SourceGit rev-parse HEAD
if ($sourceHead -ne $dependency.SourceCommit) {
    throw "CommonLibSSE-NG source checkout does not match configured commit $($dependency.SourceCommit): $sourceHead"
}
$tagCommit = Invoke-SourceGit rev-parse "$($dependency.SourceTag)^{commit}"
if ($tagCommit -ne $dependency.SourceCommit) {
    throw "CommonLibSSE-NG source tag $($dependency.SourceTag) does not resolve to the configured commit."
}
$sourceStatus = Invoke-SourceGit status --porcelain=v1 --untracked-files=all
if ($sourceStatus) {
    throw 'CommonLibSSE-NG source checkout is dirty and cannot be used for a release package.'
}

[pscustomobject]@{
    Dependency = 'CommonLibSSE-NG'
    Version = $dependency.Version
    SourceTag = $dependency.SourceTag
    SourceCommit = $sourceHead
    SourceUrl = $dependency.SourceUrl
    SourceDirectory = $normalizedSource
    PrebuiltDirectory = $normalizedPrebuilt
    PrebuiltManifestSha256 = $prebuiltManifestSha256
    ToolsetVersion = $toolsetVersion
}
