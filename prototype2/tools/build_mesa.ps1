# Prototype 2 0.1.0
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
 New-Item -ItemType Directory -Force -Path out | Out-Null
 docker build -t prototype2-mesa-build:0.1.0 -f tools/Dockerfile.mesa tools
 if($LASTEXITCODE -ne 0){throw 'Mesa image failed'}
 docker run --rm --mount "type=bind,source=$projectRoot,target=/work" --mount "type=bind,source=$projectRoot/reference/mesa-switch-src,target=/project" -w /project prototype2-mesa-build:0.1.0 bash /work/tools/build_mesa_inside.sh
 if($LASTEXITCODE -ne 0){throw 'Mesa build failed'}
} finally {Pop-Location}
