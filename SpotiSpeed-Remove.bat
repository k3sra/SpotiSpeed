@echo off
setlocal EnableExtensions
title SpotiSpeed Remover
color 0E
echo.
echo    SpotiSpeed - remove
echo    ===================
echo.

set "HOME_=%LOCALAPPDATA%\SpotiSpeed"
set "SPICE=%LOCALAPPDATA%\spicetify\spicetify.exe"
set "UPD=%LOCALAPPDATA%\Spotify\Update"

echo    [*] Stopping engine and Spotify...
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v SpotiSpeed /f >nul 2>&1
del /F /Q "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\SpotiSpeed.vbs" >nul 2>&1
schtasks /Delete /TN "SpotiSpeed" /F >nul 2>&1
taskkill /F /IM ssinject.exe >nul 2>&1
taskkill /F /IM Spotify.exe  >nul 2>&1

rem stop the CDP knob injector (a background powershell running our script)
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='powershell.exe'\" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match 'spotispeed-cdp' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
ping 127.0.0.1 -n 4 >nul

echo    [*] Removing DevTools flags from Spotify shortcuts...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$s=New-Object -ComObject WScript.Shell;" ^
  "@((Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Spotify.lnk')," ^
  "  (Join-Path $env:PUBLIC 'Desktop\Spotify.lnk')," ^
  "  (Join-Path $env:USERPROFILE 'Desktop\Spotify.lnk')," ^
  "  (Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar\Spotify.lnk')," ^
  "  (Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\Spotify.lnk'))" ^
  " | Where-Object { Test-Path $_ } | ForEach-Object {" ^
  "    try { $l=$s.CreateShortcut($_); $l.Arguments = ($l.Arguments -replace '--remote-debugging-port=\d+','' -replace '--remote-allow-origins=\S+','').Trim(); $l.Save() } catch {}" ^
  "  }" >nul 2>&1

echo    [*] Removing the knob (spicetify path, if used)...
if exist "%SPICE%" (
  "%SPICE%" config extensions spotispeed.js- >nul 2>&1
  "%SPICE%" apply >nul 2>&1
)
del /F /Q "%APPDATA%\spicetify\Extensions\spotispeed.js" >nul 2>&1

echo    [*] Re-enabling Spotify updates...
icacls "%UPD%" /reset >nul 2>&1
attrib -r -h "%UPD%" >nul 2>&1
del /F /Q "%UPD%" >nul 2>&1

echo    [*] Deleting files...
rmdir /S /Q "%HOME_%" >nul 2>&1

echo.
echo    [+] Done. Spotify is back to normal - just launch it as usual.
echo.
pause
