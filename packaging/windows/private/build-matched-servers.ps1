# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Builds hash-verified NGINX and Apache roots containing their matched Laghu adapters.

param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("nginx", "apache", "all")]
  [string] $Server,
  [Parameter(Mandatory = $true)]
  [ValidateSet("x64", "arm64")]
  [string] $Architecture,
  [string] $Output = "tmp/windows-servers",
  [string] $Downloads = "tmp/windows-downloads",
  [string] $LaghuRevision = $env:LAGHU_REVISION
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$sources = Import-PowerShellDataFile (Join-Path $PSScriptRoot "../windows-sources.psd1")
$outputRoot = [IO.Path]::GetFullPath((Join-Path $repo $Output))
$downloadRoot = [IO.Path]::GetFullPath((Join-Path $repo $Downloads))
$workRoot = Join-Path $outputRoot "work-$Architecture"
$cmakeArchitecture = if ($Architecture -eq "x64") { "x64" } else { "ARM64" }
$vcArchitecture = if ($Architecture -eq "x64") { "amd64" } else { "amd64_arm64" }
$vcpkgRoot = if ($env:VCPKG_ROOT -and
    (Test-Path (Join-Path $env:VCPKG_ROOT "vcpkg.exe"))) {
  $env:VCPKG_ROOT
} else {
  "C:\vcpkg"
}
$vcpkgTriplet = if ($Architecture -eq "x64") { "x64-windows" } else { "arm64-windows" }
$vcpkgInstalled = Join-Path $vcpkgRoot "installed\$vcpkgTriplet"
$vcpkgToolchain = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"

