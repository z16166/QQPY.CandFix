// qqpyutil.cpp —— RAII 工具与路径解析的实现
//
// 路径来源（一律不硬编码盘符）：
//   DllDirectory()       GetModuleFileNameW(本模块)
//   ConfigDir()          DllDirectory()，退回 SHGetKnownFolderPath(FOLDERID_ProgramData)，
//                        再退回环境变量 ProgramData / ALLUSERSPROFILE
//   DefaultRealDllPath() GetSystemDirectoryW()     <- 32 位进程会得到 SysWOW64，正好对应 32 位 IME
//
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <atlbase.h>
#include <string>
#include "qqpyutil.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// ------------------------------------------------------------------ 常量
constexpr const wchar_t* kSubDirName         = L"QQPYCandFix";                          // 退路目录名
constexpr const wchar_t* kIniFileName        = L"qqpyproxy.ini";
constexpr const wchar_t* kLogFileName        = L"qqpyproxy.log";
constexpr const wchar_t* kImeRelativePath    = L"\\IME\\QQPinyinTSF\\QQPinyinTSF.dll";  // 相对 GetSystemDirectoryW()
constexpr const wchar_t* kSystem32Relative   = L"\\system32";                            // 仅当 GetSystemDirectoryW 失败时用
constexpr const wchar_t* kEnvProgramData     = L"ProgramData";
constexpr const wchar_t* kEnvAllUsersProfile = L"ALLUSERSPROFILE";
constexpr const wchar_t* kEnvSystemRoot      = L"SystemRoot";

namespace {

// 取本 DLL 的模块句柄：用本文件里的函数地址反查，永远正确，无需全局变量
HMODULE SelfModule() noexcept
{
    HMODULE m = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&SelfModule), &m);
    return m;
}

void TrimTrailingSeparators(std::wstring& s)
{
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/'))
        s.pop_back();
}

// 环境变量退路
std::wstring EnvDir(const wchar_t* name)
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(name, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return std::wstring();
    std::wstring s(buf, n);
    TrimTrailingSeparators(s);
    return s;
}

} // namespace

namespace qqpy {

std::wstring DllDirectory()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(SelfModule(), buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return std::wstring();

    std::wstring path(buf, n);
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return std::wstring();
    return path.substr(0, pos);
}

std::wstring ConfigDir()
{
    // 1) 部署时 ini / log 与 DLL 同目录 —— 最稳，且不需要任何系统 API
    if (std::wstring dir = DllDirectory(); !dir.empty())
        return dir;

    // 2) shell folder API（RAII 管理 CoTaskMemAlloc 出来的缓冲）
    PWSTR programData = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &programData)) && programData)
    {
        ATL::CComHeapPtr<WCHAR> hold(programData);   // 析构即 CoTaskMemFree
        return std::wstring(programData) + L"\\" + kSubDirName;
    }

    // 3) 环境变量
    if (std::wstring dir = EnvDir(kEnvProgramData); !dir.empty())
        return dir + L"\\" + kSubDirName;
    if (std::wstring dir = EnvDir(kEnvAllUsersProfile); !dir.empty())
        return dir + L"\\" + kSubDirName;

    return std::wstring();
}

std::wstring IniPath()
{
    const std::wstring dir = ConfigDir();
    return dir.empty() ? std::wstring() : dir + L"\\" + kIniFileName;
}

std::wstring LogPath()
{
    const std::wstring dir = ConfigDir();
    return dir.empty() ? std::wstring() : dir + L"\\" + kLogFileName;
}

std::wstring DefaultRealDllPath()
{
    wchar_t sys[MAX_PATH] = {};
    UINT n = ::GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring dir;
    if (n > 0 && n < MAX_PATH)
    {
        dir.assign(sys, n);
    }
    else
    {
        // 退路：环境变量拼 system32
        const std::wstring root = EnvDir(kEnvSystemRoot);
        if (root.empty())
            return std::wstring();
        dir = root + kSystem32Relative;
    }
    TrimTrailingSeparators(dir);
    return dir + kImeRelativePath;
}

} // namespace qqpy
