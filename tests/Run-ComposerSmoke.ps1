[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $Executable,

    [Parameter(Mandatory)]
    [string] $WorkRoot,

    [Parameter(Mandatory)]
    [string] $AllowedRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$resolvedBuildRoot = [System.IO.Path]::GetFullPath($AllowedRoot).TrimEnd('\') + '\'
$resolvedWorkRoot = [System.IO.Path]::GetFullPath($WorkRoot)
if (-not $resolvedWorkRoot.StartsWith($resolvedBuildRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace composer fixture outside the configured build tree: $resolvedWorkRoot"
}
if (Test-Path -LiteralPath $resolvedWorkRoot) {
    Remove-Item -LiteralPath $resolvedWorkRoot -Recurse -Force
}

function Write-TestManifest {
	param(
		[Parameter(Mandatory)] [string] $Sequence,
		[Parameter(Mandatory)] [string[]] $Suffixes,
		[Parameter(Mandatory)] [object[]] $Timestamps,
		[Parameter(Mandatory)] [string[]] $ArtifactPaths,
		[string[]] $States = @(),
		[bool] $Committed = $true
	)
	if ($Suffixes.Count -ne $ArtifactPaths.Count) {
		throw 'Each test output must have one artifact path.'
	}
	if ($States.Count -ne 0 -and $States.Count -ne $Timestamps.Count) {
		throw 'Each explicitly stated test child must have one timestamp.'
	}
	New-Item -ItemType Directory -Force -Path $Sequence | Out-Null
	$children = @()
	for ($index = 0; $index -lt $Timestamps.Count; $index++) {
		$state = if ($States.Count -eq 0) { 'completed' } else { $States[$index] }
		$artifacts = @()
		if ($state -eq 'completed' -or $state -eq 'completed_with_warnings') {
			$artifacts = foreach ($path in $ArtifactPaths) {
				[ordered]@{ path = $path; committed = $Committed }
			}
		}
		$children += [ordered]@{
			ordinal = $index + 1
			requestId = "fixture-frame-$($index + 1)"
			state = $state
			scheduledEngineFrame = [uint64](200 + $index)
			scheduledTimestampUs = $Timestamps[$index]
			artifacts = @($artifacts)
			error = $null
		}
	}
	$outputs = for ($index = 0; $index -lt $Suffixes.Count; $index++) {
		$view = if ($index -eq 0) { 'left_eye' } elseif ($index -eq 1) { 'right_eye' } else { 'framed_combined' }
		[ordered]@{ view = $view; nameSuffix = $Suffixes[$index] }
	}
	$written = @($children | Where-Object { $_.state -eq 'completed' -or $_.state -eq 'completed_with_warnings' }).Count
	$dropped = @($children | Where-Object { $_.state -eq 'dropped' }).Count
	$document = [ordered]@{
		contract = [ordered]@{ name = 'csx.screenshot'; major = 1; minor = 0; schemaRevision = 1 }
		sessionId = 'fixture-session'
		requestId = 'fixture-sequence'
		state = 'final'
		capture = [ordered]@{ outputs = @($outputs) }
		updatedUtc = '2026-09-09T00:00:00.000Z'
		counts = [ordered]@{ requested = $Timestamps.Count; scheduled = $Timestamps.Count; written = $written; dropped = $dropped; failed = 0; inFlight = 0 }
		children = $children
	}
	$document | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Sequence 'sequence.json') -Encoding utf8NoBOM
}

function Invoke-ExpectedFailure {
	param([Parameter(Mandatory)] [string] $Sequence)
	& $Executable --expect-failure $Sequence
	if ($LASTEXITCODE -ne 0) {
		throw "Composer did not safely reject fixture $Sequence (exit $LASTEXITCODE)."
	}
}

function Invoke-CompositionWithSampleCount {
	param(
		[Parameter(Mandatory)] [string] $Sequence,
		[Parameter(Mandatory)] [string] $Output,
		[Parameter(Mandatory)] [int] $ExpectedCount
	)
	& $Executable $Sequence
	if ($LASTEXITCODE -ne 0) {
		throw "Composer rejected valid fixture $Sequence (exit $LASTEXITCODE)."
	}
	if (-not (Test-Path -LiteralPath $Output -PathType Leaf)) {
		throw "Composer did not produce $Output"
	}
	& $Executable --verify-sample-count $ExpectedCount $Output
	if ($LASTEXITCODE -ne 0) {
		throw "Decoded sample-count verification failed for $Output."
	}
}

$sequence = Join-Path $resolvedWorkRoot 'CS_sequence_smoke_1'
$leftFrames = Join-Path $sequence 'left'
$rightFrames = Join-Path $sequence 'right'
New-Item -ItemType Directory -Force -Path $leftFrames, $rightFrames | Out-Null

Add-Type -AssemblyName System.Drawing
$colors = @(
    [System.Drawing.Color]::FromArgb(255, 220, 32, 32),
    [System.Drawing.Color]::FromArgb(255, 32, 180, 64),
    [System.Drawing.Color]::FromArgb(255, 32, 80, 220),
    [System.Drawing.Color]::FromArgb(255, 220, 180, 32)
)

$manifestChildren = @()
$formats = @(
    @{ Extension = '.bmp'; ImageFormat = [System.Drawing.Imaging.ImageFormat]::Bmp },
    @{ Extension = '.png'; ImageFormat = [System.Drawing.Imaging.ImageFormat]::Png },
    @{ Extension = '.bmp'; ImageFormat = [System.Drawing.Imaging.ImageFormat]::Bmp },
    @{ Extension = '.png'; ImageFormat = [System.Drawing.Imaging.ImageFormat]::Png }
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
                $topBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
                $bottomBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::Black)
                try {
                    $graphics.FillRectangle($topBrush, 0, 0, 64, 16)
                    $graphics.FillRectangle($bottomBrush, 0, 48, 64, 16)
                } finally {
                    $topBrush.Dispose()
                    $bottomBrush.Dispose()
                }
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
		scheduledTimestampUs = [uint64](1000000 + @(0, 16667, 50001, 66668)[$index])
		artifacts = @(
			[ordered]@{ path = (Join-Path $leftFrames $name); committed = $true },
			[ordered]@{ path = (Join-Path $rightFrames $name); committed = $true }
		)
		error = $null
	}
}
foreach ($offset in @([uint64]83335, [uint64]100002)) {
	$ordinal = $manifestChildren.Count + 1
	$manifestChildren += [ordered]@{
		ordinal = $ordinal
		requestId = "smoke-frame-$ordinal"
		state = 'dropped'
		scheduledEngineFrame = [uint64](100 + (($ordinal - 1) * 12))
		scheduledTimestampUs = [uint64](1000000 + $offset)
		artifacts = @()
		error = [ordered]@{ code = 'encoder_backpressure'; message = 'fixture drop' }
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
	counts = [ordered]@{ requested = 6; scheduled = 6; written = 4; dropped = 2; failed = 0; inFlight = 0 }
	children = $manifestChildren
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $sequence 'sequence.json') -Encoding utf8NoBOM

$sourceFiles = @(Get-ChildItem -LiteralPath $leftFrames, $rightFrames -File)
$sourceHashes = @{}
foreach ($source in $sourceFiles) {
	$sourceHashes[$source.FullName] = (Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash
}
$oldPredictableTemporary = Join-Path $resolvedWorkRoot 'CS_sequence_smoke_1-sbs.tmp.mp4'
Set-Content -LiteralPath $oldPredictableTemporary -Value 'unrelated pre-existing file' -Encoding ascii -NoNewline
$oldTemporaryHash = (Get-FileHash -LiteralPath $oldPredictableTemporary -Algorithm SHA256).Hash

& $Executable $sequence
if ($LASTEXITCODE -ne 0) {
    throw "Composer smoke executable failed with exit code $LASTEXITCODE."
}

$output = Join-Path $resolvedWorkRoot 'CS_sequence_smoke_1-sbs.mp4'
if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
	throw "Composer did not produce $output"
}
$artifact = Get-Item -LiteralPath $output
if ($artifact.Length -le 0) {
	throw 'Composer produced an empty side-by-side MP4.'
}
[pscustomobject]@{
	Path = $artifact.FullName
	Bytes = $artifact.Length
	SHA256 = (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
& $Executable --verify-orientation $output
if ($LASTEXITCODE -ne 0) {
	throw "Decoded SBS verification failed for $output."
}
foreach ($monoOutput in @('CS_sequence_smoke_1-left.mp4', 'CS_sequence_smoke_1-right.mp4')) {
	if (Test-Path -LiteralPath (Join-Path $resolvedWorkRoot $monoOutput)) {
		throw "Stereo composition unexpectedly produced a mono output: $monoOutput"
	}
}

if ((Get-FileHash -LiteralPath $oldPredictableTemporary -Algorithm SHA256).Hash -ne $oldTemporaryHash) {
	throw 'Composition changed a pre-existing predictable temporary file.'
}

& $Executable --race $sequence
if ($LASTEXITCODE -ne 0) {
	throw "Concurrent composer admission test failed with exit code $LASTEXITCODE."
}
$numberedOutput = Join-Path $resolvedWorkRoot 'CS_sequence_smoke_1-sbs-2.mp4'
if (-not (Test-Path -LiteralPath $numberedOutput -PathType Leaf)) {
	throw 'Concurrent composition did not preserve the original SBS output with a numbered filename.'
}

$leftSource = $sourceFiles | Where-Object { $_.DirectoryName -eq $leftFrames } | Select-Object -First 1
$rightSource = $sourceFiles | Where-Object { $_.DirectoryName -eq $rightFrames } | Select-Object -First 1

$longSequence = Join-Path $resolvedWorkRoot 'CS_sequence_long_cadence'
$longTimestamps = @(
	for ($index = 0; $index -lt 300; $index++) {
		[uint64](2000000 + ($index * 16667))
	}
)
Write-TestManifest -Sequence $longSequence -Suffixes @('left') -Timestamps $longTimestamps -ArtifactPaths @($leftSource.FullName)
Invoke-CompositionWithSampleCount -Sequence $longSequence -Output (Join-Path $resolvedWorkRoot 'CS_sequence_long_cadence-left.mp4') -ExpectedCount 300

$jitterSequence = Join-Path $resolvedWorkRoot 'CS_sequence_jitter_cadence'
$jitterTimestamps = [System.Collections.Generic.List[object]]::new()
$jitterTimestamp = [uint64]2500000
for ($index = 0; $index -lt 120; $index++) {
	$jitterTimestamps.Add($jitterTimestamp)
	if (($index % 2) -eq 0) {
		$jitterTimestamp += [uint64]16600
	} else {
		$jitterTimestamp += [uint64]16734
	}
}
Write-TestManifest -Sequence $jitterSequence -Suffixes @('left') -Timestamps $jitterTimestamps.ToArray() -ArtifactPaths @($leftSource.FullName)
Invoke-CompositionWithSampleCount -Sequence $jitterSequence -Output (Join-Path $resolvedWorkRoot 'CS_sequence_jitter_cadence-left.mp4') -ExpectedCount 120

$fractionalSequence = Join-Path $resolvedWorkRoot 'CS_sequence_fractional_cadence'
$fractionalTimestamps = @(
	[uint64]3000000,
	[uint64]3133333,
	[uint64]3266667,
	[uint64]3400000,
	[uint64]3533333,
	[uint64]3666667,
	[uint64]3800000,
	[uint64]3933333,
	[uint64]4066667
)
Write-TestManifest -Sequence $fractionalSequence -Suffixes @('left') -Timestamps $fractionalTimestamps -ArtifactPaths @($leftSource.FullName)
Invoke-CompositionWithSampleCount -Sequence $fractionalSequence -Output (Join-Path $resolvedWorkRoot 'CS_sequence_fractional_cadence-left.mp4') -ExpectedCount 10

$trailingSequence = Join-Path $resolvedWorkRoot 'CS_sequence_single_tail'
Write-TestManifest -Sequence $trailingSequence -Suffixes @('left') `
	-Timestamps @([uint64]4000000, [uint64]4020000, [uint64]4040000) `
	-ArtifactPaths @($leftSource.FullName) -States @('completed', 'dropped', 'dropped')
Invoke-CompositionWithSampleCount -Sequence $trailingSequence -Output (Join-Path $resolvedWorkRoot 'CS_sequence_single_tail-left.mp4') -ExpectedCount 3

$legacySequence = Join-Path $resolvedWorkRoot 'CS_sequence_legacy_tail'
New-Item -ItemType Directory -Force -Path $legacySequence | Out-Null
$legacyManifest = [ordered]@{
	schema = 'csx.frame-sequence/1'
	state = 'complete'
	eye = 'Left'
	frames = @(
		[ordered]@{ timestampUs = [uint64]5000000; written = $true; paths = @($leftSource.FullName) },
		[ordered]@{ timestampUs = [uint64]5020000; written = $false; paths = @() },
		[ordered]@{ timestampUs = [uint64]5040000; written = $false; paths = @() }
	)
}
$legacyManifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $legacySequence 'sequence.json') -Encoding utf8NoBOM
Invoke-CompositionWithSampleCount -Sequence $legacySequence -Output (Join-Path $resolvedWorkRoot 'CS_sequence_legacy_tail-left.mp4') -ExpectedCount 3

$unsafeSequence = Join-Path $resolvedWorkRoot 'CS_sequence_unsafe'
Write-TestManifest -Sequence $unsafeSequence -Suffixes @('../outside') -Timestamps @([uint64]1000) -ArtifactPaths @($leftSource.FullName)
Invoke-ExpectedFailure -Sequence $unsafeSequence

$duplicateSequence = Join-Path $resolvedWorkRoot 'CS_sequence_duplicate'
Write-TestManifest -Sequence $duplicateSequence -Suffixes @('left', 'LEFT') -Timestamps @([uint64]1000) -ArtifactPaths @($leftSource.FullName, $rightSource.FullName)
Invoke-ExpectedFailure -Sequence $duplicateSequence

$tooManyOutputsSequence = Join-Path $resolvedWorkRoot 'CS_sequence_too_many_outputs'
Write-TestManifest -Sequence $tooManyOutputsSequence -Suffixes @('left', 'right', 'combined') -Timestamps @([uint64]1000) -ArtifactPaths @($leftSource.FullName, $rightSource.FullName, $leftSource.FullName)
Invoke-ExpectedFailure -Sequence $tooManyOutputsSequence

$uncommittedSequence = Join-Path $resolvedWorkRoot 'CS_sequence_uncommitted'
Write-TestManifest -Sequence $uncommittedSequence -Suffixes @('left') -Timestamps @([uint64]1000) -ArtifactPaths @($leftSource.FullName) -Committed $false
Invoke-ExpectedFailure -Sequence $uncommittedSequence

$duplicateTimeSequence = Join-Path $resolvedWorkRoot 'CS_sequence_duplicate_time'
Write-TestManifest -Sequence $duplicateTimeSequence -Suffixes @('left') -Timestamps @([uint64]1000, [uint64]1000) -ArtifactPaths @($leftSource.FullName)
Invoke-ExpectedFailure -Sequence $duplicateTimeSequence

$decreasingTimeSequence = Join-Path $resolvedWorkRoot 'CS_sequence_decreasing_time'
Write-TestManifest -Sequence $decreasingTimeSequence -Suffixes @('left') -Timestamps @([uint64]1000, [uint64]2000, [uint64]1500) -ArtifactPaths @($leftSource.FullName)
Invoke-ExpectedFailure -Sequence $decreasingTimeSequence

$negativeTimeSequence = Join-Path $resolvedWorkRoot 'CS_sequence_negative_time'
Write-TestManifest -Sequence $negativeTimeSequence -Suffixes @('left') -Timestamps @([int64]-1) -ArtifactPaths @($leftSource.FullName)
Invoke-ExpectedFailure -Sequence $negativeTimeSequence

$fractionalTimeSequence = Join-Path $resolvedWorkRoot 'CS_sequence_fractional_time'
Write-TestManifest -Sequence $fractionalTimeSequence -Suffixes @('left') -Timestamps @([double]1000.5) -ArtifactPaths @($leftSource.FullName)
Invoke-ExpectedFailure -Sequence $fractionalTimeSequence

$overflowTimeSequence = Join-Path $resolvedWorkRoot 'CS_sequence_overflow_time'
Write-TestManifest -Sequence $overflowTimeSequence -Suffixes @('left') -Timestamps @([uint64]0, [uint64]922337203685477581) -ArtifactPaths @($leftSource.FullName)
Invoke-ExpectedFailure -Sequence $overflowTimeSequence

$invalidTailSequence = Join-Path $resolvedWorkRoot 'CS_sequence_invalid_tail'
Write-TestManifest -Sequence $invalidTailSequence -Suffixes @('left') `
	-Timestamps @([uint64]1000, [int64]-1) -ArtifactPaths @($leftSource.FullName) `
	-States @('completed', 'dropped')
Invoke-ExpectedFailure -Sequence $invalidTailSequence

$overflowTailSequence = Join-Path $resolvedWorkRoot 'CS_sequence_overflow_tail'
Write-TestManifest -Sequence $overflowTailSequence -Suffixes @('left') `
	-Timestamps @([uint64]0, [uint64]922337203685477581) -ArtifactPaths @($leftSource.FullName) `
	-States @('completed', 'dropped')
Invoke-ExpectedFailure -Sequence $overflowTailSequence

$aliasSequence = Join-Path $resolvedWorkRoot 'CS_sequence_alias'
$aliasSource = Join-Path $resolvedWorkRoot 'CS_sequence_alias-left.mp4'
Copy-Item -LiteralPath $leftSource.FullName -Destination $aliasSource
$aliasHash = (Get-FileHash -LiteralPath $aliasSource -Algorithm SHA256).Hash
Write-TestManifest -Sequence $aliasSequence -Suffixes @('left') -Timestamps @([uint64]1000) -ArtifactPaths @($aliasSource)
& $Executable $aliasSequence
if ($LASTEXITCODE -ne 0) {
	throw 'Composer could not avoid an existing source/output name collision.'
}
if ((Get-FileHash -LiteralPath $aliasSource -Algorithm SHA256).Hash -ne $aliasHash) {
	throw 'Composition changed a source file that occupied the default output name.'
}
$aliasOutput = Join-Path $resolvedWorkRoot 'CS_sequence_alias-left-2.mp4'
if (-not (Test-Path -LiteralPath $aliasOutput -PathType Leaf)) {
	throw 'Composer did not select a non-conflicting final output name.'
}

foreach ($source in $sourceFiles) {
	if ((Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash -ne $sourceHashes[$source.FullName]) {
		throw "Composer changed lossless source evidence: $($source.FullName)"
	}
}

$unexpectedOutputs = @(
	'CS_sequence_unsafe-left.mp4',
	'CS_sequence_duplicate-sbs.mp4',
	'CS_sequence_too_many_outputs-left.mp4',
	'CS_sequence_uncommitted-left.mp4',
	'CS_sequence_duplicate_time-left.mp4',
	'CS_sequence_decreasing_time-left.mp4',
	'CS_sequence_negative_time-left.mp4',
	'CS_sequence_fractional_time-left.mp4',
	'CS_sequence_overflow_time-left.mp4',
	'CS_sequence_invalid_tail-left.mp4',
	'CS_sequence_overflow_tail-left.mp4'
)
foreach ($name in $unexpectedOutputs) {
	if (Test-Path -LiteralPath (Join-Path $resolvedWorkRoot $name)) {
		throw "Invalid fixture created an output: $name"
	}
}
