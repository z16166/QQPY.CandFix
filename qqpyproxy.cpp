// qqpyproxy.cpp —— QQ拼音 TIP 代理 DLL（x64）
//
// 把 CLSID {AE51F1C0-807F-4A64-AC55-F2ADF92E2603} 的 InprocServer32 从真身
// QQPinyinTSF.dll 改成本代理；代理加载真身（原路径、原文件、签名不动）并转发导出。
// 因为是 COM 在"输入法激活"时加载的，所以不需要任何常驻进程、不做任何注入。
//
// 只在 hosts 名单里的宿主（默认 WindowsTerminal.exe）才动手：
// 改 QQPinyinTSF.dll / QQPinyin.ime 自己的 IAT 里 user32!GetGUIThreadInfo 那一格，
// 宿主没有 Win32 插入符时替它编一个位置。
//
// 配置 C:\ProgramData\QQPYCandFix\qqpyproxy.ini
//   real=C:\WINDOWS\system32\IME\QQPinyinTSF\QQPinyinTSF.dll
//   hosts=WindowsTerminal.exe
//   mode=1       0=只挂钩不动 1=修复 2=反向验证(抹掉插入符)
//   log=1        1=写诊断日志 C:\ProgramData\QQPYCandFix\qqpyproxy.log
//
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

typedef BOOL (WINAPI *PFN_GGTI)(DWORD, PGUITHREADINFO);

static HMODULE       g_real = nullptr;
static PFN_GGTI      g_realGGTI = nullptr;
static volatile LONG g_mode = 1;
static volatile LONG g_dx = 8;   // 假插入符相对客户区左边界的偏移（进程坐标）
static volatile LONG g_dy = 8;   // 假插入符【底边】距客户区下边界的距离：调大 = 候选框整体往上走
static volatile LONG g_logOn = 0;
static volatile LONG g_logCount = 0;
static volatile LONG g_hookCount = 0;
static HANDLE        g_logFile = INVALID_HANDLE_VALUE;
static WCHAR         g_realPath[MAX_PATH] = L"C:\\WINDOWS\\system32\\IME\\QQPinyinTSF\\QQPinyinTSF.dll";
static WCHAR         g_logPath[MAX_PATH]  = L"C:\\ProgramData\\QQPYCandFix\\qqpyproxy.log";
static char          g_hosts[512] = "WindowsTerminal.exe";

// ---------------------------------------------------------------- 调用者
// 注意：必须在钩子函数内部调用 _ReturnAddress() 再把结果传进来，
// 否则取到的是本 DLL 内部的返回地址。
static void CallerTagOf(void* ra, char* out, int cch)
{
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)ra, &m) && m) {
        char name[MAX_PATH] = {0};
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = strrchr(name, '\\'); base = base ? base + 1 : name;
        wsprintfA(out, "%s+%X", base, (unsigned)((BYTE*)ra - (BYTE*)m));
    } else {
        wsprintfA(out, "%p", ra);
    }
}

// ---------------------------------------------------------------- log
static void Log(const char* fmt, ...)
{
    if (!g_logOn) return;
    if (InterlockedIncrement(&g_logCount) > 400) return;
    if (g_logFile == INVALID_HANDLE_VALUE)
        g_logFile = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    char buf[768];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = wsprintfA(buf, "[%02d:%02d:%02d.%03d pid=%lu] ", st.wHour, st.wMinute, st.wSecond,
                      st.wMilliseconds, (unsigned long)GetCurrentProcessId());
    va_list ap; va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';
    DWORD wr = 0; WriteFile(g_logFile, buf, (DWORD)n, &wr, nullptr);
}

