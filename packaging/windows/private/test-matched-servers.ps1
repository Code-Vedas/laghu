# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Runs cold and warm Laghu response smoke tests against the matched NGINX and Apache roots.

param(
  [Parameter(Mandatory = $true)] [string] $NginxRoot,
  [Parameter(Mandatory = $true)] [string] $ApacheRoot,
  [Parameter(Mandatory = $true)] [string] $Optimizer,
  [Parameter(Mandatory = $true)] [string] $Vips
)

$ErrorActionPreference = "Stop"
$optimizerPath = (Resolve-Path $Optimizer).Path
$vipsPath = (Resolve-Path $Vips).Path
$testRoot = Join-Path $env:TEMP "laghu-matched-server-smoke"
$cache = "$testRoot\cache"
$queue = "$testRoot\jobs.queue"
$web = "$testRoot\www"
$token = "$testRoot\purge.token"
$purgeToken = "matched-server-purge-token"

function Get-FreePort {
  $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
  $listener.Start()
  $port = ([Net.IPEndPoint]$listener.LocalEndpoint).Port
  $listener.Stop()
  return $port
}

function Stop-TestProcesses {
  Get-Process nginx, httpd -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -and $_.Path.StartsWith($testRoot, [StringComparison]::OrdinalIgnoreCase)
  } | ForEach-Object {
    & taskkill.exe /PID $_.Id /T /F 2>$null | Out-Null
  }
  if (-not (Get-Service laghu-libvips -ErrorAction SilentlyContinue)) {
    Get-Process laghu-libvips -ErrorAction SilentlyContinue | Where-Object {
      $_.Path -and $_.Path.Equals($optimizerPath, [StringComparison]::OrdinalIgnoreCase)
    } | ForEach-Object {
      & taskkill.exe /PID $_.Id /T /F 2>$null | Out-Null
    }
  }
}

function Wait-ForServer([int] $Port) {
  for ($attempt = 0; $attempt -lt 100; ++$attempt) {
    try {
      return Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://127.0.0.1:$Port/index.html"
    } catch {
      Start-Sleep -Milliseconds 100
    }
  }
  throw "server did not become ready"
}

function Get-HeaderValue($Response, [string] $Name) {
  return [string]::Join(",", [string[]]$Response.Headers[$Name])
}

function Invoke-Purge([int] $Port, [hashtable] $Headers) {
  $request = [Net.HttpWebRequest]::Create("http://127.0.0.1:$Port/image.png")
  $request.Method = "PURGE"
  foreach ($name in $Headers.Keys) { $request.Headers[$name] = $Headers[$name] }
  $response = $request.GetResponse()
  try {
    return [pscustomobject]@{
      StatusCode = [int]$response.StatusCode
      Headers = $response.Headers
    }
  } finally {
    $response.Close()
  }
}

function Copy-TestServerRoot([string] $Source, [string] $Destination) {
  & robocopy.exe ((Resolve-Path $Source).Path) $Destination /E /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -gt 7) { throw "server fixture copy failed" }
}

function Invoke-NativeTestCommand(
  [string] $Executable,
  [string[]] $Arguments,
  [string] $Failure
) {
  $stdout = Join-Path $testRoot "native-command.stdout"
  $stderr = Join-Path $testRoot "native-command.stderr"
  $commandLine = [string]::Join(" ", [string[]]($Arguments | ForEach-Object {
    '"' + $_.Replace('"', '\"') + '"'
  }))
  Remove-Item -Force $stdout, $stderr -ErrorAction SilentlyContinue
  $process = Start-Process -FilePath $Executable -ArgumentList $commandLine `
    -NoNewWindow -Wait -PassThru -RedirectStandardOutput $stdout `
    -RedirectStandardError $stderr
  if ($process.ExitCode -ne 0) {
    Get-Content $stdout, $stderr -ErrorAction SilentlyContinue
    throw $Failure
  }
  Remove-Item -Force $stdout, $stderr -ErrorAction SilentlyContinue
}

