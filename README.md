# QQ拼音输入法 · Windows Terminal 候选框修复

> 现象：在 Windows Terminal（以及 Windows 控制台主机 conhost）里用 QQ拼音打拼音时，
> **能组字、能上屏，但候选框从来不出现**。Notepad、Chrome 等一切正常。
>
> 结论：**问题在 QQ拼音，不在 Windows Terminal。** WT 的行为虽然特殊，但符合 TSF 规范。

---

## 一、根因

### 根因 1：用 Win32 插入符定位候选窗，而 WT 根本没有插入符

`QQPinyinTSF.dll!sub_1800051A0`（`+0x52A2` 处调用 `GetGUIThreadInfo`）这样定位：

```c
GetWindowThreadProcessId(hWnd, NULL);
GetGUIThreadInfo(tid, &pgui);
if ( pgui.hwndCaret ) {                       // 0x1800052A9: jz -> 直接放弃
    GetWindowRect(pgui.hwndCaret, &Rect);
    v47.left = Rect.left + pgui.rcCaret.left; // ★ rcCaret 是【客户区坐标】，外壳自己加窗口原点
    ...
    sub_18000B1C0(a1, v47, 0);                // 写进 IMC 的 cfCandForm/cfCompForm
}
```

而 `WindowsTerminal.exe` / `conhost.exe` / `OpenConsole.exe` / `Microsoft.Terminal.Control.dll`
的导入表里**完全没有 `CreateCaret`/`SetCaretPos`** —— 它们从不创建 Win32 插入符
（新版 WT 还专门移除了 `TF_TMAE_UIELEMENTENABLEDONLY`，见 microsoft/terminal PR
#19046 / #19117 / #19584 / #19738 / #20542）。

| 宿主 | `GetGUIThreadInfo().hwndCaret` | 候选框 |
|---|---|---|
| Notepad（Win11 商店版，RichEditD2DPT） | `0x…`，`rcCaret=(32,32,33,75)` | 有 |
| **Windows Terminal** | **NULL** | **无** |
| **conhost / cmd** | **NULL** | **无** |

正确做法是问 TSF 要（`ITfContextView::GetTextExt`）。**WT 已经正确实现了它**：

```c
// src/tsf/Implementation.cpp:315
STDMETHODIMP Implementation::GetTextExt(LONG acpStart, LONG acpEnd, RECT* prc, BOOL* pfClipped) {
    *prc = _provider ? _provider->GetCursorPosition() : RECT{};   // 忽略 ACP，直接给光标格
// src/cascadia/TerminalControl/TermControl.cpp:203
RECT TsfDataProvider::GetCursorPosition() { ... 终端光标格的屏幕矩形 ... }
```

QQ拼音只是**从来不去问**。

### 根因 2：外壳会把"打开候选窗"的消息吞掉

引擎 `QQPinyin.ime` 在 `ImeToAsciiEx` 返回的消息数组里塞入
`WM_IME_NOTIFY (0x282)` + `IMN_OPENCANDIDATE (5) / IMN_CHANGECANDIDATE (3) / IMN_CLOSECANDIDATE (4)`。
外壳本该把它转发给**引擎自己的 UI 窗口**（`CTextService+176` = `QQPinyinUIWndTSF`），
引擎收到后才 `ShowWindow(QQPinyinCompWndTSF, SW_SHOWNOACTIVATE)`。

但外壳的 `QQPinyinTSF.dll!sub_18000AF70` 是个过滤器：

```c
_BOOL8 sub_18000AF70(__int64 a1, __int64 a2, __int64 a3) {
    if ( *(_DWORD *)a3 != 642 ) return false;             // 642 = 0x282
    v4 = *(_QWORD *)(a3 + 8);                             // wParam
    if ( (v4 - 3) > 2 ) return false;                     // 只管 3 / 4 / 5
    if ( dword_18002A330 && v4 == 4 ) PostMessageW(GetFocus(), 0x282, 4, 1);
    return (*(_BYTE *)(a1 + 15260) & 0x40) != 0           // ★ 条件A
        || *(_DWORD *)(a1 + 1688) == 0;                   // ★ 条件B
}
```

