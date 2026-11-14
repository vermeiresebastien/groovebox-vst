; Groovebox - Windows installer
;
; Installs the VST3 into a folder the host scans and the standalone into a per-user
; program folder. Follows the conventions of the PulseKit and SKULPT installers so all
; three products behave the same way for a user who installs them side by side:
;
;   * no administrator rights needed by default -- everything lands under LOCALAPPDATA;
;   * the VST3 goes into the per-user VST3 folder, which Bitwig, Reaper and Studio One
;     scan without any configuration -- and which, measured on this machine, is the
;     custom folder Ableton Live 12 is already set to scan;
;   * a task copies it into Documents\VST3 as well, for hosts that only see a folder the
;     user names explicitly (Live does not look in Documents\VST3 unless you point its
;     VST3 Plug-In Custom Folder setting there);
;   * the uninstaller removes every copy, so uninstalling really uninstalls.
;
; Sources are read straight from launch/, the one folder the build exports to, so the
; installer can never contain binaries older than the last build.
;
; Build with installer\build-installer.ps1, which calls ISCC.

#define MyAppName "Groovebox"
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#define MyAppPublisher "Skeer Audio"
#define MyAppURL "https://skeer.audio/"
#define MyAppExeName "Groovebox.exe"
#define Vst3Bundle "Groovebox.vst3"
#define Deliverables "..\launch"

[Setup]
; A fixed GUID identifies this product across versions, so an upgrade replaces the
; previous install rather than sitting beside it. Never reuse PulseKit's or SKULPT's.
AppId={{F3641E04-0627-4EDD-8510-050E4D0C7C93}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} installer
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
DefaultDirName={autopf}\{#MyAppPublisher}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Lowest by default so anyone can install; the dialog override adds an "all users" option
; on the wizard's first page for those who want the system-wide VST3 folder, which is what
; Cubase and Nuendo need. "commandline" is listed as well so /CURRENTUSER and /ALLUSERS are
; honored by scripted installs -- with "dialog" alone they are silently ignored, and a
; scripted install then lands machine-wide while looking like it succeeded.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#Deliverables}
OutputBaseFilename={#MyAppName} Setup {#MyAppVersion}
SetupIconFile=skeer.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
SetupLogging=yes
; Do NOT let Setup close running applications to get at a file. With "yes", a silent
; install that finds its target locked force-quits whatever holds it -- and on a machine
; where a host has the plugin loaded, that host is Ableton Live, unsaved set and all.
; Failing to write a locked file is the safe outcome: the wizard says so, the README
; says to quit the host first, and nothing gets killed on somebody's behalf. The house
; template (PulseKit, SKULPT) still sets "yes" with a *.vst3 filter, which is the worst
; combination -- it names exactly the file the DAW is holding.
CloseApplications=no
RestartApplications=no
AllowNoIcons=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "abletonfolder"; Description: "Also copy the VST3 into Documents\VST3 (needed for Ableton's custom folder)"; GroupDescription: "DAW integration"; Flags: checkedonce

[Files]
; The plugin itself. Which folder it lands in depends on the install mode chosen on the
; wizard's first page; see [Code] for why both are needed.
Source: "{#Deliverables}\{#Vst3Bundle}\*"; DestDir: "{commonpf64}\Common Files\VST3\{#Vst3Bundle}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: IsAdminInstallMode
Source: "{#Deliverables}\{#Vst3Bundle}\*"; DestDir: "{localappdata}\Programs\Common\VST3\{#Vst3Bundle}"; Flags: ignoreversion recursesubdirs createallsubdirs; Check: not IsAdminInstallMode

; The extra copy for hosts that only see a user-chosen folder.
Source: "{#Deliverables}\{#Vst3Bundle}\*"; DestDir: "{userdocs}\VST3\{#Vst3Bundle}"; Flags: ignoreversion recursesubdirs createallsubdirs; Tasks: abletonfolder

; The standalone, so the kit can be played without a host.
Source: "{#Deliverables}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "Software\Skeer\{#MyAppName}"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Skeer\{#MyAppName}"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Skeer\{#MyAppName}"; ValueType: string; ValueName: "VST3Path"; ValueData: "{code:GetVst3Path}"; Flags: uninsdeletekey

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Remove the plugin from whichever VST3 folder this install used, plus the Ableton copy.
; Inno removes files it installed anyway, but the bundle directory itself would survive.
Type: filesandordirs; Name: "{commonpf64}\Common Files\VST3\{#Vst3Bundle}"; Check: IsAdminInstallMode
Type: filesandordirs; Name: "{localappdata}\Programs\Common\VST3\{#Vst3Bundle}"; Check: not IsAdminInstallMode
Type: filesandordirs; Name: "{userdocs}\VST3\{#Vst3Bundle}"

[Code]
// The path recorded in the registry, so another tool can find the plugin without
// guessing which of the two possible locations was used.
function GetVst3Path(Param: String): String;
begin
  if IsAdminInstallMode then
    Result := ExpandConstant('{commonpf64}\Common Files\VST3\{#Vst3Bundle}')
  else
    Result := ExpandConstant('{localappdata}\Programs\Common\VST3\{#Vst3Bundle}');
end;

// Warn once, plainly, that the installer is not code-signed. Better a sentence here than
// a SmartScreen dialog the user has to interpret on their own.
//
// WizardForm does not exist in silent mode, so this has to bail out before touching it --
// doing so anyway makes the installer die on an exception, which an unattended install
// experiences as a hang: an error dialog nobody is there to dismiss.
procedure InitializeWizard;
begin
  if WizardSilent then
    Exit;

  WizardForm.WelcomeLabel2.Caption :=
    WizardForm.WelcomeLabel2.Caption + #13#10#13#10 +
    'This installer is not code-signed, so Windows may show a "Windows protected your PC" ' +
    'notice when it starts. Choose "More info" then "Run anyway" if you trust where you ' +
    'downloaded it from.';
end;
