// qqpytsf.cpp —— 向 TSF 索取宿主的真实光标矩形
//
// 独立编译单元：<objbase.h> / ATL 会和本 DLL 的 DllGetClassObject / DllCanUnloadNow
// 声明冲突（C2375 different linkage），所以 COM 相关的代码全部集中在这里，
// 导出所在的 qqpyproxy.cpp 保持"干净"。
//
// 背景：WT 实现了 ITfContextOwner::GetTextExt
//   src/tsf/Implementation.cpp:315  ->  *prc = _provider->GetCursorPosition();
//   src/cascadia/TerminalControl/TermControl.cpp:203
// 它就是"终端光标所在字符格的屏幕矩形"。QQ拼音外壳只问 Win32 插入符（WT 没有），
// 所以这里替它去问 TSF。
//
// 资源管理：COM 接口指针一律用 ATL 智能指针（CComPtr / CComQIPtr），
// 自定义 COM 对象用 CComObjectRootEx + CComObject（不手写 AddRef/Release），
// 定时器用 qqpy::UniqueTimer（RAII）。
//
// 取矩形只用【异步只读编辑会话】：RequestEditSession(..., TF_ES_READ, ...) 不阻塞
// UI 线程，没有死锁风险；外加 100ms 定时器持续刷新，光标一动缓存就更新。
//
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <msctf.h>
#include <atlbase.h>
#include <atlcom.h>
#include <stdarg.h>
#include <string>
#include "qqpyutil.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")

using ATL::CComPtr;
using ATL::CComQIPtr;

// ------------------------------------------------------------------ 常量
constexpr DWORD  kExtentRequestMinIntervalMs = 80;    // 异步会话请求限流
constexpr UINT   kExtentRefreshIntervalMs    = 100;   // 定时器刷新间隔
constexpr DWORD  kExtentMaxAgeMs             = 3000;  // 光标矩形的新鲜度上限
constexpr LONG   kFirstNSessionLogs          = 6;     // 前 N 次会话写日志
constexpr LONG   kLogEveryNSessions          = 500;   // 之后每 N 次写一条
constexpr LONG   kFirstNRequestLogs          = 6;     // 前 N 次请求写日志
constexpr size_t kLogLineMax                 = 512;
constexpr ULONG  kSelectionCount             = 1;  // 只要"当前选区"这一项

namespace
{

CComPtr<ITfThreadMgr> g_ptm;          // 本线程的 TSF 线程管理器（RAII）
TfClientId            g_tid     = 0;  // 本线程的 TSF client id
RECT                  g_rc      = {0, 0, 0, 0};
volatile LONG         g_rcValid = 0;
volatile LONG         g_rcTick  = 0;
volatile LONG         g_lastReq = 0;
volatile LONG         g_req     = 0;
volatile LONG         g_sess    = 0;
void (*g_log)(const char*)      = nullptr;
qqpy::UniqueTimer g_timer;  // 定时器（RAII）

void Lf(const char* fmt, ...)
{
    if (!g_log) return;
    char    buf[kLogLineMax];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    g_log(buf);
}

void L(const char* s)
{
    if (g_log) g_log(s);
}

// 只读编辑会话：在 TSF 给的 edit cookie 下问宿主"光标在哪一格"
class CExtentSession : public CComObjectRootEx<CComSingleThreadModel>, public ITfEditSession
{
public:
    BEGIN_COM_MAP(CExtentSession)
    COM_INTERFACE_ENTRY(ITfEditSession)
    END_COM_MAP()

    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie ec) override
    {
        CComPtr<ITfDocumentMgr> pdim;
        CComPtr<ITfContext>     pic;
        CComPtr<ITfContextView> pview;
        CComPtr<ITfRange>       range;

        HRESULT    h1 = E_FAIL, h2 = E_FAIL, h3 = E_FAIL, h4 = E_FAIL, h5 = E_FAIL;
        RECT       rc      = {0, 0, 0, 0};
        BOOL       clipped = FALSE;
        const LONG n       = InterlockedIncrement(&g_sess);

        if (g_ptm) h1 = g_ptm->GetFocus(&pdim);
        if (SUCCEEDED(h1) && pdim) h2 = pdim->GetTop(&pic);
        if (SUCCEEDED(h2) && pic)
        {
            h3 = pic->GetActiveView(&pview);
            if (SUCCEEDED(h3) && pview)
            {
                // GetTextExt 的 pRange 不能为 NULL，先取当前选区；失败则退到文档起点
                TF_SELECTION sel  = {};
                ULONG        nsel = 0;
                h5                = pic->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &nsel);
                if (SUCCEEDED(h5) && nsel == 1 && sel.range)
                    range.Attach(sel.range);  // 接管 GetSelection 给出的引用，RAII 释放
                else if (SUCCEEDED(pic->GetStart(ec, &range)) && range)
                    h5 = S_OK;

                if (range) h4 = pview->GetTextExt(ec, range, &rc, &clipped);
            }
        }

