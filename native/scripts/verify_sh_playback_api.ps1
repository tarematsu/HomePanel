$ErrorActionPreference = 'Stop'

$logDir = Join-Path $PSScriptRoot '..\ci-logs'
New-Item -ItemType Directory -Force $logDir | Out-Null
$logPath = Join-Path $logDir 'stationhead-playback-api.log'
$lines = New-Object System.Collections.Generic.List[string]
$url = 'https://skrzk.pages.dev/api/dashboard?history=0'

try {
  $response = Invoke-WebRequest -Uri $url -Headers @{ Origin = 'https://app.homepanel'; Accept = 'application/json' } -UseBasicParsing -TimeoutSec 20
  $contentType = [string]$response.Headers['Content-Type']
  $cors = [string]$response.Headers['Access-Control-Allow-Origin']
  $lines.Add("status=$($response.StatusCode) contentType=$contentType cors=$cors")
  if ($response.StatusCode -ne 200) {
    throw "HTTP $($response.StatusCode)"
  }
  if ($contentType -notmatch 'application/json') {
    throw "unexpected content type '$contentType'"
  }
  if ($cors -ne '*' -and $cors -ne 'https://app.homepanel') {
    throw "CORS does not allow https://app.homepanel; Access-Control-Allow-Origin='$cors'"
  }

  $json = $response.Content | ConvertFrom-Json
  if ($json.ok -ne $true) {
    throw "dashboard JSON returned ok=$($json.ok)"
  }
  if ($null -eq $json.generated_at) {
    throw "dashboard JSON is missing 'generated_at'"
  }
  if ($null -eq $json.latest) {
    throw "dashboard JSON is missing 'latest'"
  }
  if ($null -eq $json.queue_status) {
    throw "dashboard JSON is missing 'queue_status'"
  }
  if ($null -eq $json.queue) {
    throw "dashboard JSON is missing 'queue'"
  }

  $lines.Add("ok=$($json.ok) generated_at=$($json.generated_at) broadcasting=$($json.latest.is_broadcasting) listeners=$($json.latest.listener_count) queue_count=$(@($json.queue).Count)")
} catch {
  $lines.Add("error=$($_.Exception.Message)")
  $lines | Set-Content -LiteralPath $logPath -Encoding utf8
  $lines | ForEach-Object { Write-Host $_ }
  throw "Stationhead dashboard API verification failed. See $logPath"
}

$lines | Set-Content -LiteralPath $logPath -Encoding utf8
$lines | ForEach-Object { Write-Host $_ }
