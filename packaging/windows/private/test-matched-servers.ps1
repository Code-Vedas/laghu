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

function Assert-CommonBehavior([int] $Port, $Cold) {
  if ($Cold.StatusCode -ne 200 -or (Get-HeaderValue $Cold "X-Laghu") -ne "pass") {
    throw "cold response did not pass through Laghu"
  }
  $api = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/api/data.json"
  if ((Get-HeaderValue $api "X-Laghu") -ne "bypass-api") {
    throw "API response did not retain the shared exclusion"
  }
  $authorized = Invoke-WebRequest -UseBasicParsing -Headers @{ Authorization = "Bearer fixture" } "http://127.0.0.1:$Port/index.html"
  if ((Get-HeaderValue $authorized "X-Laghu") -ne "bypass-authorized") {
    throw "authorized response did not retain the shared exclusion"
  }
  $imageCold = Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$Port/image.png"
  if ((Get-HeaderValue $imageCold "X-Laghu") -ne "pass") {
    throw "cold image response was not preserved"
  }
  $imageWarm = $null
  for ($attempt = 0; $attempt -lt 100; ++$attempt) {
    Start-Sleep -Milliseconds 100
    try {
      $candidate = Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://127.0.0.1:$Port/image.png"
    } catch {
      continue
    }
    if ((Get-HeaderValue $candidate "X-Laghu") -eq "image-hit") {
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
}

Stop-TestProcesses
Remove-Item -Recurse -Force $testRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $cache, $web, "$web\api" | Out-Null
Set-Content -Encoding UTF8 "$web\index.html" '<!doctype html><html><head></head><body>  <!-- remove -->  <img src="/image.png" width="512" height="512">  </body></html>'
Set-Content -Encoding ASCII "$web\api\data.json" '{"ok":true}'
& $vipsPath black "$testRoot\image.v" 512 512 --bands 3
& $vipsPath pngsave "$testRoot\image.v" "$web\image.png" --compression 0
Remove-Item "$testRoot\image.v"
& $optimizerPath --init $queue $cache
if ($LASTEXITCODE -ne 0) { throw "optimizer initialization failed" }
$worker = Start-Process -PassThru -WindowStyle Hidden $optimizerPath -ArgumentList @("--serve", $queue, $cache)

try {
  $nginxTestRoot = "$testRoot\nginx"
  Copy-Item -Recurse (Resolve-Path $NginxRoot) $nginxTestRoot
  $nginxPort = Get-FreePort
  $nginxWeb = $web.Replace('\', '/')
  $nginxQueue = $queue.Replace('\', '/')
  $nginxCache = $cache.Replace('\', '/')
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
    laghu worker_queue $nginxQueue;
    laghu image_cache $nginxCache;
  }
}
"@ | Set-Content -Encoding ASCII "$nginxTestRoot\conf\nginx.conf"
  & "$nginxTestRoot\nginx.exe" -t -p $nginxTestRoot
  if ($LASTEXITCODE -ne 0) { throw "NGINX matched configuration failed" }
  $nginx = Start-Process -PassThru -WindowStyle Hidden "$nginxTestRoot\nginx.exe" -ArgumentList @("-p", $nginxTestRoot)
  try {
    Assert-CommonBehavior $nginxPort (Wait-ForServer $nginxPort)
  } finally {
    & "$nginxTestRoot\nginx.exe" -s quit -p $nginxTestRoot
    $nginx.WaitForExit(10000) | Out-Null
    if (-not $nginx.HasExited) { $nginx.Kill() }
  }

  Remove-Item -Recurse -Force $cache
  New-Item -ItemType Directory -Force $cache | Out-Null
  & $optimizerPath --init $queue $cache
  $apacheTestRoot = "$testRoot\apache"
  Copy-Item -Recurse (Resolve-Path $ApacheRoot) $apacheTestRoot
  $apachePort = Get-FreePort
  $apacheWeb = $web.Replace('\', '/')
  $apacheQueue = $queue.Replace('\', '/')
  $apacheCache = $cache.Replace('\', '/')
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
Laghu WorkerQueue "$apacheQueue"
Laghu ImageCache "$apacheCache"
"@
  [IO.File]::WriteAllText("$apacheTestRoot\conf\httpd.conf", $apacheConfig,
                          [Text.ASCIIEncoding]::new())
  & "$apacheTestRoot\bin\httpd.exe" -t -d $apacheTestRoot
  if ($LASTEXITCODE -ne 0) { throw "Apache matched configuration failed" }
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
