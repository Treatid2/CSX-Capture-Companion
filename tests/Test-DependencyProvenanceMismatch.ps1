[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ProvenanceScript,

    [Parameter(Mandatory = $true)]
    [string] $CommonLibSsePrebuilt,

    [Parameter(Mandatory = $true)]
    [string] $WorkRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = [System.IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $root) {
    $resolved = (Resolve-Path -LiteralPath $root).Path
    if ($resolved -ne $root) {
        throw "Refusing to replace an unexpected provenance-test root: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
[System.IO.Directory]::CreateDirectory($root) | Out-Null
[System.IO.File]::WriteAllText(
    (Join-Path $root 'README.md'),
    "mismatched source checkout`n",
    [System.Text.UTF8Encoding]::new($false))

& git -C $root init --quiet
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the mismatched source fixture.' }
& git -C $root add README.md
if ($LASTEXITCODE -ne 0) { throw 'Could not stage the mismatched source fixture.' }
& git -C $root -c user.name='CSX test' -c user.email='csx-test@invalid.example' commit --quiet -m 'test fixture'
if ($LASTEXITCODE -ne 0) { throw 'Could not commit the mismatched source fixture.' }

$rejected = $false
try {
    & $ProvenanceScript `
        -CommonLibSseSource $root `
        -CommonLibSsePrebuilt $CommonLibSsePrebuilt | Out-Null
} catch {
    if ($_.Exception.Message -notmatch 'does not match configured commit') {
        throw
    }
    $rejected = $true
}
if (-not $rejected) {
    throw 'A clean but mismatched CommonLibSSE-NG source checkout passed provenance validation.'
}

Write-Host 'Mismatched CommonLibSSE-NG source checkout rejected.'
