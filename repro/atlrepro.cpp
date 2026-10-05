// 隔离实验：证明 ATL 的 CComObject<>::CreateInstance() 在没有 ATL 模块对象时崩溃。
//   cl ... /DWITH_ATL_MODULE   -> 提供 CAtlModule 派生对象（构造函数会设置 _pAtlModule）
//   cl ...                     -> 不提供
// 期望：不提供时 CreateInstance 内部 _pAtlModule->Lock() 解引用 NULL，0xC0000005。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <msctf.h>
#include <atlbase.h>
#include <atlcom.h>
#include <stdio.h>

using ATL::CComObject;
using ATL::CComObjectRootEx;
using ATL::CComSingleThreadModel;

class CSession : public CComObjectRootEx<CComSingleThreadModel>, public ITfEditSession
{
public:
    BEGIN_COM_MAP(CSession)
    COM_INTERFACE_ENTRY(ITfEditSession)
    END_COM_MAP()

    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie) override { return S_OK; }
};

#ifdef WITH_ATL_MODULE
// 最小 ATL 模块对象：CAtlModule 的构造函数会把自己赋给 _pAtlModule
class CMyModule : public ATL::CAtlModule
{
};
CMyModule g_module;
#endif

int wmain()
{
#ifdef WITH_ATL_MODULE
    wprintf(L"[test] WITH_ATL_MODULE=1  _pAtlModule=%p\n", (void*)ATL::_pAtlModule);
#else
    wprintf(L"[test] WITH_ATL_MODULE=0  _pAtlModule=%p\n", (void*)ATL::_pAtlModule);
#endif

    CComObject<CSession>* raw = nullptr;
    const HRESULT hr = CComObject<CSession>::CreateInstance(&raw);
    wprintf(L"[test] CreateInstance hr=0x%08X raw=%p\n", (unsigned)hr, raw);
    if (raw) raw->Release();
    wprintf(L"[test] survived\n");
    return 0;
}
