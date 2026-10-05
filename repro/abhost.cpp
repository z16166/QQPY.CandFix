// 回归测试宿主：刻意命名为 WindowsTerminal.exe，让代理的 hosts 匹配生效，
// 从而完整走一遍"WT 专属路径"：EnsureReal -> IAT 补丁 -> PatchCandFilter -> TsfInit
// -> 100ms 定时器 -> TsfRequest -> 创建只读编辑会话。
//
// 用法: WindowsTerminal.exe <qqpyproxy.dll 的完整路径>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* dll = (argc > 1) ? argv[1] : L"qqpyproxy.dll";

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HMODULE m = ::LoadLibraryW(dll);
    if (!m)
    {
        wprintf(L"[host] LoadLibrary failed, err=%lu\n", ::GetLastError());
        return 2;
    }
    wprintf(L"[host] loaded %ls at %p\n", dll, m);

    typedef HRESULT(__stdcall * PFN)(REFCLSID, REFIID, void**);
    PFN f = (PFN)::GetProcAddress(m, "DllGetClassObject");
    if (!f)
    {
        wprintf(L"[host] DllGetClassObject not found\n");
        return 3;
    }

    const GUID clsid = {0xAE51F1C0, 0x807F, 0x4A64, {0xAC, 0x55, 0xF2, 0xAD, 0xF9, 0x2E, 0x26, 0x03}};
    void* cf = nullptr;
    const HRESULT hr = f(clsid, IID_IClassFactory, &cf);
    wprintf(L"[host] DllGetClassObject -> 0x%08X cf=%p\n", (unsigned)hr, cf);

    // 泵一会儿消息，让 SetTimer(100ms) -> TsfRequest 真正跑起来
    const DWORD until = ::GetTickCount() + 1500;
    MSG msg;
    while (::GetTickCount() < until)
    {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(50);
    }
    wprintf(L"[host] timer/TSF path survived 1.5s\n");

    if (cf) static_cast<IUnknown*>(cf)->Release();
    return 0;
}
