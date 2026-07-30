# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Installs the pinned, verified Perl and Inno Setup tools required by Windows builds.

$ErrorActionPreference = "Stop"
$innoUrl = "https://github.com/jrsoftware/issrc/releases/download/is-7_0_2/innosetup-7.0.2-x64.exe"
$innoHash = "5ad54ca3def786f8f4212552e54cc6d8d61329e2d24a1cfee0571d42c2684ff1"
$installer = Join-Path $env:TEMP "innosetup-7.0.2-x64.exe"
$perlUrl = "https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/download/SP_54221_64bit/strawberry-perl-5.42.2.1-64bit.msi"
$perlHash = "1b473a709e4d69128f52b30426a423f44d181cefc6460e88b4f519f534cc6583"
$perlInstaller = Join-Path $env:TEMP "strawberry-perl-5.42.2.1-64bit.msi"
$vcpkg = if ($env:VCPKG_ROOT -and
    (Test-Path (Join-Path $env:VCPKG_ROOT "vcpkg.exe"))) {
  $env:VCPKG_ROOT
} else {
  "C:\vcpkg"
}
$bash = Get-ChildItem "$vcpkg\downloads\tools\msys2" -Filter bash.exe `
  -File -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $bash) {
  throw "vcpkg's verified MSYS2 tool is required; install libvips first"
}
if (-not (Test-Path "C:\Strawberry\perl\bin\perl.exe")) {
  Invoke-WebRequest -UseBasicParsing -Uri $perlUrl -OutFile $perlInstaller
  $actual = (Get-FileHash -Algorithm SHA256 $perlInstaller).Hash.ToLowerInvariant()
  if ($actual -ne $perlHash) { throw "Strawberry Perl package hash mismatch" }
  $process = Start-Process -Wait -PassThru msiexec.exe -ArgumentList @(
    "/i", $perlInstaller, "/qn", "/norestart"
  )
  if ($process.ExitCode -ne 0) {
    throw "Strawberry Perl installation failed with $($process.ExitCode)"
  }
}
if (-not (Test-Path "${env:ProgramFiles}\Inno Setup 7\ISCC.exe")) {
  Invoke-WebRequest -UseBasicParsing -Uri $innoUrl -OutFile $installer
  $actual = (Get-FileHash -Algorithm SHA256 $installer).Hash.ToLowerInvariant()
  if ($actual -ne $innoHash) { throw "Inno Setup source hash mismatch" }
  $signature = Get-AuthenticodeSignature $installer
  if ($signature.Status -ne "Valid" -or
      $signature.SignerCertificate.Subject -notmatch "Pyrsys B.V.") {
    throw "Inno Setup publisher signature is invalid"
  }
  $process = Start-Process -Wait -PassThru $installer -ArgumentList @(
    "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/ALLUSERS"
  )
  if ($process.ExitCode -ne 0) {
    throw "Inno Setup installation failed with $($process.ExitCode)"
  }
}
Write-Output "Windows build tools are ready"
