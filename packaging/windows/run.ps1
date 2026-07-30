# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Provides the supported entry point for Windows bootstrap, build, test, packaging, and manifest workflows.

param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("Validate", "Bootstrap", "Build", "Test", "Package", "All", "Manifests")]
  [string] $Action,
  [ValidateSet("x64", "arm64")]
  [string] $Architecture = "x64",
  [string] $BuildDirectory = "tmp/windows",
  [string] $Version = "0.1.0",
  [string] $X64Artifacts,
  [string] $Arm64Artifacts
)

$ErrorActionPreference = "Stop"
$private = Join-Path $PSScriptRoot "private"
$cmakeArchitecture = if ($Architecture -eq "x64") { "x64" } else { "ARM64" }
$triplet = if ($Architecture -eq "x64") { "x64-windows" } else { "arm64-windows" }
$vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:/vcpkg" }
$toolchain = Join-Path $vcpkg "scripts/buildsystems/vcpkg.cmake"
$runtime = "tmp/windows-runtime/$Architecture"
$nginx = "tmp/windows-servers/nginx-1.31.3-$Architecture"
$apache = "tmp/windows-servers/apache-2.4.68-$Architecture"
$configuration = @(
  "-S", ".", "-B", $BuildDirectory, "-A", $cmakeArchitecture,
  "-DLAGHU_WITH_VIPS=ON", "-DLAGHU_BUILD_SERVICE=ON",
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DVCPKG_TARGET_TRIPLET=$triplet"
)

function Invoke-Validate {
  $errorsFound = $false
  Get-ChildItem $PSScriptRoot -Filter "*.ps1" -Recurse | ForEach-Object {
    $tokens = $null
    $errors = $null
    [Management.Automation.Language.Parser]::ParseFile(
      $_.FullName, [ref]$tokens, [ref]$errors
    ) | Out-Null
    foreach ($parseError in $errors) {
      Write-Error "$($_.FullName):$($parseError.Extent.StartLineNumber): $($parseError.Message)"
      $errorsFound = $true
    }
  }
  if ($errorsFound) { throw "PowerShell syntax validation failed" }
  Write-Output "PowerShell syntax passed"
}

function Invoke-Bootstrap {
  & "$vcpkg/vcpkg.exe" install "libvips[jpeg,nsgif,png,webp]:$triplet" `
    "openssl[tools]:$triplet" "--overlay-ports=$PSScriptRoot/vcpkg-ports"
  if ($LASTEXITCODE -ne 0) { throw "vcpkg dependency installation failed" }
  & "$private/install-build-tools.ps1"
}

function Invoke-Build {
  & cmake @configuration
  if ($LASTEXITCODE -ne 0) { throw "Windows CMake configuration failed" }
  & cmake --build $BuildDirectory --config Debug --parallel
  if ($LASTEXITCODE -ne 0) { throw "Windows Debug build failed" }
  & cmake --build $BuildDirectory --config Release --target laghu-libvips --parallel
  if ($LASTEXITCODE -ne 0) { throw "Windows Release worker build failed" }
  & "$private/build-matched-servers.ps1" -Server all -Architecture $Architecture
  & "$private/stage-runtime.ps1" -BuildDirectory $BuildDirectory `
    -Architecture $Architecture -Configuration Release
}

function Invoke-Test {
  & ctest --test-dir $BuildDirectory -C Debug --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "Windows codec-enabled tests failed" }
  $noVips = "$BuildDirectory-no-vips"
  & cmake -S . -B $noVips -A $cmakeArchitecture -DLAGHU_WITH_VIPS=OFF `
    -DLAGHU_BUILD_SERVICE=ON "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    "-DVCPKG_TARGET_TRIPLET=$triplet"
  if ($LASTEXITCODE -ne 0) { throw "Windows no-libvips configuration failed" }
  & cmake --build $noVips --config Debug --parallel
  if ($LASTEXITCODE -ne 0) { throw "Windows no-libvips build failed" }
  & ctest --test-dir $noVips -C Debug --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "Windows no-libvips tests failed" }
  $optimizer = "$BuildDirectory/workers/laghu-libvips/Debug/laghu-libvips.exe"
  $vips = "$vcpkg/installed/$triplet/tools/libvips/vips.exe"
  & "$private/test-worker-service.ps1" -Optimizer $optimizer -Vips $vips
  & "$private/test-matched-servers.ps1" -NginxRoot $nginx -ApacheRoot $apache `
    -Optimizer $optimizer -Vips $vips
}

function Invoke-Package {
  foreach ($offering in @("ngx-laghu", "mod-laghu")) {
    $server = if ($offering -eq "ngx-laghu") { $nginx } else { $apache }
    & "$private/test-installer-validation.ps1" -Offering $offering `
      -Architecture $Architecture -ServerRoot $server -RuntimeRoot $runtime
    foreach ($packageVersion in @("0.0.9", $Version, "0.2.0")) {
      & "$private/build-installers.ps1" -Offering $offering -Architecture $Architecture `
        -ServerRoot $server -RuntimeRoot $runtime -Version $packageVersion
    }
  }
  & "$private/test-installers.ps1" `
    -NginxInstaller "tmp/windows-installers/ngx-laghu-$Version-windows-$Architecture.exe" `
    -ApacheInstaller "tmp/windows-installers/mod-laghu-$Version-windows-$Architecture.exe" `
    -NginxOlderInstaller "tmp/windows-installers/ngx-laghu-0.0.9-windows-$Architecture.exe" `
    -ApacheOlderInstaller "tmp/windows-installers/mod-laghu-0.0.9-windows-$Architecture.exe" `
    -NginxNewerInstaller "tmp/windows-installers/ngx-laghu-0.2.0-windows-$Architecture.exe" `
    -ApacheNewerInstaller "tmp/windows-installers/mod-laghu-0.2.0-windows-$Architecture.exe"
  $invalidNginx = "tmp/windows-invalid-nginx-$Architecture"
  Remove-Item -Recurse -Force $invalidNginx -ErrorAction SilentlyContinue
  Copy-Item -Recurse $nginx $invalidNginx
  Add-Content "$invalidNginx/conf/nginx.conf" "invalid_configuration_directive;"
  & "$private/build-installers.ps1" -Offering ngx-laghu -Architecture $Architecture `
    -ServerRoot $invalidNginx -RuntimeRoot $runtime -Version 0.1.1
  & "$private/test-installer-rollback.ps1" `
    -Installer "tmp/windows-installers/ngx-laghu-0.1.1-windows-$Architecture.exe"
}

if ($Action -eq "Validate") { Invoke-Validate; exit }
if ($Action -eq "Bootstrap") { Invoke-Bootstrap; exit }
if ($Action -eq "Build") { Invoke-Validate; Invoke-Build; exit }
if ($Action -eq "Test") { Invoke-Validate; Invoke-Test; exit }
if ($Action -eq "Package") { Invoke-Validate; Invoke-Package; exit }
if ($Action -eq "Manifests") {
  if (-not $X64Artifacts -or -not $Arm64Artifacts) {
    throw "Manifests requires -X64Artifacts and -Arm64Artifacts"
  }
  & "$private/generate-winget-manifests.ps1" -X64Directory $X64Artifacts `
    -Arm64Directory $Arm64Artifacts -Version $Version
  exit
}

Invoke-Validate
Invoke-Build
Invoke-Test
Invoke-Package
