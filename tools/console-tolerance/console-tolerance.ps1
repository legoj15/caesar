#Requires -Version 7.0
<#
.SYNOPSIS
    The suite stage-2 Phase IV (C11) console-tolerance net: drives caesar-play to
    render each captured sequence and runs console_tolerance.py to verdict the
    render against the New 3DS line-in capture (match within tolerance EXCEPT the
    reverb tail). One command when the captures land.

.DESCRIPTION
    Two modes, both with the ab-verify / diag-goldens / play-goldens discipline
    (build-freshness gate, Stop-funnel, exit 0/1/2, never a false pass):

      -SelfValidate  render two REAL base sequences (BGM_DEN_RESULT + BGM_DEN_MAP)
                     and run console_tolerance.py --self-validate on each: the
                     realistic capture-chain fixture must PASS and the three
                     negative controls must FAIL their named metric. This proves
                     the harness works BEFORE any console capture exists.

      (default)      compare mode. For every capture WAV present in -CapturesDir
                     (default tools\console-tolerance\captures\), render the mapped
                     sequence at the CAPTURE's own sample rate and verdict it. Exit
                     0 iff every present capture passes; 1 if any is out of
                     tolerance; 2 on any harness error.

    Nothing corpus-derived is committed: renders go to a work dir, captures are
    gitignored, only this script + console_tolerance.py + README are in the repo.

.NOTES
    Exit codes (same contract as ab-verify / diag-goldens / play-goldens):
      0  self-validate passed / every present capture within tolerance
      1  one or more captures out of tolerance (reverb tail excepted)
      2  harness error -- NOTHING verified, never read as a pass
#>
[CmdletBinding()]
param(
    [switch] $SelfValidate,
    [string] $Exe,
    [string] $Python,
    [string] $CorpusRoot = 'E:\legoj\Documents\3DSWii Dumps\Dumps',
    [string] $CapturesDir,
    [string] $WorkDir,
    [switch] $AllowStaleExe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$HarnessVersion = 'console-tolerance/1'
$ExitPass = 0; $ExitFail = 1; $ExitHarnessError = 2

# --------------------------------------------------------------------------- #
# Plumbing (mirrors play-goldens)
# --------------------------------------------------------------------------- #
function Write-CtHeading { param([string] $Text) Write-Host ''; Write-Host "== $Text" -ForegroundColor Cyan }
function Write-CtInfo    { param([string] $Text) Write-Host "   $Text" -ForegroundColor DarkGray }
function Write-CtWarn    { param([string] $Text) Write-Host "   ! $Text" -ForegroundColor Yellow }

function Stop-CtHarness { param([string] $Text)
    Write-Host ''
    Write-Host "HARNESS ERROR: $Text" -ForegroundColor Red
    Write-Host 'Nothing was verified. Do NOT read this as a pass.' -ForegroundColor Red
    exit $ExitHarnessError
}
# Every unanticipated terminating error funnels to 2, never 1.
trap { Stop-CtHarness "unhandled error: $($_.Exception.Message)`n$($_.ScriptStackTrace)" }

# THE 'man' GUARD (see ab-verify / play-goldens): assert every function this
# script defines resolves to OUR function, so a shadowing alias can't no-op a guard.
function Assert-CtNoShadowedHelpers { param([string] $ScriptPath)
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref] $null, [ref] $null)
    $defined = @($ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) |
                 ForEach-Object { $_.Name } | Sort-Object -Unique)
    if ($defined.Count -lt 6) {
        Write-Host "HARNESS ERROR: could not enumerate this script's own functions (found $($defined.Count))." -ForegroundColor Red
        exit 2
    }
    foreach ($name in $defined) {
        $resolved = @(Get-Command -Name $name -All -ErrorAction SilentlyContinue)
        if ($resolved.Count -eq 0 -or $resolved[0].CommandType -ne 'Function') {
            Write-Host "HARNESS ERROR: helper '$name' does not resolve to our function. Rename it." -ForegroundColor Red
            exit 2
        }
    }
}

function Remove-CtDir { param([string] $Path)
    if ([System.IO.Directory]::Exists($Path)) {
        try { [System.IO.Directory]::Delete("\\?\$Path", $true) } catch { [System.IO.Directory]::Delete($Path, $true) }
    }
}

