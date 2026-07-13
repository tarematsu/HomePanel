[CmdletBinding()]
param(
  [string]$ChromePath,
  [string]$ProfileDir = (Join-Path $env:LOCALAPPDATA "HomePanel\StationheadCaptureProfile"),
  [string]$OutDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "native\data\stationhead-capture"),
  [string]$Url = "https://stationhead.com/c/buddies",
  [int]$DebugPort = 9222,
  [int]$DurationSeconds = 300,
  [switch]$IncludeAllResourceTypes
)
# Launches Chrome with a dedicated profile + remote debugging, attaches to the
# Stationhead tab over the Chrome DevTools Protocol, and records every XHR/Fetch
# request+response body and WebSocket frame to a JSONL file. First run: log in
# to Stationhead in the opened window (the profile persists for later runs).
#
# WARNING: the output contains your live Stationhead session (cookies, auth
# tokens, buddy/room data). It is written under native/data/ which is
# git-ignored - do not commit it or paste it anywhere public.

$ErrorActionPreference = "Stop"

function Resolve-ChromePath {
  param([string]$Explicit)
  if ($Explicit) { return $Explicit }
  $candidates = @(
    "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
    "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
    "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe"
  )
  foreach ($c in $candidates) {
    if (Test-Path $c) { return $c }
  }
  throw "Chrome executable not found. Pass -ChromePath explicitly."
}

Add-Type -AssemblyName System.Net.WebSockets.Client -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.Net.Http -ErrorAction SilentlyContinue

$chrome = Resolve-ChromePath -Explicit $ChromePath
New-Item -ItemType Directory -Force -Path $ProfileDir | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outFile = Join-Path $OutDir "capture-$stamp.jsonl"

Write-Host "Chrome:      $chrome"
Write-Host "Profile dir: $ProfileDir  (persists your Stationhead login between runs)"
Write-Host "Output file: $outFile"
Write-Host ""
Write-Host "If this is the first run, log in to Stationhead in the window that opens."
Write-Host "Capturing for $DurationSeconds seconds once the tab is found..."
Write-Host ""

$chromeArgs = @(
  "--remote-debugging-port=$DebugPort",
  "--user-data-dir=$ProfileDir",
  "--no-first-run",
  "--no-default-browser-check",
  $Url
)
$proc = Start-Process -FilePath $chrome -ArgumentList $chromeArgs -PassThru

$targetInfo = $null
for ($i = 0; $i -lt 60; $i++) {
  Start-Sleep -Milliseconds 500
  try {
    $list = Invoke-RestMethod -Uri "http://127.0.0.1:$DebugPort/json/list" -TimeoutSec 2
    $targetInfo = $list | Where-Object { $_.type -eq "page" -and $_.url -like "*stationhead.com*" } | Select-Object -First 1
    if ($targetInfo) { break }
  } catch { }
}
if (-not $targetInfo) {
  throw "Could not find the Stationhead tab via the Chrome DevTools protocol on port $DebugPort."
}

$wsUrl = $targetInfo.webSocketDebuggerUrl
Write-Host "Attached to tab: $($targetInfo.url)"

$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$connectCts = [System.Threading.CancellationTokenSource]::new(5000)
$ws.ConnectAsync([Uri]$wsUrl, $connectCts.Token).GetAwaiter().GetResult()

$script:msgId = 0
function Send-Cdp {
  param([string]$Method, [hashtable]$Params = @{}, [System.Threading.CancellationToken]$Token)
  $script:msgId++
  $payload = @{ id = $script:msgId; method = $Method; params = $Params } | ConvertTo-Json -Depth 10 -Compress
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
  $segment = [ArraySegment[byte]]::new($bytes)
  $ws.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $Token).GetAwaiter().GetResult() | Out-Null
  return $script:msgId
}

function Receive-CdpMessage {
  param([System.Threading.CancellationToken]$Token)
  $buffer = [byte[]]::new(1MB)
  $segment = [ArraySegment[byte]]::new($buffer)
  $sb = [System.Text.StringBuilder]::new()
  do {
    $result = $ws.ReceiveAsync($segment, $Token).GetAwaiter().GetResult()
    if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { return $null }
    $sb.Append([System.Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count)) | Out-Null
  } while (-not $result.EndOfMessage)
  return $sb.ToString()
}

$runCts = [System.Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds($DurationSeconds))
$token = $runCts.Token

Send-Cdp -Method "Network.enable" -Token $token | Out-Null
Send-Cdp -Method "Page.enable" -Token $token | Out-Null

$requests = @{}         # requestId -> request info (url/method/headers/postData/type)
$responses = @{}        # requestId -> response info (status/mimeType/headers)
$pendingBodyCmd = @{}   # cdp command id -> requestId

