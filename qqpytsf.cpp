// qqpytsf.cpp —— 向 TSF 索取宿主的真实光标矩形
//
// 独立编译单元：<objbase.h>/<msctf.h> 会和本 DLL 的 DllGetClassObject/DllCanUnloadNow
// 声明冲突（C2375 different linkage），所以隔离开。
//
// 背景：WT 实现了 ITfContextOwner::GetTextExt
//   src/tsf/Implementation.cpp:315  ->  *prc = _provider->GetCursorPosition();
//   src/cascadia/TerminalControl/TermControl.cpp:203
// 它就是"终端光标所在字符格的屏幕矩形"。QQ拼音外壳只问 Win32 插入符（WT 没有），
// 所以这里替它去问 TSF。
//
// 只用【异步只读编辑会话】取矩形：RequestEditSession(..., TF_ES_READ, ...) 不阻塞
// UI 线程，没有死锁风险；外加一个 100ms 线程定时器持续刷新，光标一动缓存就更新。
//
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <msctf.h>
#include <stdarg.h>
#include <new>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")

static ITfThreadMgr* g_ptm = nullptr;
static TfClientId    g_tid = 0;
static RECT          g_rc = {0, 0, 0, 0};
static volatile LONG g_rcValid = 0;
static volatile LONG g_rcTick = 0;
static volatile LONG g_lastReq = 0;
static volatile LONG g_req = 0;
static volatile LONG g_sess = 0;
static void        (*g_log)(const char*) = nullptr;

static void Lf(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    g_log(buf);
}
static void L(const char* s) { if (g_log) g_log(s); }

class CExtentSession : public ITfEditSession
{
    volatile LONG m_ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
            *ppv = static_cast<ITfEditSession*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        ITfDocumentMgr* pdim = nullptr;
        ITfContext* pic = nullptr;
        ITfContextView* pview = nullptr;
        ITfRange* prange = nullptr;
        ULONG nsel = 0;
        HRESULT h1 = E_FAIL, h2 = E_FAIL, h3 = E_FAIL, h4 = E_FAIL, h5 = E_FAIL;
        RECT rc = {0, 0, 0, 0};
        BOOL clipped = FALSE;
        LONG n = InterlockedIncrement(&g_sess);

        if (g_ptm) h1 = g_ptm->GetFocus(&pdim);
        if (SUCCEEDED(h1) && pdim) h2 = pdim->GetTop(&pic);
        if (SUCCEEDED(h2) && pic) {
            h3 = pic->GetActiveView(&pview);
            if (SUCCEEDED(h3) && pview) {
                // GetTextExt 的 pRange 不能为 NULL，先取当前选区；失败则退到文档起点
                TF_SELECTION sel = {};
                h5 = pic->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &nsel);
                if (SUCCEEDED(h5) && nsel == 1 && sel.range) {
                    h4 = pview->GetTextExt(ec, sel.range, &rc, &clipped);
                    sel.range->Release();
                } else if (SUCCEEDED(pic->GetStart(ec, &prange)) && prange) {
                    h4 = pview->GetTextExt(ec, prange, &rc, &clipped);
                    prange->Release();
                }
            }
        }
        if (n <= 6 || (n % 500) == 0)
            Lf("TsfSession#%ld GetFocus=%08X GetTop=%08X GetView=%08X GetRange=%08X GetTextExt=%08X rc=(%d,%d-%d,%d) clipped=%d",
               n, (unsigned)h1, (unsigned)h2, (unsigned)h3, (unsigned)h5, (unsigned)h4,
               rc.left, rc.top, rc.right, rc.bottom, (int)clipped);

        if (SUCCEEDED(h4) && rc.right > rc.left && rc.bottom > rc.top) {
            g_rc = rc;
            g_rcTick = (LONG)GetTickCount();
            InterlockedExchange(&g_rcValid, 1);
        }
        if (pview) pview->Release();
        if (pic) pic->Release();
        if (pdim) pdim->Release();
        return S_OK;
    }
};

static void TsfRequest()
{
    if (!g_ptm || !g_tid) return;
    DWORD now = GetTickCount();
    LONG prev = g_lastReq;
    if (now - (DWORD)prev < 80) return;                       // 限流
    if (InterlockedCompareExchange(&g_lastReq, (LONG)now, prev) != prev) return;

    ITfDocumentMgr* pdim = nullptr;
    if (FAILED(g_ptm->GetFocus(&pdim)) || !pdim) return;

    ITfContext* pic = nullptr;
    if (SUCCEEDED(pdim->GetTop(&pic)) && pic) {
        CExtentSession* s = new (std::nothrow) CExtentSession();
        if (s) {
            HRESULT phr = S_OK;
            HRESULT hreq = pic->RequestEditSession(g_tid, s, TF_ES_READ, &phr);
            LONG k = InterlockedIncrement(&g_req);
            if (k <= 6) Lf("TsfRequest#%ld RequestEditSession h=%08X phr=%08X", k, (unsigned)hreq, (unsigned)phr);
            s->Release();
        }
        pic->Release();
    }
    pdim->Release();
}

static VOID CALLBACK TsfTimer(HWND, UINT, UINT_PTR, DWORD) { TsfRequest(); }

extern "C" void TsfInit(void (*logfn)(const char* msg))
{
    g_log = logfn;
    if (g_ptm) return;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // 宿主已初始化，这里只是 +1
    ITfThreadMgr* ptm = nullptr;
    HRESULT h = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_ITfThreadMgr, (void**)&ptm);
    if (FAILED(h) || !ptm) { Lf("TsfInit: CoCreateInstance h=%08X FAILED", (unsigned)h); return; }
    g_ptm = ptm;

    // ITfThreadMgr 没有 CreateClientId；Activate 返回本线程的 client id
    TfClientId tid = 0;
    HRESULT ha = g_ptm->Activate(&tid);
    if (SUCCEEDED(ha) && tid) {
        g_tid = tid;
        Lf("TsfInit: ok tid=%u h=%08X", (unsigned)tid, (unsigned)ha);
    } else {
        Lf("TsfInit: Activate h=%08X tid=%u FAILED", (unsigned)ha, (unsigned)tid);
    }

    SetTimer(nullptr, 0, 100, TsfTimer);   // 线程定时器，持续刷新光标矩形
    TsfRequest();
}

// TRUE = 有 3 秒内的新鲜光标矩形（屏幕坐标）
extern "C" BOOL TsfGetCaret(RECT* prc)
{
    TsfRequest();
    if (!prc || !g_rcValid) return FALSE;
    DWORD age = GetTickCount() - (DWORD)g_rcTick;
    if (age > 3000) return FALSE;
    RECT rc = g_rc;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return FALSE;
    *prc = rc;
    return TRUE;
}
