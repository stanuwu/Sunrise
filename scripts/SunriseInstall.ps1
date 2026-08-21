<#
.SYNOPSIS
    Sunrise dev bootstrap (v2) - automates runbook Phases 0-3 with hardened error
    handling, network retries, download validation, a run log, and a phase summary.
.DESCRIPTION
    Phase 0  Toolchain check (Git, MSBuild/VS, Windows SDK). Offers winget/VS install.
    Phase 1  Game folder, Defender exclusions, DepotDownloader, both depots.
    Phase 2  Back up original steam_api64.dll, install official Sunrise release DLL.
    Phase 3  Clone Sunrise, build Release|x64 (64-bit host), write deploy-mybuild.ps1.

    Re-runnable: finished steps are detected and skipped. Every run writes a full
    transcript to <WorkDir>\logs so a failed run can be diagnosed after the fact.
.NOTES
    Run from an ELEVATED PowerShell (Defender exclusions need admin):
      powershell -ExecutionPolicy Bypass -File .\SunriseInstall.ps1

    v2 changes over the original:
      - Native commands (git/winget/DepotDownloader/VS) run through Invoke-Native so a
        program writing to stderr can never crash the script (the PS 5.1 stderr trap).
      - Network downloads retry with backoff (Invoke-WithRetry).
      - Downloads are validated: DLL/EXE must be real PE files, zips must be openable,
        GitHub API rate-limit (HTTP 403) is detected and explained.
      - Full transcript log per run; a trap prints the log path on any fatal error.
      - Phase results are tracked and printed as a summary at the end.
      - Manifests can be overridden with -Depot1Manifest / -Depot2Manifest without
        editing the file (they go stale; Discord pins are the source of truth).
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$WorkDir = "$env:USERPROFILE\SunriseDev",
    [string]$SteamUser,
    [string]$Depot1Manifest = '7180122903232116872',
    [string]$Depot2Manifest = '2210332166360342287',
    [switch]$SkipDepots,
    [switch]$SkipReleaseDll,
    [switch]$SkipBuild,
    [switch]$ForceReclone
)

# Cmdlet errors terminate (so try/catch works); native-command stderr is handled
# separately via Invoke-Native so it can never trigger a terminating error.
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # faster Invoke-WebRequest, no progress spam
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# --- Config (verify manifests against Discord pins if downloads misbehave) ---
$AppId  = '1085660'
$Depots = @(
    @{ Depot = '1085661'; Manifest = $Depot1Manifest },
    @{ Depot = '1085662'; Manifest = $Depot2Manifest }
)
$SunriseRepo = 'https://github.com/stanuwu/Sunrise.git'
$SunriseApi  = 'https://api.github.com/repos/stanuwu/Sunrise/releases/latest'
$DepotDlApi  = 'https://api.github.com/repos/SteamRE/DepotDownloader/releases/latest'
$SdkVersion  = '10.0.26100'
$VsBootstrapUrls = @(
    'https://c2rsetup.officeapps.live.com/c2r/downloadVS.aspx?sku=community&channel=stable&version=VS18',
    'https://aka.ms/vs/18/release/vs_community.exe',
    'https://aka.ms/vs/stable/vs_community.exe'
)
$VsLandingPage  = 'https://visualstudio.microsoft.com/thank-you-downloading-visual-studio/?sku=Community&channel=Stable&version=VS18'
$VsWorkload     = 'Microsoft.VisualStudio.Workload.NativeDesktop'
$VsSdkComponent = 'Microsoft.VisualStudio.Component.Windows11SDK.26100'

# --- Output helpers ---
function Banner([string]$t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }
function Ok    ([string]$t) { Write-Host "  [OK]   $t" -ForegroundColor Green }
function Warn2 ([string]$t) { Write-Host "  [WARN] $t" -ForegroundColor Yellow }
function Info  ([string]$t) { Write-Host "  $t" }

# Phase-result tracking for the end-of-run summary.
$script:Results = [ordered]@{}
function Set-Result([string]$phase, [string]$state) { $script:Results[$phase] = $state }

