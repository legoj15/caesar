#Requires -Version 7.0
<#
.SYNOPSIS
    Corpus fan-out for caesar-roundtrip: proves the stage-1 byte-identical
    re-serializer reproduces every embedded child (and the container) across the
    whole .bcsar corpus.

.DESCRIPTION
    Runs the caesar-roundtrip dev exe in --verify mode over every corpus archive,
    parallelised across archives (longest-first), and aggregates the per-format
    match/mismatch/skip tallies plus a mismatches.tsv.

    The exe re-serialises each child from the parsed model and compares against a
    copy of the source span (never the live buffer). A format with no Serialize()
    yet reports SKIPPED. Until the serializers land (stage-1 commits 1/3/4), EVERY
    format is SKIPPED and every archive exits 2 ("nothing verifiable") — so this
    wrapper exits 2 as well. That is the whole point: the harness can never print a
    green pass before there is something to verify. As each Serialize() ships, its
    format flips from SKIPPED to matched and the corpus verdict tightens toward 0.

    Discipline mirrored from tools/ab-verify (see that script's notes): a single
    Stop-harness funnel, a trap so no unhandled error is mistaken for a clean run,
    an AST-derived shadowed-helper guard, a stale-exe guard (src/ newer than the
    binary is a false result waiting to happen), a zero-children guard, and a
    -SelfTest that exercises the exit contract before any real run is trusted.

.EXAMPLE
    tools\roundtrip-verify\roundtrip-verify.ps1

.EXAMPLE
    # Prove the harness plumbing before trusting a verdict.
    tools\roundtrip-verify\roundtrip-verify.ps1 -SelfTest

.NOTES
    Exit codes (same contract as the exe and as ab-verify):
      0  every enumerated child re-serialised byte-identically
      1  at least one child mismatched
      2  harness error / nothing was verifiable (NEVER read as a pass)
#>
[CmdletBinding()]
param(
    # The dev exe. Default: the repo's Release build.
    [string] $Exe,

    [string] $CorpusRoot = 'E:\legoj\Documents\3DSWii Dumps\Dumps',

    [int] $Jobs = 0,

    # Wildcard over the corpus-relative path, e.g. '*Fates*'.
    [string] $Filter,

    # Where the run report + mismatches.tsv land. Default: a temp dir off LOCALAPPDATA.
    [string] $OutDir,

    [switch] $SelfTest,

    # Escape hatch: skip the "exe older than src/" check (named for what it lets through).
    [switch] $AllowStaleExe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ExitAllMatch    = 0
$ExitMismatch    = 1
$ExitHarnessErr  = 2

# ---------------------------------------------------------------------------
# Plumbing
# ---------------------------------------------------------------------------

function Write-RtHeading { param([string] $Text)
    Write-Host ''
    Write-Host "== $Text" -ForegroundColor Cyan
}
function Write-RtInfo { param([string] $Text) Write-Host "   $Text" -ForegroundColor DarkGray }
function Write-RtWarn { param([string] $Text) Write-Host "   ! $Text" -ForegroundColor Yellow }

# Every fatal path funnels through here, so a harness failure can never be read as
# "no mismatches".
function Stop-RtHarness { param([string] $Text)
    Write-Host ''
    Write-Host "HARNESS ERROR: $Text" -ForegroundColor Red
    Write-Host 'Nothing was verified. Do NOT read this as a pass.' -ForegroundColor Red
    exit $ExitHarnessErr
}

# ...and so does every path we did not anticipate. Without this an unhandled
# terminating error exits 1 - which this contract defines as "mismatch found",
# filing a run that verified nothing as a real regression.
trap { Stop-RtHarness "unhandled error: $($_.Exception.Message)`n$($_.ScriptStackTrace)" }

# THE 'man' GUARD (see ab-verify): a helper silently resolving to a builtin/alias
# instead of our function turns its guard into a no-op. Assert every function this
# script defines resolves to OUR function. The list is the script's own AST, so it
# cannot drift - and it covers Stop-RtHarness, through which every fatal path runs.
function Assert-RtNoShadowedHelpers { param([string] $ScriptPath)
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref] $null, [ref] $null)
    $defined = @($ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) |
                 ForEach-Object { $_.Name } | Sort-Object -Unique)
    if ($defined.Count -lt 5) {
        Write-Host "HARNESS ERROR: could not enumerate this script's own functions (found $($defined.Count))." -ForegroundColor Red
        exit 2
    }
    foreach ($name in $defined) {
        $resolved = @(Get-Command -Name $name -All -ErrorAction SilentlyContinue)
        if ($resolved.Count -eq 0 -or $resolved[0].CommandType -ne 'Function') {
            $what = if ($resolved.Count -eq 0) { 'nothing' } else { "a $($resolved[0].CommandType)" }
            Write-Host ''
            Write-Host "HARNESS ERROR: helper '$name' resolves to $what, not our function. Rename it." -ForegroundColor Red
            Write-Host 'Nothing was verified. Do NOT read this as a pass.' -ForegroundColor Red
            exit 2
        }
    }
}

