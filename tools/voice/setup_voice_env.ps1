param(
    [string]$VenvPath = ".venv"
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Error "Python was not found. Please install Python >= 3.9 and ensure it is available in PATH."
}

if (-not (Test-Path $VenvPath)) {
    python -m venv $VenvPath
}

$pythonExe = Join-Path $VenvPath "Scripts\python.exe"
if (-not (Test-Path $pythonExe)) {
    Write-Error "Virtual environment Python was not found: $pythonExe"
}

& $pythonExe -m pip install --upgrade pip
& $pythonExe -m pip install -r "tools\voice\requirements.txt"

& $pythonExe -c "import onnxruntime" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "onnxruntime import failed; applying Windows compatibility fallback..."
    & $pythonExe -m pip install --force-reinstall "onnxruntime==1.20.1" "numpy<2.0"
}

Write-Host ""
Write-Host "Voice virtual environment is ready: $VenvPath"
Write-Host "Next, download GENIE assets and preset speakers, for example:"
Write-Host "  & $pythonExe tools\voice\download_genie_assets.py --preset feibi --with-roberta"
Write-Host ""
Write-Host "Note: the first GENIE base asset download is about 391MB."