**返回真 = 这条消息不转发。** 而同一个 `a1+1688` 又是根因 1 那段插入符代码的前置条件
（`if ( *(DWORD*)(a1+1688) != 0 && dword_18002A314 == 0 )`）—— 既然在 WT 里插入符代码**确实执行了**
（我们能拦到 `GetGUIThreadInfo`），说明 `a1+1688 != 0`，那么 WT 下让过滤器返回真的只能是
**条件 A：`(CTextService+15260) & 0x40`**。

实测对照（用 IAT 钩子记录，带调用者偏移）：

```
# 正常宿主（testhost.exe）
SendMessage(QQPinyinUIWndTSF, wp=5) from QQPinyinTSF.dll+AA7B   <- 转发 IMN_OPENCANDIDATE
ShowWindow(QQPinyinCompWndTSF, cmd=4) from QQPinyin.ime+1022AE  <- 引擎显示候选窗
# Windows Terminal（修复前）
（没有 wp=5，没有 ShowWindow）—— 但 wp=9 (IMN_SETCANDIDATEPOS) 有，说明位置那条线是通的
```

---

## 二、修复方案

一个**代理 DLL** 顶替 TIP 的 `InprocServer32`，由 COM 在"输入法激活"时加载
—— **不需要常驻进程、不做任何注入、不改动任何签名文件**。

代理只做三件事：

1. **转发**：加载真身 `QQPinyinTSF.dll`（原路径、原文件），原样转发
   `DllGetClassObject` / `DllCanUnloadNow` / `DllRegisterServer` / `DllUnregisterServer`。
2. **替外壳去问 TSF 要真实光标**（只在宿主 = `WindowsTerminal.exe` 时）：
   - `CoCreateInstance(CLSID_TF_ThreadMgr)` → `ITfThreadMgr::Activate` 拿 client id；
   - 用**异步只读编辑会话**（`RequestEditSession(..., TF_ES_READ, ...)`，绝不阻塞 UI 线程）
     在 `DoEditSession(ec)` 里 `GetSelection` 取 range → `ITfContextView::GetTextExt(ec, range, ...)`
     → 得到终端光标格的屏幕矩形；
   - 再加一个 100ms 的线程定时器持续刷新，光标一动缓存就更新；
   - 把 `QQPinyinTSF.dll` 与 `QQPinyin.ime` 的 IAT 里 `user32!GetGUIThreadInfo` 接到本 DLL，
     宿主没有插入符时返回这个矩形（转成 `hwndCaret` 的客户区相对坐标 —— 外壳会自己加回窗口原点）。
   - 细节：引擎把候选框顶边放在插入符矩形的**垂直中心**，会压住那行拼音的下半部分，
     所以矩形整体**下移半格**；若光标已在窗口底部放不下，则保持原位（宁可略压字也不跑出窗口）。
3. **放行候选窗消息**：把 `sub_18000AF70` 最后的过滤条件改成"永远放行"——
   在 `QQPinyinTSF.dll+0xAFBD` 写 2 字节 **`EB 1B`**（`jmp 0xAFDA`，即 `xor eax,eax; ...; retn`）。
   函数入口的 `642`/`wParam 3..5` 判断和 `GetFocus` 副作用都保留。
   **仅进程内存补丁，磁盘文件不动、数字签名不受影响。**

安全措施：
- 只对 `hosts` 名单里的宿主动手（默认仅 `WindowsTerminal.exe`），其它进程纯转发、行为与原来完全一致；
- 自身用 `GET_MODULE_HANDLE_EX_FLAG_PIN` 钉住，防止 COM 释放后卸载导致 IAT 悬空；
- `DllMain` 里不做文件 IO / LoadLibrary；钩子里只用非阻塞的异步会话。

---

### 资源管理与路径

- **COM 接口指针**一律用 ATL 智能指针：`CComPtr` / `CComQIPtr`；自定义的 COM 对象
  （只读编辑会话）用 `CComObjectRootEx` + `CComObject`（`BEGIN_COM_MAP`），
  **不手写 `AddRef` / `Release`**。
- **内核句柄**（文件、线程）用 `qqpy::UniqueHandle`；临界区用 `qqpy::CriticalSection` + `CsLock`；
  线程定时器用 `qqpy::UniqueTimer`。全部 RAII，析构即释放。