# Minimal RIFF/WAVE header parse: sample rate + duration (for rate-matched renders).
function Get-CtWavInfo { param([string] $Path)
    $b = [System.IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 44 -or [System.Text.Encoding]::ASCII.GetString($b, 0, 4) -ne 'RIFF' -or
        [System.Text.Encoding]::ASCII.GetString($b, 8, 4) -ne 'WAVE') {
        Stop-CtHarness "not a RIFF/WAVE file: $Path"
    }
    $pos = 12; $rate = 0; $ch = 0; $bits = 0; $dataSz = 0
    while ($pos + 8 -le $b.Length) {
        $cid = [System.Text.Encoding]::ASCII.GetString($b, $pos, 4)
        $csz = [System.BitConverter]::ToUInt32($b, $pos + 4)
        if ($cid -eq 'fmt ') {
            $ch   = [System.BitConverter]::ToUInt16($b, $pos + 10)
            $rate = [System.BitConverter]::ToUInt32($b, $pos + 12)
            $bits = [System.BitConverter]::ToUInt16($b, $pos + 22)
        }
        elseif ($cid -eq 'data') { $dataSz = $csz }
        $pos += 8 + $csz + ($csz -band 1)
    }
    if ($rate -eq 0 -or $ch -eq 0 -or $bits -eq 0) { Stop-CtHarness "could not parse fmt chunk in $Path" }
    $frames = if ($dataSz -gt 0) { [math]::Floor($dataSz / ($ch * ($bits / 8))) } else { 0 }
    $seconds = if ($frames -gt 0) { $frames / $rate } else { 0 }
    return [pscustomobject]@{ Rate = [int]$rate; Channels = [int]$ch; Bits = [int]$bits; Seconds = [double]$seconds }
}

