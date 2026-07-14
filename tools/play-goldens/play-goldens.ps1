#Requires -Version 7.0
<#
.SYNOPSIS
    Byte-pins caesar-play's RENDERED AUDIO (the suite stage-2 dry player) so the
    player's output — which the corpus A/B and diag-goldens cannot see — has a
    deterministic golden net from its very first .wav.

.DESCRIPTION
    ab-verify and diag-goldens guard the shipped CONVERTER (SF2/MIDI/.log/stderr).
    Neither can see caesar-play: it is a separate exe rendering PCM audio. This
    harness pins the sha256 of the rendered WAV bytes for a fixed set of render
    invocations against real corpus archives, and enforces the property that makes
    a golden trustworthy at all: DETERMINISM. Every render is run TWICE and must be
    byte-identical both times (in capture AND compare); a run-to-run difference is a
    harness error (exit 2), never a pass — the same twice-run discipline diag-goldens
    applies to its heap-sensitive -w surfaces.

    Nothing derived from corpus bytes is committed: goldens live under
    %LOCALAPPDATA%\caesar-play-goldens, exactly like ab-verify's / diag-goldens'
    caches. Only this script and its README are in the repo. Source archives are
    verified by sha before each run, so a golden can never silently drift onto a
    different archive.

    Two modes:
      -Capture   render with the CURRENT exe and store the WAV shas as goldens
                 (refuses to overwrite an existing set unless -Force).
      (default)  re-render with the CURRENT exe and byte-compare every WAV sha
                 against the goldens.

.EXAMPLE
    tools\play-goldens\play-goldens.ps1 -Capture      # first time / deliberate audio change
.EXAMPLE
    tools\play-goldens\play-goldens.ps1               # prove renders are byte-identical to goldens
.EXAMPLE
    tools\play-goldens\play-goldens.ps1 -SelfTest     # prove the harness detects a planted flip

.NOTES
    Exit codes (same contract as ab-verify / diag-goldens):
      0  every rendered WAV byte-identical to the goldens
      1  one or more renders differ
      2  harness error (nothing verified — never treat as a pass)
#>
[CmdletBinding()]
param(
    [switch] $Capture,
    [switch] $Force,
    [switch] $SelfTest,
    [string] $Exe,
    [string] $CorpusRoot = 'E:\legoj\Documents\3DSWii Dumps\Dumps',
    [string] $WorkDir,
    [switch] $AllowStaleExe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Bump when the manifest format or the exit-code contract changes.
$HarnessVersion = 'play-goldens/1'

$ExitIdentical    = 0
$ExitDiffs        = 1
$ExitHarnessError = 2

# ---------------------------------------------------------------------------
# Plumbing (mirrors ab-verify / diag-goldens discipline)
# ---------------------------------------------------------------------------

function Write-PgHeading { param([string] $Text)
    Write-Host ''
    Write-Host "== $Text" -ForegroundColor Cyan
}
function Write-PgInfo { param([string] $Text) Write-Host "   $Text" -ForegroundColor DarkGray }
function Write-PgWarn { param([string] $Text) Write-Host "   ! $Text" -ForegroundColor Yellow }

# Every fatal path funnels through here so a harness failure can never be read as
# "no differences".
function Stop-PgHarness { param([string] $Text)
    Write-Host ''
    Write-Host "HARNESS ERROR: $Text" -ForegroundColor Red
    Write-Host 'Nothing was verified. Do NOT read this as a pass.' -ForegroundColor Red
    exit $ExitHarnessError
}

# ...and so does every path we did NOT anticipate. Without this an unhandled
# terminating error would exit 1, which this script's contract defines as
# "renders differ" — filing a run that verified nothing as a real regression.
trap { Stop-PgHarness "unhandled error: $($_.Exception.Message)`n$($_.ScriptStackTrace)" }

function Get-PgFileHash { param([string] $Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.FileStream]::new($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                          [System.IO.FileShare]::Read, 1048576, [System.IO.FileOptions]::SequentialScan)
        try { return [System.Convert]::ToHexString($sha.ComputeHash($fs)) }
        finally { $fs.Dispose() }
    }
    finally { $sha.Dispose() }
}