// ---------------------------------------------------------------- ini
static void LoadIni()
{
    // 每 1.5 秒最多重读一次 —— 这样改完 ini 不用重装、不用重启 WT 就能看到效果
    static volatile LONG last = 0;
    DWORD now = GetTickCount();
    LONG prev = last;
    if (now - (DWORD)prev < 1500) return;
    if (InterlockedCompareExchange(&last, (LONG)now, prev) != prev) return;
    WCHAR path[MAX_PATH] = L"C:\\ProgramData\\QQPYCandFix\\qqpyproxy.ini";
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[2048] = {0};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);

    char* p = strstr(buf, "log=");
    if (p) g_logOn = (atoi(p + 4) != 0) ? 1 : 0;
    p = strstr(buf, "real=");
    if (p) {
        char* e = p + 5; char* nl = strpbrk(e, "\r\n");
        size_t n = nl ? (size_t)(nl - e) : strlen(e);
        if (n > 3 && n < MAX_PATH) {
            WCHAR w[MAX_PATH] = {0};
            MultiByteToWideChar(CP_ACP, 0, e, (int)n, w, MAX_PATH - 1);
            lstrcpynW(g_realPath, w, MAX_PATH);
        }
    }
    p = strstr(buf, "mode=");
    if (p) { int m = atoi(p + 5); if (m >= 0 && m <= 2) g_mode = m; }
    p = strstr(buf, "dx=");
    if (p) { int v = atoi(p + 3); if (v >= -500 && v <= 4000) g_dx = v; }
    p = strstr(buf, "dy=");
    if (p) { int v = atoi(p + 3); if (v >= 0 && v <= 4000) g_dy = v; }
    p = strstr(buf, "hosts=");
    if (p) {
        char* e = p + 6; char* nl = strpbrk(e, "\r\n");
        size_t n = nl ? (size_t)(nl - e) : strlen(e);
        if (n > 0 && n < sizeof(g_hosts)) { memcpy(g_hosts, e, n); g_hosts[n] = 0; }
    }
}

