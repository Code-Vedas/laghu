# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

param(
  [Parameter(Mandatory = $true)] [ValidateSet("ngx-laghu", "mod-laghu")] [string] $Offering,
  [Parameter(Mandatory = $true)] [ValidateSet("x64", "arm64")] [string] $Architecture,
  [Parameter(Mandatory = $true)] [string] $ServerRoot,
  [Parameter(Mandatory = $true)] [string] $ServiceBinary,
  [string] $Version = "0.1.0",
  [string] $Output = "tmp/windows-installers"
)

$ErrorActionPreference = "Stop"
$iscc = (Get-Command iscc.exe -ErrorAction Stop).Source
$server = (Resolve-Path $ServerRoot).Path
$service = (Resolve-Path $ServiceBinary).Path

if ($Offering -eq "ngx-laghu" -and -not (Test-Path "$server/nginx.exe")) {
  throw "ngx-laghu requires a matched nginx.exe containing the Laghu adapter"
}
if ($Offering -eq "mod-laghu" -and -not (Test-Path "$server/bin/httpd.exe")) {
  throw "mod-laghu requires a matched Apache HTTP Server build"
}

New-Item -ItemType Directory -Force $Output | Out-Null
& $iscc packaging/windows/laghu.iss \
  "/DOFFERING=$Offering" "/DARCHITECTURE=$Architecture" \
  "/DSERVER_ROOT=$server" "/DSERVICE_BINARY=$service" \
  "/DVERSION=$Version" "/DOUTPUT=$((Resolve-Path $Output).Path)"

if ($LASTEXITCODE -ne 0) {
  throw "Inno Setup failed with exit code $LASTEXITCODE"
}
