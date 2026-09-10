# Bakes the engine, watcher, knob script and CDP injector into ONE
# self-contained .bat that can be handed to anyone. Run after build.bat.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

function B64([string]$p) {
    if (-not (Test-Path $p)) { throw "missing payload: $p" }
    [Convert]::ToBase64String([IO.File]::ReadAllBytes($p))
}

$dll = B64 (Join-Path $root 'bin\spotispeed.dll')
$exe = B64 (Join-Path $root 'bin\ssinject.exe')
$js  = B64 (Join-Path $root 'ext\spotispeed.js')
$ps1 = B64 (Join-Path $root 'ext\spotispeed-cdp.ps1')

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
$RUNKEY = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$FLAGS  = '--remote-debugging-port=4380 --remote-allow-origins=*'

# --- 1. Spotify ------------------------------------------------------------
if (-not (Test-Path $SPOT)) {
    Die "Spotify desktop not found.`n      Install the normal installer from spotify.com (NOT the Microsoft Store version), log in once, then run this again."
}
Ok ("Found Spotify " + (Get-Item $SPOT).VersionInfo.FileVersion)

# --- 2. stop everything ----------------------------------------------------
Say '[*] Closing Spotify...'
Get-Process Spotify, ssinject -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -match 'spotispeed-cdp|guardian\.ps1' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 3

# --- 3. unpack -------------------------------------------------------------
Say '[*] Installing audio engine and knob...'
New-Item -ItemType Directory -Force $HOME_ | Out-Null
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'spotispeed.dll'),     (Payload 'DLL'))
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'ssinject.exe'),       (Payload 'EXE'))
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'spotispeed.js'),      (Payload 'JS'))
[IO.File]::WriteAllBytes((Join-Path $HOME_ 'spotispeed-cdp.ps1'), (Payload 'PS1'))
Remove-Item (Join-Path $HOME_ 'guardian.ps1') -Force -ErrorAction SilentlyContinue
Ok "Files installed to $HOME_"

# --- 4. patch Spotify launch flags -----------------------------------------
# The 1.2.99+ Spotify serves its UI from xpui.app.spotify.com and ignores the
# local xpui folder, so the old Spicetify extension slot never runs. The knob
# gets injected through Spotify's DevTools port instead - which only works if
# Spotify is launched with --remote-debugging-port. Patch every shortcut and
# the autostart Run key so any launch path gets the flags.
function AddFlags([string]$existing) {
    $a = $existing
    if ($a -notmatch '--remote-debugging-port=')  { $a = ($a + ' --remote-debugging-port=4380').Trim() }
    if ($a -notmatch '--remote-allow-origins=')   { $a = ($a + ' --remote-allow-origins=*').Trim() }
    return $a
}
function PatchLnk([string]$lnkPath) {
    if (-not (Test-Path $lnkPath)) { return $false }
    try {
        $sh = New-Object -ComObject WScript.Shell
        $l = $sh.CreateShortcut($lnkPath)
        if ([string]::IsNullOrEmpty($l.TargetPath)) { return $false }
        if (-not ($l.TargetPath -match 'Spotify\.exe$|SpotifyLauncher\.exe$')) { return $false }
        # SpotifyLauncher.exe swallows the flags; retarget those shortcuts at
        # Spotify.exe directly so the flags actually reach the browser process.
        if ($l.TargetPath -match 'SpotifyLauncher\.exe$') { $l.TargetPath = $SPOT }
        $l.Arguments = AddFlags $l.Arguments
        $l.Save()
        return $true
    } catch { return $false }
}
Say '[*] Adding DevTools port to Spotify launch shortcuts...'
$lnks = @(
    (Join-Path $env:APPDATA  'Microsoft\Windows\Start Menu\Programs\Spotify.lnk')
    (Join-Path $env:PUBLIC   'Desktop\Spotify.lnk')
    (Join-Path $env:USERPROFILE 'Desktop\Spotify.lnk')
    (Join-Path $env:APPDATA  'Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar\Spotify.lnk')
    (Join-Path $env:APPDATA  'Microsoft\Internet Explorer\Quick Launch\Spotify.lnk')
)
$hit = 0
foreach ($p in $lnks) { if (PatchLnk $p) { $hit++ } }
if ($hit -gt 0) { Ok "Patched $hit Spotify shortcut(s)" } else { Warn 'No Spotify shortcuts found to patch (start menu tile may still need manual restart)' }

