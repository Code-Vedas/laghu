# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Exercises install, repair, upgrade, downgrade rejection, shared ownership, and ordered uninstall behavior.

param(
  [Parameter(Mandatory = $true)] [string] $NginxInstaller,
  [Parameter(Mandatory = $true)] [string] $ApacheInstaller,
  [Parameter(Mandatory = $true)] [string] $NginxOlderInstaller,
  [Parameter(Mandatory = $true)] [string] $ApacheOlderInstaller,
  [Parameter(Mandatory = $true)] [string] $NginxNewerInstaller,
  [Parameter(Mandatory = $true)] [string] $ApacheNewerInstaller
)

$ErrorActionPreference = "Stop"
$nginxInstallerPath = (Resolve-Path $NginxInstaller).Path
$apacheInstallerPath = (Resolve-Path $ApacheInstaller).Path
$nginxOlderInstallerPath = (Resolve-Path $NginxOlderInstaller).Path
$apacheOlderInstallerPath = (Resolve-Path $ApacheOlderInstaller).Path
$nginxNewerInstallerPath = (Resolve-Path $NginxNewerInstaller).Path
$apacheNewerInstallerPath = (Resolve-Path $ApacheNewerInstaller).Path
$nginxRoot = "${env:ProgramFiles}\Codevedas\ngx-laghu-test"
$apacheRoot = "${env:ProgramFiles}\Codevedas\mod-laghu-test"

function Invoke-Installer([string] $Installer, [string] $Root) {
  $log = Join-Path $env:TEMP ((Split-Path $Installer -Leaf) + ".install.log")
  $process = Start-Process -Wait -PassThru $Installer -ArgumentList @(
    "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/DIR=`"$Root`"", "/LOG=$log"
  )
  if ($process.ExitCode -ne 0) {
    Get-Content -Tail 80 $log -ErrorAction SilentlyContinue | Write-Output
    throw "installer failed with $($process.ExitCode)"
  }
}

function Invoke-Uninstaller([string] $Root) {
  $uninstaller = Get-ChildItem $Root -Filter "unins*.exe" |
    Sort-Object Name -Descending | Select-Object -First 1
  if ($null -eq $uninstaller) { throw "uninstaller is missing from $Root" }
  $process = Start-Process -Wait -PassThru $uninstaller.FullName -ArgumentList @(
    "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"
  )
  if ($process.ExitCode -ne 0) { throw "uninstaller failed with $($process.ExitCode)" }
}

function Reject-Installer([string] $Installer, [string] $Root) {
  $process = Start-Process -Wait -PassThru $Installer -ArgumentList @(
    "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/DIR=`"$Root`""
  )
  if ($process.ExitCode -eq 0) { throw "downgrade installer was accepted" }
}

function Require-Service([bool] $Present) {
  $deadline = [DateTime]::UtcNow.AddSeconds(15)
  do {
    $service = Get-Service laghu-libvips -ErrorAction SilentlyContinue
    if ($Present -or $null -eq $service -or [DateTime]::UtcNow -ge $deadline) {
      break
    }
    Start-Sleep -Milliseconds 100
  } while ($true)
  if ($Present -and $null -ne $service) {
    try {
      $service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running,
                             [TimeSpan]::FromSeconds(15))
      $service.Refresh()
    } catch {
      throw "laghu-libvips service did not reach running state"
    }
  }
  if ($Present -and ($null -eq $service -or $service.Status -ne "Running")) {
    throw "laghu-libvips service is not running"
  }
  if (-not $Present -and $null -ne $service) {
    throw "laghu-libvips service was not removed"
  }
}

function Require-FetchService([bool] $Present) {
  $service = Get-Service laghu-resource-fetch -ErrorAction SilentlyContinue
  if ($Present -and $null -ne $service) {
    $service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running,
                           [TimeSpan]::FromSeconds(15))
    $service.Refresh()
  }
  if ($Present -and ($null -eq $service -or $service.Status -ne "Running")) {
    throw "laghu-resource-fetch service is not running"
  }
  if (-not $Present -and $null -ne $service) {
    throw "laghu-resource-fetch service was not removed"
  }
}