function Assert-CommonBehavior([int] $Port, $Cold) {
  $laghu = Get-HeaderValue $Cold "X-Laghu"
  if ($Cold.StatusCode -ne 200 -or $laghu -ne "pass") {
    throw "cold response did not pass through Laghu (status=$($Cold.StatusCode), x-laghu=$laghu)"
  }
  if ((Get-HeaderValue $Cold "X-Laghu-Cache") -ne "miss" -or
      (Get-HeaderValue $Cold "X-Laghu-Transform") -ne "queued") {
    throw "cold response did not expose cache and transform decisions"
  }
  $api = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/api/data.json"
  if ((Get-HeaderValue $api "X-Laghu") -ne "bypass-api") {
    throw "API response did not retain the shared exclusion"
  }
  $authorized = Invoke-WebRequest -UseBasicParsing -Headers @{ Authorization = "Bearer fixture" } "http://127.0.0.1:$Port/index.html"
  if ((Get-HeaderValue $authorized "X-Laghu") -ne "bypass-authorized") {
    throw "authorized response did not retain the shared exclusion"
  }
  $denied = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/blocked.txt"
  if ((Get-HeaderValue $denied "X-Laghu") -ne "bypass-resource-policy" -or
      $denied.Content.Trim() -ne "origin-only") {
    throw "disallowed resource was not preserved as an origin response"
  }
  $override = Invoke-WebRequest -UseBasicParsing `
    "http://127.0.0.1:$Port/index.html?laghuFilters=%2Bhtml_minify"
  if ((Get-HeaderValue $override "X-Laghu") -eq "bypass-query-override") {
    throw "valid query filter override was rejected"
  }
  $imageCold = $null
  for ($attempt = 0; $attempt -lt 100; ++$attempt) {
    try {
      $candidate = Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://127.0.0.1:$Port/image.png"
      $imageDecision = Get-HeaderValue $candidate "X-Laghu"
      if ($imageDecision -eq "pass" -or $imageDecision -eq "image-hit") {
        $imageCold = $candidate
        break
      }
    } catch {
    }
    Start-Sleep -Milliseconds 100
  }
  if ($null -eq $imageCold) { throw "cold image response was not preserved" }
  $imageWarm = $null
  for ($attempt = 0; $attempt -lt 100; ++$attempt) {
    Start-Sleep -Milliseconds 100
    try {
      $candidate = Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://127.0.0.1:$Port/image.png"
    } catch {
      continue
    }
    $imageDecision = Get-HeaderValue $candidate "X-Laghu"
    if ($imageDecision -eq "image-hit" -or $imageDecision -eq "pass") {
      $imageWarm = $candidate
      break
    }
  }
  if ($null -eq $imageWarm) { throw "warm image variant was not served" }
  $warm = $null
  for ($attempt = 0; $attempt -lt 100; ++$attempt) {
    try {
      $candidate = Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://127.0.0.1:$Port/index.html"
    } catch {
      Start-Sleep -Milliseconds 100
      continue
    }
    $candidateEtag = Get-HeaderValue $candidate "ETag"
    if ($candidateEtag.Contains("laghu-html-")) {
      $warm = $candidate
      break
    }
    Start-Sleep -Milliseconds 100
  }
  if ($null -eq $warm -or $warm.StatusCode -ne 200) {
    $etag = if ($candidate) { Get-HeaderValue $candidate "ETag" } else { "<none>" }
    $decision = if ($candidate) { Get-HeaderValue $candidate "X-Laghu" } else { "<none>" }
    throw "warm HTML response was not served (X-Laghu=$decision, ETag=$etag)"
  }

  try {
    Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/.laghu/stats" | Out-Null
    throw "unauthorized statistics request succeeded"
  } catch {
    if ($_.Exception.Response.StatusCode.value__ -ne 403) { throw }
  }
  $adminHeaders = @{ "X-Laghu-Purge-Token" = $purgeToken }
  $stats = $null
  for ($attempt = 0; $attempt -lt 100; ++$attempt) {
    try {
      $stats = Invoke-WebRequest -UseBasicParsing -Headers $adminHeaders `
        "http://127.0.0.1:$Port/.laghu/stats"
      break
    } catch {
      if ($_.Exception.Response.StatusCode.value__ -ne 503) { throw }
      Start-Sleep -Milliseconds 100
    }
  }
  if ($stats.StatusCode -ne 200 -or -not $stats.Content.Contains('"schema":"laghu-cache-stats-v1"')) {
    throw "authorized statistics request failed"
  }
  $purge = Invoke-Purge $Port $adminHeaders
  if ($purge.StatusCode -ne 202 -or (Get-HeaderValue $purge "Cache-Control") -ne "no-store") {
    throw "authenticated PURGE failed"
  }
  $queryPurge = Invoke-WebRequest -UseBasicParsing -Headers $adminHeaders "http://127.0.0.1:$Port/image.png?laghu=purge"
  if ($queryPurge.StatusCode -ne 202) { throw "authenticated query purge failed" }
}

