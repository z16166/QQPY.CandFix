// qqpyproxy.cpp —— QQ拼音 TSF TIP 代理 DLL（x64）
//
// 作用：把 CLSID {AE51F1C0-807F-4A64-AC55-F2ADF92E2603} 的 InprocServer32 指到本代理；
//       代理加载真身（原路径、原文件、签名不动）并转发 DllGetClassObject 等导出。
//       由 COM 在"输入法激活"时加载 —— 不需要常驻进程、不做任何注入。
//
// 只在 hosts 名单里的宿主（默认 WindowsTerminal.exe）动手：
//   1) 把 QQPinyinTSF.dll / QQPinyin.ime 的 IAT 里 user32!GetGUIThreadInfo 换成本 DLL：
//      宿主没有 Win32 插入符时，用 TSF 给的真实光标格（见 qqpytsf.cpp），
//      拿不到时退回窗口客户区左下角（夹在显示器工作区内）。
//   2) 把 QQPinyinTSF.dll!sub_18000AF70 的过滤条件改成"永远放行"，
//      让引擎请求的 IMN_OPENCANDIDATE 能送到引擎自己的 UI 窗口。
//   3) log=1 时再挂 SendMessageW/PostMessageW/ShowWindow/SetWindowPos 做诊断。
//
// 资源管理：文件/线程句柄用 qqpy::UniqueHandle，临界区用 qqpy::CriticalSection + CsLock（RAII）。
// 路径：全部来自 API 或环境变量（见 qqpyutil.cpp），不硬编码任何盘符目录。
//
// 注意：本编译单元定义了 DllGetClassObject / DllCanUnloadNow，
//       而 combaseapi.h 也声明了它们，所以这里【不能】引入 <objbase.h> / ATL，
//       否则 C2375 redefinition (different linkage)。COM/ATL 代码都在 qqpytsf.cpp。
//
// 配置 <DLL 所在目录>\qqpyproxy.ini
//   real=<真身完整路径>      缺省 = <SystemDir>\IME\QQPinyinTSF\QQPinyinTSF.dll
//   hosts=WindowsTerminal.exe
//   mode=1                   0=只挂钩不动 1=修复 2=反向验证(抹掉插入符)
//   log=0                    1=写 <DLL 所在目录>\qqpyproxy.log
//   dx=8 dy=8                仅在拿不到 TSF 光标时的回退偏移
//
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include "qqpyutil.h"

typedef BOOL (WINAPI *PFN_GGTI)(DWORD, PGUITHREADINFO);

