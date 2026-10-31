// Extra Settings inside the game's own OPTIONS page (docs/re_options_menu.md). The page's TV CALIBRATION
// button (B_Adjust_display) becomes EXTRA SETTINGS: its label is rewritten where the localizer hands
// it out, and a click on it opens the F1 menu instead of the calibration page (BRIGHTNESS and CONTRAST
// stay on the OPTIONS page itself). SCDA_NATIVE_CALIBRATION=1 keeps the game's own button.
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <runtime.h>
#include "overlay.h"

extern "C" PPC_FUNC(__imp__sub_8258C230); // Localize(section, key, package, ...) -> UTF-16BE temp buffer
extern "C" PPC_FUNC(__imp__sub_82A88E60); // options pages' click handler (this, event)
extern "C" PPC_FUNC(__imp__sub_82786B48); // menu: is the current page <name> (ASCII)
extern "C" PPC_FUNC(__imp__sub_82A80E50); // event: came from the widget <name> (ASCII), 0 = no

namespace
{
constexpr uint32_t kPageOptions = 0x8212D2C4;      // "P_Options"
constexpr uint32_t kButtonCalibration = 0x8212D34C; // "B_Adjust_display"

bool Enabled()
{
    static const bool on = getenv("SCDA_NATIVE_CALIBRATION") == nullptr;
    return on;
}

// Guest UTF-16BE string at addr equals text (ASCII).
bool WideEquals(uint8_t* base, uint32_t addr, const char* text)
{
    if (!addr)
        return false;
    for (size_t i = 0;; i++)
    {
        const uint16_t c = PPC_LOAD_U16(addr + 2 * uint32_t(i));
        if (c != uint16_t(uint8_t(text[i])))
            return false;
        if (!c)
            return true;
    }
}
} // namespace

PPC_FUNC(sub_8258C230)
{
    const uint32_t key = ctx.r4.u32;
    __imp__sub_8258C230(ctx, base);
    // Same length (14), written over the localizer's temporary buffer: other languages keep their text
    // (the button still opens Extra Settings).
    const uint32_t text = ctx.r3.u32;
    if (Enabled() && WideEquals(base, key, "Calibrate") && WideEquals(base, text, "TV CALIBRATION"))
    {
        const char* label = "EXTRA SETTINGS";
        for (uint32_t i = 0; label[i]; i++)
            PPC_STORE_U16(text + 2 * i, uint16_t(label[i]));
    }
}

PPC_FUNC(sub_82A88E60)
{
    if (Enabled())
    {
        const uint32_t self = ctx.r3.u32, event = ctx.r4.u32;
        PPCContext saved = ctx;
        ctx.r3.u64 = PPC_LOAD_U32(self + 16);
        ctx.r4.u64 = kPageOptions;
        __imp__sub_82786B48(ctx, base);
        bool calibration = (ctx.r3.u32 & 0xFF) != 0;
        if (calibration)
        {
            ctx = saved;
            ctx.r3.u64 = event;
            ctx.r4.u64 = kButtonCalibration;
            __imp__sub_82A80E50(ctx, base);
            calibration = ctx.r3.s32 != 0;
        }
        ctx = saved;
        if (calibration)
        {
            RuntimeLog("[menu] OPTIONS > EXTRA SETTINGS\n");
            OverlayRequestOpen();
            PPC_STORE_U8(PPC_LOAD_U32(self + 16) + 24, 0); // as the handler's own exits
            ctx.r3.u64 = 0;
            return;
        }
    }
    __imp__sub_82A88E60(ctx, base);
}
