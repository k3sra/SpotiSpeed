# SpotiSpeed guardian.
#
# Runs quietly at logon and keeps three things true:
#   1. the audio engine is injected into Spotify whenever Spotify is running
#   2. Spotify cannot update itself out from under the patch
#   3. the footer knob is still present in Spotify's UI bundle
#
# It is deliberately conservative: every action is idempotent, and a failure in
# one check never stops the others.

$ErrorActionPreference = 'SilentlyContinue'

# One guardian per session. Re-running the installer, or logging back in without
# a full reboot, would otherwise leave several copies fighting over the same files.
$mutex = New-Object System.Threading.Mutex($false, 'Local\SpotiSpeedGuardian')
if (-not $mutex.WaitOne(0)) { exit }

$Home_    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Dll      = Join-Path $Home_ 'spotispeed.dll'
$Inject   = Join-Path $Home_ 'ssinject.exe'
$ExtSrc   = Join-Path $Home_ 'spotispeed.js'
$UpdateP  = Join-Path $env:LOCALAPPDATA 'Spotify\Update'
$Spicetify = Join-Path $env:LOCALAPPDATA 'spicetify\spicetify.exe'

function Block-SpotifyUpdates {
    # Spotify stages updates into %LOCALAPPDATA%\Spotify\Update. If a read-only
    # *file* sits on that path, the directory can never be created, so the
    # updater has nowhere to unpack and quietly gives up.
    if (Test-Path -LiteralPath $UpdateP -PathType Container) {
        Remove-Item -LiteralPath $UpdateP -Recurse -Force
    }
    if (-not (Test-Path -LiteralPath $UpdateP)) {
        New-Item -ItemType File -Path $UpdateP -Force | Out-Null
    }
    $f = Get-Item -LiteralPath $UpdateP -Force
    if ($f -and -not $f.Attributes.ToString().Contains('ReadOnly')) {
        $f.Attributes = 'ReadOnly, Hidden'
    }
}

function Ensure-Injector {
    if (-not (Get-Process -Name ssinject -ErrorAction SilentlyContinue)) {
        if ((Test-Path $Inject) -and (Test-Path $Dll)) {
            Start-Process -FilePath $Inject -ArgumentList '--watch', "`"$Dll`"" -WindowStyle Hidden
        }
    }
}

function Ensure-Knob {
    # If Spotify ever replaces its UI bundle (reinstall / forced update), the
    # Spicetify patch is lost. Detect that and re-apply.
    $index = Join-Path $env:APPDATA 'Spotify\Apps\xpui\index.html'
    if (-not (Test-Path $index)) { return }          # bundle still packed as .spa
    $html = Get-Content -LiteralPath $index -Raw
    if ($html -and $html.Contains('spicetifyWrapper.js')) { return }   # still patched

    if (Test-Path $Spicetify) {
        $extDir = Join-Path $env:APPDATA 'spicetify\Extensions'
        if ((Test-Path $ExtSrc) -and (Test-Path $extDir)) {
            Copy-Item $ExtSrc $extDir -Force
        }
        # A fresh Spotify bundle needs a fresh backup before it can be patched.
        & $Spicetify backup apply | Out-Null
    }
}

while ($true) {
    try { Block-SpotifyUpdates } catch {}
    try { Ensure-Injector }      catch {}
    try { Ensure-Knob }          catch {}
    Start-Sleep -Seconds 45
}