function Import-VisualStudioEnvironment {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) {
    throw "Visual Studio Build Tools with vswhere.exe are required"
  }
  $installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $installation) {
    throw "Visual Studio C++ Build Tools are required"
  }
  $developer = Join-Path $installation "Common7\Tools\VsDevCmd.bat"
  $variables = & cmd.exe /s /c "`"$developer`" -no_logo -arch=$vcArchitecture -host_arch=amd64 && set"
  if ($LASTEXITCODE -ne 0) {
    throw "Visual Studio environment initialization failed"
  }
  foreach ($line in $variables) {
    if ($line -match '^([^=]+)=(.*)$') {
      [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
    }
  }
}

function Get-VerifiedArchive([string] $Name, [hashtable] $Source) {
  $extension = if ($Source.Url.EndsWith(".tar.bz2")) {
    ".tar.bz2"
  } elseif ($Source.Url.EndsWith(".tar.gz")) {
    ".tar.gz"
  } else {
    ".zip"
  }
  $archive = Join-Path $downloadRoot "$Name-$($Source.Version)$extension"
  if (-not (Test-Path $archive)) {
    $partial = "$archive.partial"
    Remove-Item -Force $partial -ErrorAction SilentlyContinue
    Invoke-WebRequest -UseBasicParsing -Uri $Source.Url -OutFile $partial
    $downloadHash = (Get-FileHash -Algorithm SHA256 $partial).Hash.ToLowerInvariant()
    if ($downloadHash -ne $Source.Sha256) {
      Remove-Item -Force $partial -ErrorAction SilentlyContinue
      throw "$Name downloaded source hash mismatch"
    }
    Move-Item $partial $archive
  }
  $actual = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
  if ($actual -ne $Source.Sha256) {
    throw "$Name source hash mismatch: expected $($Source.Sha256), got $actual"
  }
  return $archive
}

function Expand-VerifiedArchive([string] $Archive, [string] $Destination) {
  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  & tar.exe -xf $Archive -C $Destination
  if ($LASTEXITCODE -ne 0) {
    throw "Failed to extract $Archive"
  }
}

function Initialize-VerifiedSources(
  [string] $Destination,
  [string] $Identity,
  [scriptblock] $Expand
) {
  $marker = Join-Path $Destination ".laghu-sources.sha256"
  if ((Test-Path $marker) -and (Get-Content -Raw $marker).Trim() -eq $Identity) {
    return $true
  }
  Remove-Item -Recurse -Force $Destination -ErrorAction SilentlyContinue
  if (Test-Path $Destination) {
    throw "Unable to replace the matched-server source directory '$Destination'. Close processes using that directory and run the build again."
  }
  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  & $Expand
  Set-Content -Encoding ASCII $marker $Identity
  return $false
}

function Get-TextSha256([string] $Value) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
  $algorithm = [Security.Cryptography.SHA256]::Create()
  try {
    return ([BitConverter]::ToString($algorithm.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
  } finally {
    $algorithm.Dispose()
  }
}

function Get-MsysBash {
  $candidates = @(
    "C:\msys64\usr\bin\bash.exe",
    "C:\tools\msys64\usr\bin\bash.exe"
  )
  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) { return $candidate }
  }
  $msysRoot = Join-Path $vcpkgRoot "downloads\tools\msys2"
  if (Test-Path $msysRoot) {
    $embedded = [IO.Directory]::EnumerateFiles(
      $msysRoot, "bash.exe", [IO.SearchOption]::AllDirectories
    ) | Where-Object {
      Test-Path (Join-Path (Split-Path $_) "sed.exe")
    } | Select-Object -First 1
    if ($embedded) { return $embedded }
  }
  throw "MSYS2 bash is required to build NGINX"
}

function Convert-ToMsysPath([string] $Path) {
  $full = [IO.Path]::GetFullPath($Path).Replace('\', '/')
  if ($full -match '^([A-Za-z]):/(.*)$') {
    return "/$($Matches[1].ToLowerInvariant())/$($Matches[2])"
  }
  throw "Cannot convert path for MSYS2: $Path"
}

function Write-BuildManifest([string] $Root, [string] $ServerName, [string] $Version) {
  $revision = $LaghuRevision
  $dirty = $true
  if (-not $revision -and (Test-Path (Join-Path $repo ".git"))) {
    $revision = (& git -C $repo rev-parse HEAD).Trim()
    $dirty = [bool](& git -C $repo status --porcelain)
  }
  if (-not $revision -or $revision -notmatch '^[0-9a-fA-F]{40}$') {
    throw "LaghuRevision must be a 40-character Git revision when .git is unavailable"
  }
  $manifest = [ordered]@{
    format = 1
    server = $ServerName
    server_version = $Version
    architecture = $Architecture
    laghu_revision = $revision
    laghu_dirty = $dirty
    compiler = "MSVC $($env:VCToolsVersion)"
    sources = [ordered]@{}
    artifacts = [ordered]@{}
  }
  foreach ($entry in $sources.GetEnumerator()) {
    $manifest.sources[$entry.Key] = [ordered]@{
      version = $entry.Value.Version
      url = $entry.Value.Url
      sha256 = $entry.Value.Sha256
    }
  }
  $serverBinary = if ($ServerName -eq "nginx") {
    Join-Path $Root "nginx.exe"
  } else {
    Join-Path $Root "bin\httpd.exe"
  }
  $manifest.artifacts.server = (Get-FileHash -Algorithm SHA256 $serverBinary).Hash.ToLowerInvariant()
  if ($ServerName -eq "apache") {
    $module = Join-Path $Root "modules\mod_laghu.so"
    $manifest.artifacts.adapter = (Get-FileHash -Algorithm SHA256 $module).Hash.ToLowerInvariant()
  } else {
    $manifest.artifacts.adapter = $manifest.artifacts.server
  }
  $manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 (Join-Path $Root "laghu-build.json")
}

function Copy-License([string] $Source, [string] $Root, [string] $Name) {
  if (-not (Test-Path $Source)) { throw "required license file is missing: $Source" }
  $licenses = Join-Path $Root "licenses"
  New-Item -ItemType Directory -Force -Path $licenses | Out-Null
  Copy-Item $Source (Join-Path $licenses $Name)
}

function Build-Nginx {
  $archive = Get-VerifiedArchive "nginx" $sources.Nginx
  $pcre = Get-VerifiedArchive "pcre2" $sources.Pcre2
  $zlib = Get-VerifiedArchive "zlib" $sources.Zlib
  $openssl = Get-VerifiedArchive "openssl" $sources.OpenSsl
  $sourceParent = Join-Path $workRoot "nginx-source"
  $source = Join-Path $sourceParent "nginx-$($sources.Nginx.Version)"
  $libraries = Join-Path $source "build-libs"
  $sourceIdentity = @(
    $sources.Nginx.Sha256, $sources.Pcre2.Sha256,
    $sources.Zlib.Sha256, $sources.OpenSsl.Sha256
  ) -join "`n"
  $sourcesCached = Initialize-VerifiedSources $sourceParent $sourceIdentity {
    Expand-VerifiedArchive $archive $sourceParent
    Expand-VerifiedArchive $pcre $libraries
    Expand-VerifiedArchive $zlib $libraries
    Expand-VerifiedArchive $openssl $libraries
  }
  $laghuSnapshot = Join-Path $source "laghu"
  Remove-Item -Recurse -Force $laghuSnapshot -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path "$laghuSnapshot\libs", "$laghuSnapshot\modules" | Out-Null
  Copy-Item -Recurse "$repo\libs\laghu-core" "$laghuSnapshot\libs\laghu-core"
  Copy-Item -Recurse "$repo\libs\laghu-http" "$laghuSnapshot\libs\laghu-http"
  Copy-Item -Recurse "$repo\libs\laghu-image" "$laghuSnapshot\libs\laghu-image"
  Copy-Item -Recurse "$repo\libs\laghu-runtime" "$laghuSnapshot\libs\laghu-runtime"
  Copy-Item -Recurse "$repo\modules\ngx_http_laghu_module" "$laghuSnapshot\modules\ngx_http_laghu_module"
  # The upstream Win32 build applies /Yu ngx_config.h to every source in a
  # statically included module. Prepend the required header only to the staged
  # NGINX snapshot so the canonical shared sources remain server-independent.
  $utf8WithoutBom = [Text.UTF8Encoding]::new($false)
  Get-ChildItem "$laghuSnapshot\libs" -Filter "*.c" -File -Recurse | ForEach-Object {
    $body = [IO.File]::ReadAllText($_.FullName)
    [IO.File]::WriteAllText($_.FullName, "#include <ngx_config.h>`r`n$body", $utf8WithoutBom)
  }
  $bash = Get-MsysBash
  Write-Output "Using verified MSYS2 shell $bash"
  $sourceMsys = Convert-ToMsysPath $source
  # NGINX writes these paths into an NMAKE file consumed by cmd.exe. Windows
  # drive paths with forward slashes work in both the MSYS configure shell and
  # native build recipes; /c/... paths do not work in cmd.exe.
  $librariesBuildPath = $libraries.Replace('\', '/')
  $msvcBinMsys = Convert-ToMsysPath (Split-Path (Get-Command cl.exe).Source)
  $moduleMsys = "laghu/modules/ngx_http_laghu_module"
  $configurationArguments = @(
    "./configure --with-cc=cl --prefix= --conf-path=conf/nginx.conf --pid-path=logs/nginx.pid",
    "--http-log-path=logs/access.log --error-log-path=logs/error.log --sbin-path=nginx.exe",
    "--http-client-body-temp-path=temp/client_body_temp --http-proxy-temp-path=temp/proxy_temp",
    "--http-fastcgi-temp-path=temp/fastcgi_temp --http-scgi-temp-path=temp/scgi_temp",
    "--http-uwsgi-temp-path=temp/uwsgi_temp --with-cc-opt=-DFD_SETSIZE=1024",
    "--with-pcre=$librariesBuildPath/pcre2-$($sources.Pcre2.Version) --with-zlib=$librariesBuildPath/zlib-$($sources.Zlib.Version)",
    "--with-openssl=$librariesBuildPath/openssl-$($sources.OpenSsl.Version) --with-openssl-opt='no-asm no-tests'",
    "--with-http_ssl_module --add-module='$moduleMsys'"
  ) -join " "
  $configurationIdentity = Get-TextSha256 "$configurationArguments`n$([IO.File]::ReadAllText((Join-Path $repo 'modules/ngx_http_laghu_module/config')))"
  $configurationMarker = Join-Path $source ".laghu-nginx-config.sha256"
  $makefile = Join-Path $source "objs\Makefile"
  $moduleConfiguration = Join-Path $repo "modules\ngx_http_laghu_module\config"
  $legacyConfigurationCached = $sourcesCached -and
    -not (Test-Path $configurationMarker) -and (Test-Path $makefile) -and
    (Get-Item $moduleConfiguration).LastWriteTimeUtc -le (Get-Item $makefile).LastWriteTimeUtc
  $configurationCached = $sourcesCached -and (Test-Path $configurationMarker) -and
    (Get-Content -Raw $configurationMarker).Trim() -eq $configurationIdentity
  $configurationCached = $configurationCached -or $legacyConfigurationCached
  $buildCommand = if ($configurationCached) { "nmake" } else { "$configurationArguments && nmake" }
  $configuration = "export PATH='$msvcBinMsys':/c/Strawberry/perl/bin:/usr/bin:`$PATH && cd '$sourceMsys' && $buildCommand"
  $log = Join-Path $workRoot "nginx-build.log"
  $savedErrorActionPreference = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  & $bash -lc $configuration 2>&1 | Tee-Object -FilePath $log | Out-Null
  $buildExitCode = $LASTEXITCODE
  $ErrorActionPreference = $savedErrorActionPreference
  if ($buildExitCode -ne 0) { throw "NGINX build failed; inspect $log" }
  Set-Content -Encoding ASCII $configurationMarker $configurationIdentity
  $root = Join-Path $outputRoot "nginx-$($sources.Nginx.Version)-$Architecture"
  Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force -Path "$root\conf", "$root\logs", "$root\temp" | Out-Null
  Copy-Item "$source\objs\nginx.exe" $root
  Copy-Item "$source\conf\mime.types" "$root\conf"
  Copy-License "$source\LICENSE" $root "nginx-LICENSE.txt"
  Copy-License "$libraries\pcre2-$($sources.Pcre2.Version)\COPYING" $root "pcre2-COPYING.txt"
  Copy-License "$libraries\zlib-$($sources.Zlib.Version)\LICENSE" $root "zlib-LICENSE.txt"
  Copy-License "$libraries\openssl-$($sources.OpenSsl.Version)\LICENSE.txt" $root "openssl-LICENSE.txt"
  @"
worker_processes 1;
error_log logs/error.log;
pid logs/nginx.pid;
events { worker_connections 128; }
http {
  include mime.types;
  laghu rum_store local:;
  laghu rum_store_local_snapshot @LAGHU_RUM_SNAPSHOT@;
  laghu rum_store_timeout 100;
  laghu rum_store_ttl 604800;
  laghu rum_store_retry_limit 3;
  laghu rum_store_sync_interval 5;
  laghu rum_store_memory_limit 8m;
  laghu rum_store_pending_limit 1m;
  laghu rum_store_required off;
  server {
    listen 127.0.0.1:8080;
    location / { laghu off; return 200 "Laghu NGINX for Windows`n"; }
  }
}
"@ | Set-Content -Encoding ASCII "$root\conf\nginx.conf"
  Write-BuildManifest $root "nginx" $sources.Nginx.Version
  & "$root\nginx.exe" -t -p $root
  if ($LASTEXITCODE -ne 0) { throw "Matched NGINX configuration failed" }
  Get-ChildItem "$root\logs", "$root\temp" -Force -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force
}

