# Regenerates project/generated from assets/game_root/default.xex. Log: logs/codegen.log
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$rex = Join-Path $root 'tools/bin/rexglue-0.10.0/win-amd64/bin/rexglue.exe'
New-Item -ItemType Directory -Force -Path (Join-Path $root 'logs') | Out-Null
$xex = Join-Path $root 'assets/game_root/default.xex'
$expected = 'f6caf82c433b0bb20fe701912388b1e7ee8d6b529d8fc275af960ac2e9ddcab8'
if ((Get-FileHash -LiteralPath $xex -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'default.xex differs from the inspected Prototype 2 edition (SHA-256).'
}
Push-Location (Join-Path $root 'project')
try {
    & $rex --log-file ../logs/codegen.log codegen prototype2_manifest.toml
    if ($LASTEXITCODE -ne 0) { throw "Codegen failed ($LASTEXITCODE). See logs/codegen.log." }
} finally { Pop-Location }
# Switch/GCC: re-apply the DEFINE_REX_FUNC GCC branch to the regenerated pch (WORKLOG #13).
python -I (Join-Path $PSScriptRoot 'prepare_switch_codegen.py')
if ($LASTEXITCODE -ne 0) { throw 'prepare_switch_codegen.py failed' }
