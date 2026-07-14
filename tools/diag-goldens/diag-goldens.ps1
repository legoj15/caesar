#Requires -Version 7.0
<#
.SYNOPSIS
    Byte-pins caesar's DIAGNOSTIC surfaces (corrupted-input stderr, CLI errors,
    -w per-item detail, multi-input .log behaviour) so the stage-0 ParseContext
    fold can be proven output-identical on the surfaces the corpus A/B never sees.

.DESCRIPTION
    tools/ab-verify already pins stderr/stdout/exit AND every output file
    (including .log) for HEALTHY, single-input, default-args runs across the whole
    .bcsar corpus. It cannot see:
      * what caesar prints when the INPUT is corrupt (asserts, enum errors,
        bounds throws, empty files);
      * CLI-error paths (no args, --version, bad --pad-sustain, missing -o arg,
        flags-only);
      * -w per-item positional detail;
      * multi-input runs, where Common::Log bleeds across archives.

    This harness pins exactly those. It builds a tiny set of DETERMINISTIC
    fixtures by mutating a couple of small corpus archives, runs the exe over a
    fixed list of invocations, normalises the captured text (CRLF->LF, and the
    work-dir root + exe path folded out -- NOTHING else; positions/hex/tabs are
    the payload being pinned), and byte-compares against stored goldens.

    Nothing derived from corpus bytes is ever committed: fixtures AND goldens live
    under %LOCALAPPDATA%\caesar-diag, exactly like ab-verify's cache. Only this
    script and its README are in the repo.

    Two modes:
      -Capture   generate fixtures + goldens from the CURRENT exe (refuses to
                 overwrite an existing golden set unless -Force).
      (default)  regenerate the fixtures, run the CURRENT exe, and byte-compare
                 every surface against the goldens.

    Determinism is enforced, not assumed. -w positional output is
    heap-layout-dependent wherever a warning's position is computed by subtracting
    two pointers from DIFFERENT allocations (this happens on any archive that
    warns during wave/bank decode -- NOT only group-resident ones; see README).
    Every -w success golden is run TWICE and must be byte-identical both times, in
    capture AND compare; a run-to-run mismatch is a harness error (exit 2), never
    a pass.

.EXAMPLE
    # First time (or after a deliberate diagnostic-output change): make goldens.
    tools\diag-goldens\diag-goldens.ps1 -Capture

.EXAMPLE
    # After the ParseContext fold rebuild: prove the surfaces are byte-identical.
    tools\diag-goldens\diag-goldens.ps1

.EXAMPLE
    # Prove the harness can actually detect a diff before trusting a PASS.
    tools\diag-goldens\diag-goldens.ps1 -SelfTest

.NOTES
    Exit codes (same contract as ab-verify):
      0  every diagnostic surface byte-identical to the goldens
      1  one or more surfaces differ
      2  harness error (nothing was verified -- never treat as a pass)
#>
[CmdletBinding()]
param(
    # Generate fixtures + goldens instead of comparing.
    [switch] $Capture,

    # Overwrite an existing golden set (-Capture only).
    [switch] $Force,

    # Prove the harness detects a diff, on an isolated throwaway golden set.
    [switch] $SelfTest,

    # The exe under test. Default: the repo's Release build.
    [string] $Exe,

    # Root of the private archive corpus (fixtures are mutated from a few of these).
    [string] $CorpusRoot = 'E:\legoj\Documents\3DSWii Dumps\Dumps',

    # Local scratch: fixtures, goldens, reports. Never committed (see README).
    [string] $WorkDir,

    # Skip the "exe is older than src/ or CMakeLists" guard (compare mode).
    [switch] $AllowStaleExe
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Bump when the manifest format or the exit-code contract changes.
$HarnessVersion = 'diag-goldens/1'

$ExitIdentical    = 0
$ExitDiffs        = 1
$ExitHarnessError = 2

# ---------------------------------------------------------------------------
# Plumbing (mirrors ab-verify's discipline)
# ---------------------------------------------------------------------------

function Write-DgHeading { param([string] $Text)
    Write-Host ''
    Write-Host "== $Text" -ForegroundColor Cyan
}
function Write-DgInfo { param([string] $Text) Write-Host "   $Text" -ForegroundColor DarkGray }
function Write-DgWarn { param([string] $Text) Write-Host "   ! $Text" -ForegroundColor Yellow }

# Every fatal path funnels through here so a harness failure can never be read as
# "no differences".
function Stop-DgHarness { param([string] $Text)
    Write-Host ''
    Write-Host "HARNESS ERROR: $Text" -ForegroundColor Red
    Write-Host 'Nothing was verified. Do NOT read this as a pass.' -ForegroundColor Red
    exit $ExitHarnessError
}

# ...and so does every path we did NOT anticipate. Without this an unhandled
# terminating error (StrictMode violation, disk full, a locked file) would exit 1,
# which this script's own contract defines as "surfaces differ" -- filing a run
# that verified nothing as a real regression.
trap { Stop-DgHarness "unhandled error: $($_.Exception.Message)`n$($_.ScriptStackTrace)" }

function Get-DgStringHash { param([string] $Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return [System.Convert]::ToHexString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text))) }
    finally { $sha.Dispose() }
}

function Get-DgBytesHash { param([byte[]] $Bytes)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return [System.Convert]::ToHexString($sha.ComputeHash($Bytes)) }
    finally { $sha.Dispose() }
}