Stop-TestProcesses
Remove-Item -Recurse -Force $testRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $cache, $web, "$web\api" | Out-Null
$assetCatalog = "$testRoot\asset-catalog"
$assetQueue = "$testRoot\asset.queue"
$assetConfig = "$testRoot\asset-offload.conf"
New-Item -ItemType Directory -Force -Path $assetCatalog, $assetQueue | Out-Null
@"
version=1
source_domain=https://origin.example.test
public_domain=https://cdn.example.test
source_prefix=/assets
public_prefix=/immutable
mime_types=text/css,image/png
allow_paths=/
mode=upload_and_rewrite
preserve_query=on
trusted_origin_fallback=on
max_body_bytes=67108864
retry_limit=3
timeout_seconds=10
stale_ttl_seconds=86400
catalog_path=$($assetCatalog.Replace('\', '/'))
queue_path=$($assetQueue.Replace('\', '/'))
provider=s3
endpoint=https://s3.example.test
region=us-east-1
bucket=laghu-assets
object_prefix=production
access_key_env=LAGHU_TEST_S3_ACCESS_KEY
secret_key_env=LAGHU_TEST_S3_SECRET_KEY
"@ | Set-Content -Encoding ASCII $assetConfig
Set-Content -Encoding UTF8 "$web\index.html" '<!doctype html><html><head></head><body>  <!-- remove -->  <img src="/image.png" width="512" height="512">  </body></html>'
Set-Content -Encoding ASCII "$web\api\data.json" '{"ok":true}'
Set-Content -Encoding ASCII "$web\blocked.txt" 'origin-only'
Set-Content -Encoding ASCII -NoNewline -Path $token -Value $purgeToken
Invoke-NativeTestCommand $vipsPath @(
  "black", "$testRoot\image.v", "512", "512", "--bands", "3"
) "vips fixture creation failed"
Invoke-NativeTestCommand $vipsPath @(
  "pngsave", "$testRoot\image.v", "$web\image.png", "--compression", "0"
) "vips fixture encoding failed"
Remove-Item "$testRoot\image.v"
Invoke-NativeTestCommand $optimizerPath @("--init", $queue, $cache) `
  "optimizer initialization failed"
$worker = Start-Process -PassThru -WindowStyle Hidden $optimizerPath -ArgumentList @("--serve", $queue, $cache)

try {
  $nginxTestRoot = "$testRoot\nginx"
  New-Item -ItemType Directory -Force -Path $nginxTestRoot | Out-Null
  Copy-TestServerRoot $NginxRoot $nginxTestRoot
  $nginxPort = Get-FreePort
  $nginxWeb = $web.Replace('\', '/')
  $nginxQueue = $queue.Replace('\', '/')
  $nginxCache = $cache.Replace('\', '/')
  $nginxCacheBackend = "file:///$nginxCache"
  $nginxToken = $token.Replace('\', '/')
  $nginxAssetConfig = $assetConfig.Replace('\', '/')
  $nginxAssetQueue = $assetQueue.Replace('\', '/')
  @"
worker_processes 1;
error_log logs/error.log;
pid logs/nginx.pid;
events { worker_connections 128; }
http {
  include mime.types;
  laghu rum_store local:;
  laghu rum_store_local_snapshot $nginxCache/rum.snapshot;
  server {
    listen 127.0.0.1:$nginxPort;
    root $nginxWeb;
    laghu on;
    laghu preset balanced;
    laghu transform_deadline_ms 1000;
    laghu cache_mime_types image/png;
    laghu disallow /blocked*;
    laghu respect_vary on;
    laghu respect_x_forwarded_proto on;
    laghu trusted_proxy 127.0.0.1/32;
    laghu query_filter_overrides on;
    laghu worker_queue $nginxQueue;
    laghu asset_offload_config $nginxAssetConfig;
    laghu asset_upload_queue $nginxAssetQueue;
    laghu load_from_file both;
    laghu file_source_map https://origin.example.test/assets/ $nginxWeb;
    laghu file_cache_backend $nginxCacheBackend;
    laghu purge_method PURGE;
    laghu purge_query on;
    laghu purge_token_file $nginxToken;
    laghu purge_allow 127.0.0.1/32;
    laghu statistics on;
  }
}
"@ | Set-Content -Encoding ASCII "$nginxTestRoot\conf\nginx.conf"
  Invoke-NativeTestCommand "$nginxTestRoot\nginx.exe" @(
    "-t", "-p", $nginxTestRoot
  ) "NGINX matched configuration failed"
  if (-not (Test-Path "$assetQueue.sources")) {
    throw "NGINX source-loader registry was not published"
  }
  $nginx = Start-Process -PassThru -WindowStyle Hidden "$nginxTestRoot\nginx.exe" -ArgumentList @("-p", $nginxTestRoot)
  try {
    Assert-CommonBehavior $nginxPort (Wait-ForServer $nginxPort)
  } finally {
    Invoke-NativeTestCommand "$nginxTestRoot\nginx.exe" @(
      "-s", "quit", "-p", $nginxTestRoot
    ) "NGINX matched shutdown failed"
    $nginx.WaitForExit(10000) | Out-Null
    if (-not $nginx.HasExited) { $nginx.Kill() }
  }

  Remove-Item -Recurse -Force $cache
  New-Item -ItemType Directory -Force $cache | Out-Null
  $apacheQueuePath = "$testRoot\apache.jobs.queue"
  if (-not $worker.HasExited) {
    & taskkill.exe /PID $worker.Id /T /F 2>$null | Out-Null
    $worker.WaitForExit()
  }
  Invoke-NativeTestCommand $optimizerPath @(
    "--init", $apacheQueuePath, $cache
  ) "optimizer reinitialization failed"
  $worker = Start-Process -PassThru -WindowStyle Hidden $optimizerPath -ArgumentList @("--serve", $apacheQueuePath, $cache)
  Start-Sleep -Seconds 1
  $apacheTestRoot = "$testRoot\apache"
  New-Item -ItemType Directory -Force -Path $apacheTestRoot | Out-Null
  Copy-TestServerRoot $ApacheRoot $apacheTestRoot
  $apachePort = Get-FreePort
  $apacheWeb = $web.Replace('\', '/')
  $apacheQueue = $apacheQueuePath.Replace('\', '/')
  $apacheCache = $cache.Replace('\', '/')
  $apacheCacheBackend = "file:///$apacheCache"
  $apacheToken = $token.Replace('\', '/')
  $apacheAssetConfig = $assetConfig.Replace('\', '/')
  $apacheAssetQueue = $assetQueue.Replace('\', '/')
  $apacheConfigLines = Get-Content "$apacheTestRoot\conf\httpd.conf" | ForEach-Object {
    if ($_ -match '^Listen\s+\d+\s*$') {
      "Listen 127.0.0.1:$apachePort"
    } elseif ($_ -match '(?i)^Laghu\s+Off\s*$') {
      "Laghu On"
    } elseif ($_ -match '(?i)^Laghu\s+RumStore\s+') {
      "Laghu RumStore local:"
    } elseif ($_ -match '(?i)^Laghu\s+RumStoreLocalSnapshot\s+') {
      "Laghu RumStoreLocalSnapshot `"$apacheCache/rum.snapshot`""
    } else {
      $_
    }
  }
  $apacheConfig = [string]::Join("`r`n", [string[]]$apacheConfigLines)
  $apacheConfig += @"

ServerName 127.0.0.1
DocumentRoot "$apacheWeb"
<Directory "$apacheWeb">
  Require all granted
</Directory>
Laghu Preset balanced
Laghu TransformDeadlineMs 1000
Laghu CacheMimeTypes image/png
Laghu Disallow "/blocked*"
Laghu RespectVary On
Laghu RespectXForwardedProto On
Laghu TrustedProxy 127.0.0.1/32
Laghu QueryFilterOverrides On
Laghu WorkerQueue "$apacheQueue"
Laghu AssetOffloadConfig "$apacheAssetConfig"
Laghu AssetUploadQueue "$apacheAssetQueue"
Laghu LoadFromFile Both
Laghu FileSourceMap "https://origin.example.test/assets/" "$apacheWeb"
Laghu FileCacheBackend "$apacheCacheBackend"
Laghu PurgeMethod PURGE
Laghu PurgeQuery On
Laghu PurgeTokenFile "$apacheToken"
Laghu PurgeAllow 127.0.0.1/32
Laghu Statistics On
"@
  [IO.File]::WriteAllText("$apacheTestRoot\conf\httpd.conf", $apacheConfig,
                          [Text.ASCIIEncoding]::new())
  Invoke-NativeTestCommand "$apacheTestRoot\bin\httpd.exe" @(
    "-t", "-d", $apacheTestRoot
  ) "Apache matched configuration failed"
  if (-not (Test-Path "$assetQueue.sources")) {
    throw "Apache source-loader registry was not published"
  }
  $apache = Start-Process -PassThru -WindowStyle Hidden "$apacheTestRoot\bin\httpd.exe" -ArgumentList @("-d", $apacheTestRoot, "-f", "conf/httpd.conf")
  try {
    Assert-CommonBehavior $apachePort (Wait-ForServer $apachePort)
  } finally {
    $apache.Kill()
    $apache.WaitForExit()
  }
} finally {
  if (-not $worker.HasExited) {
    & taskkill.exe /PID $worker.Id /T /F 2>$null | Out-Null
    $worker.WaitForExit()
  }
  Stop-TestProcesses
}

Write-Output "Laghu matched Windows server smoke passed"
