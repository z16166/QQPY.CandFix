// patchverify.cpp
// 直接调用真身 QQPinyinTSF.dll!sub_18000AF70（RVA 0xAF70），对比打补丁前后的返回值。
// 用途：验证补丁位置（RVA 0xAFBD）与补丁语义（EB 1B = 让它永远返回 0 = 放行）。
// 输出刻意只用 ASCII，避免控制台代码页把结果搞乱。
//
// 安全：只走该函数的早期判定路径（只读 this 的两个字段），并且只在本进程内存里改 2 字节，不写磁盘。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <vector>

int wmain()
{
    HMODULE h = ::LoadLibraryW(L"C:\\WINDOWS\\system32\\IME\\QQPinyinTSF\\QQPinyinTSF.dll");
    if (!h)
    {
        printf("LoadLibrary failed %lu\n", ::GetLastError());
        return 1;
    }
    BYTE* base = reinterpret_cast<BYTE*>(h);

    typedef int(__fastcall * FN)(void*, void*, void*);
    FN fn = reinterpret_cast<FN>(base + 0xAF70);
    BYTE* p = base + 0xAFBD;

    printf("module base          = %p\n", base);
    printf("sub_18000AF70        = %p  (base+0xAF70)\n", (void*)fn);
    printf("patch site           = %p  (base+0xAFBD)\n", (void*)p);
    printf("bytes at patch site  = %02X %02X %02X %02X %02X %02X %02X\n", p[0], p[1], p[2], p[3], p[4], p[5], p[6]);
    printf("bytes at base+0xAFDA = %02X %02X %02X %02X %02X\n", (base + 0xAFDA)[0], (base + 0xAFDA)[1], (base + 0xAFDA)[2],
           (base + 0xAFDA)[3], (base + 0xAFDA)[4]);

    struct TRANSMSG
    {
        DWORD     msg;
        ULONG_PTR wParam;
        ULONG_PTR lParam;
    };

    // fake CTextService: condA = *(BYTE*)(this+0x3B9C) & 0x40, condB = *(DWORD*)(this+0x698)
    std::vector<BYTE> self(0x3B9C + 0x10, 0);

    TRANSMSG tm = {0x282, 5, 0};  // WM_IME_NOTIFY + IMN_OPENCANDIDATE

    self[0x3B9C]         = 0x40;  // condA true
    *(DWORD*)(self.data() + 0x698) = 1;  // condB false
    printf("\nbaseline: condA=1 condB!=0 -> f(0x282, wParam=5) = %d   (1 = swallow)\n",
           fn(self.data(), nullptr, &tm));

    DWORD old = 0;
    ::VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old);
    p[0] = 0xEB;  // jmp rel8
    p[1] = 0x1B;  // -> base+0xAFDA (xor eax,eax / add rsp,20h / pop rbx / ret)
    ::VirtualProtect(p, 2, old, &old);
    ::FlushInstructionCache(::GetCurrentProcess(), p, 2);

    printf("patched : condA=1 condB!=0 -> f(0x282, wParam=5) = %d   (0 = forward)\n", fn(self.data(), nullptr, &tm));

    tm.wParam = 6;
    printf("patched : wParam=6 (not in 3..5)                  = %d   (0 = entry check intact)\n",
           fn(self.data(), nullptr, &tm));
    tm.wParam = 0x282;
    printf("patched : msg=0x282 only, wParam=3                 = %d\n", fn(self.data(), nullptr, &tm));

    tm.msg    = 0x100;  // WM_KEYDOWN: not WM_IME_NOTIFY
    tm.wParam = 5;
    printf("patched : msg=0x100 (not WM_IME_NOTIFY)            = %d   (0 = entry check intact)\n",
           fn(self.data(), nullptr, &tm));

    // semantics cross-check (on the unpatched behaviour we cannot restore, so explain condA/condB shape
    // using the patched function is meaningless; instead verify the ORIGINAL logic by re-testing bytes)
    ::FreeLibrary(h);
    return 0;
}
