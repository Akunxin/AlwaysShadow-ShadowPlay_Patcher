# AlwaysShadow

Shadowplay's Instant Replay feature is unreliable. You often find out it is turned off when you need it most. This is despite the fact that it's supposed to run on startup. This is a simple Windows program which will make sure Instant Replay is on at all times.

## Usage instructions

Run AlwaysShadow.exe. The program will make sure to turn Instant Replay back on should it ever turn off. Additionally, there is a system tray icon with some options you can check out.

For this program to work, you have to turn on NVIDIA overlay in your NVIDIA App settings.

### Recovery after remote access

Recovery is automatic while AlwaysShadow is running and enabled:

- **Microsoft Remote Desktop (RDP):** AlwaysShadow pauses replay commands while its session is remote, disconnected or locked. After you log in or unlock the **same Windows account at the physical PC**, it waits about 10 seconds for NVIDIA to initialize, reloads the replay controls and tries to restore Instant Replay. With the normal polling interval, the first attempt is usually within 10–20 seconds of returning to the local desktop. Disconnecting RDP can leave Windows locked; in that case, recovery waits for local login/unlock.
- **Sunlogin and similar remote-control apps:** these apps may not generate Windows session notifications. AlwaysShadow keeps checking replay and, after two unsuccessful attempts, retries about every 30 seconds. When remote control ends and NVIDIA permits capture again, a subsequent attempt restores replay. If the app locks the PC on disconnect, recovery waits for local unlock. A remote-control service can remain running in the background; you do not need to quit it.
- Manual disabling, timers, the whitelist and the exclusives list still take precedence. Recovery only enables replay when those rules allow it. It checks the current replay state before toggling, so it does not turn off replay that NVIDIA has already restored.

Keep NVIDIA overlay enabled and configure its Instant Replay toggle shortcut. AlwaysShadow refreshes the shortcut and the legacy GeForce Experience connection before retrying. It does not start a disabled NVIDIA overlay. Enable **Run at startup** if you also want recovery after signing out and signing back in, since signing out closes AlwaysShadow.

