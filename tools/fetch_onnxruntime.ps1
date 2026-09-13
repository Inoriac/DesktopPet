param(
    [string]$Version = "1.28.0",
    [string]$Destination = "third_party\onnxruntime"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot ".."))
$dest = Join-Path $root $Destination
$archive = Join-Path $dest "onnxruntime-win-x64-$Version.zip"
$url = "https://github.com/microsoft/onnxruntime/releases/download/v$Version/onnxruntime-win-x64-$Version.zip"

New-Item -ItemType Directory -Force -Path $dest | Out-Null
Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
Expand-Archive -Path $archive -DestinationPath $dest -Force
Remove-Item -LiteralPath $archive -Force

$package = Join-Path $dest "onnxruntime-win-x64-$Version"
if (!(Test-Path (Join-Path $package "include\onnxruntime_cxx_api.h"))) {
    throw "Downloaded package is missing include\onnxruntime_cxx_api.h"
}
if (!(Test-Path (Join-Path $package "lib\onnxruntime.lib"))) {
    throw "Downloaded package is missing lib\onnxruntime.lib"
}
if (!(Test-Path (Join-Path $package "bin\onnxruntime.dll"))) {
    throw "Downloaded package is missing bin\onnxruntime.dll"
}
Write-Host "Installed $package"
