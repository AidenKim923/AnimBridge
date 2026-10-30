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
; The names below are what ActiveLanguage returns; native/include/animbridge/install_discovery.hpp
; (languageFromInstaller) maps them to the app's UI locales, so keep both in sync.
; Inno Setup ships no Chinese or Hindi translation, so zh-CN, zh-TW and hi have no installer
; language. The app itself still supports them (the OS UI language is used as the default).
Name: english; MessagesFile: compiler:Default.isl
Name: arabic; MessagesFile: compiler:Languages\Arabic.isl
Name: brazilianportuguese; MessagesFile: compiler:Languages\BrazilianPortuguese.isl
Name: french; MessagesFile: compiler:Languages\French.isl
Name: japanese; MessagesFile: compiler:Languages\Japanese.isl
Name: korean; MessagesFile: compiler:Languages\Korean.isl
Name: polish; MessagesFile: compiler:Languages\Polish.isl
Name: portuguese; MessagesFile: compiler:Languages\Portuguese.isl
Name: russian; MessagesFile: compiler:Languages\Russian.isl
Name: spanish; MessagesFile: compiler:Languages\Spanish.isl
Name: thai; MessagesFile: compiler:Languages\Thai.isl
Name: turkish; MessagesFile: compiler:Languages\Turkish.isl

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
; The in-app updater runs this installer with /VERYSILENT ... /RELAUNCH=1 after closing
; AnimBridge. A silent install shows no finish page, but postinstall entries still run
; unless skipped, so this entry (silent only, and only when asked) restarts the app.
; A plain silent install by an administrator does not start the app.
Filename: "{app}\AnimBridge.exe"; Flags: nowait postinstall skipifnotsilent; Check: RelaunchRequested

; Only installed payload files, shortcuts and the installer-written language file are
; removed; user caches, export folders, and input projects are retained.
[UninstallDelete]
Type: files; Name: "{app}\install-language.txt"

[Code]
{ True when the in-app updater started Setup with /RELAUNCH=1. }
function RelaunchRequested: Boolean;
begin
  Result := ExpandConstant('{param:relaunch|0}') = '1';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  Parameters: String;
begin
  if CurStep = ssPostInstall then
  begin
    { Remember the language chosen in Setup. The app reads this file on its first run
      to preselect its interface language (ASCII, e.g. "korean"). A failed write only
      means the app falls back to the Windows display language. }
    SaveStringToFile(ExpandConstant('{app}\install-language.txt'), ActiveLanguage, False);
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