# Write-then-rename: an interrupted in-place write can leave a truncated golden
# that is then trusted forever. File::Move(overwrite) is atomic on NTFS.
function Set-PgFileAtomic { param([string] $Path, [string] $Text)
    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Path)) | Out-Null
    $tmp = "$Path.tmp"
    [System.IO.File]::WriteAllText($tmp, $Text, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::Move($tmp, $Path, $true)
}

function Get-PgText { param([string] $Path)
    if (-not [System.IO.File]::Exists($Path)) { return $null }
    return [System.IO.File]::ReadAllText($Path)
}

# THE 'man' GUARD (see ab-verify): a helper that silently resolves to a built-in
# alias/cmdlet instead of our function turns its guard into a no-op. Assert every
# function this script defines resolves to OUR function — including Stop-PgHarness,
# which every fatal path funnels through.
function Assert-PgNoShadowedHelpers { param([string] $ScriptPath)
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref] $null, [ref] $null)
    $defined = @($ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) |
                 ForEach-Object { $_.Name } | Sort-Object -Unique)
    if ($defined.Count -lt 8) {
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

function Remove-PgDir { param([string] $Path)
    if ([System.IO.Directory]::Exists($Path)) {
        try { [System.IO.Directory]::Delete("\\?\$Path", $true) }
        catch { [System.IO.Directory]::Delete($Path, $true) }
    }
}

# ---------------------------------------------------------------------------
# Source archives + render invocations (the single source of truth)
# ---------------------------------------------------------------------------
#
# Source archives — picked for these exact properties:
#   caravel    F-Zero — smallest Csar DIRECT-path archive with real banks/seqs/
#              wave-archives; the C2 single-note DSP proof renders from its bank.
#   meetsound  MiiPlaza MeetSound — a direct-path archive carrying real BGM (the
#              C3 first-audible proof: BGM_MAIN_Mii_Only_One, the Net-B
#              discriminating track) and the MeetSound SE set (the caesar_AB
#              FluidSynth comparison targets). Its sequences convert direct off the
#              Csar, so the loader reaches them without group handling.
#   torte      Kirby: Planet Robobot — a direct-path archive carrying the Phase III
#              feature repros: a clean single long `_t` volume fade (the C7 ramp
#              witness) and the tied no-rest WarpstarUp sweeps (the C8 tie repro).
#   sounddata1 WarioWare Gold SoundData1 — a pitch-vibrato mini-game tune (the C9
#              LFO witness) and the mid-sequence bank-switch repro (0xB6, 4,585
#              selects; the C10 bank witness, where dropped notes resolve once the
#              switch loads the target bank).
$SourceArchives = @(
    @{ Key = 'caravel';    Rel = 'F-Zero\caravel.bcsar' }
    @{ Key = 'meetsound';  Rel = 'MiiPlaza\sound\MeetSound.bcsar' }
    @{ Key = 'torte';      Rel = 'Kirby Planet Robobot\snd\Torte.bcsar' }
    @{ Key = 'sounddata1'; Rel = 'wariowareG\Sound\SoundData1.bcsar' }
)

# Every render invocation, in a fixed order. Fields:
#   Name        golden id (and the WAV basename)
#   Mode        --render-note (C2 DSP proof) or --render (C3 sequencer)
#   Source      source-archive key
#   Seq         --seq value (name or index)
#   Rate        --rate
#   MaxSeconds  --max-seconds (render mode only; caps a looping sequence)
#   ExpectExit  required process exit code
$Invocations = @(
    # C2 — the single-voice DSP proof (one hard-coded note at its root key).
    @{ Name = 'note-caravel'; Mode = '--render-note'; Source = 'caravel'; Seq = 'SE_CTR_COMMON_OK'; Rate = 48000; ExpectExit = 0 }

    # C3 — the first audible sequence render. BGM_DEN_RESULT ends naturally (a
    # whole-song loop-back ends the track after one body), sets a real tempo
    # (140 BPM) and runs 10 concurrent tracks, so its ~29 s render is finite and
    # deterministic. SE_SQUARE_CONGRATULATION is a tiny 2-note SE whose duration
    # matches its FluidSynth render in caesar_AB_MiiPlaza (the structural check).
    @{ Name = 'bgm-den-result'; Mode = '--render'; Source = 'meetsound'; Seq = 'BGM_DEN_RESULT';           Rate = 48000; MaxSeconds = 120; ExpectExit = 0 }
    @{ Name = 'se-square';      Mode = '--render'; Source = 'meetsound'; Seq = 'SE_SQUARE_CONGRATULATION'; Rate = 48000; MaxSeconds = 30;  ExpectExit = 0 }

    # C7 — the `_t` ramp witness. SE_BossVo_KaenAttack carries a single ~10 s (1000-
    # tick) 0xC1 volume `_t` fade over a sustained voice, so its rendered RMS envelope
    # glides smoothly to silence — a fade that did NOT exist at C6 (volume latched at
    # note-on). The reusable timeline-flattener drives it.
    @{ Name = 'ramp-kaen';      Mode = '--render'; Source = 'torte';     Seq = 'SE_BossVo_KaenAttack';     Rate = 48000; MaxSeconds = 30;  ExpectExit = 0 }

    # C8 — the tie witness. SE_Map_WarpstarUp2 is a no-rest tied sweep: ~246 tied
    # notes that at C7 each re-attacked and stole a voice (peak 24/24, ~222 steals)
    # now collapse into 3 CONTINUOUS voices whose pitch retunes 243x with no
    # re-attack and zero steals — the both-edges-release tie semantics.
    @{ Name = 'tie-warpstar';   Mode = '--render'; Source = 'torte';     Seq = 'SE_Map_WarpstarUp2';       Rate = 48000; MaxSeconds = 15;  ExpectExit = 0 }

    # C9 — the LFO witness. SEQ_M_ZELDA1 drives the persistent track LFO (depth 0xCA
    # / rate 0xCB / delay 0xE0) on its melody, so the render carries periodic pitch
    # modulation (controlled proof: rate linear in the commanded value at 5/64 Hz per
    # unit, peak deviation = depth x range cents; targets 0/1/2 route pitch/vol/pan).
    @{ Name = 'vibrato-zelda';  Mode = '--render'; Source = 'sounddata1'; Seq = 'SEQ_M_ZELDA1';            Rate = 48000; MaxSeconds = 20;  ExpectExit = 0 }

    # C10 — the mid-sequence bank-switch witness. SEQ_M_BLACKBOARD1 fires 5 0xB6
    # switches across 9 tracks and, with the switches honoured, resolves all 122
    # notes (0 dropped) against the correct target banks -- where without them notes
    # drop (SEQ_M_ZUKAN_TEST_NG1: 12/12 dropped -> 0 once the switch loads its bank).
    @{ Name = 'bank-blackboard'; Mode = '--render'; Source = 'sounddata1'; Seq = 'SEQ_M_BLACKBOARD1';       Rate = 48000; MaxSeconds = 20;  ExpectExit = 0 }
)

# ---------------------------------------------------------------------------
# Running a render
# ---------------------------------------------------------------------------

function Invoke-PgProcess { param([string] $Exe, [string[]] $Argv)
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Exe
    foreach ($a in $Argv) { $psi.ArgumentList.Add($a) }
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow  = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $so = $proc.StandardOutput.ReadToEndAsync()
    $se = $proc.StandardError.ReadToEndAsync()
    $proc.WaitForExit()
    $r = [pscustomobject]@{ Exit = $proc.ExitCode; Stdout = $so.GetAwaiter().GetResult(); Stderr = $se.GetAwaiter().GetResult() }
    $proc.Dispose()
    return $r
}

# Render one invocation ONCE, returning @{ Exit; Sha } for the produced WAV.
function Invoke-PgRenderOnce { param([object] $Inv, [hashtable] $SourcePaths, [string] $Exe, [string] $OutBase)
    $src = $SourcePaths[$Inv.Source]
    if (-not $src) { Stop-PgHarness "invocation '$($Inv.Name)' references unknown source '$($Inv.Source)'." }

    $wav = Join-Path $OutBase ($Inv.Name + '.wav')
    if ([System.IO.File]::Exists($wav)) { [System.IO.File]::Delete($wav) }

    $argv = @($Inv.Mode, $src, '--seq', [string]$Inv.Seq, '--out', $wav, '--rate', [string]$Inv.Rate)
    if ($Inv.Mode -eq '--render' -and $Inv.ContainsKey('MaxSeconds')) {
        $argv += @('--max-seconds', [string]$Inv.MaxSeconds)
    }

    $r = Invoke-PgProcess -Exe $Exe -Argv $argv

    $sha = if ([System.IO.File]::Exists($wav)) { Get-PgFileHash -Path $wav } else { '' }
    return [pscustomobject]@{ Exit = $r.Exit; Sha = $sha; Stderr = $r.Stderr }
}

# Render an invocation TWICE, enforce byte-determinism + the expected exit, and
# return the canonical @{ Name; Exit; Sha }.
function Invoke-PgRender { param([object] $Inv, [hashtable] $SourcePaths, [string] $Exe, [string] $OutBase)
    $a = Invoke-PgRenderOnce -Inv $Inv -SourcePaths $SourcePaths -Exe $Exe -OutBase $OutBase
    $b = Invoke-PgRenderOnce -Inv $Inv -SourcePaths $SourcePaths -Exe $Exe -OutBase $OutBase

    if ($a.Exit -ne $b.Exit -or $a.Sha -ne $b.Sha) {
        Stop-PgHarness ("invocation '$($Inv.Name)' is NON-DETERMINISTIC (two renders of the same exe differ).`n" +
            '  A golden render must be byte-identical run to run; fix the non-determinism before pinning it.')
    }

    if ($Inv.ContainsKey('ExpectExit') -and $a.Exit -ne $Inv.ExpectExit) {
        Stop-PgHarness ("invocation '$($Inv.Name)' exited $($a.Exit), expected $($Inv.ExpectExit).`n" +
            "  stderr:`n$($a.Stderr)")
    }

    if ($a.Exit -eq 0 -and [string]::IsNullOrEmpty($a.Sha)) {
        Stop-PgHarness "invocation '$($Inv.Name)' exited 0 but produced no WAV."
    }

    return [pscustomobject]@{ Name = $Inv.Name; Exit = $a.Exit; Sha = $a.Sha }
}

# ---------------------------------------------------------------------------
# Source verification
# ---------------------------------------------------------------------------

function Get-PgSources { param([string] $CorpusRoot, [object] $ExpectSources)
    $paths   = @{}
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($s in $SourceArchives) {
        $path = Join-Path $CorpusRoot $s.Rel
        if (-not [System.IO.File]::Exists($path)) {
            Stop-PgHarness "source archive not found: $path`n  (is -CorpusRoot correct?)"
        }
        $sha = Get-PgFileHash -Path $path
        if ($ExpectSources) {
            $want = $ExpectSources | Where-Object { $_.key -eq $s.Key } | Select-Object -First 1
            if (-not $want) { Stop-PgHarness "manifest has no source '$($s.Key)'; goldens are from a different set." }
            if ($want.sha -ne $sha) {
                Stop-PgHarness ("source '$($s.Key)' changed since capture (sha $($sha.Substring(0,12)) != golden $((([string]$want.sha).Substring(0,12)))).`n" +
                    '  The corpus archive moved out from under the goldens; recapture with -Capture -Force.')
            }
        }
        $paths[$s.Key] = $path
        $records.Add([ordered]@{ key = $s.Key; rel = $s.Rel; sha = $sha })
    }
    return [pscustomobject]@{ Paths = $paths; Records = $records }
}

# ---------------------------------------------------------------------------
# Golden IO
# ---------------------------------------------------------------------------

function Get-PgGoldenDir { param([string] $GoldenRoot, [string] $Name) Join-Path (Join-Path $GoldenRoot 'renders') $Name }

function Write-PgGolden { param([string] $GoldenRoot, [object] $Cap)
    $dir = Get-PgGoldenDir -GoldenRoot $GoldenRoot -Name $Cap.Name
    Set-PgFileAtomic -Path (Join-Path $dir 'sha.txt')  -Text $Cap.Sha
    Set-PgFileAtomic -Path (Join-Path $dir 'exit.txt') -Text ([string] $Cap.Exit)
}

function Read-PgGolden { param([string] $GoldenRoot, [string] $Name)
    $dir = Get-PgGoldenDir -GoldenRoot $GoldenRoot -Name $Name
    [pscustomobject]@{
        Name = $Name
        Sha  = Get-PgText (Join-Path $dir 'sha.txt')
        Exit = Get-PgText (Join-Path $dir 'exit.txt')
    }
}

# ---------------------------------------------------------------------------
# Capture / Compare
# ---------------------------------------------------------------------------

function Invoke-PgCaptureRun {
    param([string] $Exe, [string] $CorpusRoot, [string] $WorkDir, [string] $GoldenRoot, [switch] $Force, [string] $ExeSha, [string] $ExeVersion)

    $manifestPath = Join-Path $GoldenRoot 'manifest.json'
    if (-not $Force -and [System.IO.File]::Exists($manifestPath)) {
        Stop-PgHarness "goldens already exist at $GoldenRoot. Pass -Force to overwrite (a deliberate audio change)."
    }
    Remove-PgDir $GoldenRoot
    [System.IO.Directory]::CreateDirectory($GoldenRoot) | Out-Null

    $outBase = Join-Path $WorkDir 'out'
    Remove-PgDir $outBase; [System.IO.Directory]::CreateDirectory($outBase) | Out-Null

    $src = Get-PgSources -CorpusRoot $CorpusRoot -ExpectSources $null

    $invRecords = [System.Collections.Generic.List[object]]::new()
    foreach ($inv in $Invocations) {
        $cap = Invoke-PgRender -Inv $inv -SourcePaths $src.Paths -Exe $Exe -OutBase $outBase
        Write-PgGolden -GoldenRoot $GoldenRoot -Cap $cap
        Write-PgInfo ("captured {0,-20} exit={1} sha={2}" -f $cap.Name, $cap.Exit, $cap.Sha.Substring(0, [Math]::Min(12, $cap.Sha.Length)))
        $invRecords.Add([ordered]@{ name = $cap.Name; exit = $cap.Exit; sha = $cap.Sha })
    }

    $manifest = [ordered]@{
        harness     = $HarnessVersion
        created     = (Get-Date).ToString('o')
        exeSha      = $ExeSha
        exeVersion  = $ExeVersion
        corpusRoot  = $CorpusRoot
        sources     = $src.Records
        invocations = $invRecords
    }
    Set-PgFileAtomic -Path $manifestPath -Text ($manifest | ConvertTo-Json -Depth 8)

    Write-PgHeading 'Capture complete'
    Write-PgInfo "goldens : $GoldenRoot"
    Write-PgInfo ("pinned  : {0} render(s), {1} source archive(s)" -f $Invocations.Count, $SourceArchives.Count)
    Write-PgInfo 'These goldens are LOCAL only — never commit them (they are corpus-derived audio hashes).'
    return $ExitIdentical
}

function Invoke-PgCompareRun {
    param([string] $Exe, [string] $CorpusRoot, [string] $WorkDir, [string] $GoldenRoot)

    $manifestPath = Join-Path $GoldenRoot 'manifest.json'
    if (-not [System.IO.File]::Exists($manifestPath)) {
        Stop-PgHarness "no goldens at $GoldenRoot. Run with -Capture first."
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.harness -ne $HarnessVersion) {
        Stop-PgHarness "golden manifest is $($manifest.harness), this harness is $HarnessVersion. Recapture with -Capture -Force."
    }

    $outBase = Join-Path $WorkDir 'out'
    Remove-PgDir $outBase; [System.IO.Directory]::CreateDirectory($outBase) | Out-Null

    $src = Get-PgSources -CorpusRoot $CorpusRoot -ExpectSources $manifest.sources

    Write-PgInfo ("goldens captured from exe sha {0} ({1})" -f ([string]$manifest.exeSha).Substring(0, 12), $manifest.exeVersion)

    $diffRows = [System.Collections.Generic.List[object]]::new()
    foreach ($inv in $Invocations) {
        $cap    = Invoke-PgRender -Inv $inv -SourcePaths $src.Paths -Exe $Exe -OutBase $outBase
        $golden = Read-PgGolden -GoldenRoot $GoldenRoot -Name $inv.Name
        if ($null -eq $golden.Sha) {
            Stop-PgHarness "no golden stored for invocation '$($inv.Name)'. Recapture with -Capture -Force."
        }
        $surfaces = [System.Collections.Generic.List[string]]::new()
        if (([string]$cap.Exit) -ne ([string]$golden.Exit)) { $surfaces.Add('exit') }
        if ($cap.Sha -ne $golden.Sha) { $surfaces.Add('wav') }

        if ($surfaces.Count -gt 0) {
            $diffRows.Add([pscustomobject]@{ Name = $inv.Name; Surfaces = $surfaces; Cap = $cap; Golden = $golden })
            Write-PgWarn ("DIFF {0,-20} {1}" -f $inv.Name, ($surfaces -join ', '))
        }
        else {
            Write-PgInfo ("ok   {0,-20} exit={1} sha={2}" -f $inv.Name, $cap.Exit, $cap.Sha.Substring(0, [Math]::Min(12, $cap.Sha.Length)))
        }
    }

    Write-PgHeading 'Result'
    if ($diffRows.Count -eq 0) {
        Write-Host ("   ALL {0} render(s) byte-identical to the goldens." -f $Invocations.Count) -ForegroundColor Green
        return $ExitIdentical
    }
    Write-Host ("   {0} of {1} render(s) DIFFER:" -f $diffRows.Count, $Invocations.Count) -ForegroundColor Yellow
    foreach ($d in $diffRows) {
        Write-Host ("     {0,-20} {1}  (golden sha {2} -> got {3})" -f $d.Name, ($d.Surfaces -join ', '),
            ([string]$d.Golden.Sha).Substring(0, [Math]::Min(12, ([string]$d.Golden.Sha).Length)),
            ([string]$d.Cap.Sha).Substring(0, [Math]::Min(12, ([string]$d.Cap.Sha).Length))) -ForegroundColor Yellow
    }
    return $ExitDiffs
}

# ---------------------------------------------------------------------------
# Self-test: prove the harness detects a diff before any PASS is trusted.
# ---------------------------------------------------------------------------
function Invoke-PgSelfTest { param([string] $Exe, [string] $CorpusRoot, [string] $WorkDir)
    Write-PgHeading 'SELF-TEST'
    Write-PgInfo 'Capture -> clean compare (0) -> flip one golden sha (1) -> restore (0).'

    $stRoot = Join-Path $WorkDir 'selftest'
    Remove-PgDir $stRoot
    $stWork   = Join-Path $stRoot 'work'
    $stGolden = Join-Path $stRoot 'goldens'
    [System.IO.Directory]::CreateDirectory($stWork) | Out-Null

    $exeSha = Get-PgFileHash -Path $Exe
    $exeVer = (Invoke-PgProcess -Exe $Exe -Argv @('--version')).Stdout.Trim()

    $failures = [System.Collections.Generic.List[string]]::new()

    Invoke-PgCaptureRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $stWork -GoldenRoot $stGolden -ExeSha $exeSha -ExeVersion $exeVer | Out-Null
    $clean = Invoke-PgCompareRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $stWork -GoldenRoot $stGolden
    if ($clean -ne $ExitIdentical) { $failures.Add("clean compare of the same exe returned $clean; expected 0") }

    # Plant a byte-flip in one stored golden sha and confirm it is caught.
    $victim = ($Invocations | Select-Object -First 1).Name
    $victimFile = Join-Path (Get-PgGoldenDir -GoldenRoot $stGolden -Name $victim) 'sha.txt'
    $orig = [System.IO.File]::ReadAllText($victimFile)
    $flipped = if ($orig.Substring(0,1) -eq '0') { '1' + $orig.Substring(1) } else { '0' + $orig.Substring(1) }
    [System.IO.File]::WriteAllText($victimFile, $flipped)
    $dirty = Invoke-PgCompareRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $stWork -GoldenRoot $stGolden
    if ($dirty -ne $ExitDiffs) { $failures.Add("planted-flip compare returned $dirty; expected 1 (a detected diff)") }
    [System.IO.File]::WriteAllText($victimFile, $orig)  # restore

    $restored = Invoke-PgCompareRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $stWork -GoldenRoot $stGolden
    if ($restored -ne $ExitIdentical) { $failures.Add("restored compare returned $restored; expected 0") }

    Remove-PgDir $stRoot

    Write-Host ''
    if ($failures.Count -gt 0) {
        foreach ($f in $failures) { Write-Host "   FAIL: $f" -ForegroundColor Red }
        Write-Host '   SELF-TEST FAILED — do not trust this harness until it is fixed.' -ForegroundColor Red
        return $ExitHarnessError
    }
    Write-Host '   SELF-TEST PASSED: a clean compare is identical; a planted golden flip is reported' -ForegroundColor Green
    Write-Host '   as a diff; and the restored set is clean again.' -ForegroundColor Green
    return $ExitIdentical
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

Assert-PgNoShadowedHelpers -ScriptPath $PSCommandPath

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).ProviderPath
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'src\play\main.cpp'))) {
    Stop-PgHarness "this script expects to live at <repo>\tools\play-goldens\; got repo root '$repoRoot'."
}

