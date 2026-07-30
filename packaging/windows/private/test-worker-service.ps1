# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Verifies Windows service processing, forced-stop cleanup, and post-stop worker recovery.

param(
  [Parameter(Mandatory = $true)] [string] $Optimizer,
  [Parameter(Mandatory = $true)] [string] $Vips
)

$ErrorActionPreference = "Stop"
$serviceName = "laghu-libvips"
$optimizerPath = (Resolve-Path $Optimizer).Path
$vipsPath = (Resolve-Path $Vips).Path
$root = "C:\ProgramData\Laghu"
$queue = "$root\jobs.queue"
$cache = "$root\images"
$source = "$root\service-source.png"

function Remove-TestService {
  Stop-Service $serviceName -Force -ErrorAction SilentlyContinue
  & sc.exe delete $serviceName 2>$null | Out-Null
  Remove-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName" -Name Environment -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 300
}

function Wait-ForVariant([int] $Minimum, [int] $Seconds) {
  $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  do {
    $count = @(Get-ChildItem $cache -Filter "variant-*.bin" -ErrorAction SilentlyContinue).Count
    if ($count -ge $Minimum) { return }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "worker service did not publish a variant"
}

function Wait-ForIndex([int] $Minimum, [int] $Seconds) {
  $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
  do {
    $count = @(Get-ChildItem $cache -Filter "index-*.meta" -ErrorAction SilentlyContinue).Count
    if ($count -ge $Minimum) { return }
    Start-Sleep -Milliseconds 100
  } while ([DateTime]::UtcNow -lt $deadline)
  throw "worker service did not publish a recovery index"
}

Remove-TestService
Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $cache | Out-Null
& $vipsPath black "$root\service-source.v" 512 512 --bands 3
if ($LASTEXITCODE -ne 0) { throw "vips fixture creation failed" }
& $vipsPath pngsave "$root\service-source.v" $source --compression 0
if ($LASTEXITCODE -ne 0) { throw "vips fixture encoding failed" }
Remove-Item "$root\service-source.v"

try {
  & sc.exe create $serviceName start= demand binPath= "`"$optimizerPath`" --service" | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "worker service creation failed" }
  Start-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Running", [TimeSpan]::FromSeconds(10))
  & $optimizerPath --submit $queue $source /service.png '"service"'
  if ($LASTEXITCODE -ne 0) { throw "worker service submission failed" }
  Wait-ForVariant 1 10
  Stop-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Stopped", [TimeSpan]::FromSeconds(10))

  New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName" -Name Environment -Value @("LAGHU_TEST_JOB_DELAY_SECONDS=2") -PropertyType MultiString -Force | Out-Null
  Start-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Running", [TimeSpan]::FromSeconds(10))
  $before = @(Get-ChildItem $cache -Filter "variant-*.bin").Count
  $temporaryBefore = @(Get-ChildItem $env:TEMP -Filter "lgh*.tmp" -ErrorAction SilentlyContinue).FullName
  & $optimizerPath --submit $queue $source /cancelled.png '"cancelled"'
  if ($LASTEXITCODE -ne 0) { throw "delayed worker submission failed" }
  Start-Sleep -Milliseconds 250
  Stop-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Stopped", [TimeSpan]::FromSeconds(10))
  Start-Sleep -Milliseconds 300
  if (@(Get-Process -Name "laghu-libvips" -ErrorAction SilentlyContinue).Count -ne 0) {
    throw "isolated worker child survived service stop"
  }
  if (@(Get-ChildItem $cache -Filter "variant-*.bin").Count -ne $before) {
    throw "cancelled worker job published a variant"
  }
  if (Get-ChildItem $cache -Filter "*.tmp-*" -ErrorAction SilentlyContinue) {
    throw "cancelled worker job left partial cache data"
  }
  $temporaryAfter = @(Get-ChildItem $env:TEMP -Filter "lgh*.tmp" -ErrorAction SilentlyContinue).FullName
  if (($temporaryBefore -join "`n") -ne ($temporaryAfter -join "`n")) {
    throw "cancelled worker job left temporary job data"
  }
  Remove-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName" -Name Environment
  Start-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Running", [TimeSpan]::FromSeconds(10))
  $indexesBefore = @(Get-ChildItem $cache -Filter "index-*.meta").Count
  & $optimizerPath --submit $queue $source /service-recovery.png '"service-recovery"'
  if ($LASTEXITCODE -ne 0) { throw "recovery job submission failed" }
  Wait-ForIndex ($indexesBefore + 1) 10
  Stop-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Stopped", [TimeSpan]::FromSeconds(10))
} finally {
  Remove-TestService
}

Write-Output "Laghu Windows worker service lifecycle passed"