function Require-AssetService([bool] $Present) {
  $service = Get-Service laghu-asset-upload -ErrorAction SilentlyContinue
  if ($Present -and $null -eq $service) {
    throw "laghu-asset-upload service is missing"
  }
  if ($Present -and $service.StartType -ne "Manual") {
    throw "laghu-asset-upload must remain demand-start until configured"
  }
  if (-not $Present -and $null -ne $service) {
    throw "laghu-asset-upload service was not removed"
  }
}

foreach ($offering in @("ngx-laghu", "mod-laghu")) {
  Remove-Item "HKLM:\Software\Codevedas\Laghu\Offerings\$offering" -Recurse -Force -ErrorAction SilentlyContinue
}

try {
  Write-Output "install ngx-laghu 0.0.9"
  Invoke-Installer $nginxOlderInstallerPath $nginxRoot
  Write-Output "upgrade ngx-laghu to 0.1.0"
  Invoke-Installer $nginxInstallerPath $nginxRoot
  Require-Service $true
  Require-FetchService $true
  Require-AssetService $true
  Write-Output "validate installed NGINX"
  & "$nginxRoot\server\nginx.exe" -t -p "$nginxRoot\server"
  if ($LASTEXITCODE -ne 0) { throw "installed NGINX validation failed" }
  Start-Sleep -Seconds 1
  Write-Output "repair ngx-laghu 0.1.0"
  Invoke-Installer $nginxInstallerPath $nginxRoot
  Require-Service $true
  Require-FetchService $true
  Require-AssetService $true
  Write-Output "upgrade ngx-laghu to 0.2.0"
  Invoke-Installer $nginxNewerInstallerPath $nginxRoot
  Write-Output "reject ngx-laghu downgrade"
  Reject-Installer $nginxInstallerPath $nginxRoot

  Write-Output "install mod-laghu 0.0.9"
  Invoke-Installer $apacheOlderInstallerPath $apacheRoot
  Write-Output "upgrade mod-laghu to 0.1.0"
  Invoke-Installer $apacheInstallerPath $apacheRoot
  Require-Service $true
  Require-FetchService $true
  Require-AssetService $true
  Write-Output "validate installed Apache"
  & "$apacheRoot\server\bin\httpd.exe" -t -d "$apacheRoot\server"
  if ($LASTEXITCODE -ne 0) { throw "installed Apache validation failed" }
  Write-Output "upgrade mod-laghu to 0.2.0"
  Invoke-Installer $apacheNewerInstallerPath $apacheRoot
  Write-Output "reject mod-laghu downgrade"
  Reject-Installer $apacheInstallerPath $apacheRoot

  Write-Output "uninstall ngx-laghu while retaining shared service"
  Invoke-Uninstaller $nginxRoot
  Require-Service $true
  Require-FetchService $true
  Require-AssetService $true
  Write-Output "uninstall mod-laghu and remove shared service"
  Invoke-Uninstaller $apacheRoot
  Require-Service $false
  Require-FetchService $false
  Require-AssetService $false
} finally {
  foreach ($root in @($nginxRoot, $apacheRoot)) {
    Get-ChildItem $root -Filter "unins*.exe" -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending | ForEach-Object {
      if (Test-Path $_.FullName) {
        try {
          Start-Process -Wait $_.FullName -ArgumentList @(
            "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"
          )
        } catch {
          Write-Warning "cleanup uninstaller failed: $($_.Exception.Message)"
        }
      }
    }
  }
  & sc.exe stop laghu-libvips 2>$null | Out-Null
  & sc.exe delete laghu-libvips 2>$null | Out-Null
  & sc.exe stop laghu-resource-fetch 2>$null | Out-Null
  & sc.exe delete laghu-resource-fetch 2>$null | Out-Null
  & sc.exe stop laghu-asset-upload 2>$null | Out-Null
  & sc.exe delete laghu-asset-upload 2>$null | Out-Null
}

Write-Output "Laghu Windows installer lifecycle passed"