# ---------------------------------------------------------------------------
# Corpus (top-level directory fan-out + longest-first, as in ab-verify)
# ---------------------------------------------------------------------------

function Get-RtCorpus { param([string] $Root, [string] $Wildcard, [switch] $SmallestFirst)
    if (-not (Test-Path -LiteralPath $Root)) { Stop-RtHarness "corpus root not found: $Root" }
    $rootFull = (Resolve-Path -LiteralPath $Root).ProviderPath.TrimEnd('\')

    $tops      = @([System.IO.Directory]::EnumerateDirectories($rootFull))
    $rootFiles = @([System.IO.Directory]::EnumerateFiles($rootFull, '*.bcsar', [System.IO.SearchOption]::TopDirectoryOnly))
    if ($tops.Count -eq 0) {
        $paths = $rootFiles
    }
    else {
        $found = $tops | ForEach-Object -ThrottleLimit ([Math]::Min(12, $tops.Count)) -Parallel {
            $ErrorActionPreference = 'Stop'
            [System.IO.Directory]::EnumerateFiles($_, '*.bcsar', [System.IO.SearchOption]::AllDirectories)
        }
        $paths = @($rootFiles) + @($found)
    }

    $selected = foreach ($p in $paths) {
        $rel    = $p.Substring($rootFull.Length + 1)
        $relKey = $rel.Replace('\', '/')
        $name   = [System.IO.Path]::GetFileName($p)
        if ($Wildcard -and -not ($relKey -like $Wildcard) -and -not ($rel -like $Wildcard) -and -not ($name -like $Wildcard)) { continue }
        [pscustomobject]@{ Key = $relKey; Path = $p; Size = [System.IO.FileInfo]::new($p).Length }
    }
    $selected = @($selected)

    if ($SmallestFirst) { return @($selected | Sort-Object Size, Key) }
    return @($selected | Sort-Object @{ Expression = 'Size'; Descending = $true }, @{ Expression = 'Key'; Descending = $false })
}

# ---------------------------------------------------------------------------
# One archive: run the exe, parse its VERIFY lines into a per-archive record.
# ---------------------------------------------------------------------------

function ConvertTo-RtRecord { param([string] $Key, [int] $Exit, [string[]] $Lines)
    $formats = [System.Collections.Generic.List[object]]::new()
    foreach ($line in $Lines) {
        $f = $line -split "`t"
        if ($f.Count -lt 1 -or $f[0] -ne 'VERIFY') { continue }
        $get = { param($p) [int](($f | Where-Object { $_ -like "$p=*" }) -replace "$p=", '') }
        $formats.Add([pscustomobject]@{
            Format     = $f[2]
            Matched    = (& $get 'matched')
            Mismatched = (& $get 'mismatched')
            Skipped    = (& $get 'skipped')
        })
    }
    [pscustomobject]@{ Key = $Key; Exit = $Exit; Formats = $formats; Lines = $Lines }
}

# ---------------------------------------------------------------------------
# The verdict: aggregate records into per-format totals + an exit code. Kept a
# pure function of the records so the self-test can drive it with synthetic input.
# ---------------------------------------------------------------------------

function Get-RtVerdict { param([object[]] $Records)
    $perFormat = [System.Collections.Generic.Dictionary[string, object]]::new()
    $matched = 0; $mismatched = 0; $skipped = 0
    $exitCounts = @{ 0 = 0; 1 = 0; 2 = 0 }

    foreach ($r in $Records) {
        if ($exitCounts.ContainsKey([int] $r.Exit)) { $exitCounts[[int] $r.Exit]++ } else { $exitCounts[2]++ }
        foreach ($fmt in $r.Formats) {
            if (-not $perFormat.ContainsKey($fmt.Format)) {
                $perFormat[$fmt.Format] = [pscustomobject]@{ Format = $fmt.Format; Matched = 0; Mismatched = 0; Skipped = 0 }
            }
            $pf = $perFormat[$fmt.Format]
            $pf.Matched    += $fmt.Matched
            $pf.Mismatched += $fmt.Mismatched
            $pf.Skipped    += $fmt.Skipped
            $matched       += $fmt.Matched
            $mismatched    += $fmt.Mismatched
            $skipped       += $fmt.Skipped
        }
    }

    $walked = $matched + $mismatched + $skipped

    # Exit rule, in priority order and cross-checked against the parsed counts so a
    # miswired exe (exit 0 while a count says mismatch, or vice versa) is caught:
    #   1  any archive reported a mismatch
    #   2  no mismatch, but at least one archive was not fully verifiable
    #   0  every archive fully verified (and something was actually re-serialised)
    $exit = $ExitAllMatch
    if ($exitCounts[1] -gt 0 -or $mismatched -gt 0) { $exit = $ExitMismatch }
    elseif ($exitCounts[2] -gt 0)                   { $exit = $ExitHarnessErr }
    elseif ($matched -eq 0)                         { $exit = $ExitHarnessErr } # all-match but nothing matched => nothing verified

    [pscustomobject]@{
        Exit = $exit; Matched = $matched; Mismatched = $mismatched; Skipped = $skipped; Walked = $walked
        PerFormat = @($perFormat.Values | Sort-Object Format); ExitCounts = $exitCounts; Records = $Records
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

Assert-RtNoShadowedHelpers -ScriptPath $PSCommandPath

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).ProviderPath
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'src\roundtrip\main.cpp'))) {
    Stop-RtHarness "this script expects to live at <repo>\tools\roundtrip-verify\; got repo root '$repoRoot'."
}

