# Sunrise-DevLoop.ps1 - edit -> build -> deploy -> run -> attach, one command.
#
#   .\Sunrise-DevLoop.ps1                build + deploy + launch + attach VS debugger
#   .\Sunrise-DevLoop.ps1 -NoAttach      skip the debugger attach
#   .\Sunrise-DevLoop.ps1 -BuildOnly     just build + deploy (game not touched)
#   .\Sunrise-DevLoop.ps1 -Config Debug  build the Debug config (better for breakpoints)
#   .\Sunrise-DevLoop.ps1 -OpenIDE       open Sunrise.sln in Visual Studio and exit
#   .\Sunrise-DevLoop.ps1 -Rollback official   restore the official release DLL
#   .\Sunrise-DevLoop.ps1 -Rollback vanilla    restore the stock game DLL
#
# First run asks for your Destiny 2 install folder once and remembers it in
# sunrise-dev.json next to this script. The repo root is auto-detected from the
# script's location (place this script in <repo>\scripts\), or pass -RepoDir.
#
# ONE-TIME manual clicks in VS: set the toolbar to Release|x64 (or Debug if you build
# Debug), and enable ClangFormat under Tools > Options > Text Editor > C/C++ > Formatting.

param(
    [switch]$NoAttach,
    [switch]$BuildOnly,
    [switch]$OpenIDE,
    [ValidateSet('Release','Debug')][string]$Config = 'Release',
    [ValidateSet('official','vanilla')][string]$Rollback,
    [string]$RepoDir,
    [string]$GameDir
)
$ErrorActionPreference = 'Continue'

function Fail([string]$m) { Write-Host "[FAIL] $m" -ForegroundColor Red; exit 1 }
function Ok  ([string]$m) { Write-Host "[OK]   $m" -ForegroundColor Green }
function Info([string]$m) { Write-Host $m }

# --- Locate the repo (folder containing Sunrise.sln) ---
if (-not $RepoDir) {
    # Script is expected at <repo>\scripts\; repo root is one level up.
    $guess = Split-Path -Parent $PSScriptRoot
    if (Test-Path (Join-Path $guess 'Sunrise.sln')) { $RepoDir = $guess }
    elseif (Test-Path (Join-Path $PSScriptRoot 'Sunrise.sln')) { $RepoDir = $PSScriptRoot }
    else { Fail "Couldn't find Sunrise.sln. Put this script in <repo>\scripts\ or pass -RepoDir." }
}
$Sln = Join-Path $RepoDir 'Sunrise.sln'
if (-not (Test-Path $Sln)) { Fail "Solution not found: $Sln" }

# --- Remember the game folder in a config file next to the script ---
$configFile = Join-Path $PSScriptRoot 'sunrise-dev.json'
if (-not $GameDir -and (Test-Path $configFile)) {
    try { $GameDir = (Get-Content $configFile -Raw | ConvertFrom-Json).GameDir } catch {}
}
if (-not $GameDir) {
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = 'Pick your Destiny 2 (Sunrise) install folder'
        $owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true; ShowInTaskbar = $false }
        if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { $GameDir = $dlg.SelectedPath }
        $owner.Dispose()
    } catch {}
    if (-not $GameDir) { $GameDir = Read-Host "Destiny 2 install folder (e.g. D:\Destiny2-Sunrise)" }
    if ([string]::IsNullOrWhiteSpace($GameDir)) { Fail "No game folder given." }
    try { @{ GameDir = $GameDir } | ConvertTo-Json | Set-Content $configFile -Encoding UTF8; Info "Saved game folder to $configFile" } catch {}
}

$BuildDll   = Join-Path $RepoDir "build\x64\$Config\steam_api64.dll"
$BuildPdb   = Join-Path $RepoDir "build\x64\$Config\steam_api64.pdb"
$GameExe    = Join-Path $GameDir 'destiny2.exe'
$GameDllDir = Join-Path $GameDir 'bin\x64'
$GameDll    = Join-Path $GameDllDir 'steam_api64.dll'

