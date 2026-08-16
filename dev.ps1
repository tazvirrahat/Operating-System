# dev.ps1 — run the build inside the Docker container from Windows.
#
#   .\dev.ps1            build the ISO
#   .\dev.ps1 run        boot it in QEMU (serial output to this window)
#   .\dev.ps1 test       headless boot + self-test assertion
#   .\dev.ps1 clean      remove build artefacts
#   .\dev.ps1 shell      drop into a shell inside the container
#
# The container image is built automatically if it does not exist.

param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Args)

$ErrorActionPreference = "Stop"
$image = "myos-dev"
$root  = $PSScriptRoot

if (-not (docker image inspect $image 2>$null)) {
    Write-Host "building $image image (first run only)..." -ForegroundColor Cyan
    docker build -t $image $root
}

if ($Args -and $Args[0] -eq "shell") {
    docker run --rm -it -v "${root}:/os" $image bash
    exit $LASTEXITCODE
}

$target = if ($Args) { $Args -join " " } else { "all" }
docker run --rm -i -v "${root}:/os" $image make $target.Split(" ")
exit $LASTEXITCODE
