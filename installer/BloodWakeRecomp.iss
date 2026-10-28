; Blood Wake Recomp: Setup and Uninstall (Inno Setup 6.7.3).
;
; Build it with installer/build_installer.py, which stages the public source
; tree and passes SrcTree, OutDir, AppVer and the retail XBE fingerprint.
;
; Setup's pages: welcome, disclaimer (tick "I own Blood Wake..."), your disc
; (a folder, an XISO or a full disc image; its main program is checked against
; the retail fingerprint at once), install folder (per-user, no admin), what's
; needed (detects Visual Studio 2022's C++ tools; offers Microsoft's Build Tools
; if missing), shortcuts, ready, then the install: our files, a private Python
; and the pinned Python packages (downloaded, SHA-256 checked), then
; tools/build_from_disc.py with its progress on the bar, then the shortcuts.
;
; Unattended:
;   BloodWakeRecomp-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /IOWNTHEGAME
;       /DISC="<folder or image>" [/DIR="<install folder>"] [/TASKS=desktopicon]
;       [/INSTALLVS] [/VSPLANONLY] [/JOBS=N] [/TOOLKIT=<dir>] [/LOG="<file>"]
;   /IOWNTHEGAME  you own Blood Wake and made this copy from your own disc (required when silent)
;   /INSTALLVS    allowed to install Microsoft's Build Tools if no Visual Studio 2022 C++ is found
;   /VSPLANONLY   download and hash-check the Build Tools bootstrapper and write the
;                 command Setup would run to deps\vs_plan.txt, but never run it (testing)
;   /SIMULATENOVS act as if no Visual Studio were found (testing the Build Tools path)
;   /SIMULATENOCMAKE act as if no CMake were found (testing pip's CMake)
;   /STUBBUILD    a fake 5-second build with placeholder files (testing the pages and uninstall)
;   Exit codes: 0 ok; 100 + build_from_disc.py's code when the build stopped (102 not an
;   unmodified retail copy, 103 a dependency, 104 pipeline, 105 generated code differs,
;   106 compiler, 107 disk space or I/O); 110 the Build Tools install failed or was declined;
;   Inno's own codes otherwise (2 = stopped on a page: the log says why).
;
; Uninstall (unins000.exe, or Settings > Apps): always removes the build, the
; installed files and the shortcuts. Options (a window, or these when silent):
;   saves and settings   kept by default            /REMOVESAVES removes them
;   the private Python   removed by default         /KEEPPYTHON keeps it
;   the game's files     removed by default         /KEEPGAME keeps them
;   the shader cache     removed by default         /KEEPSHADERCACHE keeps it
;   Build Tools          kept by default; offered only if this Setup installed them
;                                                   /REMOVEVS removes them
;   /USERDATAROOT=<dir>  (testing) act on <dir>\xboxrecomp and <dir>\BloodWake instead of
;                        %LOCALAPPDATA%'s, so a test uninstall never touches real saves
;   /VSPLANONLY          (testing) log the Build Tools uninstall command instead of running it

#ifndef SrcTree
  #error Build with installer/build_installer.py (SrcTree is not defined)
#endif
#ifndef OutDir
  #define OutDir "out"
#endif
#ifndef AppVer
  #define AppVer "0.1.0"
#endif
#ifndef XbeSize
  #error XbeSize is not defined (build_installer.py reads it from fingerprints/retail.json)
#endif
#ifndef XbeSha256
  #error XbeSha256 is not defined
#endif
#ifndef RetailLabel
  #define RetailLabel "Xbox retail"
#endif

; ── Pinned downloads (official sources; SHA-256 checked by Setup) ─────────
#define PyVer        "3.13.15"
#define PyZipUrl     "https://www.python.org/ftp/python/3.13.15/python-3.13.15-amd64.zip"
#define PyZipSha256  "6479223746cdfb79d25865110d6f524ac98de081324e119af1dc3ae36bddc7a5"
#define PyZipBytes   "34324622"
#define PyUnpacked   "125031554"
#define CapWhl       "capstone-5.0.9-py3-none-win_amd64.whl"
#define CapUrl       "https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/capstone-5.0.9-py3-none-win_amd64.whl"
#define CapSha256    "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0"
#define CapBytes     "1273459"
#define PefWhl       "pefile-2024.8.26-py3-none-any.whl"
#define PefUrl       "https://files.pythonhosted.org/packages/54/16/12b82f791c7f50ddec566873d5bdd245baa1491bac11d15ffb98aecc8f8b/pefile-2024.8.26-py3-none-any.whl"
#define PefSha256    "76f8b485dcd3b1bb8166f1128d395fa3d87af26360c2358fb75b80019b957c6f"
#define PefBytes     "74766"
#define CmakeWhl     "cmake-4.4.4-py3-none-win_amd64.whl"
#define CmakeUrl     "https://files.pythonhosted.org/packages/87/9c/a7918eeeed09b34aa5dc1b767367fb1a2dcbb56171be00be0fd1280fe752/cmake-4.4.4-py3-none-win_amd64.whl"
#define CmakeSha256  "ad8e0a38b5707e27882701146bdfffecacd80e7703fa0fc36c77528518d545af"
#define CmakeBytes   "42321211"
; Microsoft's fixed-version bootstrapper for Build Tools 17.14.43 (2026-10-25), from the
; Visual Studio 2022 release history page; the URL's second part is the file's SHA-256.
#define VsVer        "17.14.43"
#define VsUrl        "https://download.visualstudio.microsoft.com/download/pr/4bc08c27-bd31-470c-9a30-3b62707fbf0f/dbea89df9de387e415075d40d713ecc1a73091d03e81080c3a957101f390f499/vs_BuildTools.exe"
#define VsSha256     "dbea89df9de387e415075d40d713ecc1a73091d03e81080c3a957101f390f499"
#define VsBytes      "4474088"

