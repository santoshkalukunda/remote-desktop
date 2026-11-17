; RemoteDesk installer (Inno Setup 6). Build with build-installer.bat.
;
; Wizard: Welcome -> Privacy policy (must accept) -> Permissions (must grant each one)
;         -> Folder -> Optional services -> Install.
;
; Users are never asked for a server: the app connects to our relay, compiled in from
; src\core\server_config.h. Administrators can point one installation elsewhere (testing,
; private networks) on the command line only:
;   setup.exe /RELAYHOST=relay.example.com /RELAYPORT=21116
;
; Silent install needs explicit consent:
;   setup.exe /VERYSILENT /ACCEPTPRIVACY=yes [/TASKS=autostart]
; In-app updates run this installer with /SILENT /ACCEPTPRIVACY=yes and it restarts the app.

#define AppName      "RemoteDesk"
; The version is read from the exe (src\core\version.h -> app.rc): one place to bump it.
#define AppVersion   RemoveFileExt(GetVersionNumbersString(AddBackslash(SourcePath) + "..\build\RemoteDesk.exe"))
#define AppExe       "RemoteDesk.exe"
#define PolicyDate   "2026-10-26"

[Setup]
AppId={{6B1D3C52-8F7A-4E0B-9C1E-2A5D7F4B9E31}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; Setup asks "all users" (default: needed for the lock-screen service) or "only me" (no admin rights
; needed). In-app updates keep whichever was chosen (/ALLUSERS or /CURRENTUSER).
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
; Always ask (don't silently reuse "only me" from an earlier install: that would hide the lock-screen option).
UsePreviousPrivileges=no
LicenseFile=..\PRIVACY.txt
OutputDir=..\build
OutputBaseFilename=setup
SetupIconFile=..\src\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
AppMutex=Local\RemoteDeskSingleInstance
CloseApplications=yes
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} Setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WizardLicense=Privacy Policy
LicenseLabel=Please read how {#AppName} handles your information.
LicenseLabel3=You must accept the privacy policy before installation can continue.
LicenseAccepted=I &accept the privacy policy
LicenseNotAccepted=I &do not accept the privacy policy

[Tasks]
Name: "autostart"; Description: "Start {#AppName} when I sign in to Windows (keeps this computer reachable by its address)"; GroupDescription: "Optional services:"; Flags: unchecked
; Lock screen, sign-in screen and Ctrl+Alt+Del need the RemoteDesk service: only when installing for all users.
Name: "lockscreen"; Description: "Allow access to the lock and sign-in screens (installs the RemoteDesk service; only with your password or approval)"; GroupDescription: "Optional services:"; Check: IsAdminInstallMode
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "..\build\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\PRIVACY.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\RemoteDesk - Wake-on-LAN guide.pdf"; DestDir: "{app}"; DestName: "Wake-on-LAN guide.pdf"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Privacy Policy"; Filename: "{app}\PRIVACY.txt"
Name: "{group}\Wake-on-LAN guide"; Filename: "{app}\Wake-on-LAN guide.pdf"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[InstallDelete]
; Older versions started with Windows through a Startup-folder shortcut; the Run value replaces it.
Type: files; Name: "{autostartup}\{#AppName}.lnk"

[Registry]
; The app's "Start with Windows" toggle reads and changes this same value.
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"" --autostart"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
; The service first, so the app finds its agent when it starts.
Filename: "{app}\{#AppExe}"; Parameters: "--install-service"; StatusMsg: "Starting the RemoteDesk service..."; Flags: runhidden waituntilterminated; Tasks: lockscreen
Filename: "{app}\{#AppExe}"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated; Tasks: not lockscreen; Check: IsAdminInstallMode
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName} now"; Flags: nowait postinstall skipifsilent runasoriginaluser
; In-app updates run setup silently: start the new version again afterwards.
Filename: "{app}\{#AppExe}"; Flags: nowait skipifnotsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\{#AppExe}"; Parameters: "--uninstall-service"; Flags: runhidden waituntilterminated; RunOnceId: "RemoteDeskService"

[UninstallDelete]
Type: files; Name: "{app}\config.ini"

[Code]
const
  PERM_COUNT = 5;
  RUN_KEY = 'Software\Microsoft\Windows\CurrentVersion\Run';

var
  PermPage: TInputOptionWizardPage;

function InitializeSetup(): Boolean;
begin
  Result := True;
  // Silent installs skip the wizard, so consent must be given on the command line.
  if WizardSilent and (CompareText(ExpandConstant('{param:ACCEPTPRIVACY|no}'), 'yes') <> 0) then
  begin
    Log('Silent install refused: pass /ACCEPTPRIVACY=yes to accept the privacy policy and permissions.');
    Result := False;
  end;
end;

procedure InitializeWizard();
var
  I: Integer;
begin
  PermPage := CreateInputOptionPage(wpLicense,
    'Permissions',
    '{#AppName} needs your permission to provide its services.',
    'Tick each permission to allow it. {#AppName} cannot work without all of them. ' +
    'Nobody can connect unless you accept the request or have set an unattended access password.',
    False, False);
  PermPage.Add('Screen sharing: show this computer''s screen to a viewer you accept');
  PermPage.Add('Remote control: let that viewer use the mouse and keyboard on this computer');
  PermPage.Add('Clipboard sync: exchange copied text with the other computer during a session');
  PermPage.Add('Network: stay connected to the RemoteDesk server so others can reach this computer');
  PermPage.Add('Keep awake: stop this computer from sleeping while a session is running');
  if WizardSilent then   // consent was given with /ACCEPTPRIVACY=yes
    for I := 0 to PERM_COUNT - 1 do
      PermPage.Values[I] := True;
end;

// "Only me": the lock-screen option can't be offered (a service needs all users). Say so.
procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpSelectTasks) and not IsAdminInstallMode then
    WizardForm.SelectTasksLabel.Caption := WizardForm.SelectTasksLabel.Caption + #13#10#13#10 +
      'To allow access to this computer''s lock and sign-in screens, go back to the start and choose ' +
      '"Install for all users".';
end;

// Installing for all users over an earlier "only me" copy: remove that copy first (settings in
// %APPDATA% stay), so there aren't two RemoteDesks starting with Windows.
procedure RemovePerUserCopy();
var
  Uninst: String;
  Code: Integer;
begin
  if IsAdminInstallMode and
     RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B1D3C52-8F7A-4E0B-9C1E-2A5D7F4B9E31}_is1',
                         'UninstallString', Uninst) then
  begin
    Log('Removing the per-user copy: ' + Uninst);
    // Its app may still be running (the uninstaller won't remove a running copy); setup starts RemoteDesk again at the end.
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#AppExe}', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Exec(RemoveQuotes(Uninst), '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;
end;

