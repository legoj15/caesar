# Build + verify the battery-v2 console capture cartridge (see README.md, the
# "Battery v2" section). Produces an SD-ready Luma LayeredFS folder, both
# prediction renders (recalibrated caesar_play), and a v2 manifest, then gates
# everything through the full chain: converter parse, byte-identical round-trip,
# both prediction renders, a numerical schedule check per track, and the
# steal-saturation voice-count assertion from the render log.
#
# v1 (build-cartridge.ps1 / build_cartridge.py) stays the shipped tool; this is
# the parallel v2 builder. Same source default (the MiiPlazaUpdate dump).
param(
    [string]$Source = 'E:\legoj\Documents\3DSWii Dumps\Dumps\MiiPlazaUpdate\region_common\frame\sound\MeetSound.bcsar',
    [string]$OutDir = '',
    [string]$Tid = '0004001000021800',
    [string]$RomfsRel = 'romfs/region_common/frame/sound/MeetSound.bcsar'
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $OutDir) { $OutDir = Join-Path $repo 'build\cartridge-v2' }
$caesar = Join-Path $repo 'build\Release\caesar.exe'
$roundtrip = Join-Path $repo 'build\Release\caesar-roundtrip.exe'
$play = Join-Path $repo 'build\Release\caesar-play.exe'
foreach ($exe in @($caesar, $roundtrip, $play)) {
    if (-not (Test-Path $exe)) { throw "missing $exe — build Release first (recalibrated player required)" }
}
if (-not (Test-Path $Source)) { throw "source archive not found: $Source" }

$patched = Join-Path $OutDir "sd\luma\titles\$Tid\$($RomfsRel -replace '/', '\')"
$steps = @()

python (Join-Path $PSScriptRoot 'build_cartridge_v2.py') --source $Source --out $OutDir --tid $Tid --romfs-rel $RomfsRel
$steps += @{ name = 'build cartridge v2 (+ static schedule check)'; ok = ($LASTEXITCODE -eq 0) }

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
    $renderLog = Join-Path $OutDir "verify\render_$($t.letter).log"
    & $play --render $patched --seq $t.seq --out $prediction > $renderLog 2>&1
    $steps += @{ name = "prediction render track $($t.letter) ($($t.seq))"; ok = ($LASTEXITCODE -eq 0) }
    if ($haveNumpy) {
        Write-Host "--- track $($t.letter) schedule check ---"
        python (Join-Path $PSScriptRoot 'check_prediction_v2.py') $prediction (Join-Path $OutDir 'MANIFEST.md') $t.letter
        $steps += @{ name = "schedule check track $($t.letter)"; ok = ($LASTEXITCODE -eq 0) }
    }
    # Steal-saturation voice-count gate (track A): the render log must report a
    # saturated pool (peak 24/24) with forced steals (> 24 stacked voices).
    if ($t.letter -eq 'A') {
        $log = Get-Content $renderLog -Raw
        $sat = $log -match 'peak 24/24' -and $log -match '(\d+) stolen'
        $stolen = if ($log -match '(\d+) stolen') { [int]$Matches[1] } else { 0 }
        $ok = $sat -and $stolen -ge 6
        Write-Host "  steal saturation: peak 24/24 = $($log -match 'peak 24/24'), stolen = $stolen (need >= 6)"
        $steps += @{ name = 'steal saturation voice count (peak 24/24, >=6 stolen)'; ok = $ok }
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
    Write-Host "`nBattery v2 ready. Copy the contents of:`n  $OutDir\sd\`nonto the SD card root (merging the luma folder), enable Luma's Game Patching, and see MANIFEST.md + README.md (Battery v2 run-sheet) for the capture schedule."
}
exit $failed
