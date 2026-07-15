; Copyright Codevedas Inc. 2026-present
;
; This source code is licensed under the MIT license found in the
; LICENSE file in the root directory of this source tree.

#ifndef OFFERING
  #error OFFERING is required
#endif
#ifndef ARCHITECTURE
  #error ARCHITECTURE is required
#endif

#if OFFERING == "ngx-laghu"
  #define ServerName "NGINX"
  #define ServerExe "nginx.exe"
  #define ConfigTest """{app}\server\nginx.exe"" -t -p ""{app}\server"""
#else
  #define ServerName "Apache HTTP Server"
  #define ServerExe "bin\httpd.exe"
  #define ConfigTest """{app}\server\bin\httpd.exe"" -t -d ""{app}\server"""
#endif

[Setup]
AppId=CodeVedas.{#OFFERING}
AppName={#OFFERING}
AppVersion={#VERSION}
AppPublisher=Codevedas Inc.
AppPublisherURL=https://laghu.codevedas.com
DefaultDirName={autopf}\Codevedas\{#OFFERING}
ArchitecturesAllowed={#ARCHITECTURE}
ArchitecturesInstallIn64BitMode={#ARCHITECTURE}
OutputDir={#OUTPUT}
OutputBaseFilename={#OFFERING}-{#VERSION}-windows-{#ARCHITECTURE}
PrivilegesRequired=admin
UninstallDisplayName={#OFFERING}
VersionInfoVersion={#VERSION}

[Dirs]
Name: "{commonappdata}\Laghu"; Permissions: users-modify
Name: "{commonappdata}\Laghu\images"; Permissions: users-modify

[Files]
Source: "{#SERVER_ROOT}\*"; DestDir: "{app}\server"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SERVICE_BINARY}"; DestDir: "{app}\bin"; DestName: "laghu-libvips.exe"; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "packaging\NOTICE-libvips.md"; DestDir: "{app}"; Flags: ignoreversion

[Run]
Filename: "{sys}\sc.exe"; Parameters: "create laghu-libvips start= auto binPath= """"{app}\bin\laghu-libvips.exe"" --service"""; Flags: runhidden
Filename: "{sys}\sc.exe"; Parameters: "start laghu-libvips"; Flags: runhidden
Filename: "{cmd}"; Parameters: "/c {#ConfigTest}"; Flags: runhidden

[UninstallRun]
Filename: "{sys}\sc.exe"; Parameters: "stop laghu-libvips"; Flags: runhidden; RunOnceId: "StopLaghuLibvips"
Filename: "{sys}\sc.exe"; Parameters: "delete laghu-libvips"; Flags: runhidden; RunOnceId: "DeleteLaghuLibvips"
