[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Executable,

    [Parameter(Mandatory)]
    [string] $WorkRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$resolvedBuildRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build')).TrimEnd('\') + '\'
$resolvedWorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
if (-not $resolvedWorkRoot.StartsWith($resolvedBuildRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace composer fixture outside the project build tree: $resolvedWorkRoot"
}
if (Test-Path -LiteralPath $resolvedWorkRoot) {
    Remove-Item -LiteralPath $resolvedWorkRoot -Recurse -Force
}

$sequence = Join-Path $resolvedWorkRoot 'CS_sequence_smoke_1'
$leftFrames = Join-Path $sequence 'left'
$rightFrames = Join-Path $sequence 'right'
New-Item -ItemType Directory -Force -Path $leftFrames, $rightFrames | Out-Null

Add-Type -AssemblyName System.Drawing
$colors = @(
    [System.Drawing.Color]::FromArgb(255, 220, 32, 32),
    [System.Drawing.Color]::FromArgb(255, 32, 180, 64),
    [System.Drawing.Color]::FromArgb(255, 32, 80, 220)
)

$manifestFrames = @()
$formats = @(
    @{ Extension = '.bmp'; ImageFormat = [System.Drawing.Imaging.ImageFormat]::Bmp },
    @{ Extension = '.png'; ImageFormat = [System.Drawing.Imaging.ImageFormat]::Png },
    @{ Extension = '.bmp'; ImageFormat = [System.Drawing.Imaging.ImageFormat]::Bmp }
)
for ($index = 0; $index -lt $colors.Count; $index++) {
    $name = ('frame_{0:D9}{1}' -f ($index + 1), $formats[$index].Extension)
    foreach ($eyeFrames in @($leftFrames, $rightFrames)) {
        $path = Join-Path $eyeFrames $name
        $bitmap = [System.Drawing.Bitmap]::new(64, 64)
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
            try {
                $colorIndex = if ($eyeFrames -eq $leftFrames) { $index } else { $colors.Count - 1 - $index }
                $graphics.Clear($colors[$colorIndex])
            } finally {
                $graphics.Dispose()
            }
            $bitmap.Save($path, $formats[$index].ImageFormat)
        } finally {
            $bitmap.Dispose()
        }
    }
    $manifestFrames += [ordered]@{
        index = $index + 1
        timestampUs = [uint64](1000000 + ($index * 16667))
        written = $true
        paths = @("left/$name", "right/$name")
        error = $null
    }
}

$manifest = [ordered]@{
    schema = 'csx.frame-sequence/1'
    sessionId = 1
    state = 'complete'
    eye = 'Both'
    audio = $false
    startedUtc = '2026-08-24T00:00:00.000Z'
    updatedUtc = '2026-08-24T00:00:01.000Z'
    counts = [ordered]@{ scheduled = 3; written = 3; dropped = 0 }
    frames = $manifestFrames
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $sequence 'sequence.json') -Encoding utf8NoBOM

& $Executable $sequence
if ($LASTEXITCODE -ne 0) {
    throw "Composer smoke executable failed with exit code $LASTEXITCODE."
}

foreach ($suffix in @('left', 'right')) {
    $output = Join-Path $resolvedWorkRoot "CS_sequence_smoke_1-$suffix.mp4"
    if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
        throw "Composer did not produce $output"
    }
    $artifact = Get-Item -LiteralPath $output
    if ($artifact.Length -le 0) {
        throw "Composer produced an empty $suffix-eye MP4."
    }
    [pscustomobject]@{
        Path = $artifact.FullName
        Bytes = $artifact.Length
        SHA256 = (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
