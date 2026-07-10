#Requires -Version 5.1
[CmdletBinding()]
param(
  [string]$InstallDirectory = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
  [string]$TaskName = "HomePanel Native",
  [switch]$HighestPrivileges
)
$ErrorActionPreference = "Stop"
$exe = Join-Path $InstallDirectory "HomePanel.exe"
if (-not (Test-Path $exe)) { throw "HomePanel.exe not found: $exe" }

$action = New-ScheduledTaskAction -Execute $exe -WorkingDirectory $InstallDirectory
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet `
  -AllowStartIfOnBatteries `
  -DontStopIfGoingOnBatteries `
  -StartWhenAvailable `
  -RestartCount 999 `
  -RestartInterval (New-TimeSpan -Seconds 10) `
  -ExecutionTimeLimit ([TimeSpan]::Zero) `
  -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive `
  -RunLevel $(if ($HighestPrivileges) { "Highest" } else { "Limited" })
$task = New-ScheduledTask -Action $action -Trigger $trigger -Settings $settings -Principal $principal
Register-ScheduledTask -TaskName $TaskName -InputObject $task -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName
Write-Host "Registered and started: $TaskName"