function Get-DgFileHash { param([string] $Path)
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
function Set-DgFileAtomic { param([string] $Path, [string] $Text)
    [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($Path)) | Out-Null
    $tmp = "$Path.tmp"
    [System.IO.File]::WriteAllText($tmp, $Text, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::Move($tmp, $Path, $true)
}

function Get-DgText { param([string] $Path)
    if (-not [System.IO.File]::Exists($Path)) { return $null }
    return [System.IO.File]::ReadAllText($Path)
}

# caesar echoes the paths it reads/writes, and .log rows embed them too. Both live
# under the work dir (fixtures AND output), so fold that one root out; the exe path
# too, defensively. CRLF->LF only otherwise. Positions, hex, tabs, blank lines are
# the payload -- do not touch them. (Same rule as ab-verify's ConvertTo-AbNormalisedStream.)
function ConvertTo-DgNormalised { param([string] $Text, [string[]] $Roots)
    if ([string]::IsNullOrEmpty($Text)) { return '' }
    $out = $Text -replace "`r`n", "`n"
    foreach ($root in $Roots) {
        if ([string]::IsNullOrWhiteSpace($root)) { continue }
        foreach ($variant in @($root, $root.Replace('\', '/'))) {
            $out = [regex]::Replace($out, [regex]::Escape($variant), '<ROOT>', 'IgnoreCase')
        }
    }
    return $out
}

# THE 'man' GUARD (see ab-verify): a helper that silently resolves to a built-in
# alias/cmdlet instead of our function turns its guard into a no-op. Assert every
# function this script defines resolves to OUR function -- including Stop-DgHarness,
# which every fatal path funnels through.
function Assert-DgNoShadowedHelpers { param([string] $ScriptPath)
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($ScriptPath, [ref] $null, [ref] $null)
    $defined = @($ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) |
                 ForEach-Object { $_.Name } | Sort-Object -Unique)
    if ($defined.Count -lt 10) {
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

function Remove-DgDir { param([string] $Path)
    if ([System.IO.Directory]::Exists($Path)) {
        try { [System.IO.Directory]::Delete("\\?\$Path", $true) }
        catch { [System.IO.Directory]::Delete($Path, $true) }
    }
}

# ---------------------------------------------------------------------------
# Fixture recipes (the single source of truth)
# ---------------------------------------------------------------------------
#
# Source archives -- picked for these exact properties (empirically verified;
# see README):
#   caravel      F-Zero -- smallest Csar DIRECT-path archive with real banks/
#                seqs/wave-archives; version 0x02000000. The base for every
#                header/section mutation. (Its OWN -w output is nondeterministic,
#                so it is used only where -w is off.)
#   pksnd        pokemon red -- version != 0x02000000, so the stored-length
#                Assert (guarded out for 0x02000000) is reachable here; also a
#                clean, deterministic -w archive (metadata-only, no wave decode).
#   queenstream  Ocarina -- tiny stream-only archive; a second deterministic -w
#                archive (exercises the Csar-level external-stream warning).
$SourceArchives = @(
    @{ Key = 'caravel';     Rel = 'F-Zero\caravel.bcsar' }
    @{ Key = 'pksnd';       Rel = 'pokemon red\snd\PKSnd.bcsar' }
    @{ Key = 'queenstream'; Rel = 'Ocarina\sound\QueenStream.bcsar' }
)

# BCSAR fixed header words we need to locate mutation targets.
function Get-DgHeader { param([byte[]] $Bytes)
    if ($Bytes.Length -lt 56) { Stop-DgHarness 'source archive is too short to be a BCSAR header.' }
    [pscustomobject]@{
        Version = [BitConverter]::ToUInt32($Bytes, 8)
        StrgOff = [BitConverter]::ToUInt32($Bytes, 24)
        InfoOff = [BitConverter]::ToUInt32($Bytes, 36)
    }
}

function Set-DgDword { param([byte[]] $Bytes, [int] $Offset, [uint32] $Value)
    [Array]::Copy([BitConverter]::GetBytes($Value), 0, $Bytes, $Offset, 4)
}

# Given a loaded source archive, produce the fixture bytes deterministically.
# Each recipe targets exactly ONE diagnostic mechanism.
function Build-DgFixtureBytes { param([string] $Recipe, [byte[]] $Source)
    switch ($Recipe) {
        'verbatim'       { return ,([byte[]] $Source.Clone()) }
        'empty'          { return ,([byte[]] @()) }
        'xor-byte-0'     { $b = $Source.Clone(); $b[0] = $b[0] -bxor 0xFF; return ,([byte[]] $b) }   # Assert magic
        'xor-byte-4'     { $b = $Source.Clone(); $b[4] = $b[4] -bxor 0xFF; return ,([byte[]] $b) }   # Assert BOM
        'truncate-last'  { return ,([byte[]] $Source[0..($Source.Length - 2)]) }                     # Assert length (ver != 0x02000000)
        'info-enum-bad'  {                                                                            # Error: bad INFO offset-id
            $b = $Source.Clone(); $h = Get-DgHeader $b
            Set-DgDword $b ([int]$h.InfoOff + 8) ([uint32]0x00009999)                                 # first INFO offsetId -> out of enum
            return ,([byte[]] $b)
        }
        'strg-len-overrun' {                                                                          # CheckBounds overrun
            $b = $Source.Clone(); $h = Get-DgHeader $b
            Set-DgDword $b ([int]$h.StrgOff + 36) ([uint32]4294967040)                                # first STRG string length -> 0xFFFFFF00
            return ,([byte[]] $b)
        }
        'info-off-outside' {                                                                          # CheckBounds outside
            $b = $Source.Clone()
            Set-DgDword $b 36 ([uint32]268435456)                                                     # infoOffset -> 0x10000000 (past EOF)
            return ,([byte[]] $b)
        }
        default { Stop-DgHarness "unknown fixture recipe '$Recipe'" }
    }
}

$Fixtures = @(
    @{ Name = 'assert-magic';   Source = 'caravel';     Recipe = 'xor-byte-0' }
    @{ Name = 'assert-bom';     Source = 'caravel';     Recipe = 'xor-byte-4' }
    @{ Name = 'assert-length';  Source = 'pksnd';       Recipe = 'truncate-last' }
    @{ Name = 'error-enum';     Source = 'caravel';     Recipe = 'info-enum-bad' }
    @{ Name = 'bounds-overrun'; Source = 'caravel';     Recipe = 'strg-len-overrun' }
    @{ Name = 'bounds-outside'; Source = 'caravel';     Recipe = 'info-off-outside' }
    @{ Name = 'require-open';   Source = $null;         Recipe = 'empty' }
    @{ Name = 'caravel';        Source = 'caravel';     Recipe = 'verbatim' }
    @{ Name = 'pksnd';          Source = 'pksnd';       Recipe = 'verbatim' }
    @{ Name = 'queenstream';    Source = 'queenstream'; Recipe = 'verbatim' }
)

# The family marker each corrupted fixture MUST still emit. If a fixture stops
# firing its mechanism, that is a harness error (the golden would pin the wrong
# thing), not a pass. Checked at capture AND compare.
$FamilyMarkers = @{
    'assert'         = 'INSTEAD GOT'
    'error'          = "EXPECTED`tA valid"
    'bounds-overrun' = 'archive damaged: a read of'
    'bounds-outside' = 'a file offset points outside'
    'require-open'   = 'could not open or read file'
}

# Every invocation surface, in a fixed order. Fields:
#   Name        golden id
#   Flags       flags placed before -o (fixture invocations)
#   Fixtures    fixture names passed as inputs (an -o outDir is added)
#   RawArgv     verbatim argv (CLI-error cases); '@caravel' expands to that
#               fixture's path; NO -o is added
#   ExpectExit  required process exit code
#   Family      corrupted-fixture family (marker + no-.log asserted)
#   DetCheck    run twice; stderr/stdout/exit/logs must be byte-identical
$Invocations = @(
    # --- corrupted input, default args ---
    @{ Name = 'fix-assert-magic';   Fixtures = @('assert-magic');   Flags = @(); ExpectExit = 1; Family = 'assert' }
    @{ Name = 'fix-assert-bom';     Fixtures = @('assert-bom');     Flags = @(); ExpectExit = 1; Family = 'assert' }
    @{ Name = 'fix-assert-length';  Fixtures = @('assert-length');  Flags = @(); ExpectExit = 1; Family = 'assert' }
    @{ Name = 'fix-error-enum';     Fixtures = @('error-enum');     Flags = @(); ExpectExit = 1; Family = 'error' }
    @{ Name = 'fix-bounds-overrun'; Fixtures = @('bounds-overrun'); Flags = @(); ExpectExit = 1; Family = 'bounds-overrun' }
    @{ Name = 'fix-bounds-outside'; Fixtures = @('bounds-outside'); Flags = @(); ExpectExit = 1; Family = 'bounds-outside' }
    @{ Name = 'fix-require-open';   Fixtures = @('require-open');   Flags = @(); ExpectExit = 1; Family = 'require-open' }

    # --- corrupted input, -w (pins that -w adds nothing before an early failure) ---
    @{ Name = 'fix-assert-magic-w';   Fixtures = @('assert-magic');   Flags = @('-w'); ExpectExit = 1; Family = 'assert' }
    @{ Name = 'fix-bounds-overrun-w'; Fixtures = @('bounds-overrun'); Flags = @('-w'); ExpectExit = 1; Family = 'bounds-overrun' }

    # --- CLI-error paths ---
    @{ Name = 'cli-noargs';        RawArgv = @();                          ExpectExit = 1 }
    @{ Name = 'cli-version';       RawArgv = @('--version');               ExpectExit = 0 }
    @{ Name = 'cli-padsustain';    RawArgv = @('--pad-sustain=x', '@caravel'); ExpectExit = 1 }
    @{ Name = 'cli-missing-odir';  RawArgv = @('-o');                      ExpectExit = 1 }
    @{ Name = 'cli-noinput';       RawArgv = @('-w');                      ExpectExit = 1 }

    # --- -w success goldens (deterministic; run twice) ---
    @{ Name = 'w-queenstream'; Fixtures = @('queenstream'); Flags = @('-w'); ExpectExit = 0; DetCheck = $true }
    @{ Name = 'w-pksnd';       Fixtures = @('pksnd');       Flags = @('-w'); ExpectExit = 0; DetCheck = $true }

    # --- multi-input .log bleed (one process, two inputs; pins b.log = a's rows + b's) ---
    @{ Name = 'multi-bleed'; Fixtures = @('caravel', 'pksnd'); Flags = @(); ExpectExit = 0 }
)

# ---------------------------------------------------------------------------
# Fixture materialisation
# ---------------------------------------------------------------------------

# Verify each source archive still matches, load it once. Returns @{ key -> bytes }
# plus the per-source records for the manifest.
function Get-DgSources { param([string] $CorpusRoot, [object] $ExpectSources)
    $bytes   = @{}
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($s in $SourceArchives) {
        $path = Join-Path $CorpusRoot $s.Rel
        if (-not [System.IO.File]::Exists($path)) {
            Stop-DgHarness "source archive not found: $path`n  (needed to (re)generate fixtures; is -CorpusRoot correct?)"
        }
        $sha = Get-DgFileHash -Path $path
        if ($ExpectSources) {
            $want = $ExpectSources | Where-Object { $_.key -eq $s.Key } | Select-Object -First 1
            if (-not $want) { Stop-DgHarness "manifest has no source '$($s.Key)'; goldens are from a different fixture set." }
            if ($want.sha -ne $sha) {
                Stop-DgHarness ("source '$($s.Key)' changed since capture (sha $($sha.Substring(0,12)) != golden $((([string]$want.sha).Substring(0,12)))).`n" +
                    '  The corpus archive moved out from under the goldens; recapture with -Capture -Force.')
            }
        }
        $bytes[$s.Key] = [System.IO.File]::ReadAllBytes($path)
        $records.Add([ordered]@{ key = $s.Key; rel = $s.Rel; sha = $sha })
    }
    return [pscustomobject]@{ Bytes = $bytes; Records = $records }
}

# Regenerate every fixture file into $FixtureDir. Returns @{ name -> @{ Path; Sha } }.
function New-DgFixtures { param([string] $FixtureDir, [hashtable] $SourceBytes, [object] $ExpectFixtures)
    Remove-DgDir $FixtureDir
    [System.IO.Directory]::CreateDirectory($FixtureDir) | Out-Null
    $out = @{}
    foreach ($f in $Fixtures) {
        $src = if ($f.Source) { $SourceBytes[$f.Source] } else { [byte[]] @() }
        $data = Build-DgFixtureBytes -Recipe $f.Recipe -Source $src
        $data = [byte[]] $data
        $path = Join-Path $FixtureDir ($f.Name + '.bcsar')
        $tmp = "$path.tmp"
        [System.IO.File]::WriteAllBytes($tmp, $data)
        [System.IO.File]::Move($tmp, $path, $true)
        $sha = Get-DgBytesHash -Bytes $data
        if ($ExpectFixtures) {
            $want = $ExpectFixtures | Where-Object { $_.name -eq $f.Name } | Select-Object -First 1
            if (-not $want) { Stop-DgHarness "manifest has no fixture '$($f.Name)'; goldens are from a different fixture set." }
            if ($want.sha -ne $sha) {
                Stop-DgHarness ("regenerated fixture '$($f.Name)' does not match the golden manifest (sha mismatch).`n" +
                    '  The recipe or its source changed; recapture with -Capture -Force.')
            }
        }
        $out[$f.Name] = [pscustomobject]@{ Path = $path; Sha = $sha }
    }
    return $out
}

# ---------------------------------------------------------------------------
# Running + capturing a surface
# ---------------------------------------------------------------------------

function Invoke-DgProcess { param([string] $Exe, [string[]] $Argv)
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

# Collect every .log under $Dir into one normalised block, sorted by relpath so
# the multi-input bleed is pinned in a stable order. Empty string when none --
# which is itself the golden for the failure paths (they must produce NO .log).
function Get-DgLogBlock { param([string] $Dir, [string[]] $Roots)
    if (-not [System.IO.Directory]::Exists($Dir)) { return '' }
    $logs = @([System.IO.Directory]::EnumerateFiles($Dir, '*.log', [System.IO.SearchOption]::AllDirectories) | Sort-Object)
    if ($logs.Count -eq 0) { return '' }
    $sb = [System.Text.StringBuilder]::new()
    foreach ($log in $logs) {
        $rel = ConvertTo-DgNormalised -Text $log -Roots $Roots
        $body = ConvertTo-DgNormalised -Text ([System.IO.File]::ReadAllText($log)) -Roots $Roots
        [void] $sb.AppendLine("== $rel ==")
        [void] $sb.Append($body)
        if ($body.Length -gt 0 -and -not $body.EndsWith("`n")) { [void] $sb.AppendLine() }
    }
    return $sb.ToString()
}

# Build the argv for one invocation and return @{ Argv; OutDir(optional) }.
function Resolve-DgArgv { param([object] $Inv, [hashtable] $FixMap, [string] $OutBase )
    if ($Inv.ContainsKey('RawArgv')) {
        $argv = foreach ($a in $Inv.RawArgv) {
            if ($a -like '@*') {
                $key = $a.Substring(1)
                if (-not $FixMap.ContainsKey($key)) { Stop-DgHarness "invocation '$($Inv.Name)' references unknown fixture '$key'." }
                $FixMap[$key].Path
            } else { $a }
        }
        return [pscustomobject]@{ Argv = [string[]] @($argv); OutDir = $null }
    }
    $outDir = Join-Path $OutBase $Inv.Name
    $inputs = foreach ($fn in $Inv.Fixtures) {
        if (-not $FixMap.ContainsKey($fn)) { Stop-DgHarness "invocation '$($Inv.Name)' references unknown fixture '$fn'." }
        $FixMap[$fn].Path
    }
    $flags = if ($Inv.ContainsKey('Flags')) { @($Inv.Flags) } else { @() }
    $argv = @($flags) + @('-o', $outDir) + @($inputs)
    return [pscustomobject]@{ Argv = [string[]] $argv; OutDir = $outDir }
}

# Run one invocation once and return the normalised capture.
function Invoke-DgCaptureOnce { param([object] $Inv, [hashtable] $FixMap, [string] $Exe, [string] $OutBase, [string[]] $Roots)
    $res = Resolve-DgArgv -Inv $Inv -FixMap $FixMap -OutBase $OutBase
    if ($res.OutDir) { Remove-DgDir $res.OutDir; [System.IO.Directory]::CreateDirectory($res.OutDir) | Out-Null }
    $r = Invoke-DgProcess -Exe $Exe -Argv $res.Argv
    $logs = if ($res.OutDir) { Get-DgLogBlock -Dir $res.OutDir -Roots $Roots } else { '' }
    return [pscustomobject]@{
        Name   = $Inv.Name
        Exit   = $r.Exit
        Stdout = (ConvertTo-DgNormalised -Text $r.Stdout -Roots $Roots)
        Stderr = (ConvertTo-DgNormalised -Text $r.Stderr -Roots $Roots)
        Logs   = $logs
    }
}

# Run an invocation, enforcing determinism where required, and self-validate the
# family marker / exit. Returns the (single, canonical) capture.
function Invoke-DgCapture { param([object] $Inv, [hashtable] $FixMap, [string] $Exe, [string] $OutBase, [string[]] $Roots)
    $cap = Invoke-DgCaptureOnce -Inv $Inv -FixMap $FixMap -Exe $Exe -OutBase $OutBase -Roots $Roots

    if ($Inv.ContainsKey('DetCheck') -and $Inv.DetCheck) {
        $cap2 = Invoke-DgCaptureOnce -Inv $Inv -FixMap $FixMap -Exe $Exe -OutBase $OutBase -Roots $Roots
        if ($cap.Stderr -ne $cap2.Stderr -or $cap.Stdout -ne $cap2.Stdout -or $cap.Exit -ne $cap2.Exit -or $cap.Logs -ne $cap2.Logs) {
            Stop-DgHarness ("invocation '$($Inv.Name)' is NON-DETERMINISTIC (two runs of the same exe differ).`n" +
                '  A -w position is being computed across unrelated heap allocations. This archive must not be pinned;' + "`n" +
                '  remove it from the -w golden set (see README).')
        }
    }

    # Self-validation: a corrupted fixture that stops firing its family is a
    # harness error, never a pass -- the golden would silently pin a healthy run.
    if ($Inv.ContainsKey('ExpectExit') -and $cap.Exit -ne $Inv.ExpectExit) {
        Stop-DgHarness "invocation '$($Inv.Name)' exited $($cap.Exit), expected $($Inv.ExpectExit)."
    }
    if ($Inv.ContainsKey('Family') -and $Inv.Family) {
        $marker = $FamilyMarkers[$Inv.Family]
        if (-not $marker) { Stop-DgHarness "no marker defined for family '$($Inv.Family)'." }
        if ([string]::IsNullOrEmpty($cap.Stderr)) { Stop-DgHarness "invocation '$($Inv.Name)' produced EMPTY stderr; expected a '$($Inv.Family)' diagnostic." }
        if ($cap.Stderr -notlike "*$marker*") {
            Stop-DgHarness "invocation '$($Inv.Name)' stderr does not contain the '$($Inv.Family)' family marker ('$marker'). The fixture stopped firing its mechanism."
        }
        # Failure paths must never leave a .log behind (Dump runs only on success).
        if (-not [string]::IsNullOrEmpty($cap.Logs)) {
            Stop-DgHarness "invocation '$($Inv.Name)' produced a .log on a FAILURE path (expected none)."
        }
    }
    return $cap
}

# ---------------------------------------------------------------------------
# Golden IO
# ---------------------------------------------------------------------------

function Get-DgSurfaceDir { param([string] $GoldenRoot, [string] $Name) Join-Path (Join-Path $GoldenRoot 'surfaces') $Name }

function Write-DgGolden { param([string] $GoldenRoot, [object] $Cap)
    $dir = Get-DgSurfaceDir -GoldenRoot $GoldenRoot -Name $Cap.Name
    Set-DgFileAtomic -Path (Join-Path $dir 'stderr.txt') -Text $Cap.Stderr
    Set-DgFileAtomic -Path (Join-Path $dir 'stdout.txt') -Text $Cap.Stdout
    Set-DgFileAtomic -Path (Join-Path $dir 'exit.txt')   -Text ([string] $Cap.Exit)
    Set-DgFileAtomic -Path (Join-Path $dir 'logs.txt')   -Text $Cap.Logs
}

function Read-DgGolden { param([string] $GoldenRoot, [string] $Name)
    $dir = Get-DgSurfaceDir -GoldenRoot $GoldenRoot -Name $Name
    [pscustomobject]@{
        Name   = $Name
        Stderr = Get-DgText (Join-Path $dir 'stderr.txt')
        Stdout = Get-DgText (Join-Path $dir 'stdout.txt')
        Exit   = Get-DgText (Join-Path $dir 'exit.txt')
        Logs   = Get-DgText (Join-Path $dir 'logs.txt')
    }
}

# ---------------------------------------------------------------------------
# Capture / Compare
# ---------------------------------------------------------------------------

function Invoke-DgCaptureRun {
    param([string] $Exe, [string] $CorpusRoot, [string] $WorkDir, [string] $GoldenRoot, [switch] $Force, [string] $ExeSha, [string] $ExeVersion)

    $manifestPath = Join-Path $GoldenRoot 'manifest.json'
    if (-not $Force -and [System.IO.File]::Exists($manifestPath)) {
        Stop-DgHarness "goldens already exist at $GoldenRoot. Pass -Force to overwrite (a deliberate diagnostic-output change)."
    }
    Remove-DgDir $GoldenRoot
    [System.IO.Directory]::CreateDirectory($GoldenRoot) | Out-Null

    $fixDir = Join-Path $WorkDir 'fixtures'
    $outBase = Join-Path $WorkDir 'out'
    Remove-DgDir $outBase; [System.IO.Directory]::CreateDirectory($outBase) | Out-Null
    $roots = @($WorkDir, $Exe)

    $src = Get-DgSources -CorpusRoot $CorpusRoot -ExpectSources $null
    $fixMap = New-DgFixtures -FixtureDir $fixDir -SourceBytes $src.Bytes -ExpectFixtures $null

    $invRecords = [System.Collections.Generic.List[object]]::new()
    foreach ($inv in $Invocations) {
        $cap = Invoke-DgCapture -Inv $inv -FixMap $fixMap -Exe $Exe -OutBase $outBase -Roots $roots
        Write-DgGolden -GoldenRoot $GoldenRoot -Cap $cap
        Write-DgInfo ("captured {0,-22} exit={1} stderr={2}B stdout={3}B logs={4}B" -f $cap.Name, $cap.Exit, $cap.Stderr.Length, $cap.Stdout.Length, $cap.Logs.Length)
        $invRecords.Add([ordered]@{
            name       = $cap.Name
            exit       = $cap.Exit
            stderrSha  = (Get-DgStringHash $cap.Stderr)
            stdoutSha  = (Get-DgStringHash $cap.Stdout)
            logsSha    = (Get-DgStringHash $cap.Logs)
        })
    }

    $fixRecords = foreach ($f in $Fixtures) { [ordered]@{ name = $f.Name; source = $f.Source; recipe = $f.Recipe; sha = $fixMap[$f.Name].Sha } }

    $manifest = [ordered]@{
        harness     = $HarnessVersion
        created     = (Get-Date).ToString('o')
        exeSha      = $ExeSha
        exeVersion  = $ExeVersion
        corpusRoot  = $CorpusRoot
        sources     = $src.Records
        fixtures    = @($fixRecords)
        invocations = $invRecords
    }
    Set-DgFileAtomic -Path $manifestPath -Text ($manifest | ConvertTo-Json -Depth 8)

    Write-DgHeading 'Capture complete'
    Write-DgInfo "goldens : $GoldenRoot"
    Write-DgInfo ("pinned  : {0} invocation surfaces, {1} fixtures, {2} source archives" -f $Invocations.Count, $Fixtures.Count, $SourceArchives.Count)
    Write-DgInfo 'These goldens are LOCAL only -- never commit them (they embed corpus names/analysis rows).'
    return $ExitIdentical
}

# Compare one capture against its golden; returns a list of differing surface names.
function Compare-DgCapture { param([object] $Cap, [object] $Golden)
    $diffs = [System.Collections.Generic.List[string]]::new()
    if (([string] $Cap.Exit) -ne ([string] $Golden.Exit)) { $diffs.Add('exit') }
    if ($Cap.Stderr -ne $Golden.Stderr) { $diffs.Add('stderr') }
    if ($Cap.Stdout -ne $Golden.Stdout) { $diffs.Add('stdout') }
    if ($Cap.Logs   -ne $Golden.Logs)   { $diffs.Add('logs') }
    # Comma-wrap: a bare `return $diffs` UNROLLS the list into the pipeline, so an
    # empty result arrives as $null (and .Count then throws under StrictMode) and a
    # 1-element result arrives as a bare string. Return the List as one object.
    return ,$diffs
}

function Invoke-DgCompareRun {
    param([string] $Exe, [string] $CorpusRoot, [string] $WorkDir, [string] $GoldenRoot)

    $manifestPath = Join-Path $GoldenRoot 'manifest.json'
    if (-not [System.IO.File]::Exists($manifestPath)) {
        Stop-DgHarness "no goldens at $GoldenRoot. Run with -Capture first."
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.harness -ne $HarnessVersion) {
        Stop-DgHarness "golden manifest is $($manifest.harness), this harness is $HarnessVersion. Recapture with -Capture -Force."
    }

    $fixDir = Join-Path $WorkDir 'fixtures'
    $outBase = Join-Path $WorkDir 'out'
    Remove-DgDir $outBase; [System.IO.Directory]::CreateDirectory($outBase) | Out-Null
    $roots = @($WorkDir, $Exe)

    # Refuse to compare against goldens whose fixtures we cannot faithfully reproduce.
    $src = Get-DgSources -CorpusRoot $CorpusRoot -ExpectSources $manifest.sources
    $fixMap = New-DgFixtures -FixtureDir $fixDir -SourceBytes $src.Bytes -ExpectFixtures $manifest.fixtures

    Write-DgInfo ("goldens captured from exe sha {0} ({1})" -f ([string]$manifest.exeSha).Substring(0, 12), $manifest.exeVersion)

    $diffRows   = [System.Collections.Generic.List[object]]::new()
    foreach ($inv in $Invocations) {
        $cap    = Invoke-DgCapture -Inv $inv -FixMap $fixMap -Exe $Exe -OutBase $outBase -Roots $roots
        $golden = Read-DgGolden -GoldenRoot $GoldenRoot -Name $inv.Name
        if ($null -eq $golden.Stderr -and $null -eq $golden.Stdout) {
            Stop-DgHarness "no golden stored for invocation '$($inv.Name)'. Recapture with -Capture -Force."
        }
        $surfaceDiffs = Compare-DgCapture -Cap $cap -Golden $golden
        if ($surfaceDiffs.Count -gt 0) {
            $diffRows.Add([pscustomobject]@{ Name = $inv.Name; Surfaces = $surfaceDiffs; Cap = $cap; Golden = $golden })
            Write-DgWarn ("DIFF {0,-22} {1}" -f $inv.Name, ($surfaceDiffs -join ', '))
        }
        else {
            Write-DgInfo ("ok   {0,-22} exit={1}" -f $inv.Name, $cap.Exit)
        }
    }

    # --- report ---
    $stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
    $reportDir = Join-Path (Join-Path $WorkDir 'reports') $stamp
    if ($diffRows.Count -gt 0) {
        [System.IO.Directory]::CreateDirectory($reportDir) | Out-Null
        foreach ($d in $diffRows) {
            foreach ($surface in $d.Surfaces) {
                $old = switch ($surface) { 'stderr' { $d.Golden.Stderr } 'stdout' { $d.Golden.Stdout } 'logs' { $d.Golden.Logs } 'exit' { [string] $d.Golden.Exit } }
                $new = switch ($surface) { 'stderr' { $d.Cap.Stderr } 'stdout' { $d.Cap.Stdout } 'logs' { $d.Cap.Logs } 'exit' { [string] $d.Cap.Exit } }
                Set-DgFileAtomic -Path (Join-Path $reportDir "$($d.Name).$surface.old.txt") -Text ([string] $old)
                Set-DgFileAtomic -Path (Join-Path $reportDir "$($d.Name).$surface.new.txt") -Text ([string] $new)
            }
        }
    }

    Write-DgHeading 'Result'
    if ($diffRows.Count -eq 0) {
        Write-Host ("   ALL {0} diagnostic surfaces byte-identical to the goldens." -f $Invocations.Count) -ForegroundColor Green
        return $ExitIdentical
    }
    Write-Host ("   {0} of {1} surfaces DIFFER:" -f $diffRows.Count, $Invocations.Count) -ForegroundColor Yellow
    foreach ($d in $diffRows) { Write-Host ("     {0,-22} {1}" -f $d.Name, ($d.Surfaces -join ', ')) -ForegroundColor Yellow }
    Write-Host ''
    Write-DgInfo "side-by-side report: $reportDir"
    return $ExitDiffs
}

# ---------------------------------------------------------------------------
# Self-test: prove the harness detects a diff before any PASS is trusted.
# ---------------------------------------------------------------------------
function Invoke-DgSelfTest { param([string] $Exe, [string] $CorpusRoot, [string] $WorkDir)
    Write-DgHeading 'SELF-TEST'
    Write-DgInfo 'Capture -> clean compare (must be 0) -> perturb one golden (must be 1) -> restore (must be 0).'

    $stRoot = Join-Path $WorkDir 'selftest'
    Remove-DgDir $stRoot
    $stWork   = Join-Path $stRoot 'work'
    $stGolden = Join-Path $stRoot 'goldens'
    [System.IO.Directory]::CreateDirectory($stWork) | Out-Null

    $exeSha = Get-DgFileHash -Path $Exe
    $exeVer = (Invoke-DgProcess -Exe $Exe -Argv @('--version')).Stdout.Trim()

    $failures = [System.Collections.Generic.List[string]]::new()

    # 1. Capture, then a clean compare of the SAME exe -> must be identical.
    Invoke-DgCaptureRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $stWork -GoldenRoot $stGolden -ExeSha $exeSha -ExeVersion $exeVer | Out-Null
    $clean = Invoke-DgCompareRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $stWork -GoldenRoot $stGolden
    if ($clean -ne $ExitIdentical) { $failures.Add("clean compare of the same exe returned $clean; expected 0") }

    # 2. Perturb ONE golden surface and confirm the diff is caught and named.
    $victimName = 'fix-error-enum'
    $victimFile = Join-Path (Get-DgSurfaceDir -GoldenRoot $stGolden -Name $victimName) 'stderr.txt'
    $orig = [System.IO.File]::ReadAllText($victimFile)
    [System.IO.File]::WriteAllText($victimFile, $orig + "`nDG-SELFTEST-INJECTED")
    $dirty = Invoke-DgCompareRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $stWork -GoldenRoot $stGolden
    if ($dirty -ne $ExitDiffs) { $failures.Add("perturbed compare returned $dirty; expected 1 (a detected diff)") }
    [System.IO.File]::WriteAllText($victimFile, $orig)  # restore

    # 3. Restored -> clean again.
    $restored = Invoke-DgCompareRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $stWork -GoldenRoot $stGolden
    if ($restored -ne $ExitIdentical) { $failures.Add("restored compare returned $restored; expected 0") }

    # 4. The fixture-firing guard: a corrupted fixture whose stderr no longer
    #    carries its family marker must be a HARNESS ERROR, not a pass. Prove the
    #    check itself works (in-process, without spawning a child that calls exit).
    $probeMarker = $FamilyMarkers['assert']
    $firesOnHealthy = ("ERROR IN`nAT POSITION`nEXPECTED`nINSTEAD GOT" -like "*$probeMarker*")
    $missesOnBlank  = ('' -like "*$probeMarker*")
    if (-not $firesOnHealthy) { $failures.Add('family-marker check failed to match a valid Assert block') }
    if ($missesOnBlank)       { $failures.Add('family-marker check matched empty stderr (would accept a non-firing fixture)') }

    Remove-DgDir $stRoot

    Write-Host ''
    if ($failures.Count -gt 0) {
        foreach ($f in $failures) { Write-Host "   FAIL: $f" -ForegroundColor Red }
        Write-Host '   SELF-TEST FAILED -- do not trust this harness until it is fixed.' -ForegroundColor Red
        return $ExitHarnessError
    }
    Write-Host '   SELF-TEST PASSED: a clean compare is identical; an injected golden change is' -ForegroundColor Green
    Write-Host '   reported as a diff and named; the restored set is clean again; and the' -ForegroundColor Green
    Write-Host '   fixture-firing guard both matches a real family marker and rejects empty stderr.' -ForegroundColor Green
    return $ExitIdentical
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

Assert-DgNoShadowedHelpers -ScriptPath $PSCommandPath

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).ProviderPath
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'src\Caesar.cpp'))) {
    Stop-DgHarness "this script expects to live at <repo>\tools\diag-goldens\; got repo root '$repoRoot'."
}

if (-not $WorkDir)   { $WorkDir = Join-Path $env:LOCALAPPDATA 'caesar-diag' }
if (-not $Exe)       { $Exe = Join-Path $repoRoot 'build\Release\caesar.exe' }
$goldenRoot = Join-Path $WorkDir 'goldens'

if (-not (Test-Path -LiteralPath $CorpusRoot)) { Stop-DgHarness "corpus root not found: $CorpusRoot" }
if (-not (Test-Path -LiteralPath $Exe)) {
    Stop-DgHarness "exe not found: $Exe`n  Build it first:  cmake --build build --config Release  (from a VS dev prompt)"
}
$Exe = (Resolve-Path -LiteralPath $Exe).ProviderPath
$WorkDir = [System.IO.Path]::GetFullPath($WorkDir)
[System.IO.Directory]::CreateDirectory($WorkDir) | Out-Null

# A stale exe compares the fold's SOURCE against an exe that never contained it --
# a false PASS. The build is driven by src/ AND the CMake files (compile options,
# CAESAR_VERSION), so both are watched. (Capture is exempt: you deliberately choose
# which exe to snapshot; the manifest records its sha.)
if (-not $Capture -and -not $SelfTest -and -not $AllowStaleExe) {
    $exeTime = [System.IO.File]::GetLastWriteTimeUtc($Exe)
    $watched = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'src') -Recurse -File -Include *.cpp, *.hpp, *.h, *.c -ErrorAction SilentlyContinue)
    foreach ($bf in @('CMakeLists.txt', 'CMakePresets.json')) {
        $bp = Join-Path $repoRoot $bf
        if (Test-Path -LiteralPath $bp) { $watched += Get-Item -LiteralPath $bp }
    }
    $newer = @($watched | Where-Object { $_.LastWriteTimeUtc -gt $exeTime })
    if ($newer.Count -gt 0) {
        Stop-DgHarness ("$($newer.Count) build input(s) are NEWER than $Exe (e.g. $($newer[0].Name)).`n" +
            "  The exe does not contain the change under test; this compare would be a false PASS.`n" +
            '  Rebuild, or pass -AllowStaleExe if you really mean it.')
    }
}

$exeSha = Get-DgFileHash -Path $Exe
$exeVer = (Invoke-DgProcess -Exe $Exe -Argv @('--version')).Stdout.Trim()

Write-DgHeading 'caesar diagnostics goldens'
Write-DgInfo "repo    : $repoRoot"
Write-DgInfo "exe     : $Exe"
Write-DgInfo "exe sha : $($exeSha.Substring(0,12))  ($exeVer)"
Write-DgInfo "corpus  : $CorpusRoot"
Write-DgInfo "work    : $WorkDir"

# One run at a time per work dir (fixtures + out dirs are shared paths).
$lock = $null
try {
    $lock = [System.IO.File]::Open((Join-Path $WorkDir '.lock'), [System.IO.FileMode]::OpenOrCreate,
                                   [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
}
catch { Stop-DgHarness "another diag-goldens run already holds the lock on $WorkDir." }

try {
    if ($SelfTest) {
        $code = Invoke-DgSelfTest -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $WorkDir
    }
    elseif ($Capture) {
        Write-DgHeading 'Capturing goldens'
        $code = Invoke-DgCaptureRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $WorkDir -GoldenRoot $goldenRoot -Force:$Force -ExeSha $exeSha -ExeVersion $exeVer
    }
    else {
        Write-DgHeading 'Comparing against goldens'
        $code = Invoke-DgCompareRun -Exe $Exe -CorpusRoot $CorpusRoot -WorkDir $WorkDir -GoldenRoot $goldenRoot
    }
}
finally {
    if ($lock) { $lock.Dispose() }
}

exit $code
