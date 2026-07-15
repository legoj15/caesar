# Build + verify the console capture cartridge (see README.md).
# Produces an SD-ready Luma LayeredFS folder, the prediction render, and
# a manifest, then gates everything through the full verification chain:
# converter parse, byte-identical round-trip, prediction render, and a
# numerical schedule check of the rendered audio.
param(
    # The console runs the plaza's v14 update, whose romfs moved the archive
    # (and ships a different, larger one) — default to the update dump.
    [string]$Source = 'E:\legoj\Documents\3DSWii Dumps\Dumps\MiiPlazaUpdate\region_common\frame\sound\MeetSound.bcsar',
    [string]$OutDir = '',
    [string]$Tid = '0004001000021800',
    [string]$RomfsRel = 'romfs/region_common/frame/sound/MeetSound.bcsar'
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $OutDir) { $OutDir = Join-Path $repo 'build\cartridge' }
$caesar = Join-Path $repo 'build\Release\caesar.exe'
$roundtrip = Join-Path $repo 'build\Release\caesar-roundtrip.exe'
$play = Join-Path $repo 'build\Release\caesar-play.exe'
foreach ($exe in @($caesar, $roundtrip, $play)) {
    if (-not (Test-Path $exe)) { throw "missing $exe — build Release first" }
}
if (-not (Test-Path $Source)) { throw "source archive not found: $Source" }

$patched = Join-Path $OutDir "sd\luma\titles\$Tid\$($RomfsRel -replace '/', '\')"
$steps = @()

python (Join-Path $PSScriptRoot 'build_cartridge.py') --source $Source --out $OutDir --tid $Tid --romfs-rel $RomfsRel
$steps += @{ name = 'build cartridge'; ok = ($LASTEXITCODE -eq 0) }

New-Item -ItemType Directory -Force (Join-Path $OutDir 'verify') | Out-Null
& $caesar $patched -o (Join-Path $OutDir 'verify\extract') *> (Join-Path $OutDir 'verify\convert.log')
$steps += @{ name = 'converter parses patched archive'; ok = ($LASTEXITCODE -eq 0) }

& $roundtrip --verify $patched > (Join-Path $OutDir 'verify\roundtrip.log') 2>&1
$steps += @{ name = 'round-trip byte-identical'; ok = ($LASTEXITCODE -eq 0) }

$tracks = @(
    @{ letter = 'A'; seq = 'BGM_MAIN_Mii_Only_One' },
    @{ letter = 'B'; seq = 'BGM_DEN_EMPTY_LANDSCAPE' }
)
python -c 'import numpy' 2>$null
$haveNumpy = ($LASTEXITCODE -eq 0)
foreach ($t in $tracks) {
    $prediction = Join-Path $OutDir "PREDICTION_battery_$($t.letter).wav"
    & $play --render $patched --seq $t.seq --out $prediction `
        > (Join-Path $OutDir "verify\render_$($t.letter).log") 2>&1
    $steps += @{ name = "prediction render track $($t.letter) ($($t.seq))"; ok = ($LASTEXITCODE -eq 0) }
    if ($haveNumpy) {
        Write-Host "--- track $($t.letter) schedule check ---"
        python (Join-Path $PSScriptRoot 'check_prediction.py') $prediction (Join-Path $OutDir 'MANIFEST.md') $t.letter
        $steps += @{ name = "schedule check track $($t.letter)"; ok = ($LASTEXITCODE -eq 0) }
    }
}
if (-not $haveNumpy) { Write-Host 'SKIP schedule checks (numpy not available)' }

Write-Host ''
$failed = 0
foreach ($s in $steps) {
    $tag = if ($s.ok) { '[PASS]' } else { $failed++; '[FAIL]' }
    Write-Host "$tag $($s.name)"
}
if ($failed -eq 0) {
    Write-Host "`nCartridge ready. Copy the contents of:`n  $OutDir\sd\`nonto the SD card root (merging the luma folder), enable Luma's Game Patching, and see MANIFEST.md for the capture schedule."
}
exit $failed
