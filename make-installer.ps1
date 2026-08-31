# Bakes the engine, watcher and knob into ONE self-contained .bat that can be
# handed to anyone. Run this after build.bat.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function B64([string]$p) {
    if (-not (Test-Path $p)) { throw "missing payload: $p" }
    [Convert]::ToBase64String([IO.File]::ReadAllBytes($p))
}

$dll = B64 (Join-Path $root 'bin\spotispeed.dll')
$exe = B64 (Join-Path $root 'bin\ssinject.exe')
$js  = B64 (Join-Path $root 'ext\spotispeed.js')

# ---------------------------------------------------------------- installer --
$installer = @'
param([Parameter(Mandatory=$true)][string]$SelfBat)

$ErrorActionPreference = 'Continue'
function Say($m, $c = 'Gray') { Write-Host "  $m" -ForegroundColor $c }
function Ok  ($m) { Say "[+] $m" 'Green' }
function Warn($m) { Say "[!] $m" 'Yellow' }
function Die ($m) { Say "[x] $m" 'Red'; Write-Host; Read-Host '  Press Enter to close'; exit 1 }

$raw = [IO.File]::ReadAllText($SelfBat)
function Payload([string]$tag) {
    $m = [regex]::Match($raw, "#$tag#([A-Za-z0-9+/=]+)#/$tag#")
    if (-not $m.Success) { Die "payload $tag missing - the file was probably corrupted in transit" }
    [Convert]::FromBase64String($m.Groups[1].Value)
}

$HOME_  = Join-Path $env:LOCALAPPDATA 'SpotiSpeed'
$SPOT   = Join-Path $env:APPDATA 'Spotify\Spotify.exe'
$UPD    = Join-Path $env:LOCALAPPDATA 'Spotify\Update'
$SPICE  = Join-Path $env:LOCALAPPDATA 'spicetify\spicetify.exe'
$EXTDIR = Join-Path $env:APPDATA 'spicetify\Extensions'
$RUNKEY = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

# --- 1. Spotify ------------------------------------------------------------
if (-not (Test-Path $SPOT)) {
    Die "Spotify desktop not found.`n      Install the normal installer from spotify.com (NOT the Microsoft Store version), log in once, then run this again."
}
Ok ("Found Spotify " + (Get-Item $SPOT).VersionInfo.FileVersion)

# --- 2. Spicetify (draws the knob) -----------------------------------------
if (-not (Test-Path $SPICE)) {
    Say '[*] Installing Spicetify (needed to draw the knob)...'
    try {
        $prev = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -UseBasicParsing 'https://raw.githubusercontent.com/spicetify/cli/main/install.ps1' |
            Invoke-Expression
        $ProgressPreference = $prev
    } catch { Warn "Spicetify install failed: $($_.Exception.Message)" }
}
if (-not (Test-Path $SPICE)) {
    Die "Spicetify could not be installed automatically.`n      Install it from https://spicetify.app/docs/getting-started then re-run this."
}
Ok 'Spicetify ready'

# --- 3. stop everything ----------------------------------------------------
Say '[*] Closing Spotify...'
Get-Process Spotify, ssinject -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match 'guardian\.ps1' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 3

# --- 4. unpack -------------------------------------------------------------
Say '[*] Installing audio engine...'
New-Item -ItemType Directory -Force $HOME_ | Out-Null
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'spotispeed.dll'), (Payload 'DLL'))
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'ssinject.exe'),   (Payload 'EXE'))
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'spotispeed.js'),  (Payload 'JS'))
# leftovers from older versions
Remove-Item (Join-Path $HOME_ 'guardian.ps1') -Force -ErrorAction SilentlyContinue
Ok "Engine installed to $HOME_"

