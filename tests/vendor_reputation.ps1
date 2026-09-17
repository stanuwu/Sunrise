$ErrorActionPreference = 'Stop'
$repRoot = Split-Path $PSScriptRoot -Parent
$repBuild = Join-Path $repRoot 'build/vendor-reputation-tests'
New-Item -ItemType Directory -Path $repBuild -Force | Out-Null
$repVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$repVs = & $repVsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $repVs) { throw 'Visual Studio C++ tools are required.' }
$repVcVars = Join-Path $repVs 'VC/Auxiliary/Build/vcvars64.bat'
$repSource = Join-Path $repRoot 'Sunrise/src'
$repSqlite = Join-Path $repRoot 'Sunrise/vendor/sqlite'
$repSources = @(
    'tests/vendor_reputation.cpp',
    'Sunrise/src/state/runtime/state_vendor_reputation_runtime.cpp',
    'Sunrise/src/state/runtime/state_account_profile_runtime.cpp',
    'Sunrise/src/state/runtime/state_account_acquisition_runtime.cpp',
    'Sunrise/src/state/runtime/state_account_equipment_runtime.cpp',
    'Sunrise/src/state/runtime/state_account_identity_runtime.cpp',
    'Sunrise/src/state/account/account_state.cpp',
    'Sunrise/src/state/account/inventory/inventory_state.cpp',
    'Sunrise/src/state/account/settings/settings_state.cpp',
    'Sunrise/src/state/investment/investment_database.cpp',
    'Sunrise/src/state/investment/investment_account_store.cpp',
    'Sunrise/src/state/investment/investment_inventory_store.cpp',
    'Sunrise/src/state/investment/investment_settings_store.cpp',
    'Sunrise/src/state/investment/investment_unlock_store.cpp'
) | ForEach-Object { '"' + (Join-Path $repRoot $_) + '"' }
Push-Location $repBuild
try {
    $repSqliteObject = Join-Path $repBuild 'sqlite3.obj'
    if (-not (Test-Path -LiteralPath $repSqliteObject) -or
        (Get-Item -LiteralPath "$repSqlite/sqlite3.c").LastWriteTimeUtc -gt
        (Get-Item -LiteralPath $repSqliteObject).LastWriteTimeUtc) {
        & cmd.exe /d /s /c "`"$repVcVars`" >nul && cl /nologo /O2 /w /TC /c /DSQLITE_OMIT_LOAD_EXTENSION /DSQLITE_THREADSAFE=1 `"$repSqlite/sqlite3.c`" /Fosqlite3.obj"
        if ($LASTEXITCODE -ne 0) { throw 'SQLite test build failed.' }
    }
    $repCommand = "`"$repVcVars`" >nul && cl /nologo /std:c++20 /EHsc /O2 /Gy /W4 /WX /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I`"$repSource`" /external:I`"$repSqlite`" /external:W0 " + ($repSources -join ' ') + ' sqlite3.obj /Fevendor_reputation.exe /link /OPT:REF /STACK:8388608'
    & cmd.exe /d /s /c $repCommand
    if ($LASTEXITCODE -ne 0) { throw 'Reputation test build failed.' }
    $repScratch = Join-Path $repBuild ('reputation-' + [guid]::NewGuid().ToString('N') + '.sqlite3')
    & (Join-Path $repBuild 'vendor_reputation.exe') (Join-Path $repRoot 'Sunrise/resources/database') $repScratch
    if ($LASTEXITCODE -ne 0) { throw 'Reputation checks failed.' }
} finally {
    Pop-Location
}
