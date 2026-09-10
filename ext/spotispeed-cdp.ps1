# SpotiSpeed CDP knob injector.
# Spotify 1.2.99+ serves its UI from xpui.app.spotify.com and ignores the local
# xpui folder, so the Spicetify extension slot never fires. This connects to
# Spotify's DevTools port (added to its launch args) and installs the knob into
# every page target via Page.addScriptToEvaluateOnNewDocument + Runtime.evaluate.

$ErrorActionPreference = 'Continue'
$scriptFile = "$env:LOCALAPPDATA\SpotiSpeed\spotispeed.js"
$logFile    = "$env:LOCALAPPDATA\SpotiSpeed\cdp.log"
$port       = 4380

function Log([string]$m) {
    try {
        if ((Test-Path $logFile) -and (Get-Item $logFile).Length -gt 262144) {
            Remove-Item $logFile -Force -ErrorAction SilentlyContinue
        }
        Add-Content -Path $logFile -Value ("{0} {1}" -f (Get-Date -Format 'HH:mm:ss'), $m)
    } catch {}
}

# ConvertTo-Json truncates or mangles long source strings, so hand-roll the
# JSON strings we send. Only two field types matter: plain identifiers and one
# big source blob.
function JsonEsc([string]$s) {
    if ($null -eq $s) { return '""' }
    $sb = New-Object System.Text.StringBuilder
    $null = $sb.Append('"')
    for ($i = 0; $i -lt $s.Length; $i++) {
        $c = $s[$i]
        switch ($c) {
            '\'   { $null = $sb.Append('\\'); continue }
            '"'   { $null = $sb.Append('\"'); continue }
            "`b"  { $null = $sb.Append('\b'); continue }
            "`f"  { $null = $sb.Append('\f'); continue }
            "`n"  { $null = $sb.Append('\n'); continue }
            "`r"  { $null = $sb.Append('\r'); continue }
            "`t"  { $null = $sb.Append('\t'); continue }
            default {
                $u = [int]$c
                if ($u -lt 0x20) { $null = $sb.AppendFormat('\u{0:x4}', $u) }
                else             { $null = $sb.Append($c) }
            }
        }
    }
    $null = $sb.Append('"')
    return $sb.ToString()
}

Log "cdp injector started"

while ($true) {
    $ws = $null
    try {
        if (-not (Test-Path $scriptFile)) { Start-Sleep 5; continue }
        $script = Get-Content $scriptFile -Raw
        $escScript = JsonEsc $script

        $ver = Invoke-RestMethod "http://127.0.0.1:$port/json/version" -TimeoutSec 3
        $browserWs = $ver.webSocketDebuggerUrl
        if (-not $browserWs) { Start-Sleep 3; continue }

        $ws = New-Object System.Net.WebSockets.ClientWebSocket
        $ws.Options.SetRequestHeader('Origin', 'http://localhost') | Out-Null
        $cts = New-Object System.Threading.CancellationTokenSource
        $cts.CancelAfter(5000)
        $ws.ConnectAsync([Uri]$browserWs, $cts.Token).GetAwaiter().GetResult() | Out-Null
        $cts.Dispose()
        Log "connected"

        $script:sendId = 0
        $sendRaw = {
            param([string]$json)
            $b = [System.Text.Encoding]::UTF8.GetBytes($json)
            $seg = New-Object 'System.ArraySegment[byte]' -ArgumentList (,$b)
            $ws.SendAsync($seg, 'Text', $true, [System.Threading.CancellationToken]::None).GetAwaiter().GetResult() | Out-Null
        }
        $recvMsg = {
            $buf = New-Object byte[] 131072
            $seg = New-Object 'System.ArraySegment[byte]' -ArgumentList (,$buf)
            $sb  = New-Object System.Text.StringBuilder
            do {
                $r = $ws.ReceiveAsync($seg, [System.Threading.CancellationToken]::None).GetAwaiter().GetResult()
                if ($r.MessageType -eq 'Close') { return $null }
                $null = $sb.Append([System.Text.Encoding]::UTF8.GetString($buf, 0, $r.Count))
            } while (-not $r.EndOfMessage)
            return $sb.ToString()
        }
        function NextId { $script:sendId++; return $script:sendId }

        & $sendRaw ('{"id":' + (NextId) + ',"method":"Target.setDiscoverTargets","params":{"discover":true}}')
        & $sendRaw ('{"id":' + (NextId) + ',"method":"Target.setAutoAttach","params":{"autoAttach":true,"waitForDebuggerOnStart":false,"flatten":true}}')

        $seen = @{}
        while ($ws.State -eq 'Open') {
            $raw = & $recvMsg
            if (-not $raw) { break }
            try { $ev = $raw | ConvertFrom-Json } catch { continue }

            if ($ev.method -eq 'Target.attachedToTarget' -and
                $ev.params -and $ev.params.targetInfo -and
                $ev.params.targetInfo.type -eq 'page') {

                $sid = $ev.params.sessionId
                $url = [string]$ev.params.targetInfo.url
                if ($url -notmatch 'xpui|spotify') { continue }
                if ($seen.ContainsKey($sid)) { continue }
                $seen[$sid] = $true
                Log ("attach {0}" -f $url)

                $sidStr = JsonEsc $sid
                & $sendRaw ('{"id":' + (NextId) + ',"sessionId":' + $sidStr + ',"method":"Page.enable"}')
                & $sendRaw ('{"id":' + (NextId) + ',"sessionId":' + $sidStr + ',"method":"Runtime.enable"}')
                & $sendRaw ('{"id":' + (NextId) + ',"sessionId":' + $sidStr + ',"method":"Page.addScriptToEvaluateOnNewDocument","params":{"source":' + $escScript + '}}')
                & $sendRaw ('{"id":' + (NextId) + ',"sessionId":' + $sidStr + ',"method":"Runtime.evaluate","params":{"expression":' + $escScript + ',"awaitPromise":false,"returnByValue":true}}')
            }
            elseif ($ev.method -eq 'Target.detachedFromTarget' -and $ev.params) {
                $sid = $ev.params.sessionId
                if ($seen.ContainsKey($sid)) { $seen.Remove($sid) | Out-Null }
            }
        }
        Log "socket closed, reconnecting"
    } catch {
        # quiet: connect refused / Spotify not running yet
    } finally {
        if ($ws) { try { $ws.Dispose() } catch {} }
    }
    Start-Sleep 3
}