# --- Rollback mode ---
if ($Rollback) {
    $backup = Join-Path $GameDllDir ($(if ($Rollback -eq 'vanilla') { 'steam_api64.dll.orig' } else { 'steam_api64.dll.release' }))
    if (-not (Test-Path $backup)) { Fail "No $Rollback backup found at $backup" }
    if (Get-Process destiny2 -ErrorAction SilentlyContinue) { Fail "Close the game first - the DLL is locked." }
    Copy-Item $backup $GameDll -Force
    Ok "Restored $Rollback DLL."
    exit 0
}

# --- Resolve MSBuild (survives VS updates and any edition) ---
function Resolve-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
        if ($p -and (Test-Path $p)) { return $p }
    }
    foreach ($ed in 'Community','Professional','Enterprise','BuildTools') {
        $p = "C:\Program Files\Microsoft Visual Studio\18\$ed\MSBuild\Current\Bin\MSBuild.exe"
        if (Test-Path $p) { return $p }
    }
    return $null
}

if ($OpenIDE) { Start-Process $Sln; Info "Opening in Visual Studio."; exit 0 }

if (-not (Test-Path $GameDllDir)) { Fail "Game folder not found: $GameDllDir  (delete $configFile to re-pick)" }
$MSBuild = Resolve-MSBuild
if (-not $MSBuild) { Fail "MSBuild not found. Install VS with the C++ workload." }
Info "MSBuild: $MSBuild"

# --- Game must be closed (its DLL is locked while loaded) ---
$game = Get-Process destiny2 -ErrorAction SilentlyContinue
if ($game) {
    if ($BuildOnly) { Fail "destiny2.exe is running - close it first." }
    if ((Read-Host "destiny2.exe is running - close it and continue? (y/N)") -ne 'y') { exit 1 }
    $game | Stop-Process -Force
    Start-Sleep 2
}

# --- Build (64-bit compiler host = the C1060 fix) ---
Info "Building $Config|x64 ..."
$sw = [Diagnostics.Stopwatch]::StartNew()
& $MSBuild $Sln /m /p:Configuration=$Config /p:Platform=x64 /p:PreferredToolArchitecture=x64 /v:minimal
$code = $LASTEXITCODE
$sw.Stop()
if ($code -ne 0) { Fail "Build failed (exit $code) after $([int]$sw.Elapsed.TotalSeconds)s." }
if (-not (Test-Path $BuildDll)) { Fail "Build succeeded but $BuildDll is missing - wrong -Config?" }
Ok "Build succeeded in $([int]$sw.Elapsed.TotalSeconds)s."

# --- Deploy + verify ---
Copy-Item $BuildDll $GameDll -Force
if (Test-Path $BuildPdb) { Copy-Item $BuildPdb $GameDllDir -Force }
if ((Get-Item $BuildDll).Length -ne (Get-Item $GameDll).Length) { Fail "Deploy verification failed - sizes differ." }
Ok "Deployed your $Config build (DLL + symbols)."
if ($BuildOnly) { exit 0 }

# --- Launch ---
if (-not (Test-Path $GameExe)) { Fail "Game exe not found: $GameExe" }
Start-Process $GameExe -WorkingDirectory $GameDir
Ok "Launched destiny2.exe"
if ($NoAttach) { exit 0 }

# --- Attach VS debugger (needs VS already open) ---
$dte = $null
foreach ($progId in 'VisualStudio.DTE.18.0','VisualStudio.DTE.17.0','VisualStudio.DTE') {
    try { $dte = [Runtime.InteropServices.Marshal]::GetActiveObject($progId); break } catch {}
}
if (-not $dte) {
    Write-Host "[WARN] Visual Studio isn't running - skipping attach. Attach manually: Debug > Attach to Process > destiny2.exe" -ForegroundColor Yellow
    exit 0
}
Info "Waiting for destiny2.exe, then attaching ..."
$attached = $false
for ($i = 0; $i -lt 30 -and -not $attached; $i++) {
    Start-Sleep 2
    try {
        $proc = @($dte.Debugger.LocalProcesses) | Where-Object { $_.Name -like '*destiny2.exe' } | Select-Object -First 1
        if ($proc) { $proc.Attach(); $attached = $true }
    } catch { }
}
if ($attached) { Ok "VS debugger attached. Set breakpoints and go." }
else { Write-Host "[WARN] Couldn't attach automatically - Debug > Attach to Process in VS." -ForegroundColor Yellow }
