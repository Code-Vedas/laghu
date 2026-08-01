# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Verifies asset-upload Windows service start, stop, recovery, and cleanup without contacting S3.

param([Parameter(Mandatory = $true)] [string] $Worker)

$ErrorActionPreference = "Stop"
$workerPath = (Resolve-Path $Worker).Path
$root = Join-Path $env:TEMP "laghu-asset-service"
$config = Join-Path $root "asset-offload.conf"
$serviceName = "laghu-asset-upload"

try {
  & sc.exe stop $serviceName 2>$null | Out-Null
  & sc.exe delete $serviceName 2>$null | Out-Null
  Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $root | Out-Null
  $catalog = (Join-Path $root "catalog") -replace '\\', '/'
  $queue = (Join-Path $root "queue") -replace '\\', '/'
  @"
version=1
source_domain=https://www.example.com
public_domain=https://cdn.example.com
source_prefix=/assets
public_prefix=/immutable
mime_types=image/png,text/css,application/javascript
allow_paths=/
deny_paths=/private
mode=upload_and_rewrite
preserve_query=on
trusted_origin_fallback=on
max_body_bytes=1048576
retry_limit=3
timeout_seconds=2
stale_ttl_seconds=60
catalog_path=$catalog
queue_path=$queue
provider=s3
endpoint=https://s3.example.com
region=us-east-1
bucket=laghu-test
object_prefix=immutable
access_key_env=LAGHU_TEST_S3_ACCESS_KEY
secret_key_env=LAGHU_TEST_S3_SECRET_KEY
"@ | Set-Content -Encoding ascii $config
  $binaryPath = '"' + $workerPath + '" --service "' + $config + '"'
  & sc.exe create $serviceName start= demand binPath= $binaryPath | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "asset service creation failed" }
  New-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\$serviceName" `
    -Name Environment -Value @(
      "LAGHU_TEST_S3_ACCESS_KEY=test-access",
      "LAGHU_TEST_S3_SECRET_KEY=test-secret"
    ) -PropertyType MultiString -Force | Out-Null
  Start-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Running", [TimeSpan]::FromSeconds(10))
  Stop-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Stopped", [TimeSpan]::FromSeconds(15))
  Start-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Running", [TimeSpan]::FromSeconds(10))
  Stop-Service $serviceName
  (Get-Service $serviceName).WaitForStatus("Stopped", [TimeSpan]::FromSeconds(15))
} finally {
  & sc.exe stop $serviceName 2>$null | Out-Null
  & sc.exe delete $serviceName 2>$null | Out-Null
  Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
}

if (Get-Service $serviceName -ErrorAction SilentlyContinue) {
  throw "asset service cleanup failed"
}
Write-Output "Laghu Windows asset service lifecycle passed"
