; RemoteDesk installer (Inno Setup 6). Build with build-installer.bat.
;
; Wizard: Welcome -> Privacy policy (must accept) -> Permissions (must grant
; each one) -> Relay server -> Folder -> Optional services -> Install.
;
; Silent install needs explicit consent:
;   setup.exe /VERYSILENT /ACCEPTPRIVACY=yes /RELAY=relay.example.com:21116 [/TASKS=autostart]

#define AppName      "RemoteDesk"
#define AppVersion   "1.0.0"
#define AppExe       "RemoteDesk.exe"
#define PolicyDate   "2026-10-26"
; Pre-filled on the Relay Server page (the user can still change it).
#define DefaultRelay "10.13.174.83:21116"

[Setup]
AppId={{6B1D3C52-8F7A-4E0B-9C1E-2A5D7F4B9E31}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
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
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "..\build\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\PRIVACY.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\Privacy Policy"; Filename: "{app}\PRIVACY.txt"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon
Name: "{autostartup}\{#AppName}"; Filename: "{app}\{#AppExe}"; Flags: runminimized; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Description: "Start {#AppName} now"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\config.ini"

[Code]
const
  PERM_COUNT = 5;

var
  PermPage: TInputOptionWizardPage;
  RelayPage: TInputQueryWizardPage;

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
  Relay: String;
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
  PermPage.Add('Network: stay connected to the relay server so others can reach this computer');
  PermPage.Add('Keep awake: stop this computer from sleeping while a session is running');
  if WizardSilent then   // consent was given with /ACCEPTPRIVACY=yes
    for I := 0 to PERM_COUNT - 1 do
      PermPage.Values[I] := True;

  RelayPage := CreateInputQueryPage(PermPage.ID,
    'Relay Server',
    'Which relay server should {#AppName} use?',
    'Enter the address of your relay server as host:port (for example relay.example.com:21116). ' +
    'Use the same relay on every computer. You can change it later in the program.');
  RelayPage.Add('Relay server:', False);
  Relay := ExpandConstant('{param:RELAY|}');
  if Relay = '' then
    Relay := GetPreviousData('Relay', '{#DefaultRelay}');
  RelayPage.Values[0] := Relay;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'Relay', RelayPage.Values[0]);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  I: Integer;
  Relay: String;
begin
  Result := True;
  if CurPageID = PermPage.ID then
  begin
    for I := 0 to PERM_COUNT - 1 do
      if not PermPage.Values[I] then
      begin
        SuppressibleMsgBox('{#AppName} needs every permission on this page to work.' + #13#10#13#10 +
               'Tick all of them to continue, or click Cancel to stop the installation.',
               mbError, MB_OK, IDOK);
        Result := False;
        Exit;
      end;
  end
  else if CurPageID = RelayPage.ID then
  begin
    Relay := Trim(RelayPage.Values[0]);
    RelayPage.Values[0] := Relay;
    if (Relay = '') or (Pos(' ', Relay) > 0) then
    begin
      SuppressibleMsgBox('Enter the relay server as host:port, for example relay.example.com:21116.', mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'Privacy policy:' + NewLine + Space + 'Accepted (effective {#PolicyDate})' + NewLine + NewLine +
            'Permissions granted:' + NewLine +
            Space + 'Screen sharing, remote control, clipboard sync, network, keep awake' + NewLine + NewLine +
            'Relay server:' + NewLine + Space + RelayPage.Values[0] + NewLine + NewLine +
            MemoDirInfo + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + NewLine + MemoTasksInfo + NewLine;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Cfg: String;
begin
  if CurStep = ssPostInstall then
  begin
    // RemoteDesk.exe falls back to this relay until the user saves their own.
    Cfg := ExpandConstant('{app}\config.ini');
    SetIniString('RemoteDesk', 'Relay', Trim(RelayPage.Values[0]), Cfg);
    SetIniString('Consent', 'PrivacyPolicy', '{#PolicyDate}', Cfg);
    SetIniString('Consent', 'Permissions', 'screen,control,clipboard,network,keepawake', Cfg);
    SetIniString('Consent', 'GrantedOn', GetDateTimeString('yyyy-mm-dd hh:nn', '-', ':'), Cfg);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Data: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    Data := ExpandConstant('{userappdata}\{#AppName}');
    if DirExists(Data) and not UninstallSilent and
       (MsgBox('Also delete your {#AppName} address, password and settings?' + #13#10 +
               '(' + Data + ')', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
      DelTree(Data, True, True, True);
  end;
end;