namespace {

// ------------------------------------------------------------------ 状态
HMODULE       g_real = nullptr;
PFN_GGTI      g_realGGTI = nullptr;
volatile LONG g_mode = 1;
volatile LONG g_dx = 8;      // 回退定位：假插入符距客户区左边界的偏移
volatile LONG g_dy = 8;      // 回退定位：假插入符底边距客户区下边界的距离
volatile LONG g_logOn = 0;
volatile LONG g_logCount = 0;
volatile LONG g_hookCount = 0;

qqpy::CriticalSection g_cfgLock;      // 保护下面两个字符串
std::wstring          g_realPath;     // 真身路径
std::string           g_hosts;        // 宿主名单

qqpy::UniqueHandle    g_logFile;      // 日志文件句柄（RAII）
qqpy::CriticalSection g_logLock;

int PatchIAT(HMODULE mod, void* target, void* repl);

// ------------------------------------------------------------------ 日志
// 调用者必须已持有 g_logLock
void LogFileEnsure()
{
    if (g_logFile.valid()) return;
    const std::wstring path = qqpy::LogPath();
    if (path.empty()) return;
    g_logFile.reset(::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
}

void Log(const char* fmt, ...)
{
    if (!g_logOn) return;
    if (InterlockedIncrement(&g_logCount) > 600) return;

    qqpy::CsLock lock(g_logLock);
    LogFileEnsure();
    if (!g_logFile.valid()) return;

    char buf[900];
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    int n = wsprintfA(buf, "[%02d:%02d:%02d.%03d pid=%lu] ", st.wHour, st.wMinute, st.wSecond,
                      st.wMilliseconds, (unsigned long)::GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written = 0;
    ::WriteFile(g_logFile.get(), buf, (DWORD)n, &written, nullptr);
}

// ------------------------------------------------------------------ 调用者定位
// 必须在钩子函数内部调用 _ReturnAddress() 再把结果传进来，
// 否则取到的是本 DLL 内部的返回地址。
void CallerTagOf(void* ra, char* out, int cch)
{
    HMODULE m = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(ra), &m) && m)
    {
        char name[MAX_PATH] = {};
        ::GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        base = base ? base + 1 : name;
        wsprintfA(out, "%s+%X", base, (unsigned)((const BYTE*)ra - (const BYTE*)m));
    }
    else
    {
        wsprintfA(out, "%p", ra);
    }
}

// ------------------------------------------------------------------ ini
// 只有行首（或空白后）的 key= 才算匹配，避免 "log=" 撞上别的键名
size_t FindKey(const std::string& text, const char* key)
{
    const std::string pat = std::string(key) + "=";
    size_t pos = 0;
    while ((pos = text.find(pat, pos)) != std::string::npos)
    {
        if (pos == 0 || text[pos - 1] == '\n' || text[pos - 1] == '\r' ||
            text[pos - 1] == ' ' || text[pos - 1] == '\t')
            return pos;
        pos += pat.size();
    }
    return std::string::npos;
}

bool IniGetInt(const std::string& text, const char* key, int& out)
{
    const size_t pos = FindKey(text, key);
    if (pos == std::string::npos) return false;
    out = atoi(text.c_str() + pos + strlen(key) + 1);
    return true;
}

std::string IniGetStr(const std::string& text, const char* key)
{
    const size_t pos = FindKey(text, key);
    if (pos == std::string::npos) return std::string();
    size_t begin = pos + strlen(key) + 1;
    size_t end = text.find_first_of("\r\n", begin);
    if (end == std::string::npos) end = text.size();
    return text.substr(begin, end - begin);
}

std::string IniText()   // 读 ini 全文（文件句柄 RAII）
{
    const std::wstring ini = qqpy::IniPath();
    if (ini.empty()) return std::string();

    qqpy::UniqueHandle h(::CreateFileW(ini.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.valid()) return std::string();

    char buf[2048] = {};
    DWORD rd = 0;
    if (!::ReadFile(h.get(), buf, sizeof(buf) - 1, &rd, nullptr)) return std::string();
    return std::string(buf, rd);
}

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// firstTime=true 时同时确定 real / hosts（只在激活时调用一次，之后不再改动）
void ReloadIni(bool firstTime)
{
    static volatile LONG last = 0;
    if (!firstTime)
    {
        const DWORD now = ::GetTickCount();
        const LONG prev = last;
        if (now - (DWORD)prev < 1500) return;                    // 1.5 秒内不重复读
        if (InterlockedCompareExchange(&last, (LONG)now, prev) != prev) return;
    }

    const std::string text = IniText();
    int v = 0;

    if (IniGetInt(text, "mode", v) && v >= 0 && v <= 2) InterlockedExchange(&g_mode, v);
    if (IniGetInt(text, "dx", v) && v >= -500 && v <= 4000) InterlockedExchange(&g_dx, v);
    if (IniGetInt(text, "dy", v) && v >= 0 && v <= 4000) InterlockedExchange(&g_dy, v);
    if (IniGetInt(text, "log", v)) InterlockedExchange(&g_logOn, v ? 1 : 0);

    if (firstTime)
    {
        std::wstring real = Widen(IniGetStr(text, "real"));
        if (real.empty()) real = qqpy::DefaultRealDllPath();     // 由 GetSystemDirectoryW 推导

        std::string hosts = IniGetStr(text, "hosts");
        if (hosts.empty()) hosts = "WindowsTerminal.exe";

        qqpy::CsLock lock(g_cfgLock);
        g_realPath = real;
        g_hosts = hosts;
    }
}

std::wstring RealPathCopy()
{
    qqpy::CsLock lock(g_cfgLock);
    return g_realPath;
}

bool HostMatches()
{
    char me[MAX_PATH] = {};
    {
        WCHAR path[MAX_PATH] = {};
        if (!::GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
        const WCHAR* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        ::WideCharToMultiByte(CP_ACP, 0, base, -1, me, MAX_PATH, nullptr, nullptr);
    }

    std::string hosts;
    {
        qqpy::CsLock lock(g_cfgLock);
        hosts = g_hosts;
    }

    const char* s = hosts.c_str();
    while (*s)
    {
        while (*s == ' ' || *s == ';' || *s == ',') ++s;
        const char* e = s;
        while (*e && *e != ';' && *e != ',') ++e;
        const size_t n = (size_t)(e - s);
        if (n && n == strlen(me) && _strnicmp(s, me, n) == 0) return true;
        s = e;
    }
    return false;
}

// ------------------------------------------------------------------ TSF 真实光标（实现在 qqpytsf.cpp）
extern "C" void TsfInit(void (*logfn)(const char* msg));
extern "C" BOOL TsfGetCaret(RECT* prc);
void TsfLog(const char* msg) { Log("%s", msg); }

// ------------------------------------------------------------------ 插入符钩子
BOOL WINAPI Hook_GetGUIThreadInfo(DWORD idThread, PGUITHREADINFO pgui)
{
    PFN_GGTI real = g_realGGTI;
    if (!real) return FALSE;

    const BOOL ok = real(idThread, pgui);
    const LONG mode = g_mode;
    if (!pgui || mode == 0) return ok;

    const LONG nth = InterlockedIncrement(&g_hookCount);

    if (mode == 2)
    {
        if (nth <= 6) Log("hook#%ld mode2: force hwndCaret=NULL (was %p)", nth, pgui->hwndCaret);
        pgui->hwndCaret = nullptr;
        pgui->rcCaret.left = pgui->rcCaret.top = pgui->rcCaret.right = pgui->rcCaret.bottom = 0;
        return ok;
    }

    if (pgui->hwndCaret)
    {
        if (nth <= 6) Log("hook#%ld mode1: host has caret %p -> pass through", nth, pgui->hwndCaret);
        return ok;
    }

    HWND anchor = ::GetForegroundWindow();
    if (!anchor || !::IsWindow(anchor)) anchor = pgui->hwndFocus;
    if (!anchor || !::IsWindow(anchor)) anchor = pgui->hwndActive;
    if (!anchor || !::IsWindow(anchor))
    {
        if (nth <= 6) Log("hook#%ld mode1: no anchor (fg/focus/active all null)", nth);
        return ok;
    }

    RECT wr = {};
    if (!::GetWindowRect(anchor, &wr))
    {
        if (nth <= 6) Log("hook#%ld GetWindowRect failed", nth);
        return ok;
    }

    // 首选：TSF 给的真实光标格 —— 候选框跟着终端光标走
    {
        RECT rc = {};
        if (TsfGetCaret(&rc))
        {
            const int h = rc.bottom - rc.top;
            // 引擎把候选框顶边放在插入符矩形的【垂直中心】，会压住那行拼音的下半部分，
            // 所以整体下移半格；窗口底部放不下时保持原样。
            bool shift = false;
            POINT org = {};
            RECT  crc = {};
            if (h > 2 && ::ClientToScreen(anchor, &org) && ::GetClientRect(anchor, &crc))
                shift = (rc.bottom + h / 2 + 3 * h) < (org.y + crc.bottom);
            if (shift)
            {
                rc.top += h / 2;
                rc.bottom += h / 2;
            }

            pgui->hwndCaret = anchor;
            pgui->rcCaret.left = rc.left - wr.left;
            pgui->rcCaret.top = rc.top - wr.top;
            pgui->rcCaret.right = rc.right - wr.left;
            pgui->rcCaret.bottom = rc.bottom - wr.top;

            if (nth <= 8)
            {
                char who[128];
                CallerTagOf(_ReturnAddress(), who, sizeof(who));
                Log("hook#%ld [%s] TSF-CARET screen=(%d,%d-%d,%d) h=%d shift=%d",
                    nth, who, rc.left, rc.top, rc.right, rc.bottom, h, (int)shift);
            }
            return ok;
        }
    }

    // 回退：窗口客户区左下角（夹进显示器工作区，保证不出屏）
    RECT cr = {};
    POINT cli = {};
    if (!::GetClientRect(anchor, &cr)) return ok;
    if ((wr.bottom - wr.top) < 60 || (cr.bottom - cr.top) < 60) return ok;
    if (!::ClientToScreen(anchor, &cli)) return ok;

    ReloadIni(false);                                   // 允许热改 dx/dy
    const int dx = g_dx, dy = g_dy;
    int sx = cli.x + dx;
    int sy = cli.y + (cr.bottom - cr.top) - dy - 20;    // 20 = 假插入符自身高度

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = ::MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
    if (mon && ::GetMonitorInfoW(mon, &mi))
    {
        if (sx < mi.rcWork.left + 2) sx = mi.rcWork.left + 2;
        if (sx > mi.rcWork.right - 6) sx = mi.rcWork.right - 6;
        if (sy < mi.rcWork.top + 2) sy = mi.rcWork.top + 2;
        if (sy > mi.rcWork.bottom - 24) sy = mi.rcWork.bottom - 24;
    }

    pgui->hwndCaret = anchor;
    pgui->rcCaret.left = sx - wr.left;
    pgui->rcCaret.right = pgui->rcCaret.left + 2;
    pgui->rcCaret.top = sy - wr.top;
    pgui->rcCaret.bottom = pgui->rcCaret.top + 20;

    if (nth <= 8)
    {
        char cls[64] = {};
        ::GetClassNameA(anchor, cls, sizeof(cls) - 1);
        char who[128];
        CallerTagOf(_ReturnAddress(), who, sizeof(who));
        Log("hook#%ld [%s] FAKE anchor=%p(%s) win=(%d,%d %dx%d) work=(%d,%d-%d,%d) screen=(%d,%d) rc=(%d,%d,%d,%d)",
            nth, who, anchor, cls, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
            mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom, sx, sy,
            pgui->rcCaret.left, pgui->rcCaret.top, pgui->rcCaret.right, pgui->rcCaret.bottom);
    }
    return ok;
}

// ------------------------------------------------------------------ 诊断钩子
typedef BOOL (WINAPI *PFN_ShowWindow)(HWND, int);
typedef BOOL (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef LRESULT (WINAPI *PFN_SendMessageW)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (WINAPI *PFN_PostMessageW)(HWND, UINT, WPARAM, LPARAM);

PFN_ShowWindow   g_realShowWindow = nullptr;
PFN_SetWindowPos g_realSetWindowPos = nullptr;
PFN_SendMessageW g_realSendMessageW = nullptr;
PFN_PostMessageW g_realPostMessageW = nullptr;

void ClassOf(HWND h, char* out, int cch)
{
    out[0] = 0;
    if (h && ::IsWindow(h)) ::GetClassNameA(h, out, cch - 1);
}

BOOL WINAPI Hook_ShowWindow(HWND h, int cmd)
{
    if (g_logOn)
    {
        char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
        char cls[80];  ClassOf(h, cls, sizeof(cls));
        Log("ShowWindow(%p [%s], cmd=%d) from %s", h, cls, cmd, who);
    }
    return g_realShowWindow ? g_realShowWindow(h, cmd) : FALSE;
}

BOOL WINAPI Hook_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if (g_logOn)
    {
        char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
        char cls[80];  ClassOf(h, cls, sizeof(cls));
        Log("SetWindowPos(%p [%s], %d,%d %dx%d, flags=0x%X) from %s", h, cls, x, y, cx, cy, flags, who);
    }
    return g_realSetWindowPos ? g_realSetWindowPos(h, after, x, y, cx, cy, flags) : FALSE;
}

LRESULT WINAPI Hook_SendMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == 0x282 && g_logOn)
    {
        char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
        char cls[80];  ClassOf(h, cls, sizeof(cls));
        Log("SendMessage(%p [%s], WM_IME_NOTIFY, wp=%u) from %s", h, cls, (unsigned)wp, who);
    }
    return g_realSendMessageW ? g_realSendMessageW(h, m, wp, lp) : 0;
}

BOOL WINAPI Hook_PostMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == 0x282 && g_logOn)
    {
        char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
        char cls[80];  ClassOf(h, cls, sizeof(cls));
        Log("PostMessage(%p [%s], WM_IME_NOTIFY, wp=%u) from %s", h, cls, (unsigned)wp, who);
    }
    return g_realPostMessageW ? g_realPostMessageW(h, m, wp, lp) : FALSE;
}

