// Temporary diagnostic overrides of recompiled functions (XenonRecomp emits sub_* as weak
// aliases of __imp__sub_*). Remove once the bug is understood.
#include <runtime.h>

static bool HeapListsValid(uint32_t heap, uint32_t* badIndex)
{
    for (uint32_t i = 0; i < 128; i++)
    {
        uint32_t head = heap + 0x180 + i * 8;
        if (!GuestLoad32(head) || !GuestLoad32(head + 4))
        {
            *badIndex = i;
            return false;
        }
    }
    return true;
}

// HeapAlloc-like allocator (r3 heap, r4 flags, r5 size).
PPC_FUNC_IMPL(__imp__sub_821E6FB0);
PPC_FUNC(sub_821E6FB0)
{
    static int n;
    static bool reported;
    uint32_t heap = ctx.r3.u32, size = ctx.r5.u32, lr = uint32_t(ctx.lr), bad = 0;
    n++;
    bool before = HeapListsValid(heap, &bad);
    __imp__sub_821E6FB0(ctx, base);
    uint32_t result = ctx.r3.u32;
    if (!reported && !HeapListsValid(heap, &bad))
    {
        reported = true;
        RuntimeLog("[heap] lists invalid after alloc #%d (valid before: %d) heap 0x%08X size 0x%X lr 0x%08X -> 0x%08X, head %u = 0x%08X/0x%08X\n",
            n, before, heap, size, lr, result, bad, GuestLoad32(heap + 0x180 + bad * 8), GuestLoad32(heap + 0x184 + bad * 8));
    }
}
