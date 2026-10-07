// Enhancement hooks: small register/memory patches applied at fixed instructions of the
// game, used for the widescreen modes. The addresses must match HOOKS in gen/recomp.py.
//
// Widescreen follows the CTR-ModSDK 16BY9 mod: CTR renders 512x216, which the game's
// projection assumes fills a 4:3 screen. Scaling the X row of the view-projection matrix by
// k = (4/3) / aspect fits a wider field of view into the same buffer (shown stretched to the
// target aspect), the culling frustum is widened by 1/k to match, and the far-clip distance
// used for culling is extended as the wider frustum reaches less far (x2 at 16:9, the mod's
// value, scaled with the width beyond that). 16:9 gives k = 750/1000 exactly as in the mod.
#include <algorithm>
#include <cmath>

#include "recomp.h"
#include "psx.h"

int g_hooks_on = 0;
uint32_t g_cyc_scale = 256;
static int32_t k_x1000 = 1000;    // projection X scale, 1/1000 units
static int32_t far_x1000 = 1000;  // far-clip multiplier, 1/1000 units
static double cur_aspect = 4.0 / 3.0;

namespace psx {

void set_aspect(double aspect) {
    aspect = std::clamp(aspect, 4.0 / 3.0, 2.4);  // 4:3 .. 21:9
    cur_aspect = aspect;
    k_x1000 = (int32_t)std::lround(1000.0 * (4.0 / 3.0) / aspect);
    far_x1000 = (int32_t)std::lround(2000.0 * 750.0 / k_x1000);
    g_hooks_on = k_x1000 < 1000;
}
double aspect() { return cur_aspect; }
bool widescreen() { return g_hooks_on != 0; }

// CPU overclock: a wider view makes the game draw more, which overruns the PS1's frame budget
// and slows the game down (on real hardware too). Running the CPU faster relative to the rest
// of the hardware keeps the game at full speed.
static int overclock_pct = 100;
void set_overclock(int percent) {
    overclock_pct = std::clamp(percent, 100, 400);
    g_cyc_scale = (uint32_t)(256 * 100 / overclock_pct);
}
int overclock() { return overclock_pct; }

}  // namespace psx

enum : uint32_t {
    HOOK_SETMATRIXVP_END = 0x80042E34,  // PushBuffer_SetMatrixVP, before epilogue; s0 = PushBuffer*
    HOOK_FRUSTUM_WIDTH = 0x80043170,    // PushBuffer_UpdateFrustum, a0 = rect.w just loaded
    HOOK_FRUSTUM_FARCLIP = 0x80043280,  // PushBuffer_UpdateFrustum, t0..t2 = view dir << 8
};

static int32_t scale_k(int32_t v) { return (int32_t)((int64_t)v * k_x1000 / 1000); }

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
            MEM_SH(pb + off, (uint32_t)scale_k((int16_t)MEM_LH(pb + off)));
        MEM_SW(pb + 0x3C, (uint32_t)scale_k((int32_t)MEM_LW(pb + 0x3C)));
        break;
    }
    case HOOK_FRUSTUM_WIDTH:
        c->r[4] = (uint32_t)((int16_t)c->r[4] * 1000 / k_x1000) & 0xFFFF;
        break;
    case HOOK_FRUSTUM_FARCLIP:
        for (int r = 8; r <= 10; r++) c->r[r] = (uint32_t)((int64_t)(int32_t)c->r[r] * far_x1000 / 1000);
        break;
    }
}