function Fail([string]$t) {
    Write-Host "  [FAIL] $t" -ForegroundColor Red
    if ($script:LogFile) { Write-Host "  Full log: $script:LogFile" -ForegroundColor DarkGray }
    Show-Summary
    exit 1
}

function Show-Summary {
    Banner "Summary"
    foreach ($k in $script:Results.Keys) {
        $v = $script:Results[$k]
        $color = switch ($v) { 'ok' {'Green'} 'skipped' {'DarkGray'} 'partial' {'Yellow'} default {'Red'} }
        Write-Host ("  {0,-26} {1}" -f $k, $v) -ForegroundColor $color
    }
    if ($script:LogFile) { Write-Host "`n  Log: $script:LogFile" -ForegroundColor DarkGray }
}

# --- Native command wrapper: isolates a program's stderr from PS error handling ---
# Runs the command with $ErrorActionPreference relaxed so native stderr text cannot
# raise a terminating error, and returns the real process exit code. THIS is the fix
# for the "gh/git wrote to stderr and the script died" class of bug.
function Invoke-Native {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @(), [switch]$PassThruOutput)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        if ($PassThruOutput) { & $File @Arguments 2>&1 | Write-Host }
        else                 { & $File @Arguments }
        return $LASTEXITCODE
    } finally { $ErrorActionPreference = $prev }
}

# --- Retry wrapper for flaky network operations ---
function Invoke-WithRetry {
    param([Parameter(Mandatory)][scriptblock]$Action, [int]$Tries = 3, [int]$DelaySec = 4, [string]$What = 'operation')
    for ($i = 1; $i -le $Tries; $i++) {
        try { return & $Action }
        catch {
            if ($i -eq $Tries) { throw }
            Warn2 "$What failed (attempt $i/$Tries): $($_.Exception.Message). Retrying in ${DelaySec}s..."
            Start-Sleep -Seconds $DelaySec
            $DelaySec *= 2
        }
    }
}

# --- Validate a downloaded file is a real PE binary, not an HTML error/rate-limit page ---
function Test-PEFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    if ((Get-Item -LiteralPath $Path).Length -lt 4096) { return $false }
    try {
        $bytes = [byte[]]::new(2)
        $fs = [IO.File]::OpenRead($Path)
        try { [void]$fs.Read($bytes, 0, 2) } finally { $fs.Dispose() }
        return ($bytes[0] -eq 0x4D -and $bytes[1] -eq 0x5A)   # 'MZ'
    } catch { return $false }
}

# --- GitHub asset download with rate-limit awareness and PE/zip validation ---
function Get-GithubAsset {
    param([string]$ApiUrl, [string]$NamePattern, [string]$OutFile, [switch]$ExpectPE)
    $headers = @{ 'User-Agent' = 'sunrise-setup-script'; 'Accept' = 'application/vnd.github+json' }
    try {
        $rel = Invoke-WithRetry -What 'GitHub API' -Action {
            Invoke-RestMethod -Uri $ApiUrl -Headers $headers
        }
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -eq 403) {
            Warn2 "GitHub API rate limit hit (unauthenticated = 60/hr). Wait an hour or download the asset manually."
        } else {
            Warn2 "GitHub API call failed: $($_.Exception.Message)"
        }
        return $null
    }
    $asset = $rel.assets | Where-Object { $_.name -like $NamePattern } | Select-Object -First 1
    if (-not $asset) {
        Warn2 "No release asset matching '$NamePattern'. Available:"
        $rel.assets | ForEach-Object { Info "  - $($_.name)" }
        return $null
    }
    Info "downloading $($asset.name) ($($rel.tag_name)) ..."
    try {
        Invoke-WithRetry -What "download $($asset.name)" -Action {
            Invoke-WebRequest -Uri $asset.browser_download_url -Headers $headers -OutFile $OutFile
        }
    } catch { Warn2 "Download failed after retries: $($_.Exception.Message)"; return $null }
    if ($ExpectPE -and -not (Test-PEFile $OutFile)) {
        Warn2 "Downloaded file is not a valid binary (got an error page?). Discarding."
        Remove-Item -LiteralPath $OutFile -Force -ErrorAction SilentlyContinue
        return $null
    }
    return $OutFile
}

