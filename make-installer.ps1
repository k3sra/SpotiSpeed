# Bakes the engine, injector, knob and guardian into ONE self-contained .bat
# that can be handed to anyone. Run this after build.bat.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function B64([string]$p) {
    if (-not (Test-Path $p)) { throw "missing payload: $p" }
    [Convert]::ToBase64String([IO.File]::ReadAllBytes($p))
}

$dll   = B64 (Join-Path $root 'bin\spotispeed.dll')
$exe   = B64 (Join-Path $root 'bin\ssinject.exe')
$js    = B64 (Join-Path $root 'ext\spotispeed.js')
$guard = B64 (Join-Path $root 'guardian.ps1')

# ---------------------------------------------------------------- installer --
$installer = @'
param([Parameter(Mandatory=$true)][string]$SelfBat)

$ErrorActionPreference = 'Continue'
function Say($m, $c = 'Gray') { Write-Host "  $m" -ForegroundColor $c }
function Ok ($m) { Say "[+] $m" 'Green' }
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

# --- 1. Spotify ------------------------------------------------------------
if (-not (Test-Path $SPOT)) {
    Die "Spotify desktop not found.`n      Install the normal installer from spotify.com (NOT the Microsoft Store version), log in once, then run this again."
}
$ver = (Get-Item $SPOT).VersionInfo.FileVersion
Ok "Found Spotify $ver"

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
Ok "Spicetify ready"

# --- 3. stop everything ----------------------------------------------------
Say '[*] Closing Spotify...'
schtasks /End /TN 'SpotiSpeed' 2>&1 | Out-Null
Get-Process Spotify, ssinject -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3

# --- 4. unpack -------------------------------------------------------------
Say '[*] Installing audio engine...'
New-Item -ItemType Directory -Force $HOME_ | Out-Null
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'spotispeed.dll'), (Payload 'DLL'))
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'ssinject.exe'),   (Payload 'EXE'))
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'spotispeed.js'),  (Payload 'JS'))
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'guardian.ps1'),   (Payload 'GUARD'))
Ok "Engine installed to $HOME_"

# --- 5. the knob -----------------------------------------------------------
Say '[*] Adding the knob to Spotify''s footer...'
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
    if (Test-Path -LiteralPath $UPD -PathType Container) { Remove-Item -LiteralPath $UPD -Recurse -Force }
    if (Test-Path -LiteralPath $UPD) { (Get-Item -LiteralPath $UPD -Force).Attributes = 'Normal'; Remove-Item -LiteralPath $UPD -Force }
    New-Item -ItemType File -Path $UPD -Force | Out-Null
    (Get-Item -LiteralPath $UPD -Force).Attributes = 'ReadOnly, Hidden'
    # deny writes/deletes outright so the updater cannot clear the blocker
    $me = "$env:USERDOMAIN\$env:USERNAME"
    icacls $UPD /inheritance:r 2>&1 | Out-Null
    icacls $UPD /grant:r "${me}:(R)" 2>&1 | Out-Null
    icacls $UPD /deny  "${me}:(W,D,DC,WDAC,WO)" 2>&1 | Out-Null
    Ok 'Auto-update blocked (this Spotify build is now frozen)'
} catch { Warn "Could not fully block updates: $($_.Exception.Message)" }

# --- 7. autostart ----------------------------------------------------------
# Startup folder rather than a scheduled task: same result, no admin rights,
# and the user can see and delete it.
Say '[*] Setting up autostart...'
$guardCmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$HOME_\guardian.ps1`""
$startup = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup'
New-Item -ItemType Directory -Force $startup | Out-Null
$vbs = Join-Path $startup 'SpotiSpeed.vbs'
$line = 'CreateObject("WScript.Shell").Run "' + $guardCmd.Replace('"', '""') + '", 0, False'
[IO.File]::WriteAllText($vbs, $line)
if (Test-Path $vbs) { Ok 'Will start automatically at logon' }
else { Warn 'Could not register autostart - run the setup again after each reboot' }

# --- 8. go -----------------------------------------------------------------
Say '[*] Starting engine and Spotify...'
Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-WindowStyle','Hidden','-File',"$HOME_\guardian.ps1" -WindowStyle Hidden
Start-Sleep -Seconds 2
Start-Process $SPOT

Write-Host
Ok 'SpotiSpeed is installed.'
Write-Host
Say 'The knob sits just right of the play/pause controls in the footer:' 'White'
Say '   drag up / down .... change speed (0.2x - 2.0x)'
Say '   click ............. reset to 1x'
Say '   scroll wheel ...... fine steps   (hold Shift for finer)'
Write-Host
Say 'Pitch follows speed, so it sounds like a record slowing down or speeding up.'
Say 'It restores itself at every logon, and Spotify can no longer auto-update.'
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
#GUARD#$guard#/GUARD#
"@

$out = Join-Path $root 'SpotiSpeed-Setup.bat'
[IO.File]::WriteAllText($out, $bat, (New-Object Text.UTF8Encoding($false)))
$kb = [math]::Round((Get-Item $out).Length / 1KB)
Write-Host "wrote $out  ($kb KB)" -ForegroundColor Green