void PatchApiIAT(const wchar_t* modName, const wchar_t* dll, const char* fn, void* hook)
{
    HMODULE m = ::GetModuleHandleW(modName);
    HMODULE d = ::GetModuleHandleW(dll);
    if (!m || !d) return;
    void* real = (void*)::GetProcAddress(d, fn);
    if (!real) return;
    if (const int n = PatchIAT(m, real, hook); n)
        Log("PatchApiIAT %ws!%s = %d", modName, fn, n);
}

// ------------------------------------------------------------------ IAT 补丁
int PatchIAT(HMODULE mod, void* target, void* repl)
{
    if (!mod || !target || !repl) return 0;
    __try
    {
        auto dos = (IMAGE_DOS_HEADER*)mod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        const DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva) return 0;

        int n = 0;
        for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + rva); imp->Name; ++imp)
        {
            auto iat = (void**)((BYTE*)mod + imp->FirstThunk);
            if (!iat) continue;
            for (; *iat; ++iat)
            {
                if (*iat != target) continue;
                DWORD old = 0;
                if (::VirtualProtect(iat, sizeof(void*), PAGE_READWRITE, &old))
                {
                    *iat = repl;
                    ::VirtualProtect(iat, sizeof(void*), old, &old);
                    ++n;
                }
            }
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void PatchOne(const wchar_t* name)
{
    HMODULE m = ::GetModuleHandleW(name);
    if (!m) return;
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    void* real = u32 ? (void*)::GetProcAddress(u32, "GetGUIThreadInfo") : nullptr;
    if (!real) return;
    const int n = PatchIAT(m, real, (void*)&Hook_GetGUIThreadInfo);
    Log("PatchOne %ws base=%p -> %d", name, m, n);
}

// 核心修复：外壳 sub_18000AF70 会把引擎请求的"打开候选窗"消息吞掉。
//   在 WT 里它直接 return 1（= 不转发），引擎的 UI 窗口收不到 IMN_OPENCANDIDATE，
//   于是引擎永远不 ShowWindow 它的候选窗。这里把该函数最后的过滤条件改成永远放行：
//     原: F6 83 9C 3B 00 00 40   test byte ptr [rbx+3B9Ch],40h
//     新: EB 1B                  jmp 0xAFDA (xor eax,eax; add rsp,20h; pop rbx; retn)
//   函数入口的 642 / wParam 3..5 判断，以及 wParam==4 时的 GetFocus+PostMessage 副作用都保留。
//   仅进程内存补丁，磁盘文件与数字签名都不动。
void PatchCandFilter()
{
    HMODULE m = ::GetModuleHandleW(L"QQPinyinTSF.dll");
    if (!m) { Log("PatchCandFilter: shell not loaded"); return; }

    BYTE* p = (BYTE*)m + 0xAFBD;
    if (p[0] != 0xF6 || p[1] != 0x83 || p[2] != 0x9C || p[3] != 0x3B ||
        p[4] != 0x00 || p[5] != 0x00 || p[6] != 0x40)
    {
        Log("PatchCandFilter: UNEXPECTED %02X %02X %02X %02X %02X %02X %02X",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
        return;
    }

    DWORD old = 0;
    if (!::VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old))
    {
        Log("PatchCandFilter: VirtualProtect failed %lu", ::GetLastError());
        return;
    }
    p[0] = 0xEB;
    p[1] = 0x1B;
    ::VirtualProtect(p, 2, old, &old);
    ::FlushInstructionCache(::GetCurrentProcess(), p, 2);
    Log("PatchCandFilter: APPLIED at %p (EB 1B)", p);
}

DWORD WINAPI EngineWatcher(LPVOID)
{
    for (int i = 0; i < 3600; ++i)   // 引擎是外壳稍后 LoadLibraryW 进来的，等它出现
    {
        if (::GetModuleHandleW(L"QQPinyin.ime"))
        {
            PatchOne(L"QQPinyin.ime");
            PatchApiIAT(L"QQPinyin.ime", L"user32.dll", "SendMessageW", (void*)&Hook_SendMessageW);
            PatchApiIAT(L"QQPinyin.ime", L"user32.dll", "PostMessageW", (void*)&Hook_PostMessageW);
            PatchApiIAT(L"QQPinyin.ime", L"user32.dll", "ShowWindow",   (void*)&Hook_ShowWindow);
            PatchApiIAT(L"QQPinyin.ime", L"user32.dll", "SetWindowPos", (void*)&Hook_SetWindowPos);
            break;
        }
        ::Sleep(500);
    }
    return 0;
}

// ------------------------------------------------------------------ 转发
HMODULE EnsureReal()
{
    if (g_real) return g_real;

    ReloadIni(true);                                  // 确定 real / hosts / mode / log
    const std::wstring realPath = RealPathCopy();
    const bool match = HostMatches();

    g_real = ::LoadLibraryW(realPath.c_str());
    Log("EnsureReal host_match=%d mode=%ld log=%ld real=%p path=%ws",
        (int)match, g_mode, g_logOn, g_real, realPath.c_str());
    if (!g_real || !match) return g_real;             // 不在名单里的宿主：只转发，什么都不改

    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    if (u32)
    {
        g_realGGTI         = (PFN_GGTI)::GetProcAddress(u32, "GetGUIThreadInfo");
        g_realShowWindow   = (PFN_ShowWindow)::GetProcAddress(u32, "ShowWindow");
        g_realSetWindowPos = (PFN_SetWindowPos)::GetProcAddress(u32, "SetWindowPos");
        g_realSendMessageW = (PFN_SendMessageW)::GetProcAddress(u32, "SendMessageW");
        g_realPostMessageW = (PFN_PostMessageW)::GetProcAddress(u32, "PostMessageW");
    }

    PatchOne(L"QQPinyinTSF.dll");
    PatchCandFilter();
    PatchApiIAT(L"QQPinyinTSF.dll", L"user32.dll", "SendMessageW", (void*)&Hook_SendMessageW);
    PatchApiIAT(L"QQPinyinTSF.dll", L"user32.dll", "PostMessageW", (void*)&Hook_PostMessageW);
    TsfInit(TsfLog);

    {
        // 线程句柄由 RAII 关闭（线程本身继续跑）
        qqpy::UniqueHandle thread(::CreateThread(nullptr, 0, EngineWatcher, nullptr, 0, nullptr));
        if (!thread.valid()) Log("EnsureReal: CreateThread FAILED");
    }
    return g_real;
}

void* RealProc(const char* name)
{
    HMODULE m = EnsureReal();
    return m ? (void*)::GetProcAddress(m, name) : nullptr;
}

} // namespace