if (-not $WorkDir)   { $WorkDir = Join-Path $env:LOCALAPPDATA 'caesar-play-goldens' }
if (-not $Exe)       { $Exe = Join-Path $repoRoot 'build\Release\caesar-play.exe' }
$goldenRoot = Join-Path $WorkDir 'goldens'

if (-not (Test-Path -LiteralPath $CorpusRoot)) { Stop-PgHarness "corpus root not found: $CorpusRoot" }
if (-not (Test-Path -LiteralPath $Exe)) {
    Stop-PgHarness "exe not found: $Exe`n  Build it first:  cmake --build build --config Release  (from a VS dev prompt)"
}
$Exe = (Resolve-Path -LiteralPath $Exe).ProviderPath
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)
[System.IO.Directory]::CreateDirectory($WorkDir) | Out-Null

# A stale exe compares the change's SOURCE against an exe that never contained it —
# a false PASS. The player build is driven by src/play AND the CMake files AND
# caesar_core (the models it consumes), so watch all of src/ + the CMake files.
if (-not $Capture -and -not $SelfTest -and -not $AllowStaleExe) {
    $exeTime = [System.IO.File]::GetLastWriteTimeUtc($Exe)
    $watched = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'src') -Recurse -File -Include *.cpp, *.hpp, *.h, *.c -ErrorAction SilentlyContinue)
    foreach ($bf in @('CMakeLists.txt', 'CMakePresets.json')) {
        $bp = Join-Path $repoRoot $bf
        if (Test-Path -LiteralPath $bp) { $watched += Get-Item -LiteralPath $bp }
    }
    $newer = @($watched | Where-Object { $_.LastWriteTimeUtc -gt $exeTime })
    if ($newer.Count -gt 0) {
        Stop-PgHarness ("$($newer.Count) build input(s) are NEWER than $Exe (e.g. $($newer[0].Name)).`n" +
            "  The exe does not contain the change under test; this compare would be a false PASS.`n" +
            '  Rebuild, or pass -AllowStaleExe if you really mean it.')
    }
}

