// Statically linked XAudio's XMA hardware access. It reads the context array address from
// register 0x7FEA1800 (sub_823FF580, at init) and kicks / locks its contexts with one bit per
// context in registers 0x7FEA1940 (sub_82400418) / 0x7FEA1A40 (sub_82400150), both loops over
// every context of its voice set (r3).
// shortcut: hooks on those functions instead of trapping the register writes.
#include <runtime.h>

void XmaEnsureArray();
void XmaKickVoiceSet(uint32_t set);
void XmaLockVoiceSet(uint32_t set);

extern "C" PPC_FUNC(__imp__sub_823FF580);
PPC_FUNC(sub_823FF580)
{
    XmaEnsureArray();
    __imp__sub_823FF580(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82400418);
PPC_FUNC(sub_82400418)
{
    const uint32_t set = ctx.r3.u32;
    __imp__sub_82400418(ctx, base);
    XmaKickVoiceSet(set);
}

// Locks only while neither flag 0x10000 (locked) nor 0x20000 is set.
extern "C" PPC_FUNC(__imp__sub_82400150);
PPC_FUNC(sub_82400150)
{
    const uint32_t set = ctx.r3.u32;
    const bool locks = !(GuestLoad32(set + 4) & 0x30000);
    __imp__sub_82400150(ctx, base);
    if (locks)
        XmaLockVoiceSet(set);
}