if (-not $Exe) { $Exe = Join-Path $repoRoot 'build\Release\caesar-roundtrip.exe' }
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

if (-not (Test-Path -LiteralPath $Exe)) {
    Stop-RtHarness "caesar-roundtrip exe not found: $Exe`n  Build it first:  cmake --build build --config Release  (from a VS dev prompt)"
}
$Exe = (Resolve-Path -LiteralPath $Exe).ProviderPath
$exeInfo = Get-Item -LiteralPath $Exe

# A stale exe compares against an old build of the serializer and can print a green
# pass while your change was never compiled in. The exe links caesar_core, so ANY
# src/ change (plus the CMake files) can move its behaviour.
if (-not $AllowStaleExe) {
    $watched = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'src') -Recurse -File -Include *.cpp, *.hpp, *.h, *.c -ErrorAction SilentlyContinue)
    foreach ($bf in @('CMakeLists.txt', 'CMakePresets.json')) {
        $bp = Join-Path $repoRoot $bf
        if (Test-Path -LiteralPath $bp) { $watched += Get-Item -LiteralPath $bp }
    }
    $newer = @($watched | Where-Object { $_.LastWriteTimeUtc -gt $exeInfo.LastWriteTimeUtc })
    if ($newer.Count -gt 0) {
        Stop-RtHarness ("$($newer.Count) build input(s) are NEWER than $Exe (e.g. $($newer[0].Name)).`n" +
            "  The exe does not contain your change; this run could be a false result.`n" +
            '  Rebuild, or pass -AllowStaleExe if you really mean it.')
    }
}