$exeSha = Get-PgFileHash -Path $Exe
$exeVer = (Invoke-PgProcess -Exe $Exe -Argv @('--version')).Stdout.Trim()

Write-PgHeading 'caesar-play render goldens'
Write-PgInfo "repo    : $repoRoot"
Write-PgInfo "exe     : $Exe"
Write-PgInfo "exe sha : $($exeSha.Substring(0,12))  ($exeVer)"
Write-PgInfo "corpus  : $CorpusRoot"
Write-PgInfo "work    : $WorkDir"

# One run at a time per work dir (the out dir is a shared path).
$lock = $null
try {
    $lock = [System.IO.File]::Open((Join-Path $WorkDir '.lock'), [System.IO.FileMode]::OpenOrCreate,
                                   [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
}
catch { Stop-PgHarness "another play-goldens run already holds the lock on $WorkDir." }

try {
    if ($SelfTest) {
        $code = Invoke-PgSelfTest -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $WorkDir
    }
    elseif ($Capture) {
        Write-PgHeading 'Capturing goldens'
        $code = Invoke-PgCaptureRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $WorkDir -GoldenRoot $goldenRoot -Force:$Force -ExeSha $exeSha -ExeVersion $exeVer
    }
    else {
        Write-PgHeading 'Comparing against goldens'
        $code = Invoke-PgCompareRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $WorkDir -GoldenRoot $goldenRoot
    }
}
finally {
    if ($lock) { $lock.Dispose() }
}

exit $code
