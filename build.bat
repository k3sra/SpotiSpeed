@echo off
setlocal
set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [!] could not initialise MSVC x64 environment & exit /b 1 )
cd /d "%~dp0"
if not exist bin mkdir bin

set EXTRA=
if /I "%~1"=="debug" set EXTRA=/DSS_DEBUG

echo [*] compiling spotispeed.dll %EXTRA%
cl /nologo /LD /O2 /EHsc /MT /std:c++17 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN %EXTRA% ^
   /Fe:bin\spotispeed.dll /Fo:bin\ /Fd:bin\ ^
   src\spotispeed.cpp ^
   /link /OUT:bin\spotispeed.dll ole32.lib ws2_32.lib
if errorlevel 1 ( echo [!] DLL build FAILED & exit /b 1 )

echo [*] compiling injector
cl /nologo /O2 /EHsc /MT /std:c++17 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fe:bin\ssinject.exe /Fo:bin\ /Fd:bin\ ^
   src\inject.cpp /link /OUT:bin\ssinject.exe /SUBSYSTEM:WINDOWS ^
   user32.lib shlwapi.lib shell32.lib advapi32.lib ole32.lib
if errorlevel 1 ( echo [!] injector build FAILED & exit /b 1 )
echo [+] build OK
