// Enhancement hooks: small register/memory patches applied at fixed instructions of the
// game, used for the widescreen mode. The addresses must match HOOKS in gen/recomp.py.
//
// Widescreen follows the CTR-ModSDK 16BY9 mod: CTR renders 512x216, which the game's
// projection assumes fills a 4:3 screen. Scaling the X row of the view-projection matrix
// by 3/4 fits a 16:9 field of view into the same buffer (shown stretched to 16:9), the
// culling frustum is widened by 4/3 to match, and the far-clip distance used for culling
// is doubled as the wider frustum reaches less far.
#include "recomp.h"
#include "psx.h"

int g_hooks_on = 0;

namespace psx {

void set_widescreen(bool on) { g_hooks_on = on; }
bool widescreen() { return g_hooks_on != 0; }

}  // namespace psx

enum : uint32_t {
    HOOK_SETMATRIXVP_END = 0x80042E34,  // PushBuffer_SetMatrixVP, before epilogue; s0 = PushBuffer*
    HOOK_FRUSTUM_WIDTH = 0x80043170,    // PushBuffer_UpdateFrustum, a0 = rect.w just loaded
    HOOK_FRUSTUM_FARCLIP = 0x80043280,  // PushBuffer_UpdateFrustum, t0..t2 = view dir << 8
};

static int32_t wide34(int32_t v) { return (int32_t)((int64_t)v * 750 / 1000); }

int rt_is_hook(uint32_t pc) {
    switch (pc) {
    case HOOK_SETMATRIXVP_END:
    case HOOK_FRUSTUM_WIDTH:
    case HOOK_FRUSTUM_FARCLIP: return 1;
    default: return 0;
    }
}

void rt_hook(CPU* c, uint32_t pc) {
    if (!g_hooks_on) return;
    switch (pc) {
    case HOOK_SETMATRIXVP_END: {
        // matrix_ViewProj at +0x28 (short m[3][3]), t[3] at +0x3C: scale row 0 and t[0]
        uint32_t pb = c->r[16];
        for (uint32_t off = 0x28; off <= 0x2C; off += 2)
            MEM_SH(pb + off, (uint32_t)wide34((int16_t)MEM_LH(pb + off)));
        MEM_SW(pb + 0x3C, (uint32_t)wide34((int32_t)MEM_LW(pb + 0x3C)));
        break;
    }
    case HOOK_FRUSTUM_WIDTH:
        c->r[4] = (uint32_t)((int16_t)c->r[4] * 1000 / 750) & 0xFFFF;
        break;
    case HOOK_FRUSTUM_FARCLIP:
        c->r[8] <<= 1;
        c->r[9] <<= 1;
        c->r[10] <<= 1;
        break;
    }
}
