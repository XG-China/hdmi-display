# Build the fpk inside Docker Desktop on Windows.
#
#   powershell -ExecutionPolicy Bypass -File build-kit\build-in-docker.ps1
#
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content (Join-Path $root "packaging\fpk\hdmi-display\manifest")
$versionLine = $manifest | Select-String '^version="(.+)"$' | Select-Object -First 1
if (-not $versionLine) { throw "cannot read version from manifest" }
$version = $versionLine.Matches[0].Groups[1].Value
$image = if ($env:IMAGE) { $env:IMAGE } else { "hdmi-display-build:$version" }

Write-Host "==> building/refreshing build image $image"
$buildArgs = @()
if ($env:DEBIAN_MIRROR) {
    $buildArgs += @("--build-arg", "DEBIAN_MIRROR=$env:DEBIAN_MIRROR")
}
if ($env:DEBIAN_SECURITY_MIRROR) {
    $buildArgs += @("--build-arg", "DEBIAN_SECURITY_MIRROR=$env:DEBIAN_SECURITY_MIRROR")
}
docker build @buildArgs -f (Join-Path $root "build-kit\Dockerfile") -t $image $root
if ($LASTEXITCODE -ne 0) { throw "docker build failed" }

New-Item -ItemType Directory -Force -Path (Join-Path $root "build") | Out-Null
Write-Host "==> building fpk in $image"
docker run --rm -v "${root}:/src" -w /src -e HOME=/tmp $image bash packaging/build-fpk.sh
if ($LASTEXITCODE -ne 0) { throw "docker build of the app failed" }

Write-Host ""
Write-Host "done: $root\build\hdmi-display_$version.fpk"