# If Spotify's own autostart is registered, patch it too.
try {
    $sp = (Get-ItemProperty -Path $RUNKEY -Name 'Spotify' -ErrorAction SilentlyContinue).Spotify
    if ($sp) {
        # split "exe" args
        $m = [regex]::Match($sp, '^\s*("([^"]+)"|(\S+))\s*(.*)$')
        if ($m.Success) {
            $exePart = if ($m.Groups[2].Success) { '"' + $m.Groups[2].Value + '"' } else { $m.Groups[3].Value }
            $args    = $m.Groups[4].Value
            $newVal  = ($exePart + ' ' + (AddFlags $args)).Trim()
            if ($newVal -ne $sp) {
                Set-ItemProperty -Path $RUNKEY -Name 'Spotify' -Value $newVal -Force
                Ok 'Patched Spotify autostart entry'
            }
        }
    }
} catch {}

# --- 5. freeze the client --------------------------------------------------
Say '[*] Blocking Spotify auto-update...'
try {
    if (Test-Path -LiteralPath $UPD) { icacls $UPD /reset 2>&1 | Out-Null }
    if (Test-Path -LiteralPath $UPD -PathType Container) { Remove-Item -LiteralPath $UPD -Recurse -Force }
    if (Test-Path -LiteralPath $UPD -PathType Leaf) {
        (Get-Item -LiteralPath $UPD -Force).Attributes = 'Normal'
        Remove-Item -LiteralPath $UPD -Force
    }
    New-Item -ItemType File -Path $UPD -Force | Out-Null
    (Get-Item -LiteralPath $UPD -Force).Attributes = 'ReadOnly, Hidden'
} catch { }
$blocked = (Test-Path -LiteralPath $UPD -PathType Leaf)
if ($blocked) { Ok 'Auto-update blocked (this Spotify build is now frozen)' }
else { Warn 'Could not block updates - Spotify may replace itself later' }

# --- 6. our autostart ------------------------------------------------------
Say '[*] Setting up autostart...'
$watchCmd = '"' + (Join-Path $HOME_ 'ssinject.exe') + '" --watch "' + (Join-Path $HOME_ 'spotispeed.dll') + '"'
try {
    New-Item -Path $RUNKEY -Force -ErrorAction SilentlyContinue | Out-Null
    Set-ItemProperty -Path $RUNKEY -Name 'SpotiSpeed' -Value $watchCmd -Force
    Remove-Item (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup\SpotiSpeed.vbs') -Force -ErrorAction SilentlyContinue
    schtasks /Delete /TN 'SpotiSpeed' /F 2>&1 | Out-Null
    $back = (Get-ItemProperty -Path $RUNKEY -Name 'SpotiSpeed' -ErrorAction Stop).SpotiSpeed
    if ($back -eq $watchCmd) { Ok 'Will start automatically at logon' }
    else { Warn 'Autostart entry did not verify' }
} catch { Warn "Could not register autostart: $($_.Exception.Message)" }

# --- 7. go -----------------------------------------------------------------
Say '[*] Starting engine and Spotify...'
Start-Process -FilePath (Join-Path $HOME_ 'ssinject.exe') `
              -ArgumentList '--watch', ('"' + (Join-Path $HOME_ 'spotispeed.dll') + '"')
Start-Sleep -Seconds 2
Start-Process -FilePath $SPOT -ArgumentList '--remote-debugging-port=4380','--remote-allow-origins=*'
Start-Sleep -Seconds 10

$live = $false
try { $live = ((Invoke-WebRequest 'http://127.0.0.1:4381/speed' -TimeoutSec 4 -UseBasicParsing).Content -match '"hooked":true') } catch {}
if ($live) { Ok 'Engine is live and hooked into Spotify' }
else { Warn 'Engine has not reported in yet - it usually catches up within a few seconds' }

$knob = $false
try {
    $t = Invoke-RestMethod 'http://127.0.0.1:4380/json' -TimeoutSec 3 -ErrorAction Stop
    $knob = ($t | Where-Object { $_.type -eq 'page' -and $_.url -match 'xpui' }).Count -gt 0
} catch {}
if ($knob) { Ok 'Spotify DevTools port responding - knob will appear shortly' }
else { Warn 'DevTools port did not answer - if the knob does not appear, close Spotify fully and open it from the Start menu' }

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
#PS1#$ps1#/PS1#
"@

$out = Join-Path $root 'SpotiSpeed-Setup.bat'
[IO.File]::WriteAllText($out, $bat, (New-Object Text.UTF8Encoding($false)))
$kb = [math]::Round((Get-Item $out).Length / 1KB)
Write-Host "wrote $out  ($kb KB)" -ForegroundColor Green
