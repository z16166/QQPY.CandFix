// qqpyutil.h —— 公共 RAII 工具与路径解析
//
// 注意：本头文件刻意【不】引入 <objbase.h> / ATL。
// 因为 qqpyproxy.cpp 定义了 DllGetClassObject / DllCanUnloadNow，
// 而 combaseapi.h 也声明了它们（不同 linkage），同一个编译单元里会 C2375 冲突。
// 所以：导出所在的编译单元保持"干净"，COM/ATL 相关代码放在别的编译单元里。
//
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

namespace qqpy {

// 临界区自旋次数：稍大一点可减少上下文切换
constexpr DWORD kCriticalSectionSpinCount = 4000;

// ------------------------------------------------------------------ 内核句柄
// RAII 包装：文件句柄、线程句柄等。不可拷贝、可移动。
// 析构即 CloseHandle；INVALID_HANDLE_VALUE 视为"空"。
class UniqueHandle
{
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : m_h(h) {}

    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : m_h(other.m_h) { other.m_h = nullptr; }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) { reset(); m_h = other.m_h; other.m_h = nullptr; }
        return *this;
    }

    HANDLE get() const noexcept { return m_h; }
    bool valid() const noexcept { return m_h != nullptr && m_h != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return valid(); }

    HANDLE release() noexcept { HANDLE h = m_h; m_h = nullptr; return h; }

    void reset(HANDLE h = nullptr) noexcept
    {
        if (valid()) CloseHandle(m_h);
        m_h = h;
    }

private:
    HANDLE m_h = nullptr;
};

// ------------------------------------------------------------------ 临界区
class CriticalSection
{
public:
    CriticalSection() noexcept { InitializeCriticalSectionAndSpinCount(&m_cs, kCriticalSectionSpinCount); }
    ~CriticalSection() { DeleteCriticalSection(&m_cs); }

    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;

    void Enter() noexcept { EnterCriticalSection(&m_cs); }
    void Leave() noexcept { LeaveCriticalSection(&m_cs); }

private:
    CRITICAL_SECTION m_cs;
};

// 作用域锁
class CsLock
{
public:
    explicit CsLock(CriticalSection& cs) noexcept : m_cs(cs) { m_cs.Enter(); }
    ~CsLock() { m_cs.Leave(); }

    CsLock(const CsLock&) = delete;
    CsLock& operator=(const CsLock&) = delete;

private:
    CriticalSection& m_cs;
};

// ------------------------------------------------------------------ 线程定时器
class UniqueTimer
{
public:
    UniqueTimer() noexcept = default;
    ~UniqueTimer() { reset(); }

    UniqueTimer(const UniqueTimer&) = delete;
    UniqueTimer& operator=(const UniqueTimer&) = delete;

    bool start(UINT elapseMs, TIMERPROC proc) noexcept
    {
        reset();
        m_id = SetTimer(nullptr, 0, elapseMs, proc);
        return m_id != 0;
    }

    void reset() noexcept
    {
        if (m_id)
        {
            KillTimer(nullptr, m_id);
            m_id = 0;
        }
    }

private:
    UINT_PTR m_id = 0;
};

// ------------------------------------------------------------------ 路径
// 全部通过 API 或环境变量取得，不硬编码任何盘符目录。
std::wstring DllDirectory();        // 本 DLL 所在目录（GetModuleFileNameW）
std::wstring ConfigDir();           // ini / log 所在目录（优先 DLL 目录，退回 shell folder API）
std::wstring IniPath();             // <ConfigDir>\qqpyproxy.ini
std::wstring LogPath();             // <ConfigDir>\qqpyproxy.log
std::wstring DefaultRealDllPath();  // <SystemDir>\IME\QQPinyinTSF\QQPinyinTSF.dll（GetSystemDirectoryW）

} // namespace qqpy
