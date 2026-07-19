#Requires -Version 7.0
<#
.SYNOPSIS
    The player-change regression GATE: one command that runs caesar's five offline
    verification nets, honours their shared 0/1/2 exit contract, verifies the
    goldens manifest first, and exits 0 only when every selected net fully passed.

.DESCRIPTION
    Run this before committing a player-affecting change. It orchestrates, in
    cheapest-first order:

      diag       tools/diag-goldens         converter diagnostic surfaces   (caesar.exe)
      play       tools/play-goldens         player render byte-goldens      (caesar-play.exe)
      console    tools/console-tolerance    player vs New 3DS captures      (caesar-play.exe)
      roundtrip  tools/roundtrip-verify     stage-1 serializer round-trip   (caesar-roundtrip.exe)
      ab         tools/ab-verify            converter corpus A/B            (caesar.exe)

    Each net is run as its own pwsh child process (they call `exit`, so they must
    be isolated) and streams its own rich output live; the gate then prints one
    summary table (net / result / seconds) and a single verdict line.

    BEFORE running any net it verifies tools/gate/goldens-manifest.json: every
    gate-critical golden must be present with a matching sha256, else the consuming
    net is marked DEGRADED and the gate cannot claim a full pass. This kills the
    "a missing console WAV silently shrinks console-tolerance to a smaller PASS"
    failure mode. It never reads golden CONTENT (the goldens are gitignored
    corpus derivatives) — only presence + hash.

    A net whose binary is missing FAILS FAST (it is not run and never looks like a
    pass); the exact build command is printed. This script does NOT build.

    Exit contract per net (mapped into the gate's own result):
      0 -> PASS      1 -> FAIL      2 -> HARNESS ERROR      (ab-verify also: 3 -> FAIL, console/stderr diff)

.PARAMETER Nets
    Subset to run, any of: diag play console roundtrip ab  (default: all). E.g. -Nets play,diag

.PARAMETER SkipCorpus
    For a machine without the private corpus. Every corpus-dependent net (all of
    them) is reported SKIPPED loudly and the gate runs REDUCED — a skipped net
    never looks like a pass. Use it to exercise the plumbing, not to gate.

.PARAMETER AllowStaleExe
    Passthrough escape hatch: skip each net's "exe older than src/" freshness guard
    (maps to -AllowStaleExe / ab-verify's -AllowStaleNewExe). Only correct when you
    KNOW the change under test cannot affect that net's binary — e.g. a pure
    src/play edit leaves caesar.exe and caesar-roundtrip.exe genuinely unbuilt, yet
    their over-broad freshness guard (it watches all of src/) still trips.

.EXAMPLE
    tools\gate\gate.ps1                          # full gate before committing a player change
.EXAMPLE
    tools\gate\gate.ps1 -Nets play,console       # just the two player nets
.EXAMPLE
    tools\gate\gate.ps1 -SkipCorpus              # plumbing check on a corpus-less machine (REDUCED)
.EXAMPLE
    tools\gate\gate.ps1 -ListNets                # print the net table and exit

.NOTES
    Gate exit codes (orchestrator, not a net):
      0  FULL PASS  — every selected net ran and passed; manifest fully verified
      1  FAILED     — at least one net reported a regression (diff / out-of-tolerance / mismatch)
      2  ERROR      — at least one net could not verify (harness error / missing binary)
      3  REDUCED    — nets were skipped or degraded (never a full pass), nothing failed/errored
#>
[CmdletBinding()]
param(
    [string[]] $Nets,
    [switch]   $SkipCorpus,
    [string]   $CorpusRoot = 'E:\legoj\Documents\3DSWii Dumps\Dumps',
    [string]   $BuildDir,
    [switch]   $AllowStaleExe,
    [string]   $AbBaseline,
    [switch]   $ListNets
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# A net exiting non-zero is DATA, not an error: without this, PowerShell 7.4's
# default ($PSNativeCommandUseErrorActionPreference = $true) turns every net that
# exits 1/2/3 into a throw under EAP=Stop, the trap fires, and a real FAIL/ERROR
# would be relabelled a gate harness error. Off, so the child's exit code is read.
$PSNativeCommandUseErrorActionPreference = $false

$GateVersion = 'gate/1'

$ExitFullPass = 0
$ExitFailed   = 1
$ExitError    = 2
$ExitReduced  = 3

# The exact build incantation (from the caesar-build-env memory: VS 18 / 2026, NOT
# the also-installed VS 2022 — never derive the path from scratch).
$BuildCmd = 'cmd /c ''"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cmake --build build --config Release'''

# ---------------------------------------------------------------------------
# Plumbing (mirrors the ab-verify / play-goldens / diag-goldens discipline)
# ---------------------------------------------------------------------------
function Write-GateHeading { param([string] $Text) Write-Host ''; Write-Host "== $Text" -ForegroundColor Cyan }
function Write-GateInfo    { param([string] $Text) Write-Host "   $Text" -ForegroundColor DarkGray }
function Write-GateWarn    { param([string] $Text) Write-Host "   ! $Text" -ForegroundColor Yellow }

# Every fatal path funnels through here so a gate failure can never be read as a pass.
function Stop-GateHarness { param([string] $Text)
    Write-Host ''
    Write-Host "GATE HARNESS ERROR: $Text" -ForegroundColor Red
    Write-Host 'Nothing was gated. Do NOT read this as a pass.' -ForegroundColor Red
    exit $ExitError
}
# ...and every path we did not anticipate, so an unhandled error is never a silent 0.
trap { Stop-GateHarness "unhandled error: $($_.Exception.Message)`n$($_.ScriptStackTrace)" }

# THE 'man' GUARD (see ab-verify): a helper that resolves to a builtin alias instead
# of our function turns its guard into a no-op. Assert every function this script
# defines resolves to OUR function — including Stop-GateHarness, the fatal funnel.
function Assert-GateNoShadowedHelpers { param([string] $ScriptPath)
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref] $null, [ref] $null)
    $defined = @($ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) |
                 ForEach-Object { $_.Name } | Sort-Object -Unique)
    if ($defined.Count -lt 6) {
        Write-Host "GATE HARNESS ERROR: could not enumerate this script's own functions (found $($defined.Count))." -ForegroundColor Red
        exit 2
    }
    foreach ($name in $defined) {
        $resolved = @(Get-Command -Name $name -All -ErrorAction SilentlyContinue)
        if ($resolved.Count -eq 0 -or $resolved[0].CommandType -ne 'Function') {
            $what = if ($resolved.Count -eq 0) { 'nothing' } else { "a $($resolved[0].CommandType)" }
            Write-Host ''
            Write-Host "GATE HARNESS ERROR: helper '$name' resolves to $what, not our function. Rename it." -ForegroundColor Red
            exit 2
        }
    }
}

