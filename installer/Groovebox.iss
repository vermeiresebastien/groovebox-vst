; Groovebox - Windows installer
;
; ONE destination for the plugin: C:\Program Files\Common Files\VST3. That is the folder
; every host on this machine scans, so a second copy is not a convenience -- it is a
; duplicate entry in the browser plus two bundles that drift apart the first time one of
; them is rebuilt. An earlier version of this script also wrote a per-user copy and
; offered a task that copied it into Documents\VST3; both are gone. The retired per-user
; paths are NOT kept in [UninstallDelete]: see the note there for why an elevated
; uninstaller must not resolve them, and that the README tells the user to clean them.
;
; Consequences of writing to Program Files:
;
;   * the setup asks for elevation (UAC). That is the price of a machine-wide plugin, and
;     the rule outranks installing without a prompt;
;   * because the process is elevated, it must not touch per-user areas -- the registry
;     hints are HKLM, not HKCU, and there is no {localappdata} or {userdocs} left in here.
;     Inno warns about that combination and the warning is correct, so the causes are
;     fixed rather than silenced with UsedUserAreasWarning=no;
;   * the standalone lives under {app} in Program Files too, so it needs no separate
;     privilege story, and the uninstaller removes every copy it made.
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
; Machine-wide, because the plugin has exactly one home and it sits in Program Files.
; Setup therefore asks for elevation. There is deliberately no
; PrivilegesRequiredOverridesAllowed line: /CURRENTUSER and the wizard's "all users"
; toggle are exactly how a second, drifting copy came back last time, and a run that
; silently downgraded would put the VST3 somewhere the hosts do not all scan.
PrivilegesRequired=admin
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

[Files]
; The plugin, in the one folder every host scans. No Check= and no Tasks= on this line:
; a condition here is what let a second destination exist alongside it.
Source: "{#Deliverables}\{#Vst3Bundle}\*"; DestDir: "{commonpf64}\Common Files\VST3\{#Vst3Bundle}"; Flags: ignoreversion recursesubdirs createallsubdirs

; The standalone, so the kit can be played without a host.
Source: "{#Deliverables}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "README.txt"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; HKLM, not HKCU: an elevated setup writing per-user keys leaves the next non-elevated
; read looking in the wrong hive, and HKCU here would be that mistake in the other
; direction -- this install is machine-wide, so its facts belong in the machine hive.
Root: HKLM; Subkey: "Software\Skeer\{#MyAppName}"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Skeer\{#MyAppName}"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Skeer\{#MyAppName}"; ValueType: string; ValueName: "VST3Path"; ValueData: "{commonpf64}\Common Files\VST3\{#Vst3Bundle}"; Flags: uninsdeletekey

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; The live destination: Setup removes the files it installed, but the .vst3 bundle directory
; itself would survive as an empty shell, and hosts list a bare folder.
Type: filesandordirs; Name: "{commonpf64}\Common Files\VST3\{#Vst3Bundle}"
; The retired per-user destinations are NOT deleted from here, deliberately. An elevated
; uninstaller resolves {localappdata} and {userdocs} to whoever is clicking, not to the user
; an old unprivileged install wrote for -- so the lines would delete the wrong profile's
; copy and call it done. ISCC warns about exactly this ("PrivilegesRequired is set to admin
; but per-user areas are used"), and the fix is to stop reaching into those areas, not to
; silence the warning. Nothing is orphaned by that choice: a previous Groovebox setup
; registered its own copies in its install log and removes them when it is uninstalled or
; upgraded (same AppId), and a copy somebody placed by hand is cleaned by hand -- the
; README carries the one-line check and delete for it.

[Code]
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