if (-not $OutDir) { $OutDir = Join-Path $env:LOCALAPPDATA 'caesar-roundtrip' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-RtHeading 'caesar-roundtrip corpus verify'
Write-RtInfo "repo   : $repoRoot"
Write-RtInfo "exe    : $Exe"
Write-RtInfo "jobs   : $Jobs"
Write-RtInfo "out    : $OutDir"

$corpus = Get-RtCorpus -Root $CorpusRoot -Wildcard $Filter -SmallestFirst:$SelfTest
if ($corpus.Count -eq 0) { Stop-RtHarness "no .bcsar archives found under '$CorpusRoot'$(if ($Filter) { " matching '$Filter'" })." }

# ===========================================================================
# SELF-TEST: prove the exit contract before any real verdict is trusted.
# ===========================================================================
if ($SelfTest) {
    Write-RtHeading 'SELF-TEST'
    Write-RtInfo 'Checks the exe exit contract, the wrapper aggregation, and the failure plumbing.'
    $failures = [System.Collections.Generic.List[string]]::new()
    $sample = @($corpus | Select-Object -First 2)

    # 1. A real archive with no serializers must exit 2 (nothing verifiable) AND
    #    still have walked something (skipped > 0) - the never-a-false-pass core.
    foreach ($a in $sample) {
        $out  = & $Exe --verify $a.Path
        $code = $LASTEXITCODE
        $rec  = ConvertTo-RtRecord -Key $a.Key -Exit $code -Lines @($out)
        $walked = ($rec.Formats | Measure-Object -Property Matched -Sum).Sum +
                  ($rec.Formats | Measure-Object -Property Mismatched -Sum).Sum +
                  ($rec.Formats | Measure-Object -Property Skipped -Sum).Sum
        if ($code -ne 2)  { $failures.Add("$($a.Key): expected exit 2 (nothing verifiable), got $code") }
        if ($walked -le 0) { $failures.Add("$($a.Key): exe walked 0 children - the enumeration is empty") }
        $mm = ($rec.Formats | Measure-Object -Property Mismatched -Sum).Sum
        if ($mm -ne 0) { $failures.Add("$($a.Key): unexpected $mm mismatches with no serializers present") }
    }
    Write-RtInfo "exe exit-2 contract: checked $($sample.Count) archive(s)"

    # 2. Failure plumbing: a missing archive must be a harness error (exit 2), not a pass.
    $bogus = Join-Path $OutDir 'does-not-exist.bcsar'
    & $Exe --verify $bogus | Out-Null
    if ($LASTEXITCODE -ne 2) { $failures.Add("missing-archive path returned $LASTEXITCODE, expected 2") }
    Write-RtInfo "missing-archive plumbing: exit $LASTEXITCODE (expect 2)"

    # 3. Aggregation contract: drive Get-RtVerdict with synthetic records so the
    #    0/1/2 mapping is proven directly (this is the logic a real corpus run trusts).
    $allSkip = @(
        [pscustomobject]@{ Key = 'a'; Exit = 2; Formats = @([pscustomobject]@{ Format = 'BCSEQ'; Matched = 0; Mismatched = 0; Skipped = 3 }); Lines = @() }
        [pscustomobject]@{ Key = 'b'; Exit = 2; Formats = @([pscustomobject]@{ Format = 'BCSAR'; Matched = 0; Mismatched = 0; Skipped = 1 }); Lines = @() }
    )
    $vAllSkip = Get-RtVerdict -Records $allSkip
    if ($vAllSkip.Exit -ne 2) { $failures.Add("verdict for all-skipped records was $($vAllSkip.Exit), expected 2") }

    $withMismatch = @(
        [pscustomobject]@{ Key = 'a'; Exit = 1; Formats = @([pscustomobject]@{ Format = 'BCSEQ'; Matched = 4; Mismatched = 1; Skipped = 0 }); Lines = @() }
    )
    $vMismatch = Get-RtVerdict -Records $withMismatch
    if ($vMismatch.Exit -ne 1) { $failures.Add("verdict for a mismatch record was $($vMismatch.Exit), expected 1") }

    $allMatch = @(
        [pscustomobject]@{ Key = 'a'; Exit = 0; Formats = @([pscustomobject]@{ Format = 'BCSEQ'; Matched = 5; Mismatched = 0; Skipped = 0 }); Lines = @() }
    )
    $vMatch = Get-RtVerdict -Records $allMatch
    if ($vMatch.Exit -ne 0) { $failures.Add("verdict for an all-match record was $($vMatch.Exit), expected 0") }

    # A degenerate "exit 0 but nothing actually matched" must NOT pass.
    $vacuous = @([pscustomobject]@{ Key = 'a'; Exit = 0; Formats = @(); Lines = @() })
    $vVacuous = Get-RtVerdict -Records $vacuous
    if ($vVacuous.Exit -ne 2) { $failures.Add("verdict for a vacuous (0 walked) record was $($vVacuous.Exit), expected 2") }
    Write-RtInfo 'aggregation contract: all-skip->2, mismatch->1, all-match->0, vacuous->2'

    # NOTE for the serializer commits (1/3/4): once a format re-serialises, extend
    # this self-test to mutate one source byte of a verified child and assert the
    # exe reports exactly one mismatch (exit 1) - the byte-flip proof ab-verify has.
    Write-Host ''
    if ($failures.Count -gt 0) {
        foreach ($f in $failures) { Write-Host "   FAIL: $f" -ForegroundColor Red }
        Write-Host '   SELF-TEST FAILED - do not trust this harness until it is fixed.' -ForegroundColor Red
        exit $ExitHarnessErr
    }
    Write-Host '   SELF-TEST PASSED: the exe honours exit-2-when-nothing-verifiable, a missing' -ForegroundColor Green
    Write-Host '   archive is a harness error, and the verdict maps 0/1/2 (and vacuous->2) exactly.' -ForegroundColor Green
    exit $ExitAllMatch
}

# --- real run ---------------------------------------------------------------
$corpusBytes = ($corpus | Measure-Object -Property Size -Sum).Sum
Write-RtInfo ("corpus : {0} archives, {1:N0} MB" -f $corpus.Count, ($corpusBytes / 1MB))

$results = $corpus | ForEach-Object -ThrottleLimit $Jobs -Parallel {
    $exe = $using:Exe
    $out = & $exe --verify $_.Path
    [pscustomobject]@{ Key = $_.Key; Exit = $LASTEXITCODE; Lines = @($out) }
}

$records = foreach ($r in $results) { ConvertTo-RtRecord -Key $r.Key -Exit $r.Exit -Lines $r.Lines }
$records = @($records)

if ($records.Count -eq 0) { Stop-RtHarness 'no archive produced a result.' }

$verdict = Get-RtVerdict -Records $records

# Zero-children guard: if the corpus walked nothing, the enumeration is broken and
# a "0 mismatches" verdict is meaningless.
if ($verdict.Walked -le 0) { Stop-RtHarness 'the whole corpus walked 0 children; the enumeration produced nothing to verify.' }

# mismatches.tsv - one row per mismatched format per archive.
$mmPath = Join-Path $OutDir 'mismatches.tsv'
$mmLines = [System.Collections.Generic.List[string]]::new()
$mmLines.Add("archive`tformat`tmismatched`tmatched`tskipped")
foreach ($rec in ($records | Sort-Object Key)) {
    foreach ($fmt in $rec.Formats) {
        if ($fmt.Mismatched -gt 0) {
            $mmLines.Add("$($rec.Key)`t$($fmt.Format)`t$($fmt.Mismatched)`t$($fmt.Matched)`t$($fmt.Skipped)")
        }
    }
}
[System.IO.File]::WriteAllLines($mmPath, [string[]] $mmLines)

Write-RtHeading 'RESULT'
Write-RtInfo ("archives : {0}  (exit 0/1/2 = {1}/{2}/{3})" -f $records.Count, $verdict.ExitCounts[0], $verdict.ExitCounts[1], $verdict.ExitCounts[2])
Write-Host ''
Write-Host ("   {0,-8} {1,10} {2,12} {3,10}" -f 'format', 'matched', 'mismatched', 'skipped') -ForegroundColor Gray
foreach ($pf in $verdict.PerFormat) {
    $color = if ($pf.Mismatched -gt 0) { 'Red' } elseif ($pf.Matched -gt 0) { 'Green' } else { 'DarkGray' }
    Write-Host ("   {0,-8} {1,10} {2,12} {3,10}" -f $pf.Format, $pf.Matched, $pf.Mismatched, $pf.Skipped) -ForegroundColor $color
}
Write-Host ''
Write-RtInfo ("totals   : matched={0} mismatched={1} skipped={2} (walked {3})" -f $verdict.Matched, $verdict.Mismatched, $verdict.Skipped, $verdict.Walked)
Write-RtInfo "mismatches.tsv: $mmPath"

Write-Host ''
switch ($verdict.Exit) {
    1 { Write-Host "   MISMATCH: $($verdict.Mismatched) child(ren) did not re-serialise identically." -ForegroundColor Red }
    2 {
        if ($verdict.Matched -eq 0 -and $verdict.Mismatched -eq 0) {
            Write-Host '   NOT YET VERIFIABLE: no serializer has landed, so every child is SKIPPED.' -ForegroundColor Yellow
            Write-Host '   This is the expected pre-commit-1 state - it is NOT a pass.' -ForegroundColor Yellow
        }
        else {
            Write-Host '   PARTIAL: some children verified, but at least one archive was not fully verifiable.' -ForegroundColor Yellow
        }
    }
    0 { Write-Host "   ALL MATCH: $($verdict.Matched) children re-serialised byte-identically." -ForegroundColor Green }
}
exit $verdict.Exit