# --- 4b. Marketplace (the shop icon, for installing other plugins) ----------
# Optional and non-fatal: if it does not work out, the knob is unaffected.
$MKT = Join-Path $env:APPDATA 'spicetify\CustomApps\marketplace'
try {
    if (-not (Test-Path (Join-Path $MKT 'manifest.json'))) {
        Say '[*] Installing Spicetify Marketplace (the shop icon)...'
        $prev = $ProgressPreference; $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -UseBasicParsing `
            'https://raw.githubusercontent.com/spicetify/marketplace/main/resources/install.ps1' |
            Invoke-Expression
        $ProgressPreference = $prev
    }
    if (Test-Path (Join-Path $MKT 'manifest.json')) {
        # list-type config: this appends, it never clears what is already there
        & $SPICE config custom_apps marketplace 2>&1 | Out-Null
        Ok 'Marketplace ready (shop icon in the sidebar)'
    } else {
        Warn 'Marketplace not installed - the knob still works, you just will not get the shop icon'
    }
} catch { Warn "Marketplace step skipped: $($_.Exception.Message)" }

# --- 5. the knob -----------------------------------------------------------
Say "[*] Adding the knob to Spotify's footer..."
New-Item -ItemType Directory -Force $EXTDIR | Out-Null
Copy-Item (Join-Path $HOME_ 'spotispeed.js') $EXTDIR -Force
& $SPICE config extensions spotispeed.js 2>&1 | Out-Null
# apply first: "backup apply" on an already-patched client would snapshot the
# patched bundle as though it were pristine
& $SPICE apply 2>&1 | Out-Null
$index = Join-Path $env:APPDATA 'Spotify\Apps\xpui\index.html'
$patched = (Test-Path $index) -and ((Get-Content $index -Raw) -match 'spicetifyWrapper')
if (-not $patched) {
    & $SPICE backup apply 2>&1 | Out-Null
    $patched = (Test-Path $index) -and ((Get-Content $index -Raw) -match 'spicetifyWrapper')
}
if ($patched) { Ok 'Knob added' } else { Warn 'Spicetify could not patch this Spotify build - speed still works, but the knob may not show' }

# --- 6. freeze the client --------------------------------------------------
Say '[*] Blocking Spotify auto-update...'
try {
    # An older SpotiSpeed put a deny ACE on this path, which would stop us
    # rewriting it now. Clear any inherited weirdness before touching it.
    if (Test-Path -LiteralPath $UPD) { icacls $UPD /reset 2>&1 | Out-Null }
    if (Test-Path -LiteralPath $UPD -PathType Container) { Remove-Item -LiteralPath $UPD -Recurse -Force }
    if (Test-Path -LiteralPath $UPD -PathType Leaf) {
        (Get-Item -LiteralPath $UPD -Force).Attributes = 'Normal'
        Remove-Item -LiteralPath $UPD -Force
    }
    New-Item -ItemType File -Path $UPD -Force | Out-Null
    (Get-Item -LiteralPath $UPD -Force).Attributes = 'ReadOnly, Hidden'
} catch { }
# What matters is the end state, not which step complained: a *file* on that
# path means the updater has nowhere to unpack.
$blocked = (Test-Path -LiteralPath $UPD -PathType Leaf)
if ($blocked) { Ok 'Auto-update blocked (this Spotify build is now frozen)' }
else { Warn 'Could not block updates - Spotify may replace itself later' }

# --- 7. autostart ----------------------------------------------------------
# The Run key, not the Startup folder. The Startup folder is silently skipped on
# some machines, which is exactly how the knob ended up dead after a reboot.
# This is the same mechanism Spotify uses for its own autostart.
Say '[*] Setting up autostart...'
$watchCmd = '"' + (Join-Path $HOME_ 'ssinject.exe') + '" --watch "' + (Join-Path $HOME_ 'spotispeed.dll') + '"'
try {
    New-Item -Path $RUNKEY -Force -ErrorAction SilentlyContinue | Out-Null
    Set-ItemProperty -Path $RUNKEY -Name 'SpotiSpeed' -Value $watchCmd -Force
    # retire the old, unreliable chain
    Remove-Item (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup\SpotiSpeed.vbs') -Force -ErrorAction SilentlyContinue
    schtasks /Delete /TN 'SpotiSpeed' /F 2>&1 | Out-Null
    $back = (Get-ItemProperty -Path $RUNKEY -Name 'SpotiSpeed' -ErrorAction Stop).SpotiSpeed
    if ($back -eq $watchCmd) { Ok 'Will start automatically at logon' }
    else { Warn 'Autostart entry did not verify' }
} catch { Warn "Could not register autostart: $($_.Exception.Message)" }

# --- 8. go -----------------------------------------------------------------
Say '[*] Starting engine and Spotify...'
Start-Process -FilePath (Join-Path $HOME_ 'ssinject.exe') `
              -ArgumentList '--watch', ('"' + (Join-Path $HOME_ 'spotispeed.dll') + '"')
Start-Sleep -Seconds 2
Start-Process $SPOT
Start-Sleep -Seconds 8

$live = $false
try { $live = ((Invoke-WebRequest 'http://127.0.0.1:4381/speed' -TimeoutSec 4 -UseBasicParsing).Content -match '"hooked":true') } catch {}
if ($live) { Ok 'Engine is live and hooked into Spotify' }
else { Warn 'Engine has not reported in yet - it usually catches up within a few seconds' }

Write-Host
Ok 'SpotiSpeed is installed.'
Write-Host
Say "The knob sits just right of the play/pause controls in the footer:" 'White'
Say '   drag up / down .... change speed (0.2x - 2.0x)'
Say '   click ............. reset to 1x'
Say '   scroll wheel ...... fine steps   (hold Shift for finer)'
Write-Host
Say 'Pitch follows speed, so it sounds like a record slowing down or speeding up.'
Say 'It comes back on its own after a reboot, and Spotify can no longer auto-update.'
Write-Host
Read-Host '  Press Enter to close'
'@

# ------------------------------------------------------------------ emit ----
$bat = @"
@echo off
setlocal EnableExtensions
title SpotiSpeed Setup
color 0A
echo.
echo    SpotiSpeed - playback speed knob for Spotify desktop
echo    ===================================================
echo.
echo    This installs a speed knob into Spotify's footer.
echo    Nothing is uploaded anywhere; everything runs on this PC.
echo.
set "SSPS=%TEMP%\spotispeed_setup.ps1"
set "SSBAT=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "`$r=[IO.File]::ReadAllText(`$env:SSBAT); `$m=[regex]::Match(`$r,'(?s)#PSBEGIN#\r?\n(.*?)\r?\n#PSEND#'); if(-not `$m.Success){exit 9}; [IO.File]::WriteAllText(`$env:SSPS,`$m.Groups[1].Value)"
if errorlevel 1 (
  echo    [x] This file looks corrupted - re-download it and keep it as a single .bat
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%SSPS%" "%SSBAT%"
del /F /Q "%SSPS%" >nul 2>&1
exit /b 0

:: ==========================================================================
::  Everything below is data. cmd never reads past the exit above.
:: ==========================================================================
#PSBEGIN#
$installer
#PSEND#
#DLL#$dll#/DLL#
#EXE#$exe#/EXE#
#JS#$js#/JS#
"@

$out = Join-Path $root 'SpotiSpeed-Setup.bat'
[IO.File]::WriteAllText($out, $bat, (New-Object Text.UTF8Encoding($false)))
$kb = [math]::Round((Get-Item $out).Length / 1KB)
Write-Host "wrote $out  ($kb KB)" -ForegroundColor Green