- **路径不硬编码盘符**：

  | 用途 | 来源 |
  |---|---|
  | 本 DLL 所在目录 | `GetModuleFileNameW(本模块)` |
  | ini / log 目录 | 优先 DLL 所在目录；退回 `SHGetKnownFolderPath(FOLDERID_ProgramData)`；再退回环境变量 `ProgramData` / `ALLUSERSPROFILE` |
  | 真身 `QQPinyinTSF.dll` | `GetSystemDirectoryW()` + `\IME\QQPinyinTSF\QQPinyinTSF.dll`（32 位进程得到 SysWOW64，正好对应 32 位 IME）；可用 ini 的 `real=` 覆盖 |

- **不写裸数字**：每个编译单元顶部都有一块"常量"，模式取值用 `enum CaretMode`，
  尺寸/时限/上限全部命名（`kFakeCaretHeight`、`kIniReloadIntervalMs`、`kExtentMaxAgeMs` …）；
  外壳补丁的地址与字节也不是硬写的 —— `kCandFilterPatch` 由 `kCandFilterFilterRva`
  与 `kCandFilterReturnZeroRva` 算出来，`kCandFilterOriginal` 是打补丁前的校验字节。
- 编译单元为什么要分开：`qqpyproxy.cpp` 定义了 `DllGetClassObject` 等导出，而 `combaseapi.h`
  （经 `<objbase.h>` / ATL 引入）也声明了它们，同一编译单元会 **C2375 redefinition, different linkage**。
  所以导出所在的文件保持"干净"（只引 `windows.h`），COM/ATL 代码放在 `qqpytsf.cpp` / `qqpyutil.cpp`。
## 三、文件

| 文件 | 说明 |
|---|---|
| `qqpyproxy.cpp` | 代理 DLL：转发 + 假插入符 + 过滤器补丁 |
| `qqpytsf.cpp` | TSF 那部分（独立编译单元，避免 `<objbase.h>` 与 `DllGetClassObject` 声明冲突） |
| `qqpyutil.h` / `qqpyutil.cpp` | RAII 工具（`UniqueHandle` / `CriticalSection` / `CsLock` / `UniqueTimer`）与路径解析（全部来自 API 或环境变量）。头文件刻意不引 ATL/objbase |
| `build-proxy-win7.cmd` | **正式构建**：v142 toolset + SDK 10.0.17763.0 + 静态 CRT，产出可在 Win7 x64 运行 |
| `build-proxy.cmd` | 同上（薄封装，直接调用 Win7 版；避免误编出非 Win7 产物） |
| `qqpyproxy-install-machine.cmd` | 安装：HKLM 重定向到代理（需管理员，自动备份原值） |
| `qqpyproxy-restore-machine.cmd` | 卸载/兜底：还原原始注册表值 |
| `qqpyproxy-install.cmd` / `qqpyproxy-restore.cmd` | 早期 HKCU 方案（**实测 COM 不认 HKCU 覆盖这个 CLSID，已弃用**，留作记录） |
| `_win.ps1` / `_poll.ps1` | 诊断用的小工具（监视 WT 进程的模块/窗口几何） |

运行时布局 `C:\ProgramData\QQPYCandFix\`：

```
qqpyproxy.dll        代理（注册表指向它）
qqpyproxy.ini        配置
qqpyproxy.log        log=1 时的日志
orig-value.first.txt 原始 InprocServer32 值（安装时备份）
backup-CLSID.reg     CLSID 注册表导出备份
```

注册表（唯一改动，**64 位视图**；32 位视图未动，32 位程序完全不受影响）：

```
HKLM\SOFTWARE\Classes\CLSID\{AE51F1C0-807F-4A64-AC55-F2ADF92E2603}\InprocServer32
    (Default)      = C:\ProgramData\QQPYCandFix\qqpyproxy.dll     <- 改这里
    ThreadingModel = Apartment                                    <- 不动
