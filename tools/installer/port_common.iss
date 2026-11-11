; Shared part of the port installers (the same file in all three ports).
; port.iss defines PortName, PortShort, PortAppId, PortUrl, GameTitle and UserDir;
; the game check uses tools\installer\check_game.ps1, gamefiles.ps1 and game_profile.psd1 (per port).
; build_installer.ps1 passes StageDir, OutDir and Version on the command line.

#ifndef StageDir
  #error StageDir is not defined. Build with tools\installer\build_installer.ps1.
#endif
#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef OutDir
  #define OutDir "dist"
#endif

[Setup]
AppId={#PortAppId}
AppName={#PortName}
AppVersion={#Version}
AppVerName={#PortName} {#Version}
AppPublisherURL={#PortUrl}
AppSupportURL={#PortUrl}/issues
AppUpdatesURL={#PortUrl}/releases
DefaultDirName={sd}\Games\{#PortShort}
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename={#PortShort}-Setup-{#Version}
SetupIconFile={#StageDir}\project\launcher.ico
UninstallDisplayIcon={app}\Launcher.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Wizard art (original, from tools\installer\make_wizard_images.py), read from the source tree at
; compile time so it does not depend on the stage. PNG and these sizes need Inno Setup 6.6+;
; older compilers keep the built-in images.
#ifndef ArtDir
  #define ArtDir AddBackslash(SourcePath) + "art"
#endif
#if (Ver >= EncodeVer(6,6,0)) && FileExists(ArtDir + "\wizimage_202x386.png")
WizardImageFile={#ArtDir}\wizimage_*.png
WizardSmallImageFile={#ArtDir}\wizsmall_*.png
; show the Welcome page (Inno 6 hides it by default) so the art is seen before the Finished page
DisableWelcomePage=no
#endif
DirExistsWarning=no

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "portable"; Description: "Portable mode: keep settings and saves only in the install folder (not in Saved Games)"; GroupDescription: "Other:"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Excludes: "\controls.txt,\mouselook.cfg"; Flags: recursesubdirs createallsubdirs ignoreversion
; default controls: an existing copy (yours) is kept, and none is installed when Saved Games has your own
Source: "{#StageDir}\controls.txt"; DestDir: "{app}"; Flags: onlyifdoesntexist; Check: NoUserCopy('controls.txt')
Source: "{#StageDir}\mouselook.cfg"; DestDir: "{app}"; Flags: onlyifdoesntexist skipifsourcedoesntexist; Check: NoUserCopy('mouselook.cfg')
; the game check, run from the wizard before anything is installed
Source: "{#StageDir}\tools\installer\check_game.ps1"; Flags: dontcopy
Source: "{#StageDir}\tools\installer\gamefiles.ps1"; Flags: dontcopy
Source: "{#StageDir}\tools\installer\game_profile.psd1"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\{#PortName}\{#PortName}"; Filename: "{app}\Launcher.exe"; WorkingDir: "{app}"; IconFilename: "{app}\project\launcher.ico"
Name: "{autoprograms}\{#PortName}\Setup (locate game, rebuild)"; Filename: "{app}\Setup.exe"; WorkingDir: "{app}"; IconFilename: "{app}\project\launcher.ico"
Name: "{autoprograms}\{#PortName}\Uninstall"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#PortName}"; Filename: "{app}\Launcher.exe"; WorkingDir: "{app}"; IconFilename: "{app}\project\launcher.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\Setup.exe"; Parameters: "{code:GameArg}"; WorkingDir: "{app}"; Description: "Open Setup now to build the port from your game (shows progress; click Install there)"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; Files made by Setup and the build. Settings are copied to Saved Games first (see usUninstall below).
Type: filesandordirs; Name: "{app}\build"
Type: filesandordirs; Name: "{app}\build_game"
Type: filesandordirs; Name: "{app}\build_regen"
Type: filesandordirs; Name: "{app}\build_rpcs3"
Type: filesandordirs; Name: "{app}\rsxcap"
Type: filesandordirs; Name: "{app}\recompiled"
Type: filesandordirs; Name: "{app}\recompiled_game"
Type: filesandordirs; Name: "{app}\logs"
Type: files; Name: "{app}\Launcher.exe"
Type: files; Name: "{app}\settings.bat"
Type: files; Name: "{app}\settings.ini"
Type: files; Name: "{app}\portable.txt"
Type: files; Name: "{app}\controls.txt"
Type: files; Name: "{app}\mouselook.cfg"

[Code]
const
  CfgFiles = 'settings.ini,settings.bat,controls.txt,mouselook.cfg';

var
  GamePage: TWizardPage;
  FoundList: TNewListBox;
  PathEdit: TNewEdit;
  StatusText: TNewStaticText;
  FoundPaths: TStringList;
  Searched, ToolsReady: Boolean;

// ---------------------------------------------------------------- settings backup (Saved Games)
function UserCfgDir(): String;
begin
  Result := ExpandConstant('{%USERPROFILE}\Saved Games\{#UserDir}\config');
end;

function NoUserCopy(Name: String): Boolean;
begin
  Result := WizardIsTaskSelected('portable') or not FileExists(UserCfgDir() + '\' + Name);
end;

procedure SaveConfigToUser(App: String);
var
  Names: TStringList;
  I: Integer;
begin
  if FileExists(App + '\portable.txt') then Exit;
  Names := TStringList.Create;
  try
    Names.CommaText := CfgFiles;
    ForceDirectories(UserCfgDir());
    for I := 0 to Names.Count - 1 do
      if FileExists(App + '\' + Names[I]) then
        FileCopy(App + '\' + Names[I], UserCfgDir() + '\' + Names[I], False);
  finally
    Names.Free;
  end;
end;

// ---------------------------------------------------------------- game check (check_game.ps1)
function PowerShellExe(): String;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

function PrepareTools(): Boolean;
begin
  if not ToolsReady then
  begin
    try
      ExtractTemporaryFile('check_game.ps1');
      ExtractTemporaryFile('gamefiles.ps1');
      ExtractTemporaryFile('game_profile.psd1');
      ToolsReady := True;
    except
      ToolsReady := False;
    end;
  end;
  Result := ToolsReady and FileExists(PowerShellExe());
end;

// "D:\" would end the quoted argument with \" ; "D:\." means the same folder
function QuoteArg(S: String): String;
begin
  if (Length(S) > 0) and (S[Length(S)] = '\') then S := S + '.';
  Result := '"' + S + '"';
end;

// Runs check_game.ps1; returns False when it could not run (no PowerShell, e.g. under Wine).
function RunCheck(Args: String; var Lines: TArrayOfString): Boolean;
var
  OutFile: String;
  Code: Integer;
begin
  Result := False;
  SetArrayLength(Lines, 0);
  if not PrepareTools() then Exit;
  OutFile := ExpandConstant('{tmp}\check_result.txt');
  DeleteFile(OutFile);
  if Exec(PowerShellExe(), '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File ' +
          QuoteArg(ExpandConstant('{tmp}\check_game.ps1')) + ' ' + Args + ' -Out ' + QuoteArg(OutFile),
          '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := LoadStringsFromFile(OutFile, Lines);
end;

function Field(const Lines: TArrayOfString; Key: String): String;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to GetArrayLength(Lines) - 1 do
    if Pos(Key + '=', Lines[I]) = 1 then
    begin
      Result := Copy(Lines[I], Length(Key) + 2, MaxInt);
      Exit;
    end;
end;

procedure SearchGames();
var
  Lines: TArrayOfString;
  I, P: Integer;
  S, St, Id, Path, Title: String;
begin
  Searched := True;
  StatusText.Caption := 'Looking for your game copy on this PC...';
  WizardForm.Refresh;
  if not RunCheck('-Find -PortDir ' + QuoteArg(WizardDirValue), Lines) then
  begin
    StatusText.Caption := 'The automatic search is not available here. Type or paste the path, or use the buttons.';
    Exit;
  end;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    S := Lines[I];
    P := Pos('|', S); if P = 0 then Continue; St := Copy(S, 1, P - 1); Delete(S, 1, P);
    P := Pos('|', S); if P = 0 then Continue; Id := Copy(S, 1, P - 1); Delete(S, 1, P);
    P := Pos('|', S); if P = 0 then Continue; Path := Copy(S, 1, P - 1); Title := Copy(S, P + 1, MaxInt);
    FoundPaths.Add(Path);
    if St = 'error' then
      FoundList.Items.Add(Path + '   (' + Title + ', ' + Id + ': not supported)')
    else
      FoundList.Items.Add(Path + '   (' + Title + ', ' + Id + ')');
  end;
  if FoundPaths.Count = 0 then
    StatusText.Caption := 'No copy was found automatically. Type or paste the path, or use the buttons.'
  else
  begin
    FoundList.ItemIndex := 0;
    PathEdit.Text := FoundPaths[0];
    StatusText.Caption := 'Select a copy in the list, or type another path.';
  end;
end;

procedure FoundListClick(Sender: TObject);
begin
  if (FoundList.ItemIndex >= 0) and (FoundList.ItemIndex < FoundPaths.Count) then
    PathEdit.Text := FoundPaths[FoundList.ItemIndex];
end;

procedure FileButtonClick(Sender: TObject);
var
  F: String;
begin
  F := PathEdit.Text;
  if GetOpenFileName('Select the game ISO, or PS3_DISC.SFB / PARAM.SFO / EBOOT.BIN of a game folder', F, ExtractFileDir(F),
                     'Game ISO or files|*.iso;PS3_DISC.SFB;PARAM.SFO;EBOOT.BIN|All files|*.*', 'iso') then
    PathEdit.Text := F;
end;

procedure FolderButtonClick(Sender: TObject);
var
  Dir: String;
begin
  Dir := PathEdit.Text;
  if (Dir <> '') and not DirExists(Dir) then Dir := ExtractFileDir(Dir);
  if BrowseForFolder('Select the game folder (the one that contains PS3_GAME).', Dir, False) then
    PathEdit.Text := Dir;
end;

// Checks the chosen copy; returns True when setup may continue.
function CheckGame(P: String): Boolean;
var
  Lines: TArrayOfString;
  St, Msg: String;
begin
  Result := True;
  StatusText.Caption := 'Checking the game files...';
  WizardForm.Refresh;
  if not RunCheck('-Game ' + QuoteArg(P) + ' -Dest ' + QuoteArg(WizardDirValue + '\gamefiles'), Lines) then
  begin
    StatusText.Caption := 'The game files could not be checked here. Setup checks them when it runs.';
    Exit;
  end;
  St := Field(Lines, 'status');
  Msg := Field(Lines, 'problem');
  if Field(Lines, 'fix') <> '' then Msg := Msg + #13#10#13#10 + 'What to do: ' + Field(Lines, 'fix');
  if Field(Lines, 'notes') <> '' then Msg := Msg + #13#10#13#10 + Field(Lines, 'notes');
  if St = 'ok' then
    StatusText.Caption := 'OK: ' + Field(Lines, 'title') + ' (' + Field(Lines, 'titleid') + ').'
  else if St = 'warn' then
  begin
    StatusText.Caption := Field(Lines, 'problem');
    Result := MsgBox(Msg + #13#10#13#10 + 'Use this copy anyway?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  end
  else if St = 'error' then
  begin
    StatusText.Caption := Field(Lines, 'problem');
    MsgBox(Msg, mbError, MB_OK);
    Result := False;
  end
  else
    StatusText.Caption := 'The game files could not be checked here. Setup checks them when it runs.';
end;

procedure InitializeWizard;
var
  Intro: TNewStaticText;
  FileBtn, FolderBtn: TNewButton;
begin
  FoundPaths := TStringList.Create;
  GamePage := CreateCustomPage(wpSelectTasks, 'Locate your game',
    'The port needs your own copy of {#GameTitle}. Game files are never included in this installer.');

  Intro := TNewStaticText.Create(GamePage);
  Intro.Parent := GamePage.Surface;
  Intro.AutoSize := False;
  Intro.WordWrap := True;
  Intro.Width := GamePage.SurfaceWidth;
  Intro.Height := ScaleY(30);
  Intro.Caption := 'Use a decrypted disc image (.iso) or a disc dump folder (the one that contains PS3_GAME). ' +
                   'Copies found on this PC are listed below. Leave the path empty to choose the game later in Setup.';

  FoundList := TNewListBox.Create(GamePage);
  FoundList.Parent := GamePage.Surface;
  FoundList.Top := Intro.Top + Intro.Height + ScaleY(6);
  FoundList.Width := GamePage.SurfaceWidth;
  FoundList.Height := ScaleY(84);
  FoundList.OnClick := @FoundListClick;

  PathEdit := TNewEdit.Create(GamePage);
  PathEdit.Parent := GamePage.Surface;
  PathEdit.Top := FoundList.Top + FoundList.Height + ScaleY(8);
  PathEdit.Width := GamePage.SurfaceWidth - ScaleX(170);

  FileBtn := TNewButton.Create(GamePage);
  FileBtn.Parent := GamePage.Surface;
  FileBtn.Caption := 'ISO...';
  FileBtn.Left := PathEdit.Left + PathEdit.Width + ScaleX(8);
  FileBtn.Top := PathEdit.Top - ScaleY(1);
  FileBtn.Width := ScaleX(77);
  FileBtn.Height := ScaleY(23);
  FileBtn.OnClick := @FileButtonClick;

  FolderBtn := TNewButton.Create(GamePage);
  FolderBtn.Parent := GamePage.Surface;
  FolderBtn.Caption := 'Folder...';
  FolderBtn.Left := FileBtn.Left + FileBtn.Width + ScaleX(8);
  FolderBtn.Top := FileBtn.Top;
  FolderBtn.Width := ScaleX(77);
  FolderBtn.Height := ScaleY(23);
  FolderBtn.OnClick := @FolderButtonClick;

  StatusText := TNewStaticText.Create(GamePage);
  StatusText.Parent := GamePage.Surface;
  StatusText.AutoSize := False;
  StatusText.WordWrap := True;
  StatusText.Top := PathEdit.Top + PathEdit.Height + ScaleY(8);
  StatusText.Width := GamePage.SurfaceWidth;
  StatusText.Height := ScaleY(44);
  StatusText.Caption := '';
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = GamePage.ID) and not Searched then SearchGames();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  P: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    if Pos(' ', WizardDirValue) > 0 then
    begin
      MsgBox('Please choose a folder without spaces in its path, for example C:\Games\{#PortShort}. The build tools need it.', mbError, MB_OK);
      Result := False;
    end;
  end
  else if CurPageID = GamePage.ID then
  begin
    P := Trim(PathEdit.Text);
    if (Length(P) > 1) and (P[1] = '"') and (P[Length(P)] = '"') then P := Copy(P, 2, Length(P) - 2);
    PathEdit.Text := P;
    if P = '' then Exit;
    if not FileExists(P) and not DirExists(P) then
    begin
      MsgBox('This file or folder does not exist:' + #13#10 + P, mbError, MB_OK);
      Result := False;
      Exit;
    end;
    Result := CheckGame(P);
  end;
end;

function GameArg(Param: String): String;
begin
  Result := '';
  if Trim(PathEdit.Text) <> '' then
    Result := '--game ' + QuoteArg(Trim(PathEdit.Text));
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then Result := Result + MemoTasksInfo + NewLine + NewLine;
  Result := Result + 'Your game copy:' + NewLine + Space;
  if Trim(PathEdit.Text) <> '' then
    Result := Result + Trim(PathEdit.Text) + NewLine + Space + '(copied into the install folder by the first-time setup)'
  else
    Result := Result + 'choose it later in Setup';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('portable') then
    SaveStringToFile(ExpandConstant('{app}\portable.txt'),
      'Portable mode: settings and saves stay in this folder. Delete this file to keep a copy in Saved Games again.' + #13#10, False);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  App: String;
begin
  App := ExpandConstant('{app}');
  if CurUninstallStep = usUninstall then
    SaveConfigToUser(App);
  if CurUninstallStep = usPostUninstall then
  begin
    if DirExists(App + '\game') or DirExists(App + '\gamefiles') or DirExists(App + '\gamedata') then
      if SuppressibleMsgBox('Also delete the game files in ' + App + ' (game, gamefiles, gamedata)?' + #13#10#13#10 +
                'If Setup moved your game folder here instead of copying it, this is your only copy. An ISO is never changed. ' +
                'gamedata holds the saves that are kept in this folder.' + #13#10#13#10 +
                'Your settings are kept in Saved Games\{#UserDir} unless portable mode was on.',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES then
      begin
        DelTree(App + '\game', True, True, True);
        DelTree(App + '\gamefiles', True, True, True);
        DelTree(App + '\gamedata', True, True, True);
        DelTree(App + '\shadercache', True, True, True);
      end;
  end;
end;