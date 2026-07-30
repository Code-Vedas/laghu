# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Stages laghu-libvips with its required DLLs, licenses, capability probe, and integrity manifest.

param(
  [Parameter(Mandatory = $true)] [string] $BuildDirectory,
  [Parameter(Mandatory = $true)] [ValidateSet("x64", "arm64")] [string] $Architecture,
  [string] $Configuration = "Release",
  [string] $Output = "tmp/windows-runtime"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$build = (Resolve-Path $BuildDirectory).Path
$destination = [IO.Path]::GetFullPath((Join-Path $repo "$Output/$Architecture"))
$binary = Join-Path $build "workers/laghu-libvips/$Configuration/laghu-libvips.exe"
$fetchBinary = Join-Path $build "workers/laghu-resource-fetch/$Configuration/laghu-resource-fetch.exe"
if (-not (Test-Path $binary)) { throw "laghu-libvips.exe was not built" }
if (-not (Test-Path $fetchBinary)) { throw "laghu-resource-fetch.exe was not built" }
Remove-Item -Recurse -Force $destination -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $destination | Out-Null
Copy-Item $binary $destination
Copy-Item $fetchBinary $destination
Copy-Item (Join-Path $repo "packaging/font-providers.conf") $destination
Get-ChildItem (Split-Path $binary) -Filter "*.dll" | Copy-Item -Destination $destination
Get-ChildItem (Split-Path $fetchBinary) -Filter "*.dll" | Copy-Item -Destination $destination

$triplet = if ($Architecture -eq "x64") { "x64-windows" } else { "arm64-windows" }
$installedRoot = if ($env:VCPKG_ROOT -and
    (Test-Path (Join-Path $env:VCPKG_ROOT "vcpkg.exe"))) {
  Join-Path $env:VCPKG_ROOT "installed/$triplet"
} else {
  "C:/vcpkg/installed/$triplet"
}
if (Test-Path "$installedRoot/bin") {
  $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "vswhere.exe is required to stage runtime dependencies" }
  $visualStudio = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
  $dumpbin = Get-ChildItem "$visualStudio/VC/Tools/MSVC/*/bin/Host*/x64/dumpbin.exe" `
    -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
  if (-not $dumpbin) { throw "dumpbin.exe is required to stage runtime dependencies" }
  $searchDirectories = @((Split-Path $binary), (Split-Path $fetchBinary), "$installedRoot/bin")
  $available = @{}
  foreach ($directory in $searchDirectories) {
    Get-ChildItem $directory -Filter "*.dll" | ForEach-Object {
      $available[$_.Name.ToLowerInvariant()] = $_.FullName
    }
  }
  $pending = [Collections.Generic.Queue[string]]::new()
  $pending.Enqueue($binary)
  $pending.Enqueue($fetchBinary)
  $visited = @{}
  while ($pending.Count -gt 0) {
    $candidate = $pending.Dequeue()
    if ($visited.ContainsKey($candidate)) { continue }
    $visited[$candidate] = $true
    & $dumpbin.FullName /dependents $candidate 2>$null | ForEach-Object {
      if ($_ -match '^\s+([A-Za-z0-9_.+-]+\.dll)\s*$') {
        $dependency = $Matches[1].ToLowerInvariant()
        if ($available.ContainsKey($dependency) -and
            -not (Test-Path (Join-Path $destination $dependency))) {
          Copy-Item $available[$dependency] $destination
          $pending.Enqueue($available[$dependency])
        }
      }
    }
  }
  $licenseRoot = Join-Path $destination "licenses"
  New-Item -ItemType Directory -Force -Path $licenseRoot | Out-Null
  $packageLists = Get-ChildItem (Join-Path (Split-Path $installedRoot) "vcpkg/info") `
    -Filter "*.list" -ErrorAction Stop
  $licensePorts = @{}
  Get-ChildItem $destination -Filter "*.dll" | ForEach-Object {
    $ownedPath = "$triplet/bin/$($_.Name)"
    foreach ($packageList in $packageLists) {
      if (Select-String -Path $packageList.FullName -SimpleMatch $ownedPath -Quiet) {
        $copyright = Get-Content $packageList.FullName | Where-Object {
          $_ -match "^$([regex]::Escape($triplet))/share/([^/]+)/copyright$"
        } | Select-Object -First 1
        if ($copyright -and $copyright -match "/share/([^/]+)/copyright$") {
          $licensePorts[$Matches[1]] = $true
        }
      }
    }
  }
  foreach ($port in $licensePorts.Keys) {
    Copy-Item "$installedRoot/share/$port/copyright" `
      (Join-Path $licenseRoot "$port.txt")
  }
}
if (-not (Get-ChildItem $destination -Filter "*vips*.dll")) {
  throw "staged runtime does not contain the libvips DLL"
}
$stagedPath = $env:Path
$env:Path = $destination + ";" + $env:Path
try {
  $backend = & (Join-Path $destination "laghu-libvips.exe") --probe
  if ($LASTEXITCODE -ne 0 -or $backend -notmatch 'available=yes') {
    throw "staged runtime has no usable codec backend"
  }
} finally {
  $env:Path = $stagedPath
}
$manifest = [ordered]@{
  format = 1
  architecture = $Architecture
  backend = $backend.Trim()
  worker_sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $destination "laghu-libvips.exe")).Hash.ToLowerInvariant()
  fetch_worker_sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $destination "laghu-resource-fetch.exe")).Hash.ToLowerInvariant()
  providers_sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $destination "font-providers.conf")).Hash.ToLowerInvariant()
  dlls = [ordered]@{}
}
Get-ChildItem $destination -Filter "*.dll" | Sort-Object Name | ForEach-Object {
  $manifest.dlls[$_.Name] = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant()
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $destination "laghu-runtime.json")
Write-Output $destination