function Get-GateFileHash { param([string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

# ---------------------------------------------------------------------------
# The net registry — cheapest first. ManifestNet ties a net to the golden rows
# whose failure DEGRADES it; StaleFlag is that net's freshness escape hatch.
# ---------------------------------------------------------------------------
function Get-GateNetRegistry {
    @(
        [pscustomobject]@{ Key='diag';      Title='diag-goldens';      ScriptRel='diag-goldens\diag-goldens.ps1';           Exe='caesar.exe';            StaleFlag='-AllowStaleExe';     ManifestNet='diag-goldens';      Approx='~30 s'  }
        [pscustomobject]@{ Key='play';      Title='play-goldens';      ScriptRel='play-goldens\play-goldens.ps1';           Exe='caesar-play.exe';       StaleFlag='-AllowStaleExe';     ManifestNet='play-goldens';      Approx='~1-2 min' }
        [pscustomobject]@{ Key='console';   Title='console-tolerance'; ScriptRel='console-tolerance\console-tolerance.ps1'; Exe='caesar-play.exe';       StaleFlag='-AllowStaleExe';     ManifestNet='console-tolerance'; Approx='~1 min'  }
        [pscustomobject]@{ Key='roundtrip'; Title='roundtrip-verify';  ScriptRel='roundtrip-verify\roundtrip-verify.ps1';   Exe='caesar-roundtrip.exe';  StaleFlag='-AllowStaleExe';     ManifestNet='roundtrip-verify';  Approx='~1-3 min' }
        [pscustomobject]@{ Key='ab';        Title='ab-verify';         ScriptRel='ab-verify\ab-verify.ps1';                 Exe='caesar.exe';            StaleFlag='-AllowStaleNewExe';  ManifestNet='ab-verify';         Approx='~3-5 min (warm)' }
    )
}

# Map a net's raw child exit code to a gate result. ab-verify has a FOURTH code
# (3 = output byte-identical but console/stderr changed) that the roadmap's
# "shared 0/1/2 contract" omits — a converter console change during a player-change
# gate is a real signal, so 3 is a FAIL here (surfaced distinctly).
function ConvertTo-GateResult { param([string] $Key, [int] $Code)
    if ($Key -eq 'ab') {
        switch ($Code) {
            0       { return @{ Result='PASS';          Detail='byte-identical + console-identical' } }
            1       { return @{ Result='FAIL';          Detail='output FILE differences' } }
            3       { return @{ Result='FAIL';          Detail='console/stderr changed (ab exit 3)' } }
            2       { return @{ Result='HARNESS ERROR'; Detail='nothing verified (ab exit 2)' } }
            default { return @{ Result='HARNESS ERROR'; Detail="unexpected ab exit $Code" } }
        }
    }
    switch ($Code) {
        0       { return @{ Result='PASS';          Detail='' } }
        1       { return @{ Result='FAIL';          Detail='diff / out-of-tolerance / mismatch' } }
        2       { return @{ Result='HARNESS ERROR'; Detail='nothing verified (exit 2)' } }
        default { return @{ Result='HARNESS ERROR'; Detail="unexpected exit $Code" } }
    }
}

# ---------------------------------------------------------------------------
# Manifest verification (runs BEFORE any net)
# ---------------------------------------------------------------------------
# Returns @{ DegradedNets = <hashset of ManifestNet names>; ... } and prints a
# loud summary. A gates=true row that is missing/mismatched degrades its net.
function Invoke-GateManifestCheck {
    param([string] $ManifestPath, [hashtable] $Roots, [switch] $SkipCorpus)

    Write-GateHeading 'Goldens manifest'
    if (-not (Test-Path -LiteralPath $ManifestPath)) {
        Stop-GateHarness "goldens manifest not found: $ManifestPath (it is checked in; the tree is broken)."
    }
    try { $m = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json }
    catch { Stop-GateHarness "goldens manifest is not valid JSON: $($_.Exception.Message)" }
    if (-not $m.PSObject.Properties['goldens']) { Stop-GateHarness 'goldens manifest has no "goldens" array.' }

    $degraded = [System.Collections.Generic.HashSet[string]]::new()
    $nOk = 0; $nNever = 0; $nObsolete = 0; $nInfo = 0; $nMissing = 0; $nMismatch = 0
    $problems = [System.Collections.Generic.List[string]]::new()
    $gaps     = [System.Collections.Generic.List[string]]::new()

    foreach ($g in $m.goldens) {
        $enforce = [string] $g.enforce
        $gates   = [bool]   $g.gates

        if ($g.status -eq 'never-recorded') { $nNever++; $gaps.Add("$($g.id)  <- $($g.rel)"); continue }
        # Obsolete rows are on record for provenance only; their calibration is
        # subsumed by another (present) row. Not a gap, not enforced.
        if ($g.status -eq 'obsolete-superseded') { $nObsolete++; continue }
        if ($enforce -ne 'sha') { $nInfo++; continue }

        # Every enforce=sha golden in this manifest is a gitignored corpus
        # derivative (see the manifest description) — CorpusRoot dumps AND the
        # RepoRoot capture WAVs alike are absent on a corpus-less clone. Under
        # -SkipCorpus none of them can be checked, so skip them all (not just
        # CorpusRoot), else the RepoRoot rows print spurious MISSING on the very
        # machine -SkipCorpus is for.
        if ($SkipCorpus) { $nInfo++; continue }

        $rootPath = $Roots[[string] $g.root]
        if (-not $rootPath) { Stop-GateHarness "manifest row '$($g.id)' has unknown root '$($g.root)'." }
        $path = Join-Path $rootPath (($g.rel) -replace '/', '\')

        if (-not (Test-Path -LiteralPath $path)) {
            $nMissing++
            $problems.Add("MISSING   $($g.id)  ($path)")
            if ($gates) { [void] $degraded.Add([string] $g.net) }
            continue
        }
        $have = Get-GateFileHash -Path $path
        $want = ([string] $g.sha256).ToUpperInvariant()
        if ($have -ne $want) {
            $nMismatch++
            $problems.Add("MISMATCH  $($g.id)  (have $($have.Substring(0,12)).. want $($want.Substring(0,12))..)")
            if ($gates) { [void] $degraded.Add([string] $g.net) }
        }
        else { $nOk++ }
    }

    if ($SkipCorpus) {
        Write-GateWarn 'manifest integrity for corpus-resident goldens SKIPPED (-SkipCorpus).'
    }
    Write-GateInfo ("sha-verified OK: {0}   never-recorded: {1}   obsolete: {2}   informational: {3}   MISSING: {4}   MISMATCH: {5}" -f $nOk, $nNever, $nObsolete, $nInfo, $nMissing, $nMismatch)

    if ($gaps.Count -gt 0) {
        Write-GateInfo 'known capture gaps (never recorded, NEEDS CONSOLE — tracked, not a failure):'
        foreach ($x in $gaps) { Write-GateInfo "   $x" }
    }
    if ($problems.Count -gt 0) {
        Write-Host ''
        Write-Host '   GOLDEN INTEGRITY PROBLEMS (a gate-critical golden moved out from under its net):' -ForegroundColor Yellow
        foreach ($p in $problems) { Write-Host "     $p" -ForegroundColor Yellow }
        if ($degraded.Count -gt 0) {
            Write-Host ("   -> DEGRADED net(s): {0}. The gate cannot claim a full pass." -f ($degraded -join ', ')) -ForegroundColor Yellow
        }
    }
    elseif ($SkipCorpus) {
        # Nothing was hash-checked (all sha goldens are corpus-derived and were
        # skipped); the yellow SKIPPED note above already said so. Do NOT print a
        # green all-clear — it would claim an integrity check that did not run.
        Write-GateInfo '   (no gate-critical golden hash-checked under -SkipCorpus)'
    }
    else {
        Write-Host '   all gate-critical goldens present and hash-matched.' -ForegroundColor Green
    }

    return [pscustomobject]@{ DegradedNets = $degraded }
}

# ---------------------------------------------------------------------------
# Running one net as an isolated child pwsh process
# ---------------------------------------------------------------------------
function Invoke-GateNet {
    param(
        [object] $Net, [string] $RepoRoot, [string] $BuildDir, [string] $PwshExe,
        [string] $CorpusRoot, [switch] $AllowStaleExe, [string] $AbBaseline
    )

    Write-GateHeading "NET: $($Net.Title)   [$($Net.Approx)]"

    $exePath = Join-Path $BuildDir $Net.Exe
    if (-not (Test-Path -LiteralPath $exePath)) {
        Write-Host "   BLOCKED: required binary not built: $exePath" -ForegroundColor Red
        Write-Host '   Build it first (VS 18 / 2026 toolchain — NOT VS 2022):' -ForegroundColor Red
        Write-Host "     $BuildCmd" -ForegroundColor DarkGray
        Write-Host '   (This gate never builds. Rebuild, then re-run.)' -ForegroundColor DarkGray
        return [pscustomobject]@{ Result='BLOCKED'; Detail="missing $($Net.Exe)"; Seconds=0.0; Code=$null }
    }

    $scriptPath = Join-Path (Join-Path $RepoRoot 'tools') $Net.ScriptRel
    if (-not (Test-Path -LiteralPath $scriptPath)) {
        return [pscustomobject]@{ Result='BLOCKED'; Detail="missing net script $($Net.ScriptRel)"; Seconds=0.0; Code=$null }
    }

    $argv = @('-CorpusRoot', $CorpusRoot)
    if ($AllowStaleExe)                     { $argv += $Net.StaleFlag }
    if ($Net.Key -eq 'ab' -and $AbBaseline) { $argv += @('-Baseline', $AbBaseline) }

    Write-GateInfo ("run: pwsh -File {0} {1}" -f $Net.ScriptRel, ($argv -join ' '))

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    # Out-Host, not a bare call: `& pwsh ...` emits the child's stdout into THIS
    # function's output pipeline, which would collide with the pscustomobject we
    # return (making $r an array whose .Result is missing). Out-Host streams the
    # child's output straight to the console and keeps it out of our return value.
    & $PwshExe -NoProfile -NoLogo -File $scriptPath @argv | Out-Host
    $code = $LASTEXITCODE
    $sw.Stop()

    $mapped = ConvertTo-GateResult -Key $Net.Key -Code $code
    return [pscustomobject]@{ Result=$mapped.Result; Detail=$mapped.Detail; Seconds=[math]::Round($sw.Elapsed.TotalSeconds, 1); Code=$code }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
Assert-GateNoShadowedHelpers -ScriptPath $PSCommandPath

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).ProviderPath
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'src\Caesar.cpp'))) {
    Stop-GateHarness "this script expects to live at <repo>\tools\gate\; got repo root '$repoRoot'."
}
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build\Release' }
$manifestPath = Join-Path $PSScriptRoot 'goldens-manifest.json'

$registry = Get-GateNetRegistry

if ($ListNets) {
    Write-GateHeading 'gate nets (cheapest first)'
    Write-Host ("   {0,-10} {1,-20} {2,-22} {3}" -f 'key', 'net', 'binary', 'approx') -ForegroundColor Gray
    foreach ($n in $registry) {
        Write-Host ("   {0,-10} {1,-20} {2,-22} {3}" -f $n.Key, $n.Title, $n.Exe, $n.Approx) -ForegroundColor DarkGray
    }
    exit $ExitFullPass
}

# Normalise -Nets: split any comma-joined element (pwsh -File passes "play,diag" as
# ONE string; an in-session call would have split it) and drop blanks.
$Nets = @($Nets | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

# Resolve the selected nets (preserve registry order).
$allKeys = $registry.Key
$selKeys = if (-not $Nets -or ($Nets.Count -eq 1 -and $Nets[0] -eq 'all')) { $allKeys }
          else {
              $bad = @($Nets | Where-Object { $_ -notin $allKeys })
              if ($bad.Count -gt 0) { Stop-GateHarness "unknown net(s): $($bad -join ', '). Valid: $($allKeys -join ', ') (or 'all')." }
              @($registry | Where-Object { $_.Key -in $Nets }).Key
          }
$selected = @($registry | Where-Object { $_.Key -in $selKeys })

$pwshExe = (Get-Process -Id $PID).Path
if (-not $pwshExe) { $pwshExe = 'pwsh' }

Write-GateHeading 'caesar regression gate'
Write-GateInfo "repo    : $repoRoot"
Write-GateInfo "build   : $BuildDir"
Write-GateInfo "corpus  : $CorpusRoot$(if ($SkipCorpus) { '   (SkipCorpus: corpus-dependent nets will be SKIPPED)' })"
Write-GateInfo "nets    : $($selKeys -join ', ')"
if ($AllowStaleExe) { Write-GateWarn 'AllowStaleExe: each net''s exe-freshness guard is bypassed (only correct if the change cannot affect that binary).' }

# --- manifest first ---------------------------------------------------------
$roots = @{ CorpusRoot = $CorpusRoot; RepoRoot = $repoRoot; LocalAppData = $env:LOCALAPPDATA }
$mfCheck = Invoke-GateManifestCheck -ManifestPath $manifestPath -Roots $roots -SkipCorpus:$SkipCorpus

# --- run the nets -----------------------------------------------------------
$rows = [System.Collections.Generic.List[object]]::new()
foreach ($net in $selected) {
    if ($SkipCorpus) {
        Write-GateHeading "NET: $($net.Title)"
        Write-GateWarn 'SKIPPED (-SkipCorpus): this net reads the private corpus.'
        $rows.Add([pscustomobject]@{ Key=$net.Key; Title=$net.Title; Result='SKIPPED'; Detail='-SkipCorpus'; Seconds=0.0 })
        continue
    }

    $r = Invoke-GateNet -Net $net -RepoRoot $repoRoot -BuildDir $BuildDir -PwshExe $pwshExe `
                        -CorpusRoot $CorpusRoot -AllowStaleExe:$AllowStaleExe -AbBaseline $AbBaseline

    $result = $r.Result
    $detail = $r.Detail
    # A PASS on a net whose gate-critical golden went missing is not a full PASS.
    if ($result -eq 'PASS' -and $mfCheck.DegradedNets.Contains($net.ManifestNet)) {
        $result = 'DEGRADED'
        $detail = 'passed on an INCOMPLETE golden set (manifest integrity failed)'
    }
    $rows.Add([pscustomobject]@{ Key=$net.Key; Title=$net.Title; Result=$result; Detail=$detail; Seconds=$r.Seconds })
}

# --- summary table ----------------------------------------------------------
Write-GateHeading 'GATE SUMMARY'
Write-Host ("   {0,-18} {1,-14} {2,8}   {3}" -f 'net', 'result', 'seconds', 'detail') -ForegroundColor Gray
$colorFor = @{ 'PASS'='Green'; 'FAIL'='Red'; 'HARNESS ERROR'='Red'; 'BLOCKED'='Red'; 'SKIPPED'='Yellow'; 'DEGRADED'='Yellow' }
foreach ($row in $rows) {
    $c = if ($colorFor.ContainsKey($row.Result)) { $colorFor[$row.Result] } else { 'Gray' }
    Write-Host ("   {0,-18} {1,-14} {2,8:N1}   {3}" -f $row.Title, $row.Result, $row.Seconds, $row.Detail) -ForegroundColor $c
}

# --- verdict + exit ---------------------------------------------------------
$results  = @($rows | ForEach-Object { $_.Result })
$anyFail  = $results -contains 'FAIL'
$anyErr   = ($results -contains 'HARNESS ERROR') -or ($results -contains 'BLOCKED')
$anyRed   = ($results -contains 'SKIPPED') -or ($results -contains 'DEGRADED')
$ranAny   = @($results | Where-Object { $_ -in @('PASS','FAIL','HARNESS ERROR','DEGRADED') }).Count -gt 0

# A gate-critical golden can be degraded for a net that this run did NOT select:
# the selected nets genuinely passed, but the manifest was NOT fully clean, so the
# FULL PASS line must not claim it was (finding: subset-run overclaim).
$selectedManifestNets = @($selected | ForEach-Object { $_.ManifestNet })
$unselectedDegraded   = @($mfCheck.DegradedNets | Where-Object { $_ -notin $selectedManifestNets })

Write-Host ''
if ($anyFail) {
    $who = @($rows | Where-Object { $_.Result -eq 'FAIL' } | ForEach-Object { $_.Title }) -join ', '
    Write-Host "   GATE: FAILED — regression detected ($who). This change is NOT safe to commit as-is." -ForegroundColor Red
    exit $ExitFailed
}
if ($anyErr) {
    $who = @($rows | Where-Object { $_.Result -in @('HARNESS ERROR','BLOCKED') } | ForEach-Object { $_.Title }) -join ', '
    Write-Host "   GATE: ERROR — a net could not verify ($who). Not a pass; fix the blocker and re-run." -ForegroundColor Red
    exit $ExitError
}
if ($anyRed -or -not $ranAny) {
    $who = @($rows | Where-Object { $_.Result -in @('SKIPPED','DEGRADED') } | ForEach-Object { "$($_.Title):$($_.Result)" }) -join ', '
    Write-Host "   GATE: REDUCED — ran at reduced coverage ($who). Not a full pass." -ForegroundColor Yellow
    exit $ExitReduced
}
if ($unselectedDegraded.Count -gt 0) {
    Write-Host ("   GATE: FULL PASS — all {0} selected net(s) passed and their goldens verified. NOTE: unselected net(s) have a gate-critical golden integrity problem ({1}); the manifest is NOT fully clean — run those nets to clear it." -f $rows.Count, ($unselectedDegraded -join ', ')) -ForegroundColor Green
} else {
    Write-Host ("   GATE: FULL PASS — all {0} selected net(s) passed; goldens manifest fully verified." -f $rows.Count) -ForegroundColor Green
}
exit $ExitFullPass