static void HostName(char* out, size_t cch)
{
    WCHAR p[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    const WCHAR* base = wcsrchr(p, L'\\'); base = base ? base + 1 : p;
    out[0] = 0;
    WideCharToMultiByte(CP_ACP, 0, base, -1, out, (int)cch, nullptr, nullptr);
}

static bool HostMatches()
{
    char ansi[MAX_PATH] = {0};
    HostName(ansi, sizeof(ansi));
    const char* s = g_hosts;
    while (*s) {
        while (*s == ' ' || *s == ';' || *s == ',') ++s;
        const char* e = s;
        while (*e && *e != ';' && *e != ',') ++e;
        size_t n = (size_t)(e - s);
        if (n && n == strlen(ansi) && _strnicmp(s, ansi, n) == 0) return true;
        s = e;
    }
    return false;
}

// ---------------------------------------------------------------- TSF 真实光标（在 qqpytsf.cpp 里）
// WT 的 ITfContextOwner::GetTextExt 直接返回终端光标格的屏幕矩形；QQ拼音外壳从不问它。
// 这两个函数替它去问，见 qqpytsf.cpp。
extern "C" void TsfInit(void (*logfn)(const char* msg));
extern "C" BOOL TsfGetCaret(RECT* prc);
static void TsfLog(const char* msg) { Log("%s", msg); }
// ---------------------------------------------------------------- caret hook
static BOOL WINAPI Hook_GetGUIThreadInfo(DWORD idThread, PGUITHREADINFO pgui)
{
    PFN_GGTI real = g_realGGTI;
    if (!real) return FALSE;
    BOOL ok = real(idThread, pgui);
    LONG mode = g_mode;
    if (!pgui || mode == 0) return ok;

    LONG nth = InterlockedIncrement(&g_hookCount);
    if (mode == 2) {
        if (nth <= 6) Log("hook#%ld mode2: force hwndCaret=NULL (was %p)", nth, pgui->hwndCaret);
        pgui->hwndCaret = nullptr;
        pgui->rcCaret.left = pgui->rcCaret.top = pgui->rcCaret.right = pgui->rcCaret.bottom = 0;
        return ok;
    }
    if (pgui->hwndCaret) {
        if (nth <= 6) Log("hook#%ld mode1: host has caret %p -> pass through", nth, pgui->hwndCaret);
        return ok;
    }

    HWND anchor = GetForegroundWindow();
    HWND src = anchor;
    if (!anchor || !IsWindow(anchor)) { anchor = pgui->hwndFocus; src = anchor; }
    if (!anchor || !IsWindow(anchor)) { anchor = pgui->hwndActive; src = anchor; }
    if (!anchor || !IsWindow(anchor)) { if (nth <= 6) Log("hook#%ld mode1: NO anchor (fg/focus/active all null)", nth); return ok; }

    RECT wr, cr; POINT cli = {0, 0};
    if (!GetWindowRect(anchor, &wr)) { if (nth <= 6) Log("hook#%ld GetWindowRect failed", nth); return ok; }

    // ★ 首选：TSF 给的真实光标矩形 —— 候选框跟着终端光标走
    {
        RECT rc = {0,0,0,0};
        if (TsfGetCaret(&rc)) {
            // 引擎把候选框的顶边放在插入符矩形的【垂直中心】，所以会压住那行拼音的下半部分。
            // 把矩形整体下移半格，框就落到拼音下面；窗口底部放不下时保持原样。
            const int h = rc.bottom - rc.top;
            bool shift = false;
            POINT org = {0, 0}; RECT crc = {0,0,0,0};
            if (h > 2 && ClientToScreen(anchor, &org) && GetClientRect(anchor, &crc))
                shift = (rc.bottom + h / 2 + 3 * h) < (org.y + crc.bottom);
            if (shift) { rc.top += h / 2; rc.bottom += h / 2; }
            pgui->hwndCaret      = anchor;
            pgui->rcCaret.left   = rc.left   - wr.left;
            pgui->rcCaret.top    = rc.top    - wr.top;
            pgui->rcCaret.right  = rc.right  - wr.left;
            pgui->rcCaret.bottom = rc.bottom - wr.top;
            if (nth <= 8) {
                char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
                Log("hook#%ld [%s] TSF-CARET screen=(%d,%d-%d,%d) shift=%d", nth, who,
                    rc.left, rc.top, rc.right, rc.bottom, (int)shift);
            }
            return ok;
        }
    }

    if (!GetClientRect(anchor, &cr)) { if (nth <= 6) Log("hook#%ld GetClientRect failed", nth); return ok; }
    if ((wr.bottom - wr.top) < 60 || (cr.bottom - cr.top) < 60) {
        if (nth <= 6) Log("hook#%ld anchor %p too small (%dx%d) -> skip", nth, anchor, wr.right - wr.left, cr.bottom - cr.top);
        return ok;
    }
    if (!ClientToScreen(anchor, &cli)) { if (nth <= 6) Log("hook#%ld ClientToScreen failed", nth); return ok; }

    // 目标：客户区左下角（等于终端提示符那一带）
    LoadIni();                                   // 允许热改 dx/dy
    const int dx = g_dx, dy = g_dy;
    int sx = cli.x + dx;
    int sy = cli.y + (cr.bottom - cr.top) - dy - 20;   // 20 = 假插入符自身高度

    // ★ 夹进该窗口所在显示器的工作区，保证候选框不会跑到屏幕外
    MONITORINFO mi = {0}; mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        if (sx < mi.rcWork.left + 2)    sx = mi.rcWork.left + 2;
        if (sx > mi.rcWork.right - 6)   sx = mi.rcWork.right - 6;
        if (sy < mi.rcWork.top + 2)     sy = mi.rcWork.top + 2;
        if (sy > mi.rcWork.bottom - 24) sy = mi.rcWork.bottom - 24;
    }

    pgui->hwndCaret      = anchor;
    pgui->rcCaret.left   = sx - wr.left;
    pgui->rcCaret.right  = pgui->rcCaret.left + 2;
    pgui->rcCaret.top    = sy - wr.top;
    pgui->rcCaret.bottom = pgui->rcCaret.top + 20;
    if (nth <= 8) {
        char cls[64] = {0}; GetClassNameA(anchor, cls, sizeof(cls) - 1);
        char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
        Log("hook#%ld [%s] FAKE anchor=%p(%s) win=(%d,%d %dx%d) work=(%d,%d-%d,%d) screen=(%d,%d) rc=(%d,%d,%d,%d)",
            nth, who, anchor, cls, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
            mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom, sx, sy,
            pgui->rcCaret.left, pgui->rcCaret.top, pgui->rcCaret.right, pgui->rcCaret.bottom);
    }
    return ok;
}

static int PatchIAT(HMODULE mod, void* target, void* repl);