        if (n <= kFirstNSessionLogs || (n % kLogEveryNSessions) == 0)
            Lf("TsfSession#%ld GetFocus=%08X GetTop=%08X GetView=%08X GetRange=%08X GetTextExt=%08X rc=(%d,%d-%d,%d) "
               "clipped=%d",
               n, (unsigned)h1, (unsigned)h2, (unsigned)h3, (unsigned)h5, (unsigned)h4, rc.left, rc.top, rc.right,
               rc.bottom, (int)clipped);

        if (SUCCEEDED(h4) && rc.right > rc.left && rc.bottom > rc.top)
        {
            g_rc     = rc;
            g_rcTick = (LONG)GetTickCount();
            InterlockedExchange(&g_rcValid, 1);
        }
        return S_OK;
    }
};

void TsfRequest()
{
    if (!g_ptm) return;
    const DWORD now  = GetTickCount();
    LONG        prev = g_lastReq;
    if (now - (DWORD)prev < kExtentRequestMinIntervalMs) return;  // 限流
    if (InterlockedCompareExchange(&g_lastReq, (LONG)now, prev) != prev) return;

    CComPtr<ITfDocumentMgr> pdim;
    if (FAILED(g_ptm->GetFocus(&pdim)) || !pdim) return;

    CComPtr<ITfContext> pic;
    if (FAILED(pdim->GetTop(&pic)) || !pic) return;

    CComObject<CExtentSession>* raw = nullptr;
    if (FAILED(CComObject<CExtentSession>::CreateInstance(&raw)) || !raw) return;

    // CreateInstance 出来的对象引用计数为 0；下面的智能指针会 QI 并 AddRef。
    // COM_MAP 里声明了 ITfEditSession，QI 必然成功。
    CComQIPtr<ITfEditSession> session(raw);
    if (!session) return;

    HRESULT       phr  = S_OK;
    const HRESULT hreq = pic->RequestEditSession(g_tid, session, TF_ES_READ, &phr);  // 异步只读
    const LONG    k    = InterlockedIncrement(&g_req);
    if (k <= kFirstNRequestLogs)
        Lf("TsfRequest#%ld RequestEditSession h=%08X phr=%08X", k, (unsigned)hreq, (unsigned)phr);
}

VOID CALLBACK TsfTimer(HWND, UINT, UINT_PTR, DWORD)
{ TsfRequest(); }

}  // namespace

extern "C" void TsfInit(void (*logfn)(const char* msg))
{
    g_log = logfn;
    if (g_ptm) return;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // 宿主已初始化过，这里只是引用计数 +1
    if (FAILED(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_ptm))) || !g_ptm)
    {
        L("TsfInit: CoCreateInstance(CLSID_TF_ThreadMgr) FAILED");
        g_ptm.Release();
        return;
    }

    // ITfThreadMgr 没有 CreateClientId；Activate 返回本线程的 client id
    TfClientId    tid = 0;
    const HRESULT ha  = g_ptm->Activate(&tid);
    if (SUCCEEDED(ha) && tid)
    {
        g_tid = tid;
        Lf("TsfInit: ok tid=%u h=%08X", (unsigned)tid, (unsigned)ha);
    }
    else
    {
        Lf("TsfInit: Activate h=%08X tid=%u FAILED", (unsigned)ha, (unsigned)tid);
    }

    if (g_timer.start(kExtentRefreshIntervalMs, TsfTimer))  // RAII：定时刷新
        TsfRequest();
    else
        L("TsfInit: SetTimer FAILED");
}

// TRUE = 有 3 秒内的新鲜光标矩形（屏幕坐标）
extern "C" BOOL TsfGetCaret(RECT* prc)
{
    TsfRequest();
    if (!prc || !g_rcValid) return FALSE;
    const DWORD age = GetTickCount() - (DWORD)g_rcTick;
    if (age > kExtentMaxAgeMs) return FALSE;
    const RECT rc = g_rc;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return FALSE;
    *prc = rc;
    return TRUE;
}
