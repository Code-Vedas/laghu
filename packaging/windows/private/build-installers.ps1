# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Validates matched server and worker artifacts, then builds one architecture-specific Inno installer.

param(
  [Parameter(Mandatory = $true)] [ValidateSet("ngx-laghu", "mod-laghu")] [string] $Offering,
  [Parameter(Mandatory = $true)] [ValidateSet("x64", "arm64")] [string] $Architecture,
  [Parameter(Mandatory = $true)] [string] $ServerRoot,
  [Parameter(Mandatory = $true)] [string] $RuntimeRoot,
  [string] $Version = "0.1.0",
  [string] $Output = "tmp/windows-installers"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$isccCommand = Get-Command iscc.exe -ErrorAction SilentlyContinue
$iscc = if ($isccCommand) {
  $isccCommand.Source
} elseif (Test-Path "${env:ProgramFiles}\Inno Setup 7\ISCC.exe") {
  "${env:ProgramFiles}\Inno Setup 7\ISCC.exe"
} else {
  "${env:ProgramFiles(x86)}\Inno Setup 6\iscc.exe"
}
if (-not (Test-Path $iscc)) { throw "Inno Setup 7 is required" }
$server = (Resolve-Path $ServerRoot).Path
$runtime = (Resolve-Path $RuntimeRoot).Path
$manifestPath = Join-Path $server "laghu-build.json"
if (-not (Test-Path $manifestPath)) { throw "matched server manifest is missing" }
$manifest = Get-Content -Raw $manifestPath | ConvertFrom-Json
$expectedServer = if ($Offering -eq "ngx-laghu") { "nginx" } else { "apache" }
if ($manifest.format -ne 1 -or $manifest.server -ne $expectedServer -or
    $manifest.architecture -ne $Architecture) {
  throw "matched server manifest does not match $Offering/$Architecture"
}
if (-not (Test-Path "$runtime/laghu-libvips.exe")) {
  throw "runtime root must contain laghu-libvips.exe and its DLLs"
}
$runtimeManifestPath = Join-Path $runtime "laghu-runtime.json"
if (-not (Test-Path $runtimeManifestPath)) { throw "runtime manifest is missing" }
$runtimeManifest = Get-Content -Raw $runtimeManifestPath | ConvertFrom-Json
if ($runtimeManifest.format -ne 1 -or $runtimeManifest.architecture -ne $Architecture -or
    $runtimeManifest.worker_sha256 -ne
      (Get-FileHash -Algorithm SHA256 "$runtime/laghu-libvips.exe").Hash.ToLowerInvariant()) {
  throw "runtime manifest does not match $Architecture"
}

function Get-PeArchitecture([string] $Path) {
  $stream = [IO.File]::OpenRead($Path)
  try {
    $reader = [IO.BinaryReader]::new($stream)
    if ($reader.ReadUInt16() -ne 0x5A4D) { throw "$Path is not a PE image" }
    $stream.Position = 0x3C
    $stream.Position = $reader.ReadUInt32() + 4
    return $reader.ReadUInt16()
  } finally {
    $stream.Dispose()
  }
}

$expectedMachine = if ($Architecture -eq "x64") { 0x8664 } else { 0xAA64 }
$serverBinary = if ($Offering -eq "ngx-laghu") {
  "$server/nginx.exe"
} else {
  "$server/bin/httpd.exe"
}
$serverHash = (Get-FileHash -Algorithm SHA256 $serverBinary).Hash.ToLowerInvariant()
if ($serverHash -ne $manifest.artifacts.server) {
  throw "matched server binary does not match its build manifest"
}
foreach ($binary in @($serverBinary, "$runtime/laghu-libvips.exe")) {
  if ((Get-PeArchitecture $binary) -ne $expectedMachine) {
    throw "$binary does not match $Architecture"
  }
}

if ($Offering -eq "ngx-laghu" -and -not (Test-Path "$server/nginx.exe")) {
  throw "ngx-laghu requires a matched nginx.exe containing the Laghu adapter"
}
if ($Offering -eq "mod-laghu" -and -not (Test-Path "$server/bin/httpd.exe")) {
  throw "mod-laghu requires a matched Apache HTTP Server build"
}
if ($Offering -eq "mod-laghu" -and
    (Get-PeArchitecture "$server/modules/mod_laghu.so") -ne $expectedMachine) {
  throw "mod_laghu.so does not match $Architecture"
}
if ($Offering -eq "mod-laghu" -and
    (Get-FileHash -Algorithm SHA256 "$server/modules/mod_laghu.so").Hash.ToLowerInvariant() -ne
      $manifest.artifacts.adapter) {
  throw "mod_laghu.so does not match its build manifest"
}

New-Item -ItemType Directory -Force $Output | Out-Null
$arguments = @(
  (Join-Path $PSScriptRoot "../laghu.iss"),
  "/DOFFERING=$Offering",
  "/DARCHITECTURE=$Architecture",
  "/DSERVER_ROOT=$server",
  "/DRUNTIME_ROOT=$runtime",
  "/DREPO_ROOT=$repo",
  "/DVERSION=$Version",
  "/DOUTPUT=$((Resolve-Path $Output).Path)"
)
& $iscc @arguments

if ($LASTEXITCODE -ne 0) {
  throw "Inno Setup failed with exit code $LASTEXITCODE"
}