// The service's agent runs RemoteDesk.exe as SYSTEM: stop it so the file can be replaced.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Result := '';
  if IsAdminInstallMode and FileExists(ExpandConstant('{app}\{#AppExe}')) then
    Exec(ExpandConstant('{app}\{#AppExe}'), '--stop-service', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  I: Integer;
begin
  Result := True;
  if CurPageID = PermPage.ID then
    for I := 0 to PERM_COUNT - 1 do
      if not PermPage.Values[I] then
      begin
        SuppressibleMsgBox('{#AppName} needs every permission on this page to work.' + #13#10#13#10 +
               'Tick all of them to continue, or click Cancel to stop the installation.',
               mbError, MB_OK, IDOK);
        Result := False;
        Exit;
      end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'Privacy policy:' + NewLine + Space + 'Accepted (effective {#PolicyDate})' + NewLine + NewLine +
            'Permissions granted:' + NewLine +
            Space + 'Screen sharing, remote control, clipboard sync, network, keep awake' + NewLine + NewLine +
            MemoDirInfo + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + MemoTasksInfo + NewLine;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Cfg, Host, Port: String;
begin
  if CurStep = ssInstall then RemovePerUserCopy();
  if CurStep = ssPostInstall then
  begin
    Cfg := ExpandConstant('{app}\config.ini');
    // Relay: our server (compiled into the app) unless an administrator overrides it here.
    // Clearing old entries moves installs made by earlier setups onto our server.
    DeleteIniEntry('RemoteDesk', 'RelayHost', Cfg);
    DeleteIniEntry('RemoteDesk', 'RelayPort', Cfg);
    DeleteIniEntry('RemoteDesk', 'Relay', Cfg);
    DeleteIniEntry('RemoteDesk', 'InstallStamp', Cfg);
    Host := Trim(ExpandConstant('{param:RELAYHOST|}'));
    Port := Trim(ExpandConstant('{param:RELAYPORT|}'));
    if Host <> '' then
    begin
      SetIniString('RemoteDesk', 'RelayHost', Host, Cfg);
      if Port <> '' then SetIniString('RemoteDesk', 'RelayPort', Port, Cfg);
      Log('Relay overridden for this installation: ' + Host + ':' + Port);
    end;
    SetIniString('Consent', 'PrivacyPolicy', '{#PolicyDate}', Cfg);
    SetIniString('Consent', 'Permissions', 'screen,control,clipboard,network,keepawake', Cfg);
    SetIniString('Consent', 'GrantedOn', GetDateTimeString('yyyy-mm-dd hh:nn', '-', ':'), Cfg);
    // Unticked on a re-install: also turn off what an earlier install or the app enabled.
    if not WizardIsTaskSelected('autostart') then
    begin
      RegDeleteValue(HKA, RUN_KEY, '{#AppName}');
      RegDeleteValue(HKCU, RUN_KEY, '{#AppName}');
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  if CurUninstallStep = usUninstall then
    RegDeleteValue(HKCU, RUN_KEY, '{#AppName}');   // set by the app's own toggle
  if CurUninstallStep = usPostUninstall then
  begin
    Data := ExpandConstant('{userappdata}\{#AppName}');
    if DirExists(Data) and not UninstallSilent and
       (MsgBox('Also delete your {#AppName} address, password and settings?' + #13#10 +
               '(' + Data + ')', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
      DelTree(Data, True, True, True);
  end;
end;