```

`{AE51F1C0-807F-4A64-AC55-F2ADF92E2603}` = QQ拼音 TSF TIP；语言配置 `{96EC4774-55A1-498B-827F-E95D5445B6C1}`（zh-CN）。

---

## 四、构建（面向 Windows 7 x64）

```cmd
build-proxy-win7.cmd
```

| 项 | 值 |
|---|---|
| toolset | v142（VS2019，MSVC **14.29.30133**） |
| Windows SDK | **10.0.17763.0**（最后一个官方支持 Win7 目标的 SDK） |
| CRT | **静态 `/MT`** —— 不依赖 VCRUNTIME140 / MSVCP140 / UCRT，无需任何运行库 |
| 定义 | `_WIN32_WINNT=0x0601`、`WINVER=0x0601`、`NTDDI_VERSION=0x06010000` |
| 链接 | `/SUBSYSTEM:WINDOWS,6.01 /DYNAMICBASE /NXCOMPAT` |
| 语言标准 | `/std:c++17` |
| ATL | `CComPtr` / `CComQIPtr` / `CComObject` / `CComHeapPtr` 来自 ATL。脚本按 toolset 从旧到新找一份**头文件与 `atls.lib` 同版本**的 ATL，并用 `/I` + `/LIBPATH` 显式指定（不依赖 vcvars 是否把它加进包含路径）。装有 "C++ ATL for v142" 时正好取到 v142 那一份 |

选 SDK 版本靠 `vcvarsall.bat x64 10.0.17763.0 -vcvars_ver=14.29`；换机器时改脚本里的 `VS=` 路径即可。
Visual Studio 与 ATL 的位置都由脚本探测（`vswhere` + ATL 存在性检查），不需要手改路径。

产物自检（实测结果）：

```
架构        : x64
子系统      : GUI, 版本 6.1              <- Win7 兼容
导入的 DLL  : KERNEL32.dll / USER32.dll / ole32.dll   （只有 3 个）
动态 CRT    : 零
Win8+ API   : 无（全部导入函数逐个核对过）
```

---

## 五、安装 / 卸载 / 验证

```
安装（管理员）：qqpyproxy-install-machine.cmd
卸载（管理员）：qqpyproxy-restore-machine.cmd      # 改回原始注册表值，重启程序即恢复原状
```

验证代理是否真的进了 WT：

```powershell
(Get-Process WindowsTerminal).Modules |
    Where-Object ModuleName -match 'qqpyproxy|QQPinyin' | Select ModuleName,FileName
# 期望：qqpyproxy.dll + QQPinyinTSF.dll + QQPinyin.ime
```

验证行为：关掉所有 WT 窗口（确保进程退出），重开一个，打 `nihao`
→ 候选框贴在终端光标处、不遮拼音。

---

## 六、配置 `<DLL 所在目录>\qqpyproxy.ini`

```ini
real=C:\WINDOWS\system32\IME\QQPinyinTSF\QQPinyinTSF.dll
hosts=WindowsTerminal.exe     ; 只有这些宿主才打补丁（分号分隔）。想连 conhost 一起修就加上 conhost.exe
mode=1                        ; 0=只挂钩不动 1=修复 2=反向验证（强行抹掉插入符，用于证明因果）
log=0                         ; 1=写 C:\ProgramData\QQPYCandFix\qqpyproxy.log（含 TSF 会话逐步结果）
dx=8                          ; 仅当 TSF 拿不到光标时的回退：距客户区左边界
dy=8                          ; 仅当 TSF 拿不到光标时的回退：插入符底边距客户区下边界
```

ini **每 1.5 秒重读一次**，改了不用重装、不用重启 WT（`real`/`hosts` 除外，它们在加载时生效）。

---

## 七、注意事项

- **QQ拼音升级 / 修复安装 / 重新注册 TIP 会把注册表改回去** → 重跑一次安装脚本即可。
- 32 位宿主不受影响（只改了 64 位视图）。
- 若中文输入在任何程序里出问题：管理员运行 `qqpyproxy-restore-machine.cmd` 并重启相关程序。
  代理本身是纯转发，出问题概率极低，这是兜底。
- 代理 DLL 位于 `C:\ProgramData\QQPYCandFix\`，权限为"Users 只读"，普通用户无法替换。

### 在 Windows 7 上使用

Win7 没有 Windows Terminal，所以默认的 `hosts=WindowsTerminal.exe` 在那边**不会做任何事**（代理只转发）。
如果在 Win7 的控制台里也要修，把 ini 改成：

```ini
hosts=WindowsTerminal.exe;conhost.exe
```

`conhost.exe`（Windows 控制台主机）同样没有 Win32 插入符，症状一模一样。
若该宿主没有实现 TSF 的 `GetTextExt`，代理会自动退回"窗口客户区左下角"的定位方式 —— 候选框照样会出现，只是不跟随光标。


