# Compiles src\shaders\lutbuilder.hlsl into headers the add-on embeds (bin\shaders\*.h).
# Needs the UE 5.4 shader headers from a local engine install. They're copied to
# bin\ue_shaders at build time (TonemapCommon.ush with its loose cbuffer globals removed,
# since our shader declares those at the game's offsets) and aren't kept in the repo.
# Usage: .\build_shaders.ps1 [-UE <engine root>]
param(
    [string]$UE = "E:\unreal\UE_5.4"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$private = Join-Path $UE "Engine\Shaders\Private"
if (-not (Test-Path "$private\TonemapCommon.ush")) { throw "UE shader headers not found in $private (pass -UE)" }

$dxc = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\dxc.exe" | Sort-Object FullName | Select-Object -Last 1
if (-not $dxc) { throw "dxc.exe not found (install the Windows SDK)" }

$ue = "$root\bin\ue_shaders"
$out = "$root\bin\shaders"
New-Item -ItemType Directory -Force $ue, $out | Out-Null
Copy-Item "$private\ACES.ush", "$private\GammaCorrectionCommon.ush" $ue -Force
# TonemapCommon.ush declares InverseGamma and Film* as loose globals.
(Get-Content "$private\TonemapCommon.ush") |
    Where-Object { $_ -notmatch '^\s*(half3|float)\s+(InverseGamma|FilmSlope|FilmToe|FilmShoulder|FilmBlackClip|FilmWhiteClip)\s*;' } |
    Set-Content "$ue\TonemapCommon.ush" -Encoding ascii

# One variant per game permutation we replace.
$variants = @(
    @{ Name = "lutbuilder_skiptemp"; Defines = @("-D", "SKIP_TEMPERATURE=1") },
    @{ Name = "lutbuilder_temp"; Defines = @("-D", "SKIP_TEMPERATURE=0") }
)
foreach ($v in $variants) {
    $args = @("-nologo", "-T", "cs_6_6", "-E", "MainCS", "-HV", "2021", "-O3", "-I", $ue) + $v.Defines +
            @("-Fh", "$out\$($v.Name).h", "-Vn", "k_$($v.Name)", "-Fo", "$out\$($v.Name).cso", "$root\src\shaders\lutbuilder.hlsl")
    & $dxc.FullName @args
    if ($LASTEXITCODE) { throw "shader $($v.Name) failed" }
    Write-Host "Built $($v.Name) ($((Get-Item "$out\$($v.Name).cso").Length) bytes)"
}
