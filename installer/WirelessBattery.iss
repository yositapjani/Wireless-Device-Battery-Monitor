; Inno Setup script for Wireless+ Battery (https://jrsoftware.org/isinfo.php).
; Per-user install by default: no administrator rights needed, installs to %LOCALAPPDATA%\Programs.
; Setup also offers "Install for all users" (Program Files, needs administrator rights).
; Build with installer\build-installer.ps1 (or: ISCC /DAppVersion=1.2.3 installer\WirelessBattery.iss).

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#define Root ".."

[Setup]
AppId={{7E0B5C2A-3F4D-4C8E-9B61-2D5A8F1C4E93}
AppName=Wireless+ Battery
AppVersion={#AppVersion}
AppVerName=Wireless+ Battery {#AppVersion}
AppPublisher=Wireless+ Battery contributors
DefaultDirName={autopf}\Wireless+ Battery
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#Root}\dist
OutputBaseFilename=WirelessPlusBattery-Setup-{#AppVersion}
SetupIconFile={#Root}\res\app.ico
UninstallDisplayIcon={app}\WirelessBattery.exe
UninstallDisplayName=Wireless+ Battery
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=no

[Tasks]
Name: "autostart"; Description: "Start Wireless+ Battery when I sign in to Windows"

[Files]
Source: "{#Root}\WirelessBattery.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\third_party\headsetcontrol\headsetcontrol.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Root}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#Root}\installer\THIRD_PARTY_NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Wireless+ Battery"; Filename: "{app}\WirelessBattery.exe"

[Run]
Filename: "{app}\WirelessBattery.exe"; Description: "Start Wireless+ Battery now"; Flags: nowait postinstall skipifsilent
Filename: "https://ko-fi.com/krisgfx"; Description: "Enjoying Wireless+ Battery? Support it on Ko-fi"; Flags: shellexec postinstall unchecked skipifsilent nowait runasoriginaluser

[UninstallRun]
Filename: "{app}\WirelessBattery.exe"; Parameters: "--quit"; Flags: runhidden waituntilterminated; RunOnceId: "QuitApp"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

procedure QuitRunningApp();
var
  Code: Integer;
begin
  if FileExists(ExpandConstant('{app}\WirelessBattery.exe')) then
    Exec(ExpandConstant('{app}\WirelessBattery.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function InstalledRunValue(): String;
begin
  Result := '"' + ExpandConstant('{app}\WirelessBattery.exe') + '"';
end;

{ Removes the startup entry only if it points at this installed copy, so a portable copy's
  "Start with Windows" setting is left alone. }
procedure RemoveOwnAutostart();
var
  Value: String;
begin
  if RegQueryStringValue(HKCU, RunKey, 'WirelessBattery', Value) and (CompareText(Value, InstalledRunValue()) = 0) then
    RegDeleteValue(HKCU, RunKey, 'WirelessBattery');
end;

{ True if this account can create the chosen folder (or write to it if it already exists).
  Without administrator rights, folders such as D:\Programs or Program Files are often read-only. }
function CanWriteTo(Dir: String): Boolean;
var
  Parent, Probe: String;
begin
  Dir := RemoveBackslashUnlessRoot(Dir);
  if DirExists(Dir) then
  begin
    Probe := AddBackslash(Dir) + '~wirelessbattery-write-test.tmp';
    Result := SaveStringToFile(Probe, '', False);
    DeleteFile(Probe);
    exit;
  end;
  { Find the nearest folder that exists and check that a new folder can be created in it. }
  Parent := Dir;
  while not DirExists(Parent) do
  begin
    if ExtractFileDir(Parent) = Parent then
    begin
      Result := False;  { the drive itself does not exist }
      exit;
    end;
    Parent := ExtractFileDir(Parent);
  end;
  Probe := AddBackslash(Parent) + '~wirelessbattery-write-test';
  Result := CreateDir(Probe);
  if Result then
    RemoveDir(Probe);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  { Not in silent installs: there the wizard would wait on a hidden page forever. }
  if (CurPageID = wpSelectDir) and not WizardSilent() and not CanWriteTo(WizardDirValue()) then
  begin
    if IsAdminInstallMode() then
      MsgBox('Setup cannot create or write to this folder:' + #13#10 + WizardDirValue() + #13#10#13#10 +
        'Please choose a different folder.', mbError, MB_OK)
    else
      MsgBox('Your account cannot create or write to this folder without administrator rights:' + #13#10 +
        WizardDirValue() + #13#10#13#10 +
        'Choose a folder you own (the default one always works), or close Setup, run it again and ' +
        'pick "Install for all users".', mbError, MB_OK);
    Result := False;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    QuitRunningApp();  { upgrade: close the old version so its files can be replaced }
  if CurStep = ssPostInstall then
  begin
    { Same value the app's own "Start with Windows" menu item writes, so the two stay in sync. }
    if WizardIsTaskSelected('autostart') then
      RegWriteStringValue(HKCU, RunKey, 'WirelessBattery', InstalledRunValue())
    else
      RemoveOwnAutostart();
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveOwnAutostart();
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\WirelessBattery');
  end;
end;
