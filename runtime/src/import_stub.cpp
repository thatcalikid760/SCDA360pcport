// Fallback for unimplemented imports: logs the call, returns 0.
#include <mutex>
#include <unordered_map>
#include <runtime.h>

void ImportStub(const char* name, PPCContext& ctx)
{
    // Log the first 16 calls per import, then every 4096th.
    static std::mutex m;
    static std::unordered_map<const char*, uint32_t> calls;
    uint32_t n;
    {
        std::lock_guard lock(m);
        n = ++calls[name];
    }
    // Log before clobbering r3: it is the first argument.
    if (n <= 16 || !(n & 0xFFF))
        RuntimeLog("[stub #%u] %s(0x%08X, 0x%08X, 0x%08X, 0x%08X) lr=0x%08X\n",
            n, name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
    ctx.r3.u64 = 0;
}