function Build-Apache {
  $archive = Get-VerifiedArchive "httpd" $sources.Apache
  $apr = Get-VerifiedArchive "apr" $sources.Apr
  $aprUtil = Get-VerifiedArchive "apr-util" $sources.AprUtil
  $sourceParent = Join-Path $workRoot "apache-source"
  $source = Join-Path $sourceParent "httpd-$($sources.Apache.Version)"
  $dependencySource = Join-Path $workRoot "apache-dependencies"
  $null = Initialize-VerifiedSources $sourceParent $sources.Apache.Sha256 {
    Expand-VerifiedArchive $archive $sourceParent
  }
  $dependencyIdentity = @($sources.Apr.Sha256, $sources.AprUtil.Sha256) -join "`n"
  $null = Initialize-VerifiedSources $dependencySource $dependencyIdentity {
    Expand-VerifiedArchive $apr $dependencySource
    Expand-VerifiedArchive $aprUtil $dependencySource
  }
  $aprSource = Join-Path $dependencySource "apr-$($sources.Apr.Version)"
  $aprUtilSource = Join-Path $dependencySource "apr-util-$($sources.AprUtil.Version)"
  $aprBuild = Join-Path $workRoot "apr-build"
  $aprUtilBuild = Join-Path $workRoot "apr-util-build"
  $build = Join-Path $workRoot "apache-build"
  $root = Join-Path $outputRoot "apache-$($sources.Apache.Version)-$Architecture"
  $rootCmake = $root.Replace('\', '/')
  $repoCmake = $repo.Replace('\', '/')
  $vcpkgInstalledCmake = $vcpkgInstalled.Replace('\', '/')
  $vcpkgToolchainCmake = $vcpkgToolchain.Replace('\', '/')
  Remove-Item -Recurse -Force $aprBuild, $aprUtilBuild, $build, $root -ErrorAction SilentlyContinue

  & cmake.exe -S $aprSource -B $aprBuild -G "Visual Studio 17 2022" -A $cmakeArchitecture `
    "-DCMAKE_INSTALL_PREFIX=$rootCmake" -DAPR_INSTALL_PRIVATE_H=ON `
    -DAPR_BUILD_TESTAPR=OFF -DAPR_BUILD_STATIC=OFF
  if ($LASTEXITCODE -ne 0) { throw "APR configure failed" }
  & cmake.exe --build $aprBuild --config Release --target INSTALL --parallel
  if ($LASTEXITCODE -ne 0) { throw "APR build failed" }

  & cmake.exe -S $aprUtilSource -B $aprUtilBuild -G "Visual Studio 17 2022" -A $cmakeArchitecture `
    "-DCMAKE_INSTALL_PREFIX=$rootCmake" "-DCMAKE_TOOLCHAIN_FILE=$vcpkgToolchainCmake" `
    "-DVCPKG_TARGET_TRIPLET=$vcpkgTriplet" "-DAPR_INCLUDE_DIR=$rootCmake/include" `
    "-DAPR_LIBRARIES=$rootCmake/lib/libapr-1.lib" -DAPU_HAVE_CRYPTO=OFF `
    -DAPU_HAVE_ODBC=OFF -DAPR_HAS_LDAP=OFF -DAPR_BUILD_TESTAPR=OFF
  if ($LASTEXITCODE -ne 0) { throw "APR-util configure failed" }
  & cmake.exe --build $aprUtilBuild --config Release --target INSTALL --parallel
  if ($LASTEXITCODE -ne 0) { throw "APR-util build failed" }

  & cmake.exe -S $source -B $build -G "Visual Studio 17 2022" -A $cmakeArchitecture `
    "-DCMAKE_INSTALL_PREFIX=$rootCmake" "-DCMAKE_PREFIX_PATH=$vcpkgInstalledCmake" `
    -DENABLE_MODULES=i -DENABLE_OPENSSL=OFF -DINSTALL_MANUAL=OFF `
    "-DAPR_INCLUDE_DIR=$rootCmake/include" `
    "-DAPR_LIBRARIES=$rootCmake/lib/libapr-1.lib;$rootCmake/lib/libaprutil-1.lib" `
    "-DPCRE_INCLUDE_DIR=$vcpkgInstalledCmake/include" `
    "-DPCRE_LIBRARIES=$vcpkgInstalledCmake/lib/pcre2-8.lib"
  if ($LASTEXITCODE -ne 0) { throw "Apache configure failed" }
  & cmake.exe --build $build --config Release --target INSTALL --parallel
  if ($LASTEXITCODE -ne 0) { throw "Apache build failed" }
  Copy-Item "$vcpkgInstalled\bin\pcre2-8.dll" "$root\bin" -ErrorAction Stop
  Copy-Item "$vcpkgInstalled\bin\libexpat.dll" "$root\bin" -ErrorAction Stop
  $moduleBuild = Join-Path $workRoot "mod-laghu-build"
  Remove-Item -Recurse -Force $moduleBuild -ErrorAction SilentlyContinue
  & cmake.exe -S "$repo\packaging\windows\mod-laghu" -B $moduleBuild -G "Visual Studio 17 2022" -A $cmakeArchitecture "-DLAGHU_SOURCE=$repoCmake" "-DAPACHE_ROOT=$rootCmake"
  if ($LASTEXITCODE -ne 0) { throw "mod_laghu configure failed" }
  & cmake.exe --build $moduleBuild --config Release --parallel
  if ($LASTEXITCODE -ne 0) { throw "mod_laghu build failed" }
  Copy-Item "$moduleBuild\Release\mod_laghu.so" "$root\modules\mod_laghu.so"
  Copy-License "$source\LICENSE" $root "apache-LICENSE.txt"
  Copy-License "$source\NOTICE" $root "apache-NOTICE.txt"
  Copy-License "$aprSource\LICENSE" $root "apr-LICENSE.txt"
  Copy-License "$aprSource\NOTICE" $root "apr-NOTICE.txt"
  Copy-License "$aprUtilSource\LICENSE" $root "apr-util-LICENSE.txt"
  Copy-License "$aprUtilSource\NOTICE" $root "apr-util-NOTICE.txt"
  Copy-License "$vcpkgInstalled\share\pcre2\copyright" $root "pcre2-copyright.txt"
  Add-Content -Encoding ASCII "$root\conf\httpd.conf" @"

LoadModule laghu_module modules/mod_laghu.so
Laghu Off
Laghu RumStore local:
Laghu RumStoreLocalSnapshot "@LAGHU_RUM_SNAPSHOT@"
Laghu RumStoreTimeout 100
Laghu RumStoreTtl 604800
Laghu RumStoreRetryLimit 3
Laghu RumStoreSyncInterval 5
Laghu RumStoreMemoryLimit 8m
Laghu RumStorePendingLimit 1m
Laghu RumStoreRequired Off
"@
  $requiredModules = @{}
  Get-Content "$root\conf\httpd.conf" | ForEach-Object {
    if ($_ -match '^\s*LoadModule\s+\S+\s+"?modules/([^"\s]+\.so)"?') {
      $requiredModules[$Matches[1].ToLowerInvariant()] = $true
    }
  }
  Get-ChildItem "$root\modules" -Filter "*.so" | Where-Object {
    -not $requiredModules.ContainsKey($_.Name.ToLowerInvariant())
  } | Remove-Item -Force
  Remove-Item -Recurse -Force "$root\include", "$root\lib", "$root\cgi-bin", `
    "$root\error", "$root\icons", "$root\manual" -ErrorAction SilentlyContinue
  Get-ChildItem "$root\htdocs" -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
  Write-BuildManifest $root "apache" $sources.Apache.Version
  & "$root\bin\httpd.exe" -t -d $root
  if ($LASTEXITCODE -ne 0) { throw "Matched Apache configuration failed" }
  Get-ChildItem "$root\logs" -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $downloadRoot, $workRoot, $outputRoot | Out-Null
Import-VisualStudioEnvironment
$env:CL = ""
$env:_CL_ = ""
$toolPaths = @("C:\Strawberry\perl\bin", "C:\Strawberry\c\bin") |
  Where-Object { Test-Path $_ }
if ($toolPaths.Count -ne 0) {
  $env:Path = ($toolPaths -join ";") + ";" + $env:Path
}
if ($Server -in @("nginx", "all")) { Build-Nginx }
if ($Server -in @("apache", "all")) { Build-Apache }