// ------------------------------------------------------------------ 导出（与真身一一对应）
extern "C" __declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    typedef HRESULT(__stdcall * PFN)(REFCLSID, REFIID, void**);
    PFN f = (PFN)RealProc("DllGetClassObject");
    return f ? f(rclsid, riid, ppv) : (HRESULT)0x80040111L;   // CLASS_E_CLASSNOTAVAILABLE
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllCanUnloadNow()
{
    typedef HRESULT(__stdcall * PFN)();
    PFN f = (PFN)RealProc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllRegisterServer()
{
    typedef HRESULT(__stdcall * PFN)();
    PFN f = (PFN)RealProc("DllRegisterServer");
    return f ? f() : S_OK;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllUnregisterServer()
{
    typedef HRESULT(__stdcall * PFN)();
    PFN f = (PFN)RealProc("DllUnregisterServer");
    return f ? f() : S_OK;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        ::DisableThreadLibraryCalls(hMod);
        // 钉住自己：COM 释放类对象时会 FreeLibrary，若我们被卸载，
        // 已经写进真身 IAT 的指针就会指向已释放代码 -> 宿主崩溃
        HMODULE self = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&DllMain), &self);
        // DllMain 里不做文件 IO / LoadLibrary / shell API
    }
    return TRUE;
}
