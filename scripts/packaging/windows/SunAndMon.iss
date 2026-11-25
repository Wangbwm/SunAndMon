; SunAndMon Windows x64 Installer (Inno Setup 6)
; Automatically stops SunAndMonHostService and running processes before upgrade,
; preserves %APPDATA%\SunAndMon user configuration, and optionally installs
; the elevated host service and ZakoVDD virtual display driver.

#ifndef AppVersion
  #define AppVersion "1.0.1"
#endif
#ifndef SourceDir
  #error "SourceDir must be defined via /DSourceDir=..."
#endif
#ifndef IconFile
  #define IconFile "..\..\..\moonlight-qt\app\sunandmon.ico"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\..\dist"
#endif
#ifndef OutputBaseFilename
  #define OutputBaseFilename "SunAndMon-Setup-1.0.1-x64"
#endif

[Setup]
AppId={{E8A5F4D2-9C3B-4A71-B5E2-7D3A6C91F0B4}
AppName=SunAndMon
AppVersion={#AppVersion}
AppVerName=SunAndMon {#AppVersion}
AppPublisher=SunAndMon
AppPublisherURL=https://github.com/Wangbwm/SunAndMon
AppSupportURL=https://github.com/Wangbwm/SunAndMon/issues
DefaultDirName={autopf}\SunAndMon
DefaultGroupName=SunAndMon
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\SunAndMon.exe
UninstallDisplayName=SunAndMon {#AppVersion}
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseFilename}
Compression=lzma2/max
SolidCompression=yes
LZMAUseSeparateProcess=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=yes
UsePreviousTasks=yes

[Languages]
#ifdef HasChineseSimplified
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#endif
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
#ifdef HasChineseSimplified
chinesesimplified.TaskDesktopIcon=创建桌面快捷方式
chinesesimplified.TaskInstallService=安装并启动 SunAndMon 系统后台服务（推荐，支持锁屏与 UAC 远程控制）
chinesesimplified.TaskInstallVdd=安装 ZakoVDD 虚拟显示器驱动（可选，支持硬件级彻底息屏隐私屏）
chinesesimplified.GroupAdditional=附加选项：
chinesesimplified.LaunchAfterInstall=安装完成后立即启动 SunAndMon
chinesesimplified.StatusStopping=正在停止旧版 SunAndMon 服务与相关进程…
chinesesimplified.StatusFirewall=正在配置 Windows 防火墙放行规则…
chinesesimplified.StatusInstallService=正在安装并启动 SunAndMon 后台系统服务…
chinesesimplified.StatusInstallVdd=正在安装 ZakoVDD 虚拟显示器驱动…
#endif
english.TaskDesktopIcon=Create a desktop shortcut
english.TaskInstallService=Install and start SunAndMon Host Service (recommended for lock-screen and UAC control)
english.TaskInstallVdd=Install ZakoVDD virtual display driver (optional, for hardware monitor-off privacy screen)
english.GroupAdditional=Additional options:
english.LaunchAfterInstall=Launch SunAndMon now
english.StatusStopping=Stopping existing SunAndMon service and running processes...
english.StatusFirewall=Configuring Windows Firewall rules...
english.StatusInstallService=Installing and starting SunAndMon Host Service...
english.StatusInstallVdd=Installing ZakoVDD virtual display driver...

[Tasks]
Name: "desktopicon"; Description: "{cm:TaskDesktopIcon}"; GroupDescription: "{cm:GroupAdditional}"
Name: "installservice"; Description: "{cm:TaskInstallService}"; GroupDescription: "{cm:GroupAdditional}"
Name: "installvdd"; Description: "{cm:TaskInstallVdd}"; GroupDescription: "{cm:GroupAdditional}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\SunAndMon"; Filename: "{app}\SunAndMon.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\SunAndMon"; Filename: "{app}\SunAndMon.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SunAndMon Host"""; StatusMsg: "{cm:StatusFirewall}"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""SunAndMon Host"" dir=in action=allow program=""{app}\host\sunshine.exe"" enable=yes profile=any"; StatusMsg: "{cm:StatusFirewall}"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SunAndMon Client"""; StatusMsg: "{cm:StatusFirewall}"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""SunAndMon Client"" dir=in action=allow program=""{app}\SunAndMon.exe"" enable=yes profile=any"; StatusMsg: "{cm:StatusFirewall}"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SunAndMon Tunnel"""; StatusMsg: "{cm:StatusFirewall}"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""SunAndMon Tunnel"" dir=in action=allow program=""{app}\tunnel\sunandmon-tunnel.exe"" enable=yes profile=any"; StatusMsg: "{cm:StatusFirewall}"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SunAndMon EasyTier"""; StatusMsg: "{cm:StatusFirewall}"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""SunAndMon EasyTier"" dir=in action=allow program=""{app}\tunnel\easytier-core.exe"" enable=yes profile=any"; StatusMsg: "{cm:StatusFirewall}"; Flags: runhidden waituntilterminated
Filename: "{app}\host\tools\sunshinesvc.exe"; Parameters: "--install-sunandmon-service"; StatusMsg: "{cm:StatusInstallService}"; Flags: runhidden waituntilterminated; Tasks: installservice
Filename: "{app}\host\scripts\install-vdd.bat"; Parameters: "--preserve-healthy-existing"; WorkingDir: "{app}\host\scripts"; StatusMsg: "{cm:StatusInstallVdd}"; Flags: runhidden waituntilterminated; Tasks: installvdd
Filename: "{app}\SunAndMon.exe"; Description: "{cm:LaunchAfterInstall}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\host\tools\sunshinesvc.exe"; Parameters: "--uninstall-sunandmon-service"; Flags: runhidden waituntilterminated; RunOnceId: "UninstallSunAndMonService"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SunAndMon Host"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveFirewallHost"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SunAndMon Client"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveFirewallClient"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SunAndMon Tunnel"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveFirewallTunnel"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SunAndMon EasyTier"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveFirewallEasyTier"

[Code]
procedure RunHiddenCmd(const FileName, Params: string);
var
  ResultCode: Integer;
begin
  Exec(FileName, Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure StopSunAndMonProcesses;
begin
  { Stop elevated host service first so sunshinesvc.exe and sunshine.exe exit cleanly }
  RunHiddenCmd(ExpandConstant('{sys}\sc.exe'), 'stop SunAndMonHostService');
  Sleep(1500);

  { Ensure no lingering SunAndMon / host / tunnel processes lock files in the install dir }
  RunHiddenCmd(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM SunAndMon.exe');
  RunHiddenCmd(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM sunshine.exe');
  RunHiddenCmd(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM sunshine-gui.exe');
  RunHiddenCmd(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM sunshinesvc.exe');
  RunHiddenCmd(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM sunandmon-tunnel.exe');
  RunHiddenCmd(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM easytier-core.exe');
  RunHiddenCmd(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM moonlight-clipboard-helper.exe');
  RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Sunshine GUI Desktop');
  Sleep(800);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  WizardForm.StatusLabel.Caption := CustomMessage('StatusStopping');
  StopSunAndMonProcesses;
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  StopSunAndMonProcesses;
  Result := True;
end;
