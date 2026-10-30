#ifndef AppVersion
  #error AppVersion is required; use Build-Distribution.ps1
#endif
#ifndef PayloadDir
  #error PayloadDir is required
#endif
#ifndef OutputDir
  #error OutputDir is required
#endif

[Setup]
AppId={{5E56C213-1CDD-46C9-AF95-B019007201D0}
AppName=AnimBridge
AppVersion={#AppVersion}
AppPublisher=AnimBridge
DefaultDirName={localappdata}\Programs\AnimBridge
DefaultGroupName=AnimBridge
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible and not arm64
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=AnimBridge-{#AppVersion}-Setup-x64
SetupIconFile=..\native\resources\animbridge.ico
UninstallDisplayIcon={app}\AnimBridge.exe
WizardStyle=modern
Compression=lzma2/fast
SolidCompression=yes
LZMANumBlockThreads=2
LZMADictionarySize=16384
DiskSpanning=no
LicenseFile={#PayloadDir}\licenses\INSTALL-TERMS.txt
InfoAfterFile={#PayloadDir}\INSTALL.txt
DisableProgramGroupPage=yes
DisableWelcomePage=no
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
Uninstallable=yes
AllowNoIcons=yes
ChangesEnvironment=no

[Languages]
Name: english; MessagesFile: compiler:Default.isl
Name: korean; MessagesFile: compiler:Languages\Korean.isl

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\AnimBridge"; Filename: "{app}\AnimBridge.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\AnimBridge"; Filename: "{app}\AnimBridge.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{group}\Uninstall AnimBridge"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\AnimBridge.exe"; Description: "Launch AnimBridge"; Flags: nowait postinstall skipifsilent

; Deliberately no [UninstallDelete]. Only installed payload files and shortcuts
; are removed; user caches, export folders, and input projects are retained.

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Parameters: String;
begin
  if CurStep = ssPostInstall then
  begin
    { Fixed WebView2 120+ on Windows 10 requires read/execute access for
      both application-package SIDs. Only the bundled runtime is changed. }
    Parameters := '"' + ExpandConstant('{app}\runtime\webview2') +
      '" /grant *S-1-15-2-1:(OI)(CI)(RX) /grant *S-1-15-2-2:(OI)(CI)(RX) /T /Q';
    if not Exec(ExpandConstant('{sys}\icacls.exe'), Parameters, '',
      SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      RaiseException('Could not prepare the bundled WebView2 runtime permissions. Windows error ' + IntToStr(ResultCode));
    if ResultCode <> 0 then
      RaiseException('Could not prepare the bundled WebView2 runtime permissions. icacls exit code ' + IntToStr(ResultCode));
  end;
end;