Session changes, local desktop readiness and retry attempts are recorded in the usual log file (see [Issues](#issues)).

#### 远控结束后恢复即时重放

此功能默认生效，只需保持 AlwaysShadow 运行且未暂停，并开启 NVIDIA 游戏内覆盖和即时重放切换快捷键。

- **微软 RDP：**远程连接、断开但未登录、锁屏期间暂停恢复；回到物理机，用同一个 Windows 账户登录或解锁后，通常在 10–20 秒内开始尝试恢复。
- **向日葵等远控：**部分软件不发送 Windows 会话事件，因此还会通过定时重试兜底。连续失败后约每 30 秒尝试一次，远控结束且 NVIDIA 允许录制后即可恢复；如果结束远控时锁屏，则等待本地解锁。无需退出常驻的远控服务。
- 手动暂停、白名单和限定游戏规则继续生效。如果执行的是“注销”而不是“断开”，建议启用托盘菜单的 **Run at startup**，让下次登录时自动启动程序。

### Whitelisting

Some programs (such as Netflix) prevent Instant Replay from being active, which conflicts with this program. You can define a list of programs that will cause this program to disable itself while they are running. To define your own list, create a file named **exactly** Whitelist.txt in the same folder where you run the executable. For every program you want to add to the list, add its command line to Whitelist.txt in its own line.

To find out a program's command line, run it and go to the Task Manager. Right click the top bar and make sure "Command line" is enabled, like so:

![Screenshot (44)](https://user-images.githubusercontent.com/30209851/132571330-e7a0415e-78b2-42d2-9607-4f8e8759c4cd.png)

Once enabled, Task Manager will show each process's command line. Run the process you want to add to the list, find it in the Task Manager, and copy-paste its command line to your Whitelist.txt in its own line.

### Advanced whitelisting

The whitelist supports a few additional, more advanced features. Each line in the whitelist may begin with an expression of the form: `??<flags> `. That is, a `??`, followed by one or more flag characters, and then a space character. The flag characters modify the behavior of the line. For example, your whitelist may contain the following line:

```
??ES Hades.exe
```

This causes AlwaysShadow to only run while a process with "Hades.exe" *anywhere* within its command line is running. The order of the flags doesn't matter. The complete list of flags is:

`E` - Adds this line to the "exclusives" list, not the whitelist. This program will force Shadowplay to be disabled when no "exclusive" command is running, and enabled while at least one is running. If there are whitelisted programs running too, the whitelist wins and AlwaysShadow is disabled.

`S` - Makes this line match any command lines of which it is a substring, as opposed to the default behavior where the lines must match exactly. This means that instead of copying long ugly command lines, you can pick a pretty part of the command line to use (like the exe). But **make sure** that the line won't also match things you don't want it to.

`N` - Makes this line be matched against the name of the program executable instead of the command line. To find out a program's name, go to Task Manager same as above and enable "Process name".

`I` - Makes this line be ignored. You can use this to add comments.

The repo includes an example Whitelist.txt but **it is only for example**, as the exact command lines may be different on your PC.

## Notes

You will need to refresh this program (click the icon in the system tray and hit Refresh) if you do one of the following things:
1. Change the shortcut for toggling Instant Replay on/off in your NVIDIA App settings
2. Create, delete, or modify your Whitelist.txt file

AlwaysShadow turns on Instant Replay by simulating the keypresses for the shortcut that toggles it in NVIDIA App, which by default is Alt+Shift+F10. This may cause AlwaysShadow to change your keyboard language because the default shortcut for cycling between languages in Windows is Alt+Shift. To resolve this issue, it is recommended to open NVIDIA overlay and change the shortcut for toggling Instant Replay. I use Ctrl+Shift+F10. Remember that after changing the shortcut you will need to refresh program.

Some programs (Netflix for example) may run in the background at all times, which means if you whitelist them AlwaysShadow will see them as always running. You can disable these programs running in the background in [Windows settings](https://support.microsoft.com/en-us/windows/windows-background-apps-and-your-privacy-83f2de44-d2d9-2b29-4649-2afe0913360a).

## Installation

1. Go to [Releases](https://github.com/Verpous/AlwaysShadow/releases)
1. Download the latest version (or any previous one)
2. Extract the exe to wherever you want
3. Run it!
4. I recommend enabling "Run at startup", which you can do from the system tray icon

And of course, you can always clone the repo and compile it yourself!

## Compilation instructions

1. Install [MSYS2](https://www.msys2.org/)
2. Install mingw-w64, [make](https://www.gnu.org/software/make/), pkg-config, libcurl and regex development packages for 64-bit using MSYS2. The makefile uses `pkg-config --libs --static regex libcurl` to find the dependencies required by your installed library versions.
3. Add mingw-w64 and make's bin folders to PATH (should look something like "C:\msys64\mingw64\bin" and "C:\msys64\usr\bin", respectively)
4. Clone this repository
5. Run make inside the root directory of the repository. This will create the program executable named "AlwaysShadow.exe" inside a folder named "bin"

The makefile includes some additional targets which are explained inside the makefile via comments.

Run `make test` for the session guard, recovery timing and NVIDIA control regression tests. These tests simulate RDP, lock/unlock, missing session notifications, extended capture failures and state changes during a recovery attempt without changing the Windows session, contacting NVIDIA or sending keyboard input. To verify on NVIDIA hardware, leave AlwaysShadow enabled, connect and disconnect the remote tool, then check Instant Replay after returning to the local desktop. Also check that a manual pause or matching whitelist entry prevents recovery.

## Uninstallation

There is no fancy uninstall process. Just delete AlwaysShadow wherever you installed it.

## GeForce Experience vs NVIDIA App

Both the old GeForce Experience and the new NVIDIA App are supported.

## Issues

Feel free to open an issue for any request or problem. When you do, please exit or refresh AlwaysShadow, then upload the log file located at:

```
%LOCALAPPDATA%/AlwaysShadow/output.log
```

I am not affiliated with NVidia in any way.
