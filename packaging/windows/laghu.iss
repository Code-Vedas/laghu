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
#ifndef RUNTIME_ROOT
  #error RUNTIME_ROOT is required
#endif

#if ARCHITECTURE == "x64"
  #define SetupArchitecture "x64os"
#else
  #define SetupArchitecture "arm64"
#endif

#if OFFERING == "ngx-laghu"
  #define ServerName "NGINX"
  #define ServerExe "nginx.exe"
  #define ConfigExe "{app}\server\nginx.exe"
  #define ConfigParams "-t -p ""{app}\server"""
#else
  #define ServerName "Apache HTTP Server"
  #define ServerExe "bin\httpd.exe"
  #define ConfigExe "{app}\server\bin\httpd.exe"
  #define ConfigParams "-t -d ""{app}\server"""
#endif

[Setup]
AppId=CodeVedas.{#OFFERING}
AppName={#OFFERING}
AppVersion={#VERSION}
AppPublisher=Codevedas Inc.
AppPublisherURL=https://laghu.codevedas.com
DefaultDirName={autopf}\Codevedas\{#OFFERING}
ArchitecturesAllowed={#SetupArchitecture}
ArchitecturesInstallIn64BitMode={#SetupArchitecture}
OutputDir={#OUTPUT}
OutputBaseFilename={#OFFERING}-{#VERSION}-windows-{#ARCHITECTURE}
PrivilegesRequired=admin
CloseApplications=no
RestartApplications=no
UninstallDisplayName={#OFFERING}
VersionInfoVersion={#VERSION}

[Dirs]
Name: "{commonappdata}\Laghu"
Name: "{commonappdata}\Laghu\images"

[Files]
Source: "{#SERVER_ROOT}\*"; DestDir: "{app}\server"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#RUNTIME_ROOT}\*"; DestDir: "{commonpf}\Codevedas\Laghu\bin"; Flags: ignoreversion recursesubdirs createallsubdirs uninsneveruninstall
Source: "{#REPO_ROOT}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#REPO_ROOT}\packaging\NOTICE-libvips.md"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKLM; Subkey: "Software\Codevedas\Laghu\Offerings\{#OFFERING}"; ValueType: string; ValueName: "Version"; ValueData: "{#VERSION}"; Flags: uninsdeletekey

[Code]
const
  SharedRuntime = '{commonpf}\Codevedas\Laghu\bin\laghu-libvips.exe';
  FetchRuntime = '{commonpf}\Codevedas\Laghu\bin\laghu-resource-fetch.exe';
  JavaScriptRuntime = '{commonpf}\Codevedas\Laghu\bin\laghu-js-optimize.exe';

procedure ExitProcess(ExitCode: Cardinal);
  external 'ExitProcess@kernel32.dll stdcall';

var
  ServiceCreatedBySetup: Boolean;
  ServiceWasPresent: Boolean;
  FetchServiceCreatedBySetup: Boolean;
  FetchServiceWasPresent: Boolean;
  JavaScriptServiceCreatedBySetup: Boolean;
  JavaScriptServiceWasPresent: Boolean;
  InstallSucceeded: Boolean;

function VersionWeight(const Value: String): Integer;
var
  Major, Minor, Patch, FirstDot, SecondDot: Integer;
begin
  FirstDot := Pos('.', Value);
  SecondDot := Pos('.', Copy(Value, FirstDot + 1, Length(Value)));
  if (FirstDot = 0) or (SecondDot = 0) then begin
    Result := -1;
    exit;
  end;
  SecondDot := SecondDot + FirstDot;
  Major := StrToIntDef(Copy(Value, 1, FirstDot - 1), -1);
  Minor := StrToIntDef(Copy(Value, FirstDot + 1, SecondDot - FirstDot - 1), -1);
  Patch := StrToIntDef(Copy(Value, SecondDot + 1, Length(Value)), -1);
  if (Major < 0) or (Minor < 0) or (Patch < 0) or
     (Major > 2000) or (Minor > 999) or (Patch > 999) then
    Result := -1
  else
    Result := Major * 1000000 + Minor * 1000 + Patch;
end;

function InitializeSetup(): Boolean;
var
  Installed: String;
begin
  Result := True;
  if RegQueryStringValue(HKLM,
    'Software\Codevedas\Laghu\Offerings\{#OFFERING}', 'Version', Installed) and
    (VersionWeight(Installed) > VersionWeight('{#VERSION}')) then begin
    if not WizardSilent then
      MsgBox('Downgrading {#OFFERING} from ' + Installed + ' to {#VERSION} is not supported.',
        mbError, MB_OK);
    Result := False;
  end;
end;

function RunAndRequire(const FileName, Parameters, Failure: String): Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(FileName, Parameters, '', SW_HIDE, ewWaitUntilTerminated,
    ResultCode) and (ResultCode = 0);
  if not Result then
    RaiseException(Failure + ' (exit ' + IntToStr(ResultCode) + ')');
end;

function ServiceExists(): Boolean;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\sc.exe'), 'query laghu-libvips', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
  Result := ResultCode = 0;
end;

function FetchServiceExists(): Boolean;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\sc.exe'), 'query laghu-resource-fetch', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
  Result := ResultCode = 0;
end;

function JavaScriptServiceExists(): Boolean;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\sc.exe'), 'query laghu-js-optimize', '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode);
  Result := ResultCode = 0;
end;

function StopSharedService(): Boolean;
var
  PowerShell, Command: String;
  ResultCode: Integer;
begin
  Result := True;
  if not ServiceExists() then
    exit;
  PowerShell := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  Command := '-NoProfile -NonInteractive -Command "' +
    '$service = Get-Service laghu-libvips -ErrorAction Stop; ' +
    'if ($service.Status -ne ''Stopped'') { ' +
    '$null = & sc.exe stop laghu-libvips; ' +
    '$service.WaitForStatus(''Stopped'', [TimeSpan]::FromSeconds(30)) }"';
  Result := Exec(PowerShell, Command, '', SW_HIDE, ewWaitUntilTerminated,
                 ResultCode) and (ResultCode = 0);
end;

function StopFetchService(): Boolean;
var
  PowerShell, Command: String;
  ResultCode: Integer;
begin
  Result := True;
  if not FetchServiceExists() then
    exit;
  PowerShell := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  Command := '-NoProfile -NonInteractive -Command "' +
    '$service = Get-Service laghu-resource-fetch -ErrorAction Stop; ' +
    'if ($service.Status -ne ''Stopped'') { ' +
    '$null = & sc.exe stop laghu-resource-fetch; ' +
    '$service.WaitForStatus(''Stopped'', [TimeSpan]::FromSeconds(30)) }"';
  Result := Exec(PowerShell, Command, '', SW_HIDE, ewWaitUntilTerminated,
                 ResultCode) and (ResultCode = 0);
end;

function StopJavaScriptService(): Boolean;
var
  PowerShell, Command: String;
  ResultCode: Integer;
begin
  Result := True;
  if not JavaScriptServiceExists() then exit;
  PowerShell := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  Command := '-NoProfile -NonInteractive -Command "' +
    '$service = Get-Service laghu-js-optimize -ErrorAction Stop; ' +
    'if ($service.Status -ne ''Stopped'') { ' +
    '$null = & sc.exe stop laghu-js-optimize; ' +
    '$service.WaitForStatus(''Stopped'', [TimeSpan]::FromSeconds(30)) }"';
  Result := Exec(PowerShell, Command, '', SW_HIDE, ewWaitUntilTerminated,
                 ResultCode) and (ResultCode = 0);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  ServiceWasPresent := ServiceExists();
  FetchServiceWasPresent := FetchServiceExists();
  JavaScriptServiceWasPresent := JavaScriptServiceExists();
  if ServiceWasPresent and not StopSharedService() then
    Result := 'Unable to stop the existing laghu-libvips service.';
  if (Result = '') and FetchServiceWasPresent and not StopFetchService() then
    Result := 'Unable to stop the existing laghu-resource-fetch service.';
  if (Result = '') and JavaScriptServiceWasPresent and
     not StopJavaScriptService() then
    Result := 'Unable to stop the existing laghu-js-optimize service.';
end;

procedure ConfigureFetchService();
var
  BinaryPath, QueuePath, CachePath, ProvidersPath: String;
begin
  QueuePath := ExpandConstant('{commonappdata}\Laghu\fonts.queue');
  CachePath := ExpandConstant('{commonappdata}\Laghu\images');
  ProvidersPath := ExpandConstant('{commonpf}\Codevedas\Laghu\bin\font-providers.conf');
  if not FileExists(QueuePath) then
    RunAndRequire(ExpandConstant(FetchRuntime),
      '--init "' + QueuePath + '" "' + CachePath + '" "' + ProvidersPath + '"',
      'Unable to initialize the Laghu font fetch queue');
  BinaryPath := GetShortName(ExpandConstant(FetchRuntime)) + ' --service ' +
    GetShortName(QueuePath) + ' ' + GetShortName(CachePath) + ' ' +
    GetShortName(ProvidersPath);
  if not FetchServiceExists() then begin
    RunAndRequire(ExpandConstant('{sys}\sc.exe'),
      'create laghu-resource-fetch start= auto binPath= "' + BinaryPath + '"',
      'Unable to create laghu-resource-fetch service');
    FetchServiceCreatedBySetup := True;
  end else
    RunAndRequire(ExpandConstant('{sys}\sc.exe'),
      'config laghu-resource-fetch start= auto binPath= "' + BinaryPath + '"',
      'Unable to update laghu-resource-fetch service');
  RunAndRequire(ExpandConstant('{sys}\sc.exe'), 'start laghu-resource-fetch',
    'Unable to start laghu-resource-fetch service');
end;

procedure ConfigureSharedService();
var
  BinaryPath, QueuePath, CachePath: String;
begin
  BinaryPath := GetShortName(ExpandConstant(SharedRuntime)) + ' --service';
  QueuePath := ExpandConstant('{commonappdata}\Laghu\jobs.queue');
  CachePath := ExpandConstant('{commonappdata}\Laghu\images');
  if not FileExists(QueuePath) then
    RunAndRequire(ExpandConstant(SharedRuntime),
      '--init "' + QueuePath + '" "' + CachePath + '"',
      'Unable to initialize the Laghu worker queue');
  if not ServiceExists() then begin
    RunAndRequire(ExpandConstant('{sys}\sc.exe'),
      'create laghu-libvips start= auto binPath= "' + BinaryPath + '"',
      'Unable to create laghu-libvips service');
    ServiceCreatedBySetup := True;
    RunAndRequire(ExpandConstant('{sys}\sc.exe'),
      'config laghu-libvips obj= "NT SERVICE\laghu-libvips"',
      'Unable to configure laghu-libvips service identity');
    RunAndRequire(ExpandConstant('{sys}\sc.exe'),
      'sidtype laghu-libvips unrestricted',
      'Unable to configure laghu-libvips service identity');
  end else begin
    RunAndRequire(ExpandConstant('{sys}\sc.exe'),
      'config laghu-libvips start= auto binPath= "' + BinaryPath + '"',
      'Unable to update laghu-libvips service');
  end;
  RunAndRequire(ExpandConstant('{sys}\icacls.exe'),
    '"' + ExpandConstant('{commonappdata}\Laghu') +
    '" /inheritance:r /grant:r "SYSTEM:(OI)(CI)F" "Administrators:(OI)(CI)F" ' +
    '"NT SERVICE\laghu-libvips:(OI)(CI)M"',
    'Unable to secure Laghu runtime directories');
  RunAndRequire(ExpandConstant('{sys}\sc.exe'), 'start laghu-libvips',
    'Unable to start laghu-libvips service');
end;

procedure ConfigureJavaScriptService();
var
  BinaryPath, QueuePath, CachePath: String;
begin
  QueuePath := ExpandConstant('{commonappdata}\Laghu\javascript.queue');
  CachePath := ExpandConstant('{commonappdata}\Laghu\images');
  if not FileExists(QueuePath) then
    RunAndRequire(ExpandConstant(JavaScriptRuntime),
      '--init "' + QueuePath + '" "' + CachePath + '"',
      'Unable to initialize the Laghu JavaScript queue');
  BinaryPath := GetShortName(ExpandConstant(JavaScriptRuntime)) +
    ' --service ' + GetShortName(QueuePath) + ' ' + GetShortName(CachePath);
  if not JavaScriptServiceExists() then begin
    RunAndRequire(ExpandConstant('{sys}\sc.exe'),
      'create laghu-js-optimize start= auto binPath= "' + BinaryPath + '"',
      'Unable to create laghu-js-optimize service');
    JavaScriptServiceCreatedBySetup := True;
  end else
    RunAndRequire(ExpandConstant('{sys}\sc.exe'),
      'config laghu-js-optimize start= auto binPath= "' + BinaryPath + '"',
      'Unable to update laghu-js-optimize service');
  RunAndRequire(ExpandConstant('{sys}\sc.exe'), 'start laghu-js-optimize',
    'Unable to start laghu-js-optimize service');
end;

function LastRuntimeReference(): Boolean;
begin
#if OFFERING == "ngx-laghu"
  Result := not RegKeyExists(HKLM,
    'Software\Codevedas\Laghu\Offerings\mod-laghu');
#else
  Result := not RegKeyExists(HKLM,
    'Software\Codevedas\Laghu\Offerings\ngx-laghu');
#endif
end;

procedure RollBackFailedInstall();
var
  ResultCode: Integer;
begin
  if ServiceCreatedBySetup then begin
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop laghu-libvips', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete laghu-libvips', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  end else if ServiceWasPresent then begin
    Exec(ExpandConstant('{sys}\sc.exe'), 'start laghu-libvips', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  end;
  if FetchServiceCreatedBySetup then begin
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop laghu-resource-fetch', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete laghu-resource-fetch', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  end else if FetchServiceWasPresent then
    Exec(ExpandConstant('{sys}\sc.exe'), 'start laghu-resource-fetch', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  if JavaScriptServiceCreatedBySetup then begin
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop laghu-js-optimize', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete laghu-js-optimize', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  end else if JavaScriptServiceWasPresent then
    Exec(ExpandConstant('{sys}\sc.exe'), 'start laghu-js-optimize', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  RegDeleteKeyIncludingSubkeys(HKLM,
    'Software\Codevedas\Laghu\Offerings\{#OFFERING}');
  RegDeleteKeyIncludingSubkeys(HKLM,
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\CodeVedas.{#OFFERING}_is1');
  DelTree(ExpandConstant('{app}'), True, True, True);
  if not ServiceWasPresent and LastRuntimeReference() then begin
    DelTree(ExpandConstant('{commonpf}\Codevedas\Laghu'), True, True, True);
    DelTree(ExpandConstant('{commonappdata}\Laghu'), True, True, True);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    try
      RunAndRequire(ExpandConstant('{#ConfigExe}'),
        ExpandConstant('{#ConfigParams}'),
        'Matched server configuration validation failed');
      ConfigureSharedService();
      ConfigureFetchService();
      ConfigureJavaScriptService();
    except
      RollBackFailedInstall();
      ExitProcess(7);
    end;
  end else if CurStep = ssDone then begin
    InstallSucceeded := True;
  end;
end;

procedure DeinitializeSetup();
var
  ResultCode: Integer;
begin
  if not InstallSucceeded and ServiceCreatedBySetup then begin
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop laghu-libvips', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete laghu-libvips', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  end else if not InstallSucceeded and ServiceWasPresent then begin
    Exec(ExpandConstant('{sys}\sc.exe'), 'start laghu-libvips', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  end;
  if not InstallSucceeded and FetchServiceCreatedBySetup then begin
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop laghu-resource-fetch', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete laghu-resource-fetch', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  end else if not InstallSucceeded and FetchServiceWasPresent then
    Exec(ExpandConstant('{sys}\sc.exe'), 'start laghu-resource-fetch', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  if not InstallSucceeded and JavaScriptServiceCreatedBySetup then begin
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop laghu-js-optimize', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete laghu-js-optimize', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
  end else if not InstallSucceeded and JavaScriptServiceWasPresent then
    Exec(ExpandConstant('{sys}\sc.exe'), 'start laghu-js-optimize', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if (CurUninstallStep = usUninstall) and LastRuntimeReference() then begin
    StopSharedService();
    StopFetchService();
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete laghu-libvips', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete laghu-resource-fetch', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'stop laghu-js-optimize', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    Exec(ExpandConstant('{sys}\sc.exe'), 'delete laghu-js-optimize', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
    DelTree(ExpandConstant('{commonpf}\Codevedas\Laghu'), True, True, True);
    DelTree(ExpandConstant('{commonappdata}\Laghu'), True, True, True);
  end;
end;