// ---------------------------------------------------------------- 诊断钩子
typedef BOOL (WINAPI *PFN_ShowWindow)(HWND, int);
typedef BOOL (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef LRESULT (WINAPI *PFN_SendMessageW)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (WINAPI *PFN_PostMessageW)(HWND, UINT, WPARAM, LPARAM);

static PFN_ShowWindow   g_realShowWindow = nullptr;
static PFN_SetWindowPos g_realSetWindowPos = nullptr;
static PFN_SendMessageW g_realSendMessageW = nullptr;
static PFN_PostMessageW g_realPostMessageW = nullptr;

static void ClassOf(HWND h, char* out, int cch)
{
    out[0] = 0;
    if (h && IsWindow(h)) GetClassNameA(h, out, cch - 1);
}

static BOOL WINAPI Hook_ShowWindow(HWND h, int cmd)
{
    LONG n = InterlockedIncrement(&g_logCount);
    if (g_logOn && n <= 400) {
        char cls[80]; ClassOf(h, cls, sizeof(cls));
        char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
        Log("ShowWindow(%p [%s], cmd=%d) from %s", h, cls, cmd, who);
    }
    return g_realShowWindow ? g_realShowWindow(h, cmd) : FALSE;
}
static BOOL WINAPI Hook_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    LONG n = InterlockedIncrement(&g_logCount);
    if (g_logOn && n <= 400) {
        char cls[80]; ClassOf(h, cls, sizeof(cls));
        char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
        Log("SetWindowPos(%p [%s], %d,%d %dx%d, flags=0x%X) from %s", h, cls, x, y, cx, cy, flags, who);
    }
    return g_realSetWindowPos ? g_realSetWindowPos(h, after, x, y, cx, cy, flags) : FALSE;
}
static LRESULT WINAPI Hook_SendMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == 0x282) {
        LONG n = InterlockedIncrement(&g_logCount);
        if (g_logOn && n <= 400) {
            char cls[80]; ClassOf(h, cls, sizeof(cls));
            char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
            Log("SendMessage(%p [%s], WM_IME_NOTIFY, wp=%u) from %s", h, cls, (unsigned)wp, who);
        }
    }
    return g_realSendMessageW ? g_realSendMessageW(h, m, wp, lp) : 0;
}
static BOOL WINAPI Hook_PostMessageW(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == 0x282) {
        LONG n = InterlockedIncrement(&g_logCount);
        if (g_logOn && n <= 400) {
            char cls[80]; ClassOf(h, cls, sizeof(cls));
            char who[128]; CallerTagOf(_ReturnAddress(), who, sizeof(who));
            Log("PostMessage(%p [%s], WM_IME_NOTIFY, wp=%u) from %s", h, cls, (unsigned)wp, who);
        }
    }
    return g_realPostMessageW ? g_realPostMessageW(h, m, wp, lp) : FALSE;
}

