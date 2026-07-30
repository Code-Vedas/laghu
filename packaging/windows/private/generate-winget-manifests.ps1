# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Generates local Winget manifests from validated x64 and ARM64 installer artifacts.

param(
  [Parameter(Mandatory = $true)] [string] $X64Directory,
  [Parameter(Mandatory = $true)] [string] $Arm64Directory,
  [string] $Version = "0.1.0",
  [string] $Output = "tmp/windows-winget"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$x64 = (Resolve-Path $X64Directory).Path
$arm64 = (Resolve-Path $Arm64Directory).Path
$destination = [IO.Path]::GetFullPath((Join-Path $repo $Output))
New-Item -ItemType Directory -Force -Path $destination | Out-Null

foreach ($offering in @("ngx-laghu", "mod-laghu")) {
  $identifier = if ($offering -eq "ngx-laghu") { "NgxLaghu" } else { "ModLaghu" }
  $x64Installer = Get-ChildItem $x64 -Recurse -Filter "$offering-$Version-windows-x64.exe" |
    Select-Object -First 1
  $arm64Installer = Get-ChildItem $arm64 -Recurse -Filter "$offering-$Version-windows-arm64.exe" |
    Select-Object -First 1
  if (-not $x64Installer -or -not $arm64Installer) {
    throw "both native installers are required for $offering"
  }
  $template = Get-Content -Raw (Join-Path $PSScriptRoot "../winget/CodeVedas.$identifier.yaml.in")
  $release = "https://github.com/codevedas/laghu/releases/download/v$Version"
  $manifest = $template.Replace("@VERSION@", $Version)
  $manifest = $manifest.Replace("@X64_URL@", "$release/$($x64Installer.Name)")
  $manifest = $manifest.Replace("@ARM64_URL@", "$release/$($arm64Installer.Name)")
  $manifest = $manifest.Replace("@X64_SHA256@", (Get-FileHash -Algorithm SHA256 $x64Installer.FullName).Hash.ToUpperInvariant())
  $manifest = $manifest.Replace("@ARM64_SHA256@", (Get-FileHash -Algorithm SHA256 $arm64Installer.FullName).Hash.ToUpperInvariant())
  $manifest | Set-Content -Encoding UTF8 (Join-Path $destination "CodeVedas.$identifier.yaml")
}

Write-Output $destination