function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    try {
        & $vswhere -latest -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
    } catch { return $null }
}

function Resolve-GameExe([string]$Dir) {
    foreach ($rel in @('destiny2.exe', 'bin\x64\destiny2.exe')) {
        $full = Join-Path $Dir $rel
        if (Test-Path $full) { return $full }
    }
    return (Join-Path $Dir 'destiny2.exe')
}

function Show-InstallPrompt([string]$Message, [string]$Title) {
    try {
        Add-Type -AssemblyName PresentationFramework -ErrorAction Stop
        return ([System.Windows.MessageBox]::Show($Message, $Title, 'YesNo', 'Warning') -eq 'Yes')
    } catch { return ((Read-Host "  $Message (y/N)") -eq 'y') }
}

# Graphical "browse for folder" dialog, with a console fallback when no GUI is
# available (headless/remoting). Returns the chosen path, or $null if cancelled.
function Select-FolderDialog([string]$Description, [string]$InitialPath) {
    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
        $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
        $dlg.Description = $Description
        $dlg.ShowNewFolderButton = $true
        if ($InitialPath -and (Test-Path $InitialPath)) { $dlg.SelectedPath = $InitialPath }
        # A hidden top-most form as owner so the dialog surfaces above the console.
        $owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true; ShowInTaskbar = $false }
        try {
            if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { return $dlg.SelectedPath }
        } finally { $owner.Dispose() }
        return $null
    } catch {
        return $null   # no GUI available
    }
}

function Install-VSCommunity {
    $go = Show-InstallPrompt -Title 'Sunrise setup - Visual Studio' -Message (
        "Visual Studio (C++ workload) was not found.`n`nInstall VS Community now with Desktop C++ + SDK $SdkVersion pre-selected?`n(~10 GB, 30-60 min.)")
    if (-not $go) { return $null }
    $boot = Join-Path $env:TEMP 'vs_community.exe'
    $got = $false
    foreach ($u in $VsBootstrapUrls) {
        try {
            Info "downloading VS bootstrapper: $u"
            Invoke-WithRetry -Tries 2 -What 'VS bootstrapper' -Action { Invoke-WebRequest -Uri $u -OutFile $boot -UseBasicParsing }
            if ((Test-Path $boot) -and (Get-Item $boot).Length -gt 1MB) { $got = $true; break }
        } catch { Warn2 "$u failed - trying next." }
    }
    if (-not $got) {
        Warn2 "Couldn't fetch the bootstrapper. Opening the download page; install manually then re-run."
        Start-Process $VsLandingPage
        return $null
    }
    Info "Launching VS installer (passive)... this is the 10 GB part, ~45 min."
    $code = Invoke-Native -File $boot -Arguments @(
        '--add', $VsWorkload, '--add', $VsSdkComponent,
        '--includeRecommended', '--passive', '--norestart', '--wait')
    switch ($code) {
        0    { Ok "Visual Studio installed." }
        3010 { Warn2 "VS installed but wants a REBOOT. Reboot, then re-run." }
        default { Warn2 "VS installer exit code $code. If it rejected the SDK, add it via the VS Installer manually." }
    }
    return (Find-MSBuild)
}

function Add-VSComponent([string]$Component) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $setup   = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
    if (-not ((Test-Path $vswhere) -and (Test-Path $setup))) { return $false }
    $instPath = & $vswhere -latest -property installationPath 2>$null | Select-Object -First 1
    if (-not $instPath) { return $false }
    if (-not (Show-InstallPrompt -Title 'Sunrise setup - Windows SDK' -Message "Add Windows SDK $SdkVersion via the VS Installer now?")) { return $false }
    $code = Invoke-Native -File $setup -Arguments @(
        'modify', '--installPath', $instPath, '--add', $Component, '--passive', '--norestart', '--wait')
    return ($code -eq 0 -or $code -eq 3010)
}