function Invoke-CtProcess { param([string] $Exe, [string[]] $Argv)
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    foreach ($a in $Argv) { $psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $so = $p.StandardOutput.ReadToEndAsync(); $se = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    $r = [pscustomobject]@{ Exit = $p.ExitCode; Stdout = $so.GetAwaiter().GetResult(); Stderr = $se.GetAwaiter().GetResult() }
    $p.Dispose(); return $r
}

# --------------------------------------------------------------------------- #
# The capture manifest -- capture filename -> (source archive, sequence).
# See docs/CAPTURE-REQUEST.md for what each capture calibrates. The two BGM
# captures already EXIST (2026-07-08 New 3DS line-in, 192 kHz/16-bit, in the
# default -CapturesDir below); the SE rows are STILL WANTED (a capture WAV that
# is not present is simply skipped, so a partial set verdicts what exists).
# --------------------------------------------------------------------------- #
$MeetSound = 'MiiPlaza\sound\MeetSound.bcsar'
$Captures = @(
    @{ File = 'BGM_MAIN_Mii_Only_One_console.wav'; Rel = $MeetSound; Seq = 'BGM_MAIN_Mii_Only_One';  Kind = 'bgm'  }
    @{ File = 'EMPTY_LANDSCAPE_console.wav';        Rel = $MeetSound; Seq = 'BGM_DEN_EMPTY_LANDSCAPE'; Kind = 'bgm' }
    @{ File = 'SE_NEW_DRUM01_console.wav';       Rel = $MeetSound; Seq = 'SE_NEW_DRUM01';         Kind = 'note' }
    @{ File = 'SE_LEGEND_KEY_FLY_console.wav';   Rel = $MeetSound; Seq = 'SE_LEGEND_KEY_FLY';     Kind = 'note' }
    @{ File = 'SE_NEW_SLIDE_MAP_console.wav';    Rel = $MeetSound; Seq = 'SE_NEW_SLIDE_MAP';      Kind = 'note' }
    @{ File = 'SE_LEGEND_MENU_CURSOR_console.wav'; Rel = $MeetSound; Seq = 'SE_LEGEND_MENU_CURSOR'; Kind = 'note' }
)

# The real captures live alongside the extracted MeetSound bank (the 2026-07-08
# console line-in WAVs). Overridable with -CapturesDir.
$DefaultCapturesDir = Join-Path $CorpusRoot 'MiiPlaza\sound\MeetSound\BANK_BGM'

# Base sequences for -SelfValidate (real, finite, multi-onset renders).
$SelfValidateBases = @(
    @{ Name = 'base_den'; Rel = $MeetSound; Seq = 'BGM_DEN_RESULT' }
    @{ Name = 'base_map'; Rel = $MeetSound; Seq = 'BGM_DEN_MAP'    }
)

function Get-CtRender {
    param([string] $Exe, [string] $Archive, [string] $Seq, [int] $Rate, [int] $MaxSeconds, [string] $OutWav)
    if ([System.IO.File]::Exists($OutWav)) { [System.IO.File]::Delete($OutWav) }
    $argv = @('--render', $Archive, '--seq', $Seq, '--out', $OutWav, '--rate', [string]$Rate, '--max-seconds', [string]$MaxSeconds)
    $r = Invoke-CtProcess -Exe $Exe -Argv $argv
    if ($r.Exit -ne 0 -or -not [System.IO.File]::Exists($OutWav)) {
        Stop-CtHarness "caesar-play failed to render '$Seq' (exit $($r.Exit)).`n  stderr:`n$($r.Stderr)"
    }
    return $OutWav
}

# --------------------------------------------------------------------------- #
# Modes
# --------------------------------------------------------------------------- #
function Invoke-CtSelfValidate { param([string] $Exe, [string] $Py, [string] $Analyzer, [string] $Archive, [string] $WorkDir)
    Write-CtHeading 'SELF-VALIDATE (proves the harness before any capture exists)'
    $outBase = Join-Path $WorkDir 'sv'; Remove-CtDir $outBase; [System.IO.Directory]::CreateDirectory($outBase) | Out-Null

    $rendered = @{}
    foreach ($b in $SelfValidateBases) {
        $wav = Join-Path $outBase ($b.Name + '.wav')
        Get-CtRender -Exe $Exe -Archive $Archive -Seq $b.Seq -Rate 48000 -MaxSeconds 120 -OutWav $wav | Out-Null
        $rendered[$b.Name] = $wav
        Write-CtInfo "rendered $($b.Seq) -> $($b.Name).wav"
    }

    $names = @($SelfValidateBases | ForEach-Object { $_.Name })
    $failures = 0
    for ($i = 0; $i -lt $names.Count; $i++) {
        $base = $names[$i]; $other = $names[($i + 1) % $names.Count]
        Write-CtHeading "self-validate base '$base' (other '$other')"
        $r = Invoke-CtProcess -Exe $Py -Argv @($Analyzer, '--self-validate', $rendered[$base], '--other', $rendered[$other])
        Write-Host $r.Stdout
        if ($r.Exit -ne 0) { Write-Host $r.Stderr -ForegroundColor Red; $failures++ }
    }

    Write-CtHeading 'SELF-VALIDATE result'
    if ($failures -gt 0) {
        Write-Host "   $failures base(s) FAILED self-validate -- do not trust this harness." -ForegroundColor Red
        return $ExitHarnessError
    }
    Write-Host '   PASSED: realistic fixture passes and every negative control fails its named metric,' -ForegroundColor Green
    Write-Host '   on two real caesar-play renders. The harness is ready for the console captures.' -ForegroundColor Green
    return $ExitPass
}

function Invoke-CtCompare { param([string] $Exe, [string] $Py, [string] $Analyzer, [string] $CorpusRoot, [string] $CapturesDir, [string] $WorkDir)
    Write-CtHeading "Compare captures in $CapturesDir"
    $outBase = Join-Path $WorkDir 'out'; Remove-CtDir $outBase; [System.IO.Directory]::CreateDirectory($outBase) | Out-Null

    $present = @($Captures | Where-Object { [System.IO.File]::Exists((Join-Path $CapturesDir $_.File)) })
    if ($present.Count -eq 0) {
        Write-CtWarn "no capture WAVs found in $CapturesDir."
        Write-CtInfo 'Expected any of: ' ; foreach ($c in $Captures) { Write-CtInfo "   $($c.File)  <- $($c.Seq)" }
        Write-CtInfo 'See docs\CAPTURE-REQUEST.md for how to record them. (Nothing to verify yet.)'
        Stop-CtHarness "no captures present -- nothing was verified."
    }

    $fails = [System.Collections.Generic.List[string]]::new()
    foreach ($c in $present) {
        $cap = Join-Path $CapturesDir $c.File
        $info = Get-CtWavInfo -Path $cap
        $archive = Join-Path $CorpusRoot $c.Rel
        if (-not [System.IO.File]::Exists($archive)) { Stop-CtHarness "source archive not found: $archive" }
        $maxSec = [int][math]::Ceiling($info.Seconds) + 3
        if ($maxSec -lt 5) { $maxSec = 15 }
        $wav = Join-Path $outBase ($c.Seq + '.wav')
        Write-CtHeading "$($c.File)  ($($c.Seq), $($info.Rate) Hz, $([math]::Round($info.Seconds,1)) s)"
        Get-CtRender -Exe $Exe -Archive $archive -Seq $c.Seq -Rate $info.Rate -MaxSeconds $maxSec -OutWav $wav | Out-Null
        $r = Invoke-CtProcess -Exe $Py -Argv @($Analyzer, $cap, $wav)
        Write-Host $r.Stdout
        if ($r.Exit -eq 2) { Stop-CtHarness "analyzer errored on $($c.File):`n$($r.Stderr)" }
        if ($r.Exit -ne 0) { $fails.Add($c.File) }
    }

    Write-CtHeading 'Result'
    if ($fails.Count -eq 0) {
        Write-Host ("   ALL {0} present capture(s) within tolerance (reverb tail excepted)." -f $present.Count) -ForegroundColor Green
        return $ExitPass
    }
    Write-Host ("   {0} of {1} capture(s) OUT OF TOLERANCE: {2}" -f $fails.Count, $present.Count, ($fails -join ', ')) -ForegroundColor Yellow
    return $ExitFail
}

# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
Assert-CtNoShadowedHelpers -ScriptPath $PSCommandPath

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).ProviderPath
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'src\play\main.cpp'))) {
    Stop-CtHarness "this script expects to live at <repo>\tools\console-tolerance\; got repo root '$repoRoot'."
}
$analyzer = Join-Path $PSScriptRoot 'console_tolerance.py'
if (-not (Test-Path -LiteralPath $analyzer)) { Stop-CtHarness "missing analyzer: $analyzer" }

