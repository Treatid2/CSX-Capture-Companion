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

$manifestChildren = @()
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
	$manifestChildren += [ordered]@{
		ordinal = $index + 1
		requestId = "smoke-frame-$($index + 1)"
		state = 'completed'
		scheduledEngineFrame = [uint64](100 + ($index * 12))
		scheduledTimestampUs = [uint64](1000000 + ($index * 16667))
		artifacts = @(
			[ordered]@{ path = (Join-Path $leftFrames $name); committed = $true },
			[ordered]@{ path = (Join-Path $rightFrames $name); committed = $true }
		)
		error = $null
	}
}

$manifest = [ordered]@{
	contract = [ordered]@{ name = 'csx.screenshot'; major = 1; minor = 0; schemaRevision = 1 }
	sessionId = 'smoke-session'
	requestId = 'smoke-sequence'
	state = 'final'
	capture = [ordered]@{
		outputs = @(
			[ordered]@{ view = 'left_eye'; nameSuffix = 'left' },
			[ordered]@{ view = 'right_eye'; nameSuffix = 'right' }
		)
	}
    updatedUtc = '2026-08-24T00:00:01.000Z'
	counts = [ordered]@{ requested = 3; scheduled = 3; written = 3; dropped = 0; failed = 0; inFlight = 0 }
	children = $manifestChildren
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
