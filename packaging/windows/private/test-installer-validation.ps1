# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Verifies that installer construction rejects mismatched architecture, manifest, and adapter binaries.

param(
  [Parameter(Mandatory = $true)] [ValidateSet("ngx-laghu", "mod-laghu")] [string] $Offering,
  [Parameter(Mandatory = $true)] [ValidateSet("x64", "arm64")] [string] $Architecture,
  [Parameter(Mandatory = $true)] [string] $ServerRoot,
  [Parameter(Mandatory = $true)] [string] $RuntimeRoot
)

$ErrorActionPreference = "Stop"
$copy = Join-Path $env:TEMP "laghu-invalid-server-$Offering-$Architecture"
Remove-Item -Recurse -Force $copy -ErrorAction SilentlyContinue
Copy-Item -Recurse $ServerRoot $copy
$manifest = Get-Content -Raw "$copy\laghu-build.json" | ConvertFrom-Json
$manifest.architecture = if ($Architecture -eq "x64") { "arm64" } else { "x64" }
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "$copy\laghu-build.json"
try {
  & "$PSScriptRoot\build-installers.ps1" -Offering $Offering -Architecture $Architecture -ServerRoot $copy -RuntimeRoot $RuntimeRoot -Output "tmp/invalid-installer"
  if ($LASTEXITCODE -eq 0) { throw "installer accepted a mismatched server manifest" }
} catch {
  if ($_.Exception.Message -notmatch "manifest does not match") { throw }
}

$manifest.architecture = $Architecture
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "$copy\laghu-build.json"
$binary = if ($Offering -eq "ngx-laghu") {
  "$copy\nginx.exe"
} else {
  "$copy\modules\mod_laghu.so"
}
[IO.File]::AppendAllText($binary, "mismatch")
try {
  & "$PSScriptRoot\build-installers.ps1" -Offering $Offering -Architecture $Architecture -ServerRoot $copy -RuntimeRoot $RuntimeRoot -Output "tmp/invalid-installer"
  if ($LASTEXITCODE -eq 0) { throw "installer accepted a mismatched adapter ABI" }
} catch {
  if ($_.Exception.Message -notmatch "does not match its build manifest") { throw }
} finally {
  Remove-Item -Recurse -Force $copy -ErrorAction SilentlyContinue
}
Write-Output "Laghu Windows installer rejection passed"