if (-not $WorkDir)     { $WorkDir = Join-Path $env:LOCALAPPDATA 'caesar-console-tolerance' }
if (-not $CapturesDir) { $CapturesDir = $DefaultCapturesDir }
if (-not $Exe)         { $Exe = Join-Path $repoRoot 'build\Release\caesar-play.exe' }

# Resolve python (python, then python3) and assert numpy is importable.
if (-not $Python) {
    foreach ($cand in @('python', 'python3')) {
        $cmd = Get-Command -Name $cand -ErrorAction SilentlyContinue
        if ($cmd) { $Python = $cmd.Source; break }
    }
}
if (-not $Python) { Stop-CtHarness 'no python interpreter found (need python or python3 with numpy).' }
$numpy = Invoke-CtProcess -Exe $Python -Argv @('-c', 'import numpy')
if ($numpy.Exit -ne 0) { Stop-CtHarness "python at '$Python' cannot import numpy:`n$($numpy.Stderr)" }

if (-not (Test-Path -LiteralPath $CorpusRoot)) { Stop-CtHarness "corpus root not found: $CorpusRoot" }
if (-not (Test-Path -LiteralPath $Exe)) {
    Stop-CtHarness "exe not found: $Exe`n  Build it first:  cmake --build build --config Release  (from a VS dev prompt)"
}
$Exe = (Resolve-Path -LiteralPath $Exe).ProviderPath
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)
[System.IO.Directory]::CreateDirectory($WorkDir) | Out-Null

# Build-freshness gate (same as play-goldens): a stale exe would verify a change
# that is not in it -- a false result. Watch all of src/ + the CMake files.
if (-not $AllowStaleExe) {
    $exeTime = [System.IO.File]::GetLastWriteTimeUtc($Exe)
    $watched = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'src') -Recurse -File -Include *.cpp, *.hpp, *.h, *.c -ErrorAction SilentlyContinue)
    foreach ($bf in @('CMakeLists.txt', 'CMakePresets.json')) {
        $bp = Join-Path $repoRoot $bf
        if (Test-Path -LiteralPath $bp) { $watched += Get-Item -LiteralPath $bp }
    }
    $newer = @($watched | Where-Object { $_.LastWriteTimeUtc -gt $exeTime })
    if ($newer.Count -gt 0) {
        Stop-CtHarness ("$($newer.Count) build input(s) are NEWER than $Exe (e.g. $($newer[0].Name)).`n" +
            "  The exe does not contain the change under test.`n  Rebuild, or pass -AllowStaleExe if you really mean it.")
    }
}

Write-CtHeading 'caesar console-tolerance net'
Write-CtInfo "repo     : $repoRoot"
Write-CtInfo "exe      : $Exe"
Write-CtInfo "python   : $Python"
Write-CtInfo "corpus   : $CorpusRoot"
Write-CtInfo "captures : $CapturesDir"
Write-CtInfo "work     : $WorkDir"

$archive = Join-Path $CorpusRoot $MeetSound

$lock = $null
try { $lock = [System.IO.File]::Open((Join-Path $WorkDir '.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
catch { Stop-CtHarness "another console-tolerance run already holds the lock on $WorkDir." }
try {
    if ($SelfValidate) {
        if (-not [System.IO.File]::Exists($archive)) { Stop-CtHarness "source archive not found: $archive" }
        $code = Invoke-CtSelfValidate -Exe $Exe -Py $Python -Analyzer $analyzer -Archive $archive -WorkDir $WorkDir
    }
    else {
        $code = Invoke-CtCompare -Exe $Exe -Py $Python -Analyzer $analyzer -CorpusRoot $CorpusRoot -CapturesDir $CapturesDir -WorkDir $WorkDir
    }
}
finally { if ($lock) { $lock.Dispose() } }

exit $code