# ============================================================ Trap + logging
# Any uncaught terminating error lands here with the log path, instead of a raw stack.
trap {
    Write-Host "`n[FATAL] $($_.Exception.Message)" -ForegroundColor Red
    if ($script:LogFile) { Write-Host "Full log: $script:LogFile" -ForegroundColor DarkGray }
    try { Stop-Transcript | Out-Null } catch {}
    exit 1
}

# WorkDir + log must exist before anything else so the transcript captures the whole run.
try { New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null } catch { Write-Host "[FATAL] Cannot create WorkDir '$WorkDir': $($_.Exception.Message)" -ForegroundColor Red; exit 1 }
$logDir = Join-Path $WorkDir 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:LogFile = Join-Path $logDir "install-$stamp.log"
try { Start-Transcript -Path $script:LogFile -Append | Out-Null } catch { Warn2 "Could not start transcript: $($_.Exception.Message)" }

Banner "Sunrise dev bootstrap v2"
Info "Log: $script:LogFile"

# --- Environment sanity ---
if ([IntPtr]::Size -eq 4) {
    Warn2 "You're in 32-bit PowerShell. Some checks may misread 64-bit paths. Prefer the 64-bit shell."
}
$psv = $PSVersionTable.PSVersion
Info "PowerShell $psv"

# --- Elevation ---
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Warn2 "Not elevated. Defender exclusions need admin; without them AV may quarantine the DLL or eat files mid-download."
    if ((Read-Host "  Continue without exclusions? (y = yes / N = quit)") -ne 'y') {
        Info "Re-run from an elevated PowerShell."; Stop-Transcript | Out-Null; exit 1
    }
}

# ============================================================ Phase 0
Banner "Phase 0 - Toolchain check"
$missingFatal = $false

# Git
if (Get-Command git -ErrorAction SilentlyContinue) {
    $gitVer = (Invoke-Native -File git -Arguments @('--version') 2>&1 | Out-String).Trim()
    Ok "Git: $gitVer"
} else {
    Warn2 "Git not found."
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        if ((Read-Host "  Install Git via winget now? (Y/n)") -ne 'n') {
            Invoke-Native -File winget -Arguments @('install','--id','Git.Git','-e','--accept-source-agreements','--accept-package-agreements') -PassThruOutput | Out-Null
            $env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
            if (Get-Command git -ErrorAction SilentlyContinue) { Ok "Git installed." }
            else { Warn2 "Git installed but not on PATH yet - open a fresh shell and re-run for Phase 3." }
        } else { $missingFatal = $true }
    } else { Warn2 "winget unavailable - install Git manually: https://git-scm.com"; $missingFatal = $true }
}

# MSBuild / VS
$msbuild = Find-MSBuild
if ($msbuild) { Ok "MSBuild: $msbuild" }
else {
    Warn2 "Visual Studio / MSBuild not found."
    $msbuild = Install-VSCommunity
    if ($msbuild) { Ok "MSBuild: $msbuild" }
    elseif (-not $SkipBuild) { Warn2 "No VS - Phase 3 (build) will be skipped. Re-run after installing." }
}

# Windows SDK
$sdkPath = "${env:ProgramFiles(x86)}\Windows Kits\10\Include\$SdkVersion.0"
if (Test-Path $sdkPath) { Ok "Windows SDK $SdkVersion present." }
else {
    Warn2 "Windows SDK $SdkVersion not found (the build pins this exact version)."
    if ($msbuild -and (Add-VSComponent $VsSdkComponent) -and (Test-Path $sdkPath)) { Ok "Windows SDK $SdkVersion installed." }
    elseif ($msbuild) { Warn2 "Add it via VS Installer > Modify > Individual components > 'Windows 11 SDK ($SdkVersion)', then re-run." }
}