[Setup]
AppId={{0FB930E5-4F64-4720-9243-3E2404D38AB5}
AppName=Blood Wake Recomp
AppVersion={#AppVer}
AppVerName=Blood Wake Recomp {#AppVer}
AppPublisher=Blood Wake Recomp (unofficial fan project)
AppPublisherURL=https://github.com/minimalice92/bloodwake-recomp
AppSupportURL=https://github.com/minimalice92/bloodwake-recomp/issues
AppComments=Unofficial, non-commercial. Not affiliated with Microsoft or Stormfront Studios. Built on this PC from your own copy of Blood Wake.
DefaultDirName={autopf}\Blood Wake Recomp
DefaultGroupName=Blood Wake Recomp
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableDirPage=no
UsePreviousAppDir=yes
DirExistsWarning=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
ArchiveExtraction=full
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern dynamic windows11
OutputDir={#OutDir}
OutputBaseFilename=BloodWakeRecomp-Setup-{#AppVer}
; The unpacked game (2.0 GB), the work folder (toolkit, generated code, build: about 0.25 GB), margin.
ExtraDiskSpaceRequired=2700000000
UninstallDisplayName=Blood Wake Recomp
UninstallDisplayIcon={uninstallexe}
SetupLogging=yes
SetupMutex=BloodWakeRecompSetup
CloseApplications=no
ShowLanguageDialog=no
VersionInfoDescription=Blood Wake Recomp Setup (unofficial)
VersionInfoProductName=Blood Wake Recomp

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel1=Welcome to Blood Wake Recomp
WelcomeLabel2=This sets up Blood Wake Recomp, an unofficial fan port that runs Blood Wake (Xbox, 2001) natively on Windows.%n%nThe game is built on this PC from your own copy, so you need:%n%n-  the files from your own Blood Wake disc: a folder of them, an XISO or a full disc image%n-  about 3 GB of free space (more if Setup has to add Microsoft's C++ build tools)%n-  an internet connection, for Python, the toolkit and the build tools%n-  about 15 to 30 minutes: Setup translates the game's code and compiles it.%n%nNothing from the game is downloaded or sent anywhere.
ConfirmUninstall=Remove %1 now, with the choices you made?
FinishedHeadingLabel=Blood Wake Recomp

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts (a Start menu shortcut is always added):"

[Files]
Source: "disclaimer.txt"; Flags: dontcopy
Source: "setup_helper.py"; DestDir: "{app}\setup"; Flags: ignoreversion
Source: "{#SrcTree}\*"; DestDir: "{app}\source"; Flags: recursesubdirs createallsubdirs ignoreversion
; The private Python: python.org's full Windows package (a zip: nothing is registered,
; PATH is untouched). Kept or removed by the uninstaller's choice, not Inno's log.
Source: "{#PyZipUrl}"; DestDir: "{app}\python"; DestName: "python-{#PyVer}-amd64.zip"; \
  ExternalSize: {#PyUnpacked}; Hash: "{#PyZipSha256}"; \
  Flags: external download extractarchive recursesubdirs createallsubdirs ignoreversion uninsneveruninstall; \
  Check: NeedPython; AfterInstall: PythonUnpacked
Source: "{#CapUrl}"; DestDir: "{app}\deps"; DestName: "{#CapWhl}"; ExternalSize: {#CapBytes}; \
  Hash: "{#CapSha256}"; Flags: external download ignoreversion
Source: "{#PefUrl}"; DestDir: "{app}\deps"; DestName: "{#PefWhl}"; ExternalSize: {#PefBytes}; \
  Hash: "{#PefSha256}"; Flags: external download ignoreversion
Source: "{#CmakeUrl}"; DestDir: "{app}\deps"; DestName: "{#CmakeWhl}"; ExternalSize: {#CmakeBytes}; \
  Hash: "{#CmakeSha256}"; Flags: external download ignoreversion; Check: NeedPipCMake
Source: "{#VsUrl}"; DestDir: "{app}\deps"; DestName: "vs_BuildTools.exe"; ExternalSize: {#VsBytes}; \
  Hash: "{#VsSha256}"; Flags: external download ignoreversion; Check: WantVS

[UninstallDelete]
Type: files; Name: "{autoprograms}\Blood Wake Recomp.lnk"
Type: files; Name: "{autodesktop}\Blood Wake Recomp.lnk"

[Run]
Filename: "{app}\python\pythonw.exe"; Parameters: "-E -s ""{app}\tools\launcher.pyw"""; \
  WorkingDir: "{app}"; Description: "Open the Blood Wake launcher now"; \
  Flags: postinstall nowait skipifsilent; Check: BuildSucceeded

[Code]
const
  XBE_SIZE = {#XbeSize};
  XBE_SHA256 = '{#XbeSha256}';
  RETAIL_LABEL = '{#RetailLabel}';
  TITLE_DIR = '4d530010';          { the game's own folder under UDATA and TDATA }
  LINK_NAME = 'Blood Wake Recomp.lnk';

var
  DisclaimerPage, DiscPage, DepsPage: TWizardPage;
  OwnCheck: TNewCheckBox;
  ImageRadio, FolderRadio: TNewRadioButton;
  DiscEdit: TNewEdit;
  DiscStatus, DepsText, VSNote: TNewStaticText;
  VSCheck: TNewCheckBox;
  gHaveVS, gHaveCMake, gVSPlanOnly, gStub: Boolean;
  gVSPath, gVSName, gVSVersion: String;
  gDisc, gDiscChecked, gDiscVerdict: String;
  gBuildOK, gBuildRan: Boolean;
  gBuildExit: Integer;
  gBuildMsg, gLastMsg, gFingerprint: String;
  gBase, gSpan: Integer;
  gVSInstalledNow: Boolean;
  { uninstall choices }
  uKeepSaves, uRemovePython, uRemoveGame, uRemoveShaders, uRemoveVS, uVSOurs: Boolean;

{ ── Command line ─────────────────────────────────────────────────────── }

function CmdParam(const Name: String; var Value: String): Boolean;
var
  I: Integer;
  P, Key: String;
begin
  Result := False;
  Value := '';
  Key := '/' + Uppercase(Name);
  for I := 1 to ParamCount do begin
    P := ParamStr(I);
    if Uppercase(P) = Key then begin
      Result := True;
      exit;
    end;
    if Pos(Key + '=', Uppercase(P)) = 1 then begin
      Value := Copy(P, Length(Key) + 2, MaxInt);
      Result := True;
      exit;
    end;
  end;
end;

function HasParam(const Name: String): Boolean;
var
  V: String;
begin
  Result := CmdParam(Name, V) and (V <> '0') and (Lowercase(V) <> 'no');
end;

{ A path as one command-line argument. A trailing backslash would escape the
  closing quote, so "E:\" becomes "E:\." (the same folder). }
function ArgQuote(S: String): String;
begin
  if (Length(S) > 0) and (S[Length(S)] = '\') then
    S := S + '.';
  Result := '"' + S + '"';
end;

{ ── The disc: its main program against the retail fingerprint ───────── }

function U16(const S: AnsiString; Off: Integer): Integer;
begin
  Result := Ord(S[Off]) + Ord(S[Off + 1]) * 256;
end;

function U32(const S: AnsiString; Off: Integer): Int64;
var
  Hi: Int64;
begin
  Hi := Ord(S[Off + 3]);
  Result := Ord(S[Off]) + Ord(S[Off + 1]) * 256 + Ord(S[Off + 2]) * 65536 + Hi * 16777216;
end;

function ReadAt(F: TFileStream; Offset: Int64; Count: Integer; var Buf: AnsiString): Boolean;
begin
  Result := False;
  try
    if F.Seek(Offset, 0) <> Offset then
      exit;
    SetLength(Buf, Count);
    F.ReadBuffer(Buf, Count);
    Result := True;
  except
    Result := False;
  end;
end;

{ Finds default.xbe in an Xbox image's root folder (XDVDFS, as the toolkit's
  tools/xiso reads it) and hashes it. Layout: 'XISO' or 'full disc image'. }
function ImageXbe(const Path: String; var Sha, Layout: String; var Size: Int64): Boolean;
var
  F: TFileStream;
  Bases: array[0..4] of Int64;
  I, Off, NLen, L, R: Integer;
  Base, RootSec, RootSize, Sec, Chunk: Int64;
  Buf, Dir, Name: AnsiString;
begin
  Result := False;
  Bases[0] := 0; Bases[1] := $FD90; Bases[2] := $30600; Bases[3] := $FD90000; Bases[4] := $18300000;
  try
    F := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone);
  except
    exit;
  end;
  try
    Base := -1;
    for I := 0 to 4 do
      if ReadAt(F, Bases[I] + 32 * 2048, 2048, Buf) and (Copy(Buf, 1, 20) = 'MICROSOFT*XBOX*MEDIA') then begin
        Base := Bases[I];
        break;
      end;
    if Base < 0 then
      exit;
    if Base = 0 then Layout := 'an XISO' else Layout := 'a full disc image';
    RootSec := U32(Buf, 21);
    RootSize := U32(Buf, 25);
    if (RootSize <= 0) or (RootSize > 4194304) then
      exit;
    if not ReadAt(F, Base + RootSec * 2048, RootSize, Dir) then
      exit;
    Chunk := 0;
    while Chunk < RootSize do begin
      Off := 0;
      while (Off + 14 <= 2048) and (Chunk + Off + 14 <= RootSize) do begin
        L := U16(Dir, Chunk + Off + 1);
        R := U16(Dir, Chunk + Off + 3);
        NLen := Ord(Dir[Chunk + Off + 14]);
        if ((L = $FFFF) and (R = $FFFF)) or (NLen = 0) then
          break;
        Name := Copy(Dir, Chunk + Off + 15, NLen);
        if Lowercase(Name) = 'default.xbe' then begin
          Sec := U32(Dir, Chunk + Off + 5);
          Size := U32(Dir, Chunk + Off + 9);
          if (Size <= 0) or (Size > 67108864) then
            exit;
          if not ReadAt(F, Base + Sec * 2048, Size, Buf) then
            exit;
          Sha := GetSHA256OfString(Buf);
          Result := True;
          exit;
        end;
        Off := Off + ((14 + NLen + 3) div 4) * 4;
      end;
      Chunk := Chunk + 2048;
    end;
  finally
    F.Free;
  end;
end;

function CheckDisc(const Path: String; var Msg: String): Boolean;
var
  Sha, Layout, X: String;
  Size: Int64;
begin
  Result := False;
  Sha := '';
  Size := -1;
  if Trim(Path) = '' then begin
    Msg := 'Pick your copy of Blood Wake first.';
    exit;
  end;
  if DirExists(Path) then begin
    X := AddBackslash(Path) + 'default.xbe';
    if not FileExists(X) then begin
      Msg := 'This folder has no default.xbe. Pick the folder that holds your disc''s files: default.xbe sits at its top, next to the game''s folders.';
      exit;
    end;
    Layout := 'a folder of the disc''s files';
    try
      Sha := GetSHA256OfFile(X);
    except
      Msg := 'Setup couldn''t read ' + X + ': ' + GetExceptionMessage;
      exit;
    end;
    if not FileSize64(X, Size) then
      Size := -1;
  end else if FileExists(Path) then begin
    if not ImageXbe(Path, Sha, Layout, Size) then begin
      Msg := 'This file isn''t an Xbox disc image Setup can read: it has no Xbox file system, or no default.xbe. Pick a full disc image or an XISO of your Blood Wake disc, or a folder of its files.';
      exit;
    end;
  end else begin
    Msg := 'Setup can''t find ' + Path + '.';
    exit;
  end;
  if (Lowercase(Sha) <> XBE_SHA256) or (Size <> XBE_SIZE) then begin
    Msg := 'This isn''t the supported version of Blood Wake: its main program (default.xbe) doesn''t match the retail fingerprint. Only an unmodified retail copy works.';
    exit;
  end;
  Msg := 'Recognised: Blood Wake, ' + RETAIL_LABEL + ', ' + Layout + '. Its main program matches the retail fingerprint; Setup checks every other file while it copies them.';
  Result := True;
end;

{ ── Dependencies ─────────────────────────────────────────────────────── }

function VsWhere(const Prop: String): String;
var
  Exe: String;
  Code: Integer;
  Output: TExecOutput;
begin
  Result := '';
  Exe := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\vswhere.exe');
  if not FileExists(Exe) then
    exit;
  try
    if ExecAndCaptureOutput(Exe, '-products * -version [17.0,18.0) -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -utf8 -format value -property ' + Prop,
        '', SW_HIDE, ewWaitUntilTerminated, Code, Output) and (Code = 0) and (GetArrayLength(Output.StdOut) > 0) then
      Result := Trim(Output.StdOut[0]);
  except
    Log('vswhere: ' + GetExceptionMessage);
  end;
end;

procedure DetectDeps();
begin
  gVSPath := '';
  gVSName := '';
  gVSVersion := '';
  if not HasParam('SIMULATENOVS') then begin
    gVSPath := VsWhere('installationPath');
    if gVSPath <> '' then begin
      gVSName := VsWhere('displayName');
      gVSVersion := VsWhere('installationVersion');
    end;
  end;
  gHaveVS := (gVSPath <> '') and DirExists(gVSPath);
  gHaveCMake := (gHaveVS and FileExists(gVSPath + '\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))
    or (FileSearch('cmake.exe', GetEnv('PATH')) <> '');
  if HasParam('SIMULATENOCMAKE') then
    gHaveCMake := False;
  Log(Format('Detected: Visual Studio 2022 C++ = %s [%s %s], CMake = %d', [gVSPath, gVSName, gVSVersion, Ord(gHaveCMake)]));
end;

function WantVS(): Boolean;
begin
  Result := (not gHaveVS) and (VSCheck.Checked or gVSPlanOnly);
end;

function NeedPipCMake(): Boolean;
begin
  { The Build Tools Setup adds include CMake; otherwise pip's CMake when none is found. }
  Result := (not gHaveCMake) and not (WantVS() and not gVSPlanOnly);
end;

function NeedPython(): Boolean;
var
  S: AnsiString;
begin
  // WizardDirValue: the app constant isn't set yet while the pages are being filled
  Result := not (FileExists(AddBackslash(WizardDirValue) + 'python\python.exe')
    and LoadStringFromFile(AddBackslash(WizardDirValue) + 'python\.bw_python', S)
    and (Trim(S) = '{#PyVer} {#PyZipSha256}'));
end;

procedure PythonUnpacked();
begin
  { Python's manual (71 MB) isn't needed to build or play. }
  DelTree(ExpandConstant('{app}\python\Doc'), True, True, True);
  SaveStringToFile(ExpandConstant('{app}\python\.bw_python'), '{#PyVer} {#PyZipSha256}', False);
end;

function BuildSucceeded(): Boolean;
begin
  Result := gBuildOK;
end;

{ ── Pages ────────────────────────────────────────────────────────────── }

procedure OwnCheckClick(Sender: TObject);
begin
  WizardForm.NextButton.Enabled := OwnCheck.Checked;
end;

procedure ShowDiscResult();
var
  Msg: String;
begin
  gDisc := Trim(DiscEdit.Text);
  if CheckDisc(gDisc, Msg) then begin
    gDiscChecked := gDisc;
    gDiscVerdict := Msg;
    DiscStatus.Caption := Msg;
  end else begin
    gDiscChecked := '';
    DiscStatus.Caption := 'Not usable: ' + Msg;
  end;
end;

procedure BrowseClick(Sender: TObject);
var
  S: String;
begin
  S := Trim(DiscEdit.Text);
  if ImageRadio.Checked then begin
    if GetOpenFileName('Pick your Blood Wake disc image', S, ExtractFileDir(S),
        'Xbox disc images (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*', 'iso') then begin
      DiscEdit.Text := S;
      ShowDiscResult();
    end;
  end else begin
    if BrowseForFolder('Pick the folder with your Blood Wake disc''s files (the one with default.xbe in it):', S, False) then begin
      DiscEdit.Text := S;
      ShowDiscResult();
    end;
  end;
end;

procedure FillDepsPage();
var
  T, NL: String;
begin
  NL := #13#10;
  T := 'What Setup found on this PC:' + NL + NL;
  T := T + '-  Python {#PyVer}, private to Blood Wake Recomp: ';
  if NeedPython() then
    T := T + 'from python.org (33 MB). It doesn''t change PATH or any other Python.' + NL
  else
    T := T + 'already in the install folder.' + NL;
  T := T + '-  Python packages capstone 5.0.9 and pefile 2024.8.26: from pypi.org (1.3 MB).' + NL;
  T := T + '-  Microsoft''s C++ compiler (Visual Studio 2022): ';
  if gHaveVS then
    T := T + 'found, ' + gVSName + ' ' + gVSVersion + '.' + NL
  else
    T := T + 'not found.' + NL;
  T := T + '-  CMake 3.20 or newer: ';
  if gHaveCMake then
    T := T + 'found.' + NL
  else if gHaveVS then
    T := T + 'not found; cmake 4.4.4 from pypi.org (42 MB).' + NL
  else
    T := T + 'comes with the Build Tools below.' + NL;
  T := T + '-  The xboxrecomp toolkit (commit f0f8a46): from GitHub, during the build.';
  DepsText.Caption := T;
  VSCheck.Visible := not gHaveVS;
  VSNote.Visible := not gHaveVS;
end;

procedure InitializeWizard();
var
  Memo: TNewMemo;
  Intro: TNewStaticText;
  Browse: TNewButton;
  Text: AnsiString;
  V: String;
begin
  gVSPlanOnly := HasParam('VSPLANONLY');
  gStub := HasParam('STUBBUILD');
  gBuildExit := 0;
  DetectDeps();

  { The disclaimer }
  DisclaimerPage := CreateCustomPage(wpWelcome, 'Before you start',
    'Please read this. Setup continues only if it applies to you.');
  ExtractTemporaryFile('disclaimer.txt');
  LoadStringFromFile(ExpandConstant('{tmp}\disclaimer.txt'), Text);
  Memo := TNewMemo.Create(DisclaimerPage);
  Memo.Parent := DisclaimerPage.Surface;
  Memo.Left := 0;
  Memo.Top := 0;
  Memo.Width := DisclaimerPage.SurfaceWidth;
  Memo.Height := DisclaimerPage.SurfaceHeight - ScaleY(30);
  Memo.ScrollBars := ssVertical;
  Memo.ReadOnly := True;
  Memo.WordWrap := True;
  V := String(Text);
  StringChangeEx(V, ' <!-- audit:allow off-limits -->', '', True);   { the public audit's marker }
  StringChangeEx(V, #13#10, #10, True);
  StringChangeEx(V, #10, #13#10, True);
  Memo.Text := V;
  OwnCheck := TNewCheckBox.Create(DisclaimerPage);
  OwnCheck.Parent := DisclaimerPage.Surface;
  OwnCheck.Left := 0;
  OwnCheck.Top := Memo.Top + Memo.Height + ScaleY(10);
  OwnCheck.Width := DisclaimerPage.SurfaceWidth;
  OwnCheck.Height := ScaleY(17);
  OwnCheck.Caption := 'I own Blood Wake and made this copy from my own disc';
  OwnCheck.OnClick := @OwnCheckClick;
  OwnCheck.Checked := HasParam('IOWNTHEGAME');

  { The disc }
  DiscPage := CreateCustomPage(DisclaimerPage.ID, 'Your copy of Blood Wake',
    'Where are the files from your disc?');
  Intro := TNewStaticText.Create(DiscPage);
  Intro.Parent := DiscPage.Surface;
  Intro.Width := DiscPage.SurfaceWidth;
  Intro.WordWrap := True;
  Intro.Caption := 'Pick a disc image you made from your own disc (a full disc image or an XISO), or a folder with the disc''s files. Setup only reads it: it copies the game''s files into the install folder.';
  Intro.AdjustHeight;
  ImageRadio := TNewRadioButton.Create(DiscPage);
  ImageRadio.Parent := DiscPage.Surface;
  ImageRadio.Top := Intro.Top + Intro.Height + ScaleY(12);
  ImageRadio.Width := DiscPage.SurfaceWidth;
  ImageRadio.Height := ScaleY(17);
  ImageRadio.Caption := 'A disc image file (.iso)';
  ImageRadio.Checked := True;
  FolderRadio := TNewRadioButton.Create(DiscPage);
  FolderRadio.Parent := DiscPage.Surface;
  FolderRadio.Top := ImageRadio.Top + ImageRadio.Height + ScaleY(6);
  FolderRadio.Width := DiscPage.SurfaceWidth;
  FolderRadio.Height := ScaleY(17);
  FolderRadio.Caption := 'A folder with the disc''s files';
  DiscEdit := TNewEdit.Create(DiscPage);
  DiscEdit.Parent := DiscPage.Surface;
  DiscEdit.Top := FolderRadio.Top + FolderRadio.Height + ScaleY(12);
  DiscEdit.Width := DiscPage.SurfaceWidth - ScaleX(90);
  Browse := TNewButton.Create(DiscPage);
  Browse.Parent := DiscPage.Surface;
  Browse.Top := DiscEdit.Top - ScaleY(1);
  Browse.Left := DiscPage.SurfaceWidth - ScaleX(80);
  Browse.Width := ScaleX(80);
  Browse.Height := ScaleY(23);
  Browse.Caption := 'B&rowse...';
  Browse.OnClick := @BrowseClick;
  DiscStatus := TNewStaticText.Create(DiscPage);
  DiscStatus.Parent := DiscPage.Surface;
  DiscStatus.Top := DiscEdit.Top + DiscEdit.Height + ScaleY(14);
  DiscStatus.Width := DiscPage.SurfaceWidth;
  DiscStatus.AutoSize := False;
  DiscStatus.WordWrap := True;
  DiscStatus.Height := ScaleY(80);
  DiscStatus.Caption := '';
  if not CmdParam('DISC', V) or (V = '') then
    V := GetPreviousData('Disc', '');
  DiscEdit.Text := V;
  if DirExists(V) then
    FolderRadio.Checked := True;
  if V <> '' then
    ShowDiscResult();

  { What's needed }
  DepsPage := CreateCustomPage(wpSelectDir, 'What Blood Wake needs',
    'Setup adds what''s missing. Everything comes from its official source and is checked against a pinned SHA-256 hash.');
  DepsText := TNewStaticText.Create(DepsPage);
  DepsText.Parent := DepsPage.Surface;
  DepsText.Width := DepsPage.SurfaceWidth;
  DepsText.AutoSize := False;
  DepsText.WordWrap := True;
  DepsText.Height := ScaleY(132);
  VSCheck := TNewCheckBox.Create(DepsPage);
  VSCheck.Parent := DepsPage.Surface;
  VSCheck.Top := DepsText.Top + DepsText.Height + ScaleY(6);
  VSCheck.Width := DepsPage.SurfaceWidth;
  VSCheck.Height := ScaleY(17);
  VSCheck.Caption := 'Install Microsoft''s Build Tools for Visual Studio 2022 (C++ only), version {#VsVer}';
  VSCheck.Checked := HasParam('INSTALLVS') or gVSPlanOnly or not WizardSilent();
  VSNote := TNewStaticText.Create(DepsPage);
  VSNote.Parent := DepsPage.Surface;
  VSNote.Top := VSCheck.Top + VSCheck.Height + ScaleY(4);
  VSNote.Left := ScaleX(16);
  VSNote.Width := DepsPage.SurfaceWidth - ScaleX(16);
  VSNote.AutoSize := False;
  VSNote.WordWrap := True;
  VSNote.Height := ScaleY(48);
  VSNote.Caption := 'From microsoft.com, about 3 GB on drive C:. Windows asks for permission for this one step. Other programs can use them too, so the uninstaller keeps them unless you ask.';
  FillDepsPage();
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = DisclaimerPage.ID then begin
    { interactive only: silent installs reach NextButtonClick, which logs why it stops }
    if not WizardSilent() then
      WizardForm.NextButton.Enabled := OwnCheck.Checked;
  end else if CurPageID = DepsPage.ID then
    FillDepsPage()
  else if CurPageID = wpFinished then begin
    if gBuildOK then
      WizardForm.FinishedLabel.Caption := 'Blood Wake is built and ready.' + #13#10#13#10 + gFingerprint + #13#10#13#10 +
        'Start it from the Start menu: "Blood Wake Recomp". The launcher lets you pick Original (exactly as shipped) or Remastered, set up your screen and controls, and press Play.'
    else
      WizardForm.FinishedLabel.Caption := 'Setup installed its files, but the game wasn''t built:' + #13#10#13#10 + gBuildMsg + #13#10#13#10 +
        'The build''s logs are in ' + ExpandConstant('{app}\work\logs') + '. Fix the problem and run Setup again: it picks up where it left off. Or remove everything from Settings > Apps.';
  end;
end;

{ The install folder must be new, empty, or an earlier Blood Wake Recomp install:
  the uninstaller removes the folders Setup and the build put there (game, build,
  tools, work, python...), and must never meet someone else's folder of that name. }
function FolderUsable(const Dir: String): Boolean;
var
  F: TFindRec;
begin
  Result := True;
  if not DirExists(Dir) then
    exit;
  if FileExists(AddBackslash(Dir) + 'installer.ini') or FileExists(AddBackslash(Dir) + 'unins000.dat') then
    exit;
  if FindFirst(AddBackslash(Dir) + '*', F) then
    try
      repeat
        if (F.Name <> '.') and (F.Name <> '..') then begin
          Result := False;
          exit;
        end;
      until not FindNext(F);
    finally
      FindClose(F);
    end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Msg: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then begin
    if not FolderUsable(WizardDirValue) then begin
      Msg := 'Please pick a new or empty folder: ' + WizardDirValue + ' already has other things in it. Setup puts several folders inside the install folder, and the uninstaller removes them again, so it needs a folder of its own.';
      Log('Stopped at the folder page: ' + Msg);
      SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
      Result := False;
    end;
  end else if CurPageID = DisclaimerPage.ID then begin
    if not OwnCheck.Checked then begin
      Log('Stopped: the disclaimer was not accepted (silent installs need /IOWNTHEGAME).');
      Result := False;
    end;
  end else if CurPageID = DiscPage.ID then begin
    gDisc := Trim(DiscEdit.Text);
    if (gDisc = '') or (gDisc <> gDiscChecked) then
      ShowDiscResult();
    if gDisc <> gDiscChecked then begin
      CheckDisc(gDisc, Msg);
      Log('Stopped at the disc page: ' + Msg);
      SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
      Result := False;
    end else
      Log('Disc: ' + gDisc + ': ' + gDiscVerdict);
  end else if CurPageID = DepsPage.ID then begin
    if (not gHaveVS) and (not WantVS()) then begin
      Msg := 'Blood Wake can''t be built without Microsoft''s C++ build tools. Tick the box to let Setup install them, or install Visual Studio 2022 (with "Desktop development with C++") yourself and run Setup again.';
      Log('Stopped at the dependencies page: ' + Msg);
      SuppressibleMsgBox(Msg, mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'Disc', gDisc);
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  S: String;
begin
  S := 'Your copy of Blood Wake:' + NewLine + Space + gDisc + NewLine + Space + 'Recognised: Blood Wake, ' + RETAIL_LABEL +
    NewLine + Space + '(its main program matches the retail fingerprint)' + NewLine + NewLine;
  S := S + MemoDirInfo + NewLine + NewLine;
  S := S + 'Setup will download:' + NewLine;
  if NeedPython() then
    S := S + Space + 'Python {#PyVer} from python.org (33 MB)' + NewLine;
  S := S + Space + 'capstone 5.0.9 and pefile 2024.8.26 from pypi.org (1.3 MB)' + NewLine;
  if NeedPipCMake() then
    S := S + Space + 'CMake 4.4.4 from pypi.org (42 MB)' + NewLine;
  S := S + Space + 'the xboxrecomp toolkit from GitHub' + NewLine;
  if WantVS() then begin
    if gVSPlanOnly then
      S := S + Space + 'Microsoft''s Build Tools bootstrapper {#VsVer} (plan only: it won''t be run)' + NewLine
    else
      S := S + Space + 'Microsoft''s Build Tools for Visual Studio 2022 {#VsVer} (about 3 GB; asks for permission)' + NewLine;
  end;
  S := S + NewLine + 'Then it builds the game and prepares its graphics: about 20 minutes on a recent PC,' + NewLine +
    'longer on an older one. You can use the PC meanwhile.' + NewLine;
  if MemoTasksInfo <> '' then
    S := S + NewLine + MemoTasksInfo + NewLine;
  Result := S;
end;

{ ── Install: Build Tools, Python packages, the build ─────────────────── }

function JsonValue(const Line, Key: String): String;
var
  P, I, N: Integer;
  C: Char;
  S: String;
begin
  Result := '';
  P := Pos('"' + Key + '": ', Line);
  if P = 0 then
    exit;
  I := P + Length(Key) + 4;
  N := Length(Line);
  S := '';
  if (I <= N) and (Line[I] = '"') then begin
    I := I + 1;
    while I <= N do begin
      C := Line[I];
      if C = '"' then
        break;
      if (C = '\') and (I < N) then begin
        I := I + 1;
        C := Line[I];
        if (C = 'n') or (C = 't') then
          S := S + ' '
        else if C = 'r' then
          S := S
        else if (C = 'u') and (I + 4 <= N) then begin
          S := S + Chr(StrToIntDef('$' + Copy(Line, I + 1, 4), 63));
          I := I + 4;
        end else
          S := S + C;
      end else
        S := S + C;
      I := I + 1;
    end;
  end else begin
    while (I <= N) and (Line[I] <> ',') and (Line[I] <> '}') do begin
      S := S + Line[I];
      I := I + 1;
    end;
    S := Trim(S);
  end;
  Result := S;
end;

function StepText(const Step: String): String;
begin
  if Step = 'deps' then Result := 'Setting up Python''s packages...'
  else if Step = 'identify' then Result := 'Looking at your copy of Blood Wake...'
  else if Step = 'toolkit' then Result := 'Getting the xboxrecomp toolkit...'
  else if Step = 'unpack' then Result := 'Copying the game''s files from your copy...'
  else if Step = 'fingerprint' then Result := 'Checking your copy against the retail fingerprints...'
  else if Step = 'pipeline' then Result := 'Translating the game''s code (about 10 minutes)...'
  else if Step = 'verify' then Result := 'Checking the translated code matches ours...'
  else if Step = 'build' then Result := 'Compiling the game (Visual Studio)...'
  else if Step = 'stage' then Result := 'Finishing...'
  else if Step = 'warmup' then Result := 'Preparing graphics (one time)...'
  else if Step = 'done' then Result := 'Done.'
  else Result := WizardForm.StatusLabel.Caption;
end;

procedure OnHelperLine(const S: String; const Error, FirstLine: Boolean);
var
  Step, Msg: String;
  Pct: Integer;
begin
  if Error then begin
    Log('helper output: ' + S);
    exit;
  end;
  Log('| ' + S);
  if (Length(S) = 0) or (S[1] <> '{') then
    exit;
  Step := JsonValue(S, 'step');
  Msg := JsonValue(S, 'msg');
  Pct := StrToIntDef(JsonValue(S, 'pct'), -1);
  if JsonValue(S, 'error') = 'true' then begin
    gBuildMsg := Msg;
    gBuildExit := StrToIntDef(JsonValue(S, 'exit'), 1);
  end;
  if Step = 'log' then
    exit;
  if (Step = 'fingerprint') and (JsonValue(S, 'error') <> 'true') and (Pct >= 20) then
    gFingerprint := 'Your copy checks out: ' + Msg + '.';
  if Msg <> '' then
    gLastMsg := Msg;
  WizardForm.StatusLabel.Caption := StepText(Step);
  WizardForm.FilenameLabel.Caption := Msg;
  if Pct >= 0 then
    WizardForm.ProgressGauge.Position := gBase + (Pct * gSpan) div 100;
end;

function RunHelper(const Params: String; var Code: Integer): Boolean;
var
  Py: String;
begin
  Py := ExpandConstant('{app}\python\python.exe');
  Log('Running: ' + Py + ' -I -u setup_helper.py ' + Params);
  try
    Result := ExecAndLogOutput(Py, '-I -u ' + ArgQuote(ExpandConstant('{app}\setup\setup_helper.py')) + ' ' + Params,
      ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, Code, @OnHelperLine);
  except
    Log('Running the helper failed: ' + GetExceptionMessage);
    Result := False;
    Code := -1;
  end;
end;

function InstallBuildTools(): Boolean;
var
  Boot, Target, Args, Ini: String;
  Code: Integer;
begin
  Result := False;
  Boot := ExpandConstant('{app}\deps\vs_BuildTools.exe');
  Target := ExpandConstant('{commonpf32}\Microsoft Visual Studio\2022\BuildTools');
  Args := '--passive --wait --norestart --nocache --installPath ' + ArgQuote(Target) +
    ' --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64' +
    ' --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended';
  if gVSPlanOnly then begin
    SaveStringToFile(ExpandConstant('{app}\deps\vs_plan.txt'),
      'Plan only (/VSPLANONLY): Setup did not run this. It would run, with a Windows permission prompt:' + #13#10 +
      ArgQuote(Boot) + ' ' + Args + #13#10 +
      'The bootstrapper was downloaded from ' + '{#VsUrl}' + #13#10 +
      'and matched the pinned SHA-256 {#VsSha256} (Build Tools {#VsVer}).' + #13#10, False);
    Log('VS Build Tools: plan only, written to deps\vs_plan.txt: ' + Boot + ' ' + Args);
    Result := True;
    exit;
  end;
  WizardForm.StatusLabel.Caption := 'Installing Microsoft''s Build Tools for Visual Studio 2022...';
  WizardForm.FilenameLabel.Caption := 'Windows asks for permission. This takes 10 to 30 minutes and downloads about 2 GB from Microsoft.';
  Log('VS Build Tools: ' + Boot + ' ' + Args);
  if not ShellExec('', Boot, Args, '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then begin
    gBuildMsg := 'Microsoft''s Build Tools couldn''t be installed (' + SysErrorMessage(Code) + '). If you declined the permission prompt, run Setup again and allow it, or install Visual Studio 2022 with "Desktop development with C++" yourself.';
    exit;
  end;
  Log(Format('VS Build Tools exit code %d', [Code]));
  if (Code <> 0) and (Code <> 3010) then begin
    gBuildMsg := Format('Microsoft''s Build Tools installer stopped with code %d. Run Setup again, or install Visual Studio 2022 with "Desktop development with C++" yourself.', [Code]);
    exit;
  end;
  Ini := ExpandConstant('{app}\installer.ini');
  SetIniString('vs', 'installed_by_setup', '1', Ini);
  SetIniString('vs', 'path', Target, Ini);
  SetIniString('vs', 'version', '{#VsVer}', Ini);
  gVSInstalledNow := True;
  DetectDeps();
  Result := gHaveVS;
  if not Result then
    gBuildMsg := 'Microsoft''s Build Tools were installed, but Setup can''t find their C++ compiler. Restart Windows and run Setup again.';
end;

procedure MakeShortcuts();
var
  Pyw, Params, Exe: String;
begin
  Pyw := ExpandConstant('{app}\python\pythonw.exe');
  Params := '-E -s ' + ArgQuote(ExpandConstant('{app}\tools\launcher.pyw'));
  Exe := ExpandConstant('{app}\build\Release\bloodwake.exe');
  try
    CreateShellLink(ExpandConstant('{autoprograms}\' + LINK_NAME), 'Blood Wake (unofficial PC port): the launcher',
      Pyw, Params, ExpandConstant('{app}'), Exe, 0, SW_SHOWNORMAL);
    if WizardIsTaskSelected('desktopicon') then
      CreateShellLink(ExpandConstant('{autodesktop}\' + LINK_NAME), 'Blood Wake (unofficial PC port): the launcher',
        Pyw, Params, ExpandConstant('{app}'), Exe, 0, SW_SHOWNORMAL);
  except
    Log('Shortcut: ' + GetExceptionMessage);
  end;
end;

procedure RunBuild();
var
  Code: Integer;
  Params, V, Ini: String;
begin
  gBuildRan := True;
  gBuildOK := False;
  WizardForm.ProgressGauge.Min := 0;
  WizardForm.ProgressGauge.Max := 1000;
  WizardForm.ProgressGauge.Position := 0;
  Ini := ExpandConstant('{app}\installer.ini');
  SetIniString('setup', 'version', '{#AppVer}', Ini);
  SetIniString('setup', 'disc', gDisc, Ini);
  SetIniString('python', 'version', '{#PyVer}', Ini);

  if WantVS() then
    if not InstallBuildTools() then begin
      gBuildExit := 10;
      exit;
    end;

  { Python's packages: 0-2% }
  gBase := 0;
  gSpan := 20;
  WizardForm.StatusLabel.Caption := StepText('deps');
  WizardForm.FilenameLabel.Caption := '';
  if not RunHelper('deps --wheels ' + ArgQuote(ExpandConstant('{app}\deps')), Code) or (Code <> 0) then begin
    if gBuildMsg = '' then
      gBuildMsg := Format('Setting up Python''s packages failed (code %d). Log: %s', [Code, ExpandConstant('{app}\deps\pip.log')]);
    gBuildExit := 3;
    exit;
  end;

  { The build: 2-100% }
  gBase := 20;
  gSpan := 980;
  gBuildMsg := '';
  gBuildExit := 0;
  Params := 'build --source ' + ArgQuote(ExpandConstant('{app}\source'));
  if gStub then
    Params := Params + ' --stub';
  Params := Params + ' -- --disc ' + ArgQuote(gDisc) + ' --install-dir ' + ArgQuote(ExpandConstant('{app}'));
  if CmdParam('JOBS', V) and (StrToIntDef(V, 0) > 0) then
    Params := Params + ' --jobs ' + V;
  if CmdParam('TOOLKIT', V) and (V <> '') then
    Params := Params + ' --toolkit ' + ArgQuote(V);
  if not RunHelper(Params, Code) then
    Code := -1;
  Log(Format('Build exit code %d', [Code]));
  if Code <> 0 then begin
    if gBuildExit = 0 then
      gBuildExit := Code;
    if gBuildMsg = '' then
      gBuildMsg := Format('The build stopped (code %d): %s', [Code, gLastMsg]);
    exit;
  end;
  if not FileExists(ExpandConstant('{app}\build\Release\bloodwake.exe')) then begin
    gBuildExit := 6;
    gBuildMsg := 'The build finished without bloodwake.exe.';
    exit;
  end;
  gBuildOK := True;
  if gFingerprint = '' then
    gFingerprint := 'Your copy checks out: ' + gDiscVerdict;
  V := 'setup_version={#AppVer} disc=' + ArgQuote(gDisc) + ' python_private=true';
  if gVSInstalledNow then
    V := V + ' vs_buildtools_installed_by_setup=true vs_buildtools_path=' + ArgQuote(GetIniString('vs', 'path', '', Ini))
  else
    V := V + ' vs_buildtools_installed_by_setup=false';
  RunHelper('record --install-json ' + ArgQuote(ExpandConstant('{app}\install.json')) + ' ' + V, Code);
  MakeShortcuts();
  WizardForm.ProgressGauge.Position := 1000;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RunBuild();
end;

function GetCustomSetupExitCode(): Integer;
begin
  Result := 0;
  if gBuildRan and not gBuildOK then begin
    if gBuildExit = 10 then
      Result := 110
    else if (gBuildExit >= 2) and (gBuildExit <= 7) then
      Result := 100 + gBuildExit
    else
      Result := 109;
  end;
end;

{ ── Uninstall ────────────────────────────────────────────────────────── }

function AddCheck(Form: TSetupForm; const Caption: String; Checked: Boolean; var Y: Integer): TNewCheckBox;
begin
  Result := TNewCheckBox.Create(Form);
  Result.Parent := Form;
  Result.Left := ScaleX(16);
  Result.Top := Y;
  Result.Width := Form.ClientWidth - ScaleX(32);
  Result.Height := ScaleY(17);
  Result.Caption := Caption;
  Result.Checked := Checked;
  Y := Y + ScaleY(24);
end;

function InitializeUninstall(): Boolean;
var
  Form: TSetupForm;
  Intro, Warn: TNewStaticText;
  CSaves, CPython, CGame, CShaders, CVS: TNewCheckBox;
  OK, Cancel: TNewButton;
  Y, W: Integer;
begin
  Result := True;
  uVSOurs := GetIniString('vs', 'installed_by_setup', '0', ExpandConstant('{app}\installer.ini')) = '1';
  uKeepSaves := not HasParam('REMOVESAVES');
  uRemovePython := not HasParam('KEEPPYTHON');
  uRemoveGame := not HasParam('KEEPGAME');
  uRemoveShaders := not HasParam('KEEPSHADERCACHE');
  uRemoveVS := uVSOurs and HasParam('REMOVEVS');
  if UninstallSilent() then
    exit;

  Form := CreateCustomForm(ScaleX(480), ScaleY(340), False, True);
  try
    Form.Caption := 'Uninstall Blood Wake Recomp';
    Intro := TNewStaticText.Create(Form);
    Intro.Parent := Form;
    Intro.Left := ScaleX(16);
    Intro.Top := ScaleY(14);
    Intro.Width := Form.ClientWidth - ScaleX(32);
    Intro.WordWrap := True;
    Intro.Caption := 'The game you built, its files in the install folder and the shortcuts are always removed. Choose what else goes:';
    Intro.AdjustHeight;
    Y := Intro.Top + Intro.Height + ScaleY(14);
    CSaves := AddCheck(Form, 'Keep my saves and settings (recommended)', uKeepSaves, Y);
    CPython := AddCheck(Form, 'Remove the private Python (in the install folder)', uRemovePython, Y);
    CGame := AddCheck(Form, 'Remove the game files copied from my disc (about 2 GB)', uRemoveGame, Y);
    CShaders := AddCheck(Form, 'Remove the shader cache (it is rebuilt when you play)', uRemoveShaders, Y);
    CVS := nil;
    if uVSOurs then begin
      CVS := AddCheck(Form, 'Remove Microsoft''s Build Tools for Visual Studio 2022', uRemoveVS, Y);
      Warn := TNewStaticText.Create(Form);
      Warn.Parent := Form;
      Warn.Left := ScaleX(34);
      Warn.Top := Y - ScaleY(4);
      Warn.Width := Form.ClientWidth - ScaleX(50);
      Warn.WordWrap := True;
      Warn.Caption := 'Setup installed these for you. Other programs may use them too: only remove them if you know nothing else needs them.';
      Warn.AdjustHeight;
      Y := Warn.Top + Warn.Height + ScaleY(8);
    end;
    OK := TNewButton.Create(Form);
    OK.Parent := Form;
    OK.Caption := '&Uninstall';
    OK.ModalResult := mrOk;
    OK.Default := True;
    Cancel := TNewButton.Create(Form);
    Cancel.Parent := Form;
    Cancel.Caption := 'Cancel';
    Cancel.ModalResult := mrCancel;
    Cancel.Cancel := True;
    W := Form.CalculateButtonWidth([OK.Caption, Cancel.Caption]);
    OK.Width := W;
    Cancel.Width := W;
    OK.Height := ScaleY(23);
    Cancel.Height := ScaleY(23);
    Form.ClientHeight := Y + ScaleY(23 + 24);
    OK.Top := Form.ClientHeight - ScaleY(23 + 12);
    Cancel.Top := OK.Top;
    Cancel.Left := Form.ClientWidth - W - ScaleX(12);
    OK.Left := Cancel.Left - W - ScaleX(6);
    Form.ActiveControl := OK;
    if Form.ShowModal() <> mrOk then begin
      Result := False;
      exit;
    end;
    uKeepSaves := CSaves.Checked;
    uRemovePython := CPython.Checked;
    uRemoveGame := CGame.Checked;
    uRemoveShaders := CShaders.Checked;
    if CVS <> nil then
      uRemoveVS := CVS.Checked;
  finally
    Form.Free();
  end;
end;

function OtherTitles(const Dir: String): Boolean;
var
  F: TFindRec;
begin
  Result := False;
  if FindFirst(Dir + '\*', F) then
    try
      repeat
        if (F.Name <> '.') and (F.Name <> '..') and (Lowercase(F.Name) <> TITLE_DIR) then begin
          Result := True;
          exit;
        end;
      until not FindNext(F);
    finally
      FindClose(F);
    end;
end;

procedure Gone(const Path: String; IsDir: Boolean);
begin
  if IsDir then begin
    if DirExists(Path) then
      Log(Format('Remove %s: %d', [Path, Ord(DelTree(Path, True, True, True))]));
  end else if FileExists(Path) then
    Log(Format('Remove %s: %d', [Path, Ord(DeleteFile(Path))]));
end;

{ Where the game keeps saves and settings: %LOCALAPPDATA%, or /USERDATAROOT=<dir>
  (testing: an uninstall that must not touch this PC's real saves). }
function UserDataBase(): String;
begin
  if not CmdParam('USERDATAROOT', Result) or (Result = '') then
    Result := ExpandConstant('{localappdata}');
end;

procedure RemoveUserData();
var
  Root: String;
begin
  Root := UserDataBase() + '\xboxrecomp';
  if uRemoveShaders then
    Gone(Root + '\shadercache', True);
  if uKeepSaves then
    exit;
  { Blood Wake's own parts of the toolkit's save folder }
  Gone(Root + '\UDATA\' + TITLE_DIR, True);
  Gone(Root + '\TDATA\' + TITLE_DIR, True);
  Gone(Root + '\editions', True);
  Gone(UserDataBase() + '\BloodWake', True);
  { The rest (the emulated hard disk, caches) is shared by any game built with
    xboxrecomp: removed only when no other game has saves in it. }
  if uRemoveShaders and not OtherTitles(Root + '\UDATA') and not OtherTitles(Root + '\TDATA') then
    Gone(Root, True)
  else
    Log('Kept the shared parts of ' + Root);
end;

procedure RemoveBuildTools();
var
  Path, Inst, Args: String;
  Code: Integer;
begin
  Path := GetIniString('vs', 'path', '', ExpandConstant('{app}\installer.ini'));
  Inst := ExpandConstant('{commonpf32}\Microsoft Visual Studio\Installer\setup.exe');
  if (Path = '') or not FileExists(Inst) then begin
    Log('Build Tools: nothing to remove (' + Path + ')');
    exit;
  end;
  Args := 'uninstall --installPath ' + ArgQuote(Path) + ' --norestart';
  if UninstallSilent() then Args := Args + ' --quiet' else Args := Args + ' --passive';
  if HasParam('VSPLANONLY') then begin
    Log('Build Tools: plan only, would run: ' + Inst + ' ' + Args);
    exit;
  end;
  Log('Build Tools: ' + Inst + ' ' + Args);
  if ShellExec('', Inst, Args, '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then
    Log(Format('Build Tools uninstall exit code %d', [Code]))
  else
    Log('Build Tools uninstall did not start: ' + SysErrorMessage(Code));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App: String;
begin
  if CurUninstallStep = usUninstall then begin
    App := ExpandConstant('{app}');
    if uRemoveVS then
      RemoveBuildTools();
    { What the build wrote (Inno's log has only what Setup itself copied) }
    Gone(App + '\build', True);
    Gone(App + '\work', True);
    Gone(App + '\tools', True);
    Gone(App + '\Blood Wake.cmd', False);
    Gone(App + '\play.cmd', False);
    Gone(App + '\install.json', False);
    Gone(App + '\deps', True);
    Gone(App + '\setup', True);
    if uRemoveGame then
      Gone(App + '\game', True);
    if uRemovePython then
      Gone(App + '\python', True);
    RemoveUserData();
    Gone(App + '\installer.ini', False);
  end else if CurUninstallStep = usPostUninstall then
    RemoveDir(ExpandConstant('{app}'));
end;
