# AlwaysShadow

[English](README.md) | [简体中文](README_zh.md)

GeForce Experience / NVIDIA App 的即时重放（Instant Replay）功能可能不稳定。您经常会在最需要它的时候发现它已经被关闭了，尽管它本来应该随系统启动而运行。这是一个简单的 Windows 托盘程序，用于确保即时重放始终处于开启状态。

## 使用说明

运行 `AlwaysShadow.exe`。该程序会确保在即时重放关闭时自动将其重新开启。此外，系统托盘图标提供了一些选项。其中一个选项是开机自启（Run at startup），建议开启该选项。

AlwaysShadow 还包含一个集成式的 ShadowPlay 受保护内容补丁程序（基于 furyzenblade/ShadowPlay_Patcher）。此补丁默认开启，并可通过系统托盘菜单中的 **ShadowPlay 补丁程序 (ShadowPlay patcher)** 复选框进行切换。禁用该选项将停止未来的自动修补尝试；但已经应用到 NVIDIA 正在运行的容器进程中的补丁仍将在内存中保持有效，直到该进程重启。

为了使本程序正常工作，您必须在 GeForce Experience 或 NVIDIA App 设置中开启“游戏内覆盖”（In-Game Overlay）。

### 白名单

某些程序（例如 Netflix）会阻止即时重放处于活动状态，这与本程序冲突。您可以定义一个程序列表，让本程序在这些程序运行时自动禁用自身。要定义您自己的列表，请在运行可执行文件的同一文件夹中创建一个**精确命名为** `Whitelist.txt` 的文件。对于您要添加到列表中的每个程序，请在 `Whitelist.txt` 中单独占一行添加其命令行。

要找出程序的命令行，请运行它并转到“任务管理器”。右键单击顶部标题栏并确保启用了“命令行”列，如下所示：

![Screenshot (44)](https://user-images.githubusercontent.com/30209851/132571330-e7a0415e-78b2-42d2-9607-4f8e8759c4cd.png)

启用后，任务管理器将显示每个进程的命令行。运行您要添加到列表中的进程，在任务管理器中找到它，并将它的命令行复制粘贴到 `Whitelist.txt` 中单独占一行。

### 高级白名单规则

白名单支持一些额外的、更高级的功能。白名单中的每一行都可以以类似 `??<flags> ` 的表达式开头。即，一个 `??` 紧跟着一个或多个标志字符，然后是一个空格字符。标志字符会修改该行的匹配行为。例如，您的白名单可能包含以下行：

```
??ES Hades.exe
```

这会导致 AlwaysShadow 仅在命令行中*任意位置*包含 "Hades.exe" 的进程运行时运行。标志的顺序无关紧要。完整的标志列表如下：

`E` - 将此行添加到“独占”列表，而不是白名单。当没有“独占”命令运行时，本程序将强制禁用 ShadowPlay，并在此类命令至少有一个在运行时启用 ShadowPlay。如果有加入白名单的程序也在运行，则白名单规则优先，AlwaysShadow 将被禁用。

`S` - 使此行匹配任何将其作为子字符串的命令行，而不是默认的完全匹配行为。这意味着您不需要复制长而难看的完整命令行，而是可以选择命令行中较好识别的部分（例如 exe 名称）进行匹配。但是请**确保**该行不会错误匹配到您不希望匹配的其他内容。

`N` - 使此行与可执行程序的名称匹配，而不是与命令行匹配。要找出程序的名称，请像上面一样转到任务管理器并启用“进程名称”列。

`I` - 使此行被忽略。您可以使用它来添加注释。

该仓库包含一个示例 `Whitelist.txt`，但**仅供参考**，因为您电脑上的实际命令行可能会有所不同。

## 注意事项

如果您做了以下操作之一，您将需要刷新此程序（单击通知栏中的图标并点击“刷新 (Refresh)”）：
1. 更改了 GeForce Experience / NVIDIA App 设置中用于切换即时重放开启/关闭的快捷键
2. 创建、删除或修改了您的 `Whitelist.txt` 文件

AlwaysShadow 可能（但在正常情况下不会）通过模拟在 GeForce Experience 中切换即时重放的快捷键（默认是 Alt+Shift+F10）来开启它。这可能会导致 AlwaysShadow 改变您的键盘输入法语言，因为 Windows 中循环切换语言的默认快捷键是 Alt+Shift。要解决此问题，建议前往 GeForce Experience / NVIDIA App 设置并更改切换即时重放的快捷键。我个人使用 Ctrl+Shift+F10。请记住，在更改快捷键后，您需要退出并重新启动此程序。

某些程序（例如 Netflix）可能始终在后台运行，这意味着如果您将它们加入白名单，AlwaysShadow 将把它们视为始终在运行。您可以在 [Windows 设置](https://support.microsoft.com/en-us/windows/windows-background-apps-and-your-privacy-83f2de44-d2d9-2b29-4649-2afe0913360a) 中禁用这些程序在后台运行。

## 下载

只需转到 [Releases](https://github.com/Verpous/AlwaysShadow/releases) 并下载最新版本或任何之前的版本。当然，您也可以随时克隆该仓库并自行编译！

## 编译指南

1. 安装 [MSYS2](https://www.msys2.org/)
2. 使用 MSYS2 安装 64 位 mingw-w64 以及 [make](https://www.gnu.org/software/make/)
3. 将 mingw-w64 和 make 的 bin 文件夹添加到 PATH 环境变量中（分别类似于 "C:\msys64\mingw64\bin" 和 "C:\msys64\usr\bin"）
4. 克隆此仓库
5. 在仓库的根目录下运行 `make`。这将在名为 `bin` 的文件夹中创建名为 `AlwaysShadow.exe` 的程序可执行文件

makefile 包含一些额外的编译目标，在 makefile 中通过注释进行了说明。

## 问题反馈

如果您有任何要求或问题，请随时开启 Issue。当您这么做时，请退出或刷新 AlwaysShadow，然后上传位于以下路径的日志文件：

```
%LOCALAPPDATA%/AlwaysShadow/output.log
```

我与 NVIDIA 公司无任何关联。
