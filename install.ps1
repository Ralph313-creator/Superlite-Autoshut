# Superlite Autoshut one-line installer. Run in an ADMINISTRATOR PowerShell:
#   irm https://raw.githubusercontent.com/Ralph313-creator/Superlite-Autoshut/main/install.ps1 | iex
# Pick the idle time (minutes) first, otherwise it keeps the saved one (10 on a new PC):
#   $env:AUTOSHUT_MINUTES = 15; irm https://raw.githubusercontent.com/Ralph313-creator/Superlite-Autoshut/main/install.ps1 | iex
$ErrorActionPreference = 'Stop'
$repo = 'Ralph313-creator/Superlite-Autoshut'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Write-Host 'Run this in an administrator PowerShell (right-click > Run as administrator).' -ForegroundColor Red
    return
}

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$release = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers @{ 'User-Agent' = 'SuperliteAutoshut-installer' }
$asset = $release.assets | Where-Object { $_.name -eq 'SuperliteAutoshut.exe' } | Select-Object -First 1
if (-not $asset) { Write-Host "The latest release ($($release.tag_name)) has no SuperliteAutoshut.exe." -ForegroundColor Red; return }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("SuperliteAutoshut-" + [guid]::NewGuid().ToString('N') + '.exe')
Write-Host "Downloading Superlite Autoshut $($release.tag_name)..."
Invoke-WebRequest $asset.browser_download_url -OutFile $tmp -UseBasicParsing -Headers @{ 'User-Agent' = 'SuperliteAutoshut-installer' }

if ($asset.digest -and $asset.digest -like 'sha256:*') {
    $want = $asset.digest.Substring(7).ToUpper()
    $got = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToUpper()
    if ($got -ne $want) { Remove-Item $tmp -Force; Write-Host 'Download check failed (SHA-256 mismatch). Nothing was installed.' -ForegroundColor Red; return }
}

try {
    if ($env:AUTOSHUT_MINUTES) { & $tmp install $env:AUTOSHUT_MINUTES } else { & $tmp install }
} finally {
    Remove-Item $tmp -Force -ErrorAction SilentlyContinue
}