# Smart App Control (check-only)
try {
    $sac = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy' -Name VerifiedAndReputablePolicyState -ErrorAction Stop
    if ($sac.VerifiedAndReputablePolicyState -eq 1) {
        Warn2 "Smart App Control is ON - scripts can't disable it. Settings > Windows Security > App & browser control > Smart App Control > Off."
        Read-Host "  Press Enter when done (or to continue anyway)" | Out-Null
    }
} catch { }   # key absent = SAC not present; fine

Set-Result 'Phase 0 toolchain' $(if ($missingFatal) {'failed'} else {'ok'})
if ($missingFatal) { Fail "Fix the missing toolchain above, then re-run." }

# ============================================================ Phase 1
Banner "Phase 1 - Game install"
if (-not $GameDir) {
    Info "Opening a folder picker for the game install location (needs ~100 GB free)..."
    # Default the picker to the largest fixed drive so an empty D:\ etc. is one click away.
    $bigDrive = Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue |
                Where-Object { $_.Free } | Sort-Object Free -Descending | Select-Object -First 1
    $seed = if ($bigDrive) { "$($bigDrive.Name):\" } else { 'C:\' }
    $GameDir = Select-FolderDialog -Description 'Pick an empty/new folder for the Destiny 2 install (~100 GB free needed)' -InitialPath $seed
    if ($GameDir) { Info "Selected: $GameDir" }
    else { $GameDir = Read-Host "  (picker cancelled/unavailable) Type the game folder e.g. D:\Destiny2-Sunrise" }
}
if ([string]::IsNullOrWhiteSpace($GameDir)) { Fail "No game folder given." }
$GameDir = $GameDir.TrimEnd('\')
New-Item -ItemType Directory -Force -Path $GameDir | Out-Null

# Free space on BOTH drives (game + workdir/build can differ)
foreach ($pair in @(@{P=$GameDir;N=100}, @{P=$WorkDir;N=5})) {
    try {
        $d = (Get-Item $pair.P).PSDrive
        if ($d.Free -lt ($pair.N * 1GB) -and -not $SkipDepots) {
            Warn2 ("Only {0:N0} GB free on {1}: (want ~{2} GB for {3})." -f ($d.Free/1GB), $d.Name, $pair.N, $pair.P)
            if ((Read-Host "  Continue anyway? (y/N)") -ne 'y') { Fail "Aborted for disk space." }
        }
    } catch { Warn2 "Couldn't check free space on $($pair.P): $($_.Exception.Message)" }
}

# Defender exclusions (cmdlet may be absent if a 3rd-party AV replaced Defender)
if ($isAdmin) {
    if (Get-Command Add-MpPreference -ErrorAction SilentlyContinue) {
        try {
            Add-MpPreference -ExclusionPath $GameDir -ErrorAction Stop
            Add-MpPreference -ExclusionPath $WorkDir -ErrorAction Stop
            Ok "Defender exclusions added: $GameDir , $WorkDir"
        } catch { Warn2 "Add-MpPreference failed ($($_.Exception.Message)). Add exclusions manually if AV interferes." }
    } else { Warn2 "Defender cmdlets not available (3rd-party AV?). Exclude $GameDir and $WorkDir manually." }
} else { Warn2 "Skipped Defender exclusions (not admin)." }

$gameExe = Resolve-GameExe $GameDir
$gameDll = Join-Path $GameDir 'bin\x64\steam_api64.dll'

if ($SkipDepots -or (Test-Path $gameExe)) {
    Ok "Game present ($gameExe) - skipping depot download."
    Set-Result 'Phase 1 game' 'skipped'
} else {
    $ddDir = Join-Path $WorkDir 'DepotDownloader'
    $ddExe = Join-Path $ddDir 'DepotDownloader.exe'
    if (-not (Test-Path $ddExe)) {
        New-Item -ItemType Directory -Force -Path $ddDir | Out-Null
        $zip = Join-Path $env:TEMP 'DepotDownloader.zip'
        if (-not (Get-GithubAsset -ApiUrl $DepotDlApi -NamePattern '*windows-x64*.zip' -OutFile $zip)) {
            Fail "Could not fetch DepotDownloader. Download it from github.com/SteamRE/DepotDownloader and re-run."
        }
        try { Expand-Archive -Path $zip -DestinationPath $ddDir -Force }
        catch { Fail "DepotDownloader zip is corrupt or unreadable: $($_.Exception.Message)" }
        Remove-Item $zip -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path $ddExe)) { Fail "DepotDownloader.exe missing after extraction - the zip layout may have changed." }
    Ok "DepotDownloader ready."

    if (-not $SteamUser) {
        Warn2 "Use your Steam ACCOUNT/LOGIN name (Steam > Account details), NOT your display name. Wrong name = exit code 1."
        $SteamUser = Read-Host "  Steam account name"
    }
    if ([string]::IsNullOrWhiteSpace($SteamUser)) { Fail "No Steam account name given." }

    Info "`n  Pulling both depots. DepotDownloader prompts for password + Steam Guard itself. 1-3 hours.`n"
    foreach ($d in $Depots) {
        Info "--- depot $($d.Depot) / manifest $($d.Manifest) ---"
        $code = Invoke-Native -File $ddExe -Arguments @(
            '-app', $AppId, '-depot', $d.Depot, '-manifest', $d.Manifest,
            '-dir', $GameDir, '-username', $SteamUser, '-remember-password', '-os', 'windows', '-osarch', '64')
        if ($code -ne 0) {
            Warn2 "DepotDownloader exit code $code."
            Warn2 "Check: account NAME (not display) / password / Steam Guard approved / account owns D2 / disk space / manifest fresh (Discord pins)."
            Fail "Depot download failed - fix and re-run (partial downloads are kept and resumed)."
        }
    }
    $gameExe = Resolve-GameExe $GameDir
}

if ((Test-Path $gameExe) -and (Test-Path $gameDll)) {
    Ok "Phase 1 exit test PASSED: destiny2.exe ($gameExe) + steam_api64.dll present."
    if ($script:Results['Phase 1 game'] -ne 'skipped') { Set-Result 'Phase 1 game' 'ok' }
} else {
    Set-Result 'Phase 1 game' 'failed'
    Fail "Phase 1 exit test FAILED: destiny2.exe (checked root and bin\x64) or $gameDll missing."
}

# ============================================================ Phase 2
Banner "Phase 2 - Official release DLL (known-good baseline)"
if ($SkipReleaseDll) {
    Ok "Skipped by flag."
    Set-Result 'Phase 2 release DLL' 'skipped'
} else {
    $origBackup = Join-Path $GameDir 'bin\x64\steam_api64.dll.orig'
    if (-not (Test-Path $origBackup)) {
        try { Copy-Item $gameDll $origBackup -ErrorAction Stop; Ok "Original DLL backed up -> steam_api64.dll.orig" }
        catch { Fail "Could not back up the original DLL: $($_.Exception.Message)" }
    } else { Ok "Original backup already exists - untouched." }

    $relDll = Join-Path $WorkDir 'sunrise-release-steam_api64.dll'
    if (Get-GithubAsset -ApiUrl $SunriseApi -NamePattern '*steam_api64*.dll' -OutFile $relDll -ExpectPE) {
        try {
            Copy-Item $relDll $gameDll -Force -ErrorAction Stop
            Copy-Item $relDll (Join-Path $GameDir 'bin\x64\steam_api64.dll.release') -Force
            Ok "Official Sunrise release DLL installed (spare kept as steam_api64.dll.release)."
            Set-Result 'Phase 2 release DLL' 'ok'
        } catch { Set-Result 'Phase 2 release DLL' 'failed'; Fail "Could not install the release DLL: $($_.Exception.Message)" }
    } else {
        Warn2 "Couldn't auto-download the release DLL. Get it from the OFFICIAL repo Releases page and copy it over $gameDll."
        Set-Result 'Phase 2 release DLL' 'partial'
    }
}

# ============================================================ Phase 3
Banner "Phase 3 - Clone + build from source"
$repoDir  = Join-Path $WorkDir 'Sunrise'
$buildDll = Join-Path $repoDir 'build\x64\Release\steam_api64.dll'

if ($SkipBuild) {
    Ok "Skipped by flag."
    Set-Result 'Phase 3 build' 'skipped'
} elseif (-not $msbuild) {
    Warn2 "No MSBuild - skipping build. Re-run after installing VS."
    Set-Result 'Phase 3 build' 'skipped'
} elseif (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Warn2 "git not on PATH - skipping build. Open a fresh shell and re-run."
    Set-Result 'Phase 3 build' 'skipped'
} else {
    if ($ForceReclone -and (Test-Path $repoDir)) {
        Warn2 "-ForceReclone set: removing existing $repoDir"
        Remove-Item -LiteralPath $repoDir -Recurse -Force
    }
    if (-not (Test-Path (Join-Path $repoDir '.git'))) {
        $code = Invoke-Native -File git -Arguments @('clone', $SunriseRepo, $repoDir) -PassThruOutput
        if ($code -ne 0) { Set-Result 'Phase 3 build' 'failed'; Fail "git clone failed (exit $code)." }
    } else { Ok "Repo already cloned (not pulling - your working tree is yours)." }

    $sln = Join-Path $repoDir 'Sunrise.sln'
    if (-not (Test-Path $sln)) { Set-Result 'Phase 3 build' 'failed'; Fail "Sunrise.sln not found at $sln - repo layout may have changed." }

    Info "Building Release|x64 (64-bit compiler host)..."
    # /p:PreferredToolArchitecture=x64 forces the 64-bit-hosted cl.exe. Without it the
    # x86 host hits its ~4 GB cap and dies C1060 on physics_state_runtime.cpp.
    $code = Invoke-Native -File $msbuild -Arguments @(
        $sln, '/m', '/p:Configuration=Release', '/p:Platform=x64',
        '/p:PreferredToolArchitecture=x64', '/v:minimal') -PassThruOutput
    if ($code -ne 0) { Set-Result 'Phase 3 build' 'failed'; Fail "Build failed (exit $code). Open the solution in VS for readable errors." }

    if (-not (Test-Path $buildDll)) { Set-Result 'Phase 3 build' 'failed'; Fail "Build reported success but $buildDll is missing." }
    Ok "Build output: $buildDll"
    Set-Result 'Phase 3 build' 'ok'
}

# --- Deploy helper: written on every run (even -SkipBuild) so it's always on hand ---
# Uses the standard build-output path even if this run didn't build, so it works as
# soon as a build exists.
if (Test-Path $repoDir) {
    $deploy = Join-Path $repoDir 'deploy-mybuild.ps1'
    @"
# Copies YOUR built DLL into the game folder. Game must NOT be running.
`$src = '$buildDll'
`$dst = '$gameDll'
if (Get-Process destiny2 -ErrorAction SilentlyContinue) { Write-Host 'Close the game first.' -ForegroundColor Red; exit 1 }
if (-not (Test-Path `$src)) { Write-Host "Build missing: `$src  (build first)" -ForegroundColor Red; exit 1 }
Copy-Item `$src `$dst -Force
Copy-Item ([IO.Path]::ChangeExtension(`$src,'pdb')) (Split-Path `$dst) -Force -ErrorAction SilentlyContinue
Write-Host 'Deployed your build. Launch destiny2.exe and press INSERT.' -ForegroundColor Green
"@ | Set-Content $deploy -Encoding UTF8
    Ok "Deploy helper: $deploy"
}

# ============================================================ Done
Banner "Next steps (not scriptable)"
Write-Host @"
  1. Test the OFFICIAL release first: launch $gameExe directly, press INSERT, run an
     activity override. If that fails, it's a setup issue, not your build.
  2. Then deploy your build:  powershell -File $repoDir\deploy-mybuild.ps1
  Rollback: copy steam_api64.dll.orig (vanilla) or .release (official) over the live DLL.
"@
Show-Summary
try { Stop-Transcript | Out-Null } catch {}
