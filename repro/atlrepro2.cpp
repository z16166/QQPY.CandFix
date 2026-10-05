// 隔离实验 2/3
//  2) 提供 ATL 模块对象后，CComObject<>::CreateInstance 不再崩 -> 证明崩溃机制
//  3) 验证修复所用的"手写 IUnknown + CComQIPtr 持有"的引用计数逻辑正确、无泄漏
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <msctf.h>
#include <atlbase.h>
#include <atlcom.h>
#include <new>
#include <stdio.h>

using ATL::CComObject;
using ATL::CComObjectRootEx;
using ATL::CComQIPtr;
using ATL::CComSingleThreadModel;

// ---------------------------------------------------------------- 实验 2
class CSession : public CComObjectRootEx<CComSingleThreadModel>, public ITfEditSession
{
public:
    BEGIN_COM_MAP(CSession)
    COM_INTERFACE_ENTRY(ITfEditSession)
    END_COM_MAP()

    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie) override { return S_OK; }
};

// CAtlModule 本身是抽象类，要用 CAtlModuleT 派生；其构造函数会设置 _pAtlModule
class CMyModule : public ATL::CAtlModuleT<CMyModule>
{
public:
    static HRESULT WINAPI InitInstance() { return S_OK; }
    static void WINAPI ExitInstance() {}
};
CMyModule g_module;

// ---------------------------------------------------------------- 实验 3
// 与修复后的 qqpytsf.cpp 里 CExtentSession 完全同构：引用计数从 0 开始，由首个 AddRef 接管
static volatile LONG g_live = 0;

class CHandSession : public ITfEditSession
{
public:
    CHandSession() noexcept : m_ref(0) { InterlockedIncrement(&g_live); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession))
        {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie) override { return S_OK; }

private:
    ~CHandSession() { InterlockedDecrement(&g_live); }
    volatile LONG m_ref;
};

int wmain()
{
    wprintf(L"[2] _pAtlModule=%p (有模块对象)\n", (void*)ATL::_pAtlModule);
    CComObject<CSession>* raw = nullptr;
    const HRESULT hr = CComObject<CSession>::CreateInstance(&raw);
    wprintf(L"[2] CComObject<CSession>::CreateInstance hr=0x%08X raw=%p\n", (unsigned)hr, raw);
    if (raw) raw->Release();

    wprintf(L"[3] hand-written session: live=%ld\n", (long)g_live);
    {
        CComQIPtr<ITfEditSession> session(new (std::nothrow) CHandSession());  // 0 -> QI(AddRef) -> 1
        wprintf(L"[3] after create: live=%ld, session=%p\n", (long)g_live, (void*)session.p);
        if (session)
        {
            session->AddRef();   // 模拟 msftf 排队期间持有一份引用 -> 2
            session->Release();  // 归还 -> 1
            wprintf(L"[3] after mscft-like add/release: live=%ld\n", (long)g_live);
        }
    }  // 作用域结束 -> Release -> 0 -> delete
    wprintf(L"[3] after scope: live=%ld (应为 0)\n", (long)g_live);
    wprintf(L"[test] survived\n");
    return 0;
}