function Test-InterestingType {
  param([string]$Type)
  if ($IncludeAllResourceTypes) { return $true }
  return $Type -in @("XHR", "Fetch", "WebSocket", "EventSource", "Document")
}

$writer = [System.IO.StreamWriter]::new($outFile, $false, [System.Text.Encoding]::UTF8)
$capturedCount = 0

try {
  while ($true) {
    $raw = $null
    try {
      $raw = Receive-CdpMessage -Token $token
    } catch [System.OperationCanceledException] {
      break
    }
    if (-not $raw) { continue }

    $msg = $raw | ConvertFrom-Json -Depth 20

    if ($msg.id -and $pendingBodyCmd.ContainsKey([string]$msg.id)) {
      $reqId = $pendingBodyCmd[[string]$msg.id]
      $pendingBodyCmd.Remove([string]$msg.id)
      $req = $requests[$reqId]
      $resp = $responses[$reqId]
      if ($req) {
        $entry = [ordered]@{
          kind              = "http"
          requestId         = $reqId
          method            = $req.method
          url               = $req.url
          resourceType      = $req.type
          requestHeaders    = $req.headers
          postData          = $req.postData
          status            = $resp.status
          mimeType          = $resp.mimeType
          responseHeaders   = $resp.headers
          bodyBase64Encoded = $msg.result.base64Encoded
          body              = $msg.result.body
          capturedAt        = (Get-Date).ToString("o")
        }
        $writer.WriteLine(($entry | ConvertTo-Json -Depth 10 -Compress))
        $writer.Flush()
        $capturedCount++
        Write-Host "[$($resp.status)] $($req.method) $($req.url)"
      }
      $requests.Remove($reqId) | Out-Null
      $responses.Remove($reqId) | Out-Null
      continue
    }

    switch ($msg.method) {
      "Network.requestWillBeSent" {
        $p = $msg.params
        $requests[$p.requestId] = @{
          url      = $p.request.url
          method   = $p.request.method
          headers  = $p.request.headers
          postData = $p.request.postData
          type     = $p.type
        }
      }
      "Network.responseReceived" {
        $p = $msg.params
        $responses[$p.requestId] = @{
          status   = $p.response.status
          mimeType = $p.response.mimeType
          headers  = $p.response.headers
        }
      }
      "Network.loadingFinished" {
        $p = $msg.params
        $reqId = $p.requestId
        if ($requests.ContainsKey($reqId) -and (Test-InterestingType $requests[$reqId].type)) {
          $cmdId = Send-Cdp -Method "Network.getResponseBody" -Params @{ requestId = $reqId } -Token $token
          $pendingBodyCmd[[string]$cmdId] = $reqId
        } else {
          $requests.Remove($reqId) | Out-Null
          $responses.Remove($reqId) | Out-Null
        }
      }
      "Network.loadingFailed" {
        $requests.Remove($msg.params.requestId) | Out-Null
        $responses.Remove($msg.params.requestId) | Out-Null
      }
      "Network.webSocketCreated" {
        $entry = [ordered]@{
          kind       = "websocket_created"
          requestId  = $msg.params.requestId
          url        = $msg.params.url
          capturedAt = (Get-Date).ToString("o")
        }
        $writer.WriteLine(($entry | ConvertTo-Json -Depth 10 -Compress))
        $writer.Flush()
        Write-Host "[WS] connected: $($msg.params.url)"
      }
      "Network.webSocketFrameSent" {
        $entry = [ordered]@{
          kind        = "websocket_sent"
          requestId   = $msg.params.requestId
          payloadData = $msg.params.response.payloadData
          capturedAt  = (Get-Date).ToString("o")
        }
        $writer.WriteLine(($entry | ConvertTo-Json -Depth 10 -Compress))
        $writer.Flush()
        $capturedCount++
      }
      "Network.webSocketFrameReceived" {
        $entry = [ordered]@{
          kind        = "websocket_received"
          requestId   = $msg.params.requestId
          payloadData = $msg.params.response.payloadData
          capturedAt  = (Get-Date).ToString("o")
        }
        $writer.WriteLine(($entry | ConvertTo-Json -Depth 10 -Compress))
        $writer.Flush()
        $capturedCount++
      }
      default { }
    }
  }
} finally {
  $writer.Flush()
  $writer.Close()
  try { $ws.Dispose() } catch { }
}

Write-Host ""
Write-Host "Done. Captured $capturedCount entries."
Write-Host "Saved to: $outFile"
Write-Host "Chrome (PID $($proc.Id)) was left running - close it manually when you're done browsing."
Write-Host ""
Write-Host "This file contains your live session (cookies/tokens) and other users' buddy/room data -"
Write-Host "keep it out of git. Send it to Claude directly (not via a commit) for analysis."
