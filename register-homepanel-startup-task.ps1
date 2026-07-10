
[CmdletBinding()]
param(
  [string]$TaskName = "HomePanel Native",
  [switch]$HighestPrivileges
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $root "native\scripts\register-task.ps1"
if (-not (Test-Path -LiteralPath $script)) { throw "Script not found: $script" }

& $script -InstallDirectory $root -TaskName $TaskName -HighestPrivileges:$HighestPrivileges