static void PatchApiIAT(const wchar_t* modName, const wchar_t* dll, const char* fn, void* hook)
{
    HMODULE m = GetModuleHandleW(modName);
    if (!m) return;
    HMODULE d = GetModuleHandleW(dll);
    if (!d) return;
    void* real = (void*)GetProcAddress(d, fn);
    if (!real) return;
    int n = PatchIAT(m, real, hook);
    if (n) Log("PatchApiIAT: %ws!%s -> %d slot(s)", modName, fn, n);
}
static int PatchIAT(HMODULE mod, void* target, void* repl)
{
    if (!mod || !target || !repl) return 0;
    __try {
        auto dos = (IMAGE_DOS_HEADER*)mod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva) return 0;
        int n = 0;
        auto imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + rva);
        for (; imp->Name; ++imp) {
            auto iat = (void**)((BYTE*)mod + imp->FirstThunk);
            if (!iat) continue;
            for (; *iat; ++iat) {
                if (*iat == target) {
                    DWORD old = 0;
                    if (VirtualProtect(iat, sizeof(void*), PAGE_READWRITE, &old)) {
                        *iat = repl;
                        VirtualProtect(iat, sizeof(void*), old, &old);
                        ++n;
                    }
                }
            }
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void PatchOne(const wchar_t* name)
{
    HMODULE m = GetModuleHandleW(name);
    if (!m) { Log("PatchOne: %ws not loaded", name); return; }
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    void* real = u32 ? (void*)GetProcAddress(u32, "GetGUIThreadInfo") : nullptr;
    if (!real) { Log("PatchOne: no GetGUIThreadInfo"); return; }
    int n = PatchIAT(m, real, (void*)&Hook_GetGUIThreadInfo);
    Log("PatchOne: %ws base=%p -> %d IAT slot(s) patched", name, m, n);
}

// ★ 核心修复：外壳 sub_18000AF70 会把引擎请求的"打开候选窗"消息吞掉。
//   在 WT 里它直接 return 1（= 不转发），引擎的 UI 窗口收不到 IMN_OPENCANDIDATE，
//   于是引擎永远不 ShowWindow 它的候选窗。
//   把该函数最后的过滤条件改成"永远放行"：
//     原: F6 83 9C 3B 00 00 40   test byte ptr [rbx+3B9Ch],40h
//     新: EB 1B                  jmp 0xAFDA (xor eax,eax; add rsp,20h; pop rbx; retn)
//   函数入口的 642 / wParam 3..5 判断，以及 wParam==4 时的 GetFocus+PostMessage 副作用都保留。
static void PatchCandFilter()
{
    HMODULE m = GetModuleHandleW(L"QQPinyinTSF.dll");
    if (!m) { Log("PatchCandFilter: shell not loaded"); return; }
    BYTE* p = (BYTE*)m + 0xAFBD;
    if (p[0] != 0xF6 || p[1] != 0x83 || p[2] != 0x9C || p[3] != 0x3B ||
        p[4] != 0x00 || p[5] != 0x00 || p[6] != 0x40) {
        Log("PatchCandFilter: UNEXPECTED %02X %02X %02X %02X %02X %02X %02X",
            p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) {
        Log("PatchCandFilter: VirtualProtect failed %lu", GetLastError());
        return;
    }
    p[0] = 0xEB; p[1] = 0x1B;
    VirtualProtect(p, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
    Log("PatchCandFilter: APPLIED at %p (EB 1B)", p);
}

static DWORD WINAPI EngineWatcher(LPVOID)
{
    for (int i = 0; i < 3600; ++i) {
        if (GetModuleHandleW(L"QQPinyin.ime")) { PatchOne(L"QQPinyin.ime"); PatchApiIAT(L"QQPinyin.ime", L"user32.dll", "SendMessageW", (void*)&Hook_SendMessageW); PatchApiIAT(L"QQPinyin.ime", L"user32.dll", "PostMessageW", (void*)&Hook_PostMessageW); PatchApiIAT(L"QQPinyin.ime", L"user32.dll", "ShowWindow", (void*)&Hook_ShowWindow); PatchApiIAT(L"QQPinyin.ime", L"user32.dll", "SetWindowPos", (void*)&Hook_SetWindowPos); break; }
        Sleep(500);
    }
    return 0;
}

// ---------------------------------------------------------------- forwarding
static HMODULE EnsureReal()
{
    if (g_real) return g_real;
    LoadIni();
    char me[MAX_PATH] = {0};
    HostName(me, sizeof(me));
    g_real = LoadLibraryW(g_realPath);
    Log("EnsureReal: host=%s match=%d mode=%ld log=%ld real=%p path=%ws",
        me, (int)HostMatches(), g_mode, g_logOn, g_real, g_realPath);
    if (!g_real) return nullptr;

    if (HostMatches()) {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32) {
            g_realGGTI        = (PFN_GGTI)GetProcAddress(u32, "GetGUIThreadInfo");
            g_realShowWindow  = (PFN_ShowWindow)GetProcAddress(u32, "ShowWindow");
            g_realSetWindowPos= (PFN_SetWindowPos)GetProcAddress(u32, "SetWindowPos");
            g_realSendMessageW= (PFN_SendMessageW)GetProcAddress(u32, "SendMessageW");
            g_realPostMessageW= (PFN_PostMessageW)GetProcAddress(u32, "PostMessageW");
        }
        PatchOne(L"QQPinyinTSF.dll");
        PatchCandFilter();
        TsfInit(TsfLog);
        PatchApiIAT(L"QQPinyinTSF.dll", L"user32.dll", "SendMessageW", (void*)&Hook_SendMessageW);
        PatchApiIAT(L"QQPinyinTSF.dll", L"user32.dll", "PostMessageW", (void*)&Hook_PostMessageW);
        HANDLE t = CreateThread(nullptr, 0, EngineWatcher, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return g_real;
}

static void* RealProc(const char* name)
{
    HMODULE m = EnsureReal();
    return m ? (void*)GetProcAddress(m, name) : nullptr;
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    typedef HRESULT (__stdcall *PFN)(REFCLSID, REFIID, void**);
    PFN f = (PFN)RealProc("DllGetClassObject");
    return f ? f(rclsid, riid, ppv) : (HRESULT)0x80040111L;
}
extern "C" __declspec(dllexport) HRESULT __stdcall DllCanUnloadNow()
{
    typedef HRESULT (__stdcall *PFN)();
    PFN f = (PFN)RealProc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
extern "C" __declspec(dllexport) HRESULT __stdcall DllRegisterServer()
{
    typedef HRESULT (__stdcall *PFN)();
    PFN f = (PFN)RealProc("DllRegisterServer");
    return f ? f() : S_OK;
}
extern "C" __declspec(dllexport) HRESULT __stdcall DllUnregisterServer()
{
    typedef HRESULT (__stdcall *PFN)();
    PFN f = (PFN)RealProc("DllUnregisterServer");
    return f ? f() : S_OK;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)&DllMain, &self);
        // DllMain 里不做文件 IO / LoadLibrary
    }
    return TRUE;
}







