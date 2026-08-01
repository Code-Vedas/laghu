# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Verifies that a failed installation removes server, service, registry, and shared-runtime state.

param(
  [Parameter(Mandatory = $true)] [string] $Installer
)

$ErrorActionPreference = "Stop"
$installerPath = (Resolve-Path $Installer).Path
$root = "${env:ProgramFiles}\Codevedas\laghu-rollback-test"
& sc.exe stop laghu-libvips 2>$null | Out-Null
& sc.exe delete laghu-libvips 2>$null | Out-Null
Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force "HKLM:\Software\Codevedas\Laghu\Offerings\ngx-laghu" `
  -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force `
  "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\CodeVedas.ngx-laghu_is1" `
  -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force "${env:ProgramFiles}\Codevedas\Laghu" `
  -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force "${env:ProgramData}\Laghu" `
  -ErrorAction SilentlyContinue
$process = Start-Process -Wait -PassThru $installerPath -ArgumentList @(
  "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/DIR=`"$root`"",
  "/LOG=$env:TEMP\laghu-installer-rollback.log"
)
if ($process.ExitCode -eq 0) { throw "invalid configuration installer succeeded" }
Start-Sleep -Milliseconds 500
if (Get-Service laghu-libvips -ErrorAction SilentlyContinue) {
  throw "failed installation retained the shared service"
}
if (Get-Service laghu-asset-upload -ErrorAction SilentlyContinue) {
  throw "failed installation retained the asset service"
}
if (Test-Path "$root\server\nginx.exe") {
  throw "failed installation retained matched server files"
}
if (Test-Path "HKLM:\Software\Codevedas\Laghu\Offerings\ngx-laghu") {
  throw "failed installation retained offering metadata"
}
if (Test-Path "${env:ProgramFiles}\Codevedas\Laghu\bin\laghu-libvips.exe") {
  throw "failed installation retained the shared runtime"
}
if (Test-Path "${env:ProgramData}\Laghu\jobs.queue") {
  throw "failed installation retained mutable runtime state"
}
Write-Output "Laghu Windows installer rollback passed"
