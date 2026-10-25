// OpenGL ES 3.0 renderer: draws the render jobs of gpu.cpp on the graphics chip.
//
// VRAM is a texture at the internal resolution (1024x512 times 1, 2, 4 or 8). Every PS1
// pixel is stored as RGBA8 with the 5-bit channels in the top bits (c << 3) and the mask bit
// in alpha, so the four PS1 blend modes are exact with fixed-function blending (b+f, b-f and
// b+f/4 give integers; (b+f)/2 leaves a remainder below 8 that the next read drops, as the
// PS1's truncation does). Shaders decode texture pages, CLUTs and texture windows with the
// same integer arithmetic as the software rasterizer and do the colour modulation and the
// dithering exactly; coverage and interpolation follow the same top-left rule and sample
// points, so the result is close to, but not bit-identical with, the software renderer.
//
// The texture is stored upside down (VRAM row y at texture row H-1-y) so that GL's top-left
// fill rule, defined for its y-up window space, matches the PS1's in VRAM space.
//
// Drawing samples a second copy of VRAM (sampling the texture being drawn into is undefined
// in GL); areas drawn since the last refresh are tracked and copied over before a primitive
// reads them as a texture.
//
// Everything here runs on one thread that owns the GL context and consumes the job queue.
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gl.h"
#include "gpu_jobs.h"

namespace psx {

#define GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

bool gl_load_functions() {
    bool ok = true;
#define GL_LOAD(ret, name, args)                                       \
    name = (PFN_##name)SDL_GL_GetProcAddress(#name);                   \
    if (!name) { LOGE("GL: missing function %s", #name); ok = false; }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return ok;
}

SDL_Window* g_gl_window = nullptr;
SDL_GLContext g_gl_context = nullptr;
int g_gl_shift = 0;  // log2 of the internal resolution scale
uint16_t* g_gl_mirror = nullptr;
std::atomic<uint64_t> g_gl_presents_done{0};

static int gsh = 0, S = 1, VW = 1024, VH = 512;
static GLuint tex_draw = 0, tex_read = 0, tex_stage = 0;
// Texture reads (texture pages, CLUTs) only ever look at native pixels, so primitives sample a
// native-resolution copy in 16-bit integers: no decoding in the shader, and at 2x and above a
// texture's texels are next to each other instead of spread over the scaled texture.
static GLuint tex_nat = 0, fbo_nat = 0, prog_pack = 0;
static struct { GLint src, shift, flip; } upk;
static void pack_native(int x0, int y0, int x1, int y1);
static GLuint fbo_draw = 0, fbo_read = 0;
static GLuint vao = 0, vbo = 0;
static GLuint prog_draw = 0, prog_copy = 0, prog_present = 0;
static struct { GLint size, vram, shift, pass, flip; } ud;
static struct { GLint size, src, dst0, src0, div, mask, flip, src_h, src_flip; } uc;
static struct { GLint vram, shift, origin, rgb24, flip, disp, cheap; } up;
int g_gl_debug = 0;  // --gl-debug bits (experiments): 1 no mask pass, 2 cheap present, 4 draw nothing,
                     // 8 no dual-source blending, 16 no framebuffer fetch

// ---- shaders ----------------------------------------------------------------------------------
static const char* kHeader = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
// Draw program variants, best first (see flush()): framebuffer fetch does all blending and the
// mask check in the shader (mobile GPUs); dual-source blending folds the mask bit into the
// colour pass; otherwise a colour pass plus a mask pass.
static const char* kHeaderFetchExt =
    "#version 300 es\n#extension GL_EXT_shader_framebuffer_fetch : require\n#define FETCH 1\n#define FETCH_EXT 1\n"
    "precision highp float;\nprecision highp int;\n";
static const char* kHeaderFetchArm =
    "#version 300 es\n#extension GL_ARM_shader_framebuffer_fetch : require\n#define FETCH 1\n#define FETCH_ARM 1\n"
    "precision highp float;\nprecision highp int;\n";
static const char* kHeaderDual =
    "#version 300 es\n#extension GL_EXT_blend_func_extended : require\n#define DUAL 1\nprecision highp float;\nprecision highp int;\n";
static bool g_dual = false, g_fetch = false;

static const char* kDrawVS = R"(
layout(location = 0) in vec2 a_pos;    // internal-resolution VRAM coordinates
layout(location = 1) in vec4 a_col;    // 0..255
layout(location = 2) in vec2 a_uv;     // texel coordinates
layout(location = 3) in uvec2 a_mode;  // x: flags / texpage / CLUT, y: texture window
uniform vec2 u_size;
out vec3 v_col;
out vec2 v_uv;
flat out uvec2 v_mode;
void main() {
    gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0);
    v_col = a_col.rgb;
    v_uv = a_uv;
    v_mode = a_mode;
}
)";

static const char* kDrawFS = R"(
uniform highp usampler2D u_nat;  // native-resolution 16-bit copy of VRAM (texture reads)
uniform int u_shift;        // log2(scale)
uniform int u_pass;         // 0 all texels, 1 opaque texels only, 2 semi-transparent texels only,
                            // 3 single-pass blending (colour), 4 mask bits (alpha)
uniform int u_flip;         // texture rows - 1 (VRAM row y is at texture row u_flip - y)
in vec3 v_col;
in vec2 v_uv;
flat in uvec2 v_mode;
#if defined(FETCH_EXT)
layout(location = 0) inout vec4 o;  // framebuffer fetch: o holds the destination on entry
#elif defined(FETCH_ARM)
layout(location = 0) out vec4 o;
#elif defined(DUAL)
layout(location = 0, index = 0) out vec4 o;
layout(location = 0, index = 1) out vec4 o1;  // background factor (dual-source blending)
#else
layout(location = 0) out vec4 o;
#endif
const int dither_tab[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);
uint fetch(int x, int y) { return texelFetch(u_nat, ivec2(x & 1023, y & 511), 0).r; }
void main() {
#ifdef DUAL
    o1 = vec4(0.0);
#endif
#if defined(FETCH_EXT)
    vec4 dst = o;
#elif defined(FETCH_ARM)
    vec4 dst = gl_LastFragColorARM;
#endif
    uint m = v_mode.x;
    bool textured = (m & 1u) != 0u, raw = (m & 2u) != 0u, dith = (m & 4u) != 0u, semi = (m & 8u) != 0u;
    bool mask_set = (m & 16u) != 0u, rect = (m & 0x8000000u) != 0u;
#ifdef FETCH
    if ((m & 0x40000000u) != 0u && dst.a > 0.5) discard;  // mask check: keep masked pixels
#endif
    ivec2 ip = ivec2(int(gl_FragCoord.x), u_flip - int(gl_FragCoord.y));  // internal VRAM pixel
    ivec2 np = ip >> u_shift;
    int dt = dither_tab[(np.y & 3) * 4 + (np.x & 3)];
    ivec3 c = clamp(ivec3(floor(v_col + 0.5)), 0, 255);
    ivec3 rgb;
    bool mask = false, semi_out = semi;  // semi_out: this pixel is blended
    if (textured) {
        vec2 uvf = rect ? floor(v_uv) : floor(v_uv + 0.5);
        int u = clamp(int(uvf.x), rect ? -100000 : 0, rect ? 100000 : 255) & 255;
        int v = clamp(int(uvf.y), rect ? -100000 : 0, rect ? 100000 : 255) & 255;
        uint tw = v_mode.y;
        int twmx = int(tw & 31u), twmy = int((tw >> 5) & 31u), twox = int((tw >> 10) & 31u), twoy = int((tw >> 15) & 31u);
        u = (u & ~(twmx * 8)) | ((twox & twmx) * 8);
        v = (v & ~(twmy * 8)) | ((twoy & twmy) * 8);
        int depth = int((m >> 5) & 3u);
        int tpx = int((m >> 7) & 15u) * 64, tpy = int((m >> 11) & 1u) * 256;
        int clx = int((m >> 12) & 63u) * 16, cly = int((m >> 18) & 511u);
        uint t;
        if (depth == 0) {
            uint w = fetch(tpx + (u >> 2), tpy + v);
            t = fetch(clx + int((w >> uint((u & 3) * 4)) & 15u), cly);
        } else if (depth == 1) {
            uint w = fetch(tpx + (u >> 1), tpy + v);
            t = fetch(clx + int((w >> uint((u & 1) * 8)) & 255u), cly);
        } else {
            t = fetch(tpx + u, tpy + v);
        }
        if (t == 0u) discard;
        bool semi_px = semi && (t & 0x8000u) != 0u;
        if ((u_pass == 1 && semi_px) || (u_pass == 2 && !semi_px)) discard;
        semi_out = semi_px;
        ivec3 tc = ivec3(int(t & 31u), int((t >> 5) & 31u), int((t >> 10) & 31u));
        if (!raw) {
            if (dith) tc = clamp((((tc << 3) * c) >> 7) + dt, 0, 255) >> 3;
            else tc = min(ivec3(31), (tc * c) >> 7);
        }
        rgb = tc;
        mask = (t & 0x8000u) != 0u;
    } else {
        if (dith) c = clamp(c + dt, 0, 255);
        rgb = c >> 3;
    }
#ifdef FETCH
    // all blending here, with the software renderer's integer arithmetic
    if (semi_out) {
        ivec3 b = ivec3(dst.rgb * 255.0 + 0.5) >> 3;
        int mode = int((m >> 28) & 3u);
        if (mode == 0) rgb = (b + rgb) >> 1;
        else if (mode == 1) rgb = min(ivec3(31), b + rgb);
        else if (mode == 2) rgb = max(ivec3(0), b - rgb);
        else rgb = min(ivec3(31), b + (rgb >> 2));
    }
    o = vec4(vec3(rgb << 3) / 255.0, (mask || mask_set) ? 1.0 : 0.0);
    return;
#endif
    vec3 col = vec3(rgb << 3) / 255.0;
    if (u_pass == 3) {
        // single-pass blending (B_MAIN): result = src * k + dst * a
        int mode = int((m >> 28) & 3u);
        float k = 1.0, a = 0.0;
        if (semi_out) {
            if (mode == 0) { k = 0.5; a = 0.5; }       // (b + f) / 2
            else if (mode == 1) { a = 1.0; }          // b + f
            else { k = 0.25; a = 1.0; }               // b + f / 4 (mode 2 never comes here)
        }
#ifdef DUAL
        o = vec4(col * k, (mask || mask_set) ? 1.0 : 0.0);  // the mask bit in the same pass
        o1 = vec4(a);
#else
        o = vec4(col * k, a);
#endif
        return;
    }
    o = vec4(col, (mask || mask_set) ? 1.0 : 0.0);  // pass 4 writes only this alpha (the mask bit)
}
)";

// fills the native 16-bit copy from the drawn VRAM (top-left sample of each native pixel)
static const char* kPackVS = R"(
layout(location = 0) in vec2 a_pos;  // native pixels
void main() { gl_Position = vec4(a_pos / vec2(1024.0, 512.0) * 2.0 - 1.0, 0.0, 1.0); }
)";
static const char* kPackFS = R"(
uniform sampler2D u_src;
uniform int u_shift;
uniform int u_flip;
layout(location = 0) out uint o;
void main() {
    ivec2 n = ivec2(gl_FragCoord.xy);  // native pixel; row y of the copy is VRAM row y
    vec4 c = texelFetch(u_src, ivec2(n.x << u_shift, u_flip - (n.y << u_shift)), 0);
    uvec4 k = uvec4(c * 255.0 + 0.5);
    o = (k.r >> 3) | ((k.g >> 3) << 5) | ((k.b >> 3) << 10) | (k.a > 127u ? 0x8000u : 0u);
}
)";

// copies a rectangle (VRAM copies from the read copy, uploads from the staging texture)
static const char* kCopyVS = R"(
layout(location = 0) in vec2 a_pos;
uniform vec2 u_size;
void main() { gl_Position = vec4(a_pos.x / u_size.x * 2.0 - 1.0, 1.0 - a_pos.y / u_size.y * 2.0, 0.0, 1.0); }
)";
static const char* kCopyFS = R"(
uniform sampler2D u_src;
uniform ivec2 u_dst0;     // destination origin (internal VRAM pixels)
uniform ivec2 u_src0;     // source origin (source texels)
uniform int u_div;        // source texel = (destination offset) >> u_div
uniform int u_mask;       // set the mask bit
uniform int u_flip;       // destination texture rows - 1
uniform int u_src_flip;   // 1: the source is stored upside down too (u_src_h rows)
uniform int u_src_h;
layout(location = 0) out vec4 o;
void main() {
    ivec2 p = ivec2(int(gl_FragCoord.x), u_flip - int(gl_FragCoord.y)) - u_dst0;
    ivec2 s = u_src0 + (p >> u_div);
    if (u_src_flip != 0) s.y = u_src_h - 1 - s.y;
    vec4 c = texelFetch(u_src, s, 0);
    o = vec4(c.rgb, u_mask != 0 ? 1.0 : c.a);
}
)";

// shows the display area: 15-bit with bilinear filtering of the decoded colours, or 24-bit
static const char* kPresentVS = R"(
layout(location = 0) in vec2 a_pos;  // NDC
layout(location = 2) in vec2 a_uv;   // display coordinates (native pixels, y down)
out vec2 v_uv;
void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }
)";
static const char* kPresentFS = R"(
uniform sampler2D u_vram;
uniform int u_shift;
uniform ivec2 u_origin;  // display origin (native)
uniform ivec2 u_disp;    // display size (native)
uniform int u_rgb24;
uniform int u_cheap;  // experiment: nearest sampling, one fetch
uniform int u_flip;
in vec2 v_uv;
layout(location = 0) out vec4 o;
vec3 px15(ivec2 ip) {  // internal-resolution pixel, display relative
    ivec2 lim = (u_disp << u_shift) - 1;
    ip = clamp(ip, ivec2(0), lim) + (u_origin << u_shift);
    vec4 c = texelFetch(u_vram, ivec2(ip.x & ((1024 << u_shift) - 1), u_flip - (ip.y & ((512 << u_shift) - 1))), 0);
    ivec3 k = ivec3(c.rgb * 255.0 + 0.5) >> 3;
    return vec3((k << 3) | (k >> 2)) / 255.0;
}
uint word(int x, int y) {
    vec4 c = texelFetch(u_vram, ivec2((x & 1023) << u_shift, u_flip - ((y & 511) << u_shift)), 0);
    uvec4 k = uvec4(c * 255.0 + 0.5);
    return (k.r >> 3) | ((k.g >> 3) << 5) | ((k.b >> 3) << 10) | (k.a > 127u ? 0x8000u : 0u);
}
uint byte_at(int bx, int y) {
    uint w = word((bx >> 1) & 1023, y);
    return (bx & 1) != 0 ? (w >> 8) : (w & 255u);
}
void main() {
    if (u_rgb24 != 0) {
        ivec2 n = ivec2(floor(v_uv));
        int y = u_origin.y + n.y;
        int bx = (u_origin.x * 2 + n.x * 3) % 2048;
        uint r = byte_at(bx, y), g = byte_at((bx + 1) % 2048, y), b = byte_at((bx + 2) % 2048, y);
        o = vec4(vec3(r, g, b) / 255.0, 1.0);
        return;
    }
    if (u_cheap != 0) { o = vec4(px15(ivec2(v_uv * float(1 << u_shift))), 1.0); return; }
    vec2 f = v_uv * float(1 << u_shift) - 0.5;
    ivec2 i0 = ivec2(floor(f));
    vec2 t = f - floor(f);
    vec3 a = mix(px15(i0), px15(i0 + ivec2(1, 0)), t.x);
    vec3 b = mix(px15(i0 + ivec2(0, 1)), px15(i0 + ivec2(1, 1)), t.x);
    o = vec4(mix(a, b, t.y), 1.0);
}
)";

static GLuint compile(GLenum type, const char* src, const char* header = kHeader) {
    GLuint s = glCreateShader(type);
    const char* parts[2] = {header, src};
    glShaderSource(s, 2, parts, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGE("GL: shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link(const char* vs, const char* fs, const char* fs_header = kHeader) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs, fs_header);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        LOGE("GL: program link failed: %s", log);
        return 0;
    }
    return p;
}

// ---- VRAM textures ------------------------------------------------------------------------
static GLuint make_tex(int w, int h, GLenum fmt = GL_RGBA8) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, fmt, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
static GLuint make_fbo(GLuint tex) {
    GLuint f;
    glGenFramebuffers(1, &f);
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOGE("GL: framebuffer incomplete");
    return f;
}

// (Re)create the VRAM textures at scale 1 << shift, keeping the contents (nearest scaling).
static void set_scale(int shift) {
    GLuint old_draw = tex_draw, old_read = tex_read, old_fdraw = fbo_draw, old_fread = fbo_read;
    int ow = VW, oh = VH;
    gsh = shift;
    S = 1 << gsh;
    VW = 1024 << gsh;
    VH = 512 << gsh;
    tex_draw = make_tex(VW, VH);
    tex_read = make_tex(VW, VH);
    fbo_draw = make_fbo(tex_draw);
    fbo_read = make_fbo(tex_read);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_read);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
    glClear(GL_COLOR_BUFFER_BIT);
    if (old_draw) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, old_fdraw);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_draw);
        glBlitFramebuffer(0, 0, ow, oh, 0, 0, VW, VH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_read);
        glBlitFramebuffer(0, 0, ow, oh, 0, 0, VW, VH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDeleteFramebuffers(1, &old_fdraw);
        glDeleteFramebuffers(1, &old_fread);
        glDeleteTextures(1, &old_draw);
        glDeleteTextures(1, &old_read);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
    pack_native(0, 0, 1024, 512);  // the native copy follows the new contents
    LOGI("GL: VRAM %dx%d", VW, VH);
}

// ---- dirty tracking (areas drawn since the read copy was refreshed) -------------------------
// 16x8 native-pixel tiles, as in gpu.cpp's hazard tracking
static uint64_t dirty_bits[64];
static bool dirty_any = false;
static int dirty_x0 = 1024, dirty_y0 = 512, dirty_x1 = 0, dirty_y1 = 0;

static void dirty_mark(Rgn r) {
    r.x0 = std::max(r.x0, 0); r.y0 = std::max(r.y0, 0);
    r.x1 = std::min(r.x1, 1024); r.y1 = std::min(r.y1, 512);
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
    for (int ty = r.y0 >> 3; ty <= (r.y1 - 1) >> 3; ty++)
        for (int tx = r.x0 >> 4; tx <= (r.x1 - 1) >> 4; tx++) dirty_bits[ty] |= 1ull << tx;
    dirty_any = true;
    dirty_x0 = std::min(dirty_x0, r.x0 & ~15); dirty_y0 = std::min(dirty_y0, r.y0 & ~7);
    dirty_x1 = std::max(dirty_x1, ((r.x1 - 1) | 15) + 1); dirty_y1 = std::max(dirty_y1, ((r.y1 - 1) | 7) + 1);
}
// marks a possibly wrapping area
static void dirty_mark_wrap(int x, int y, int w, int h) {
    for (int oy = 0; oy < 2; oy++)
        for (int ox = 0; ox < 2; ox++) dirty_mark(Rgn{x - ox * 1024, y - oy * 512, x + w - ox * 1024, y + h - oy * 512});
}
static bool dirty_test(Rgn r) {
    if (!dirty_any) return false;
    if (r.x1 - r.x0 >= 1024) { r.x0 = 0; r.x1 = 1024; }
    if (r.y1 - r.y0 >= 512) { r.y0 = 0; r.y1 = 512; }
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return false;
    for (int ty = r.y0 >> 3; ty <= (r.y1 - 1) >> 3; ty++)
        for (int tx = r.x0 >> 4; tx <= (r.x1 - 1) >> 4; tx++)
            if (dirty_bits[ty & 63] & (1ull << (tx & 63))) return true;
    return false;
}

static uint64_t g_read_syncs = 0;
static void flush();
static void bind_vertices(const void* data, size_t bytes);

// refresh the native 16-bit copy for native pixels [x0, x1) x [y0, y1) from the drawn VRAM
static void pack_native(int x0, int y0, int x1, int y1) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_nat);
    glViewport(0, 0, 1024, 512);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(prog_pack);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_draw);
    glUniform1i(upk.src, 0);
    glUniform1i(upk.shift, gsh);
    glUniform1i(upk.flip, VH - 1);
    float q[12] = {(float)x0, (float)y0, (float)x1, (float)y0, (float)x0, (float)y1,
                   (float)x1, (float)y0, (float)x1, (float)y1, (float)x0, (float)y1};
    struct P { float x, y, pad[5]; } v[6];  // the draw batches' vertex layout (position first)
    for (int i = 0; i < 6; i++) { v[i] = P{}; v[i].x = q[2 * i]; v[i].y = q[2 * i + 1]; }
    bind_vertices(v, sizeof v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
}
// bring the read copy up to date with everything drawn so far
static void sync_read() {
    if (!dirty_any) return;
    flush();
    g_read_syncs++;
    int x0 = dirty_x0 << gsh, x1 = dirty_x1 << gsh;
    int ty0 = VH - (dirty_y1 << gsh), ty1 = VH - (dirty_y0 << gsh);  // texture rows (upside down)
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_draw);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_read);
    glBlitFramebuffer(x0, ty0, x1, ty1, x0, ty0, x1, ty1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    pack_native(dirty_x0, dirty_y0, dirty_x1, dirty_y1);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
    memset(dirty_bits, 0, sizeof dirty_bits);
    dirty_any = false;
    dirty_x0 = 1024; dirty_y0 = 512; dirty_x1 = 0; dirty_y1 = 0;
}

// ---- primitive batches ------------------------------------------------------------------------
struct GV {
    float x, y;
    uint8_t r, g, b, a;
    float u, v;
    uint32_t mode, tw;
};
enum BatchKind { B_NONE, B_OPAQUE, B_SEMI, B_SEMI_TEX, B_MAIN };  // B_SEMI*: blend mode 2 (b - f) only
static std::vector<GV> batch;
static BatchKind batch_kind = B_NONE;
static int batch_semi_mode = 0;
static int sc_x0 = 0, sc_y0 = 0, sc_x1 = 0, sc_y1 = 0;  // batch scissor, internal VRAM pixels
static std::vector<Rgn> batch_boxes;                   // B_SEMI_TEX: areas of the batch's primitives
static uint64_t g_batches = 0, g_prims = 0, g_mask_check = 0;
static uint64_t g_break_why[3];  // new batch because of: kind / blend mode, drawing area, overlap (semi tex)
static uint64_t g_busy_ns = 0;   // time spent on jobs (profiling)

static void set_blend(int mode) {
    glEnable(GL_BLEND);
    switch (mode) {
    case 0:  // (b + f) / 2
        glBlendColor(0, 0, 0, 0.5f);
        glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        break;
    case 1:  // b + f
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        break;
    case 2:  // b - f
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
        break;
    default:  // b + f / 4
        glBlendColor(0, 0, 0, 0.25f);
        glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        break;
    }
}

static void bind_vertices(const void* data, size_t bytes) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, data, GL_STREAM_DRAW);
}

static void flush() {
    if (batch.empty()) return;
    g_batches++;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
    glViewport(0, 0, VW, VH);
    glEnable(GL_SCISSOR_TEST);
    glScissor(sc_x0, VH - sc_y1, sc_x1 - sc_x0, sc_y1 - sc_y0);
    glUseProgram(prog_draw);
    glUniform2f(ud.size, (float)VW, (float)VH);
    glUniform1i(ud.shift, gsh);
    glUniform1i(ud.flip, VH - 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_nat);
    glUniform1i(ud.vram, 0);
    bind_vertices(batch.data(), batch.size() * sizeof(GV));
    GLsizei n = (GLsizei)batch.size();
    if (g_gl_debug & 4) n = 0;  // experiment: draw nothing
    switch (batch_kind) {
    case B_OPAQUE:
        glDisable(GL_BLEND);
        glUniform1i(ud.pass, 0);
        glDrawArrays(GL_TRIANGLES, 0, n);
        break;
    case B_MAIN:
        // one ordered pass for opaque pixels and blend modes 0, 1, 3: the shader scales the
        // colour (x1/2, x1, x1/4) and outputs the background factor (1/2 or 1, 0 when opaque)
        // in alpha, result = src + dst * alpha; then the mask bits, in the same order
        if (g_fetch) {
            // framebuffer fetch: the shader reads the destination and blends itself
            glDisable(GL_BLEND);
            glDrawArrays(GL_TRIANGLES, 0, n);
            break;
        }
        glEnable(GL_BLEND);
        if (g_dual) {
            // the background factor comes from the second output, alpha is the mask bit
            glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_ZERO);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glUniform1i(ud.pass, 3);
            glDrawArrays(GL_TRIANGLES, 0, n);
            glDisable(GL_BLEND);
            break;
        }
        glBlendFuncSeparate(GL_ONE, GL_SRC_ALPHA, GL_ZERO, GL_ONE);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glColorMask(1, 1, 1, 0);
        glUniform1i(ud.pass, 3);
        glDrawArrays(GL_TRIANGLES, 0, n);
        glDisable(GL_BLEND);
        if (g_gl_debug & 1) { glColorMask(1, 1, 1, 1); break; }  // experiment: no mask pass
        glColorMask(0, 0, 0, 1);
        glUniform1i(ud.pass, 4);
        glDrawArrays(GL_TRIANGLES, 0, n);
        glColorMask(1, 1, 1, 1);
        break;
    case B_SEMI:
        set_blend(batch_semi_mode);
        glUniform1i(ud.pass, 0);
        glDrawArrays(GL_TRIANGLES, 0, n);
        break;
    case B_SEMI_TEX:  // opaque texels without blending, then the semi-transparent ones
        glDisable(GL_BLEND);
        glUniform1i(ud.pass, 1);
        glDrawArrays(GL_TRIANGLES, 0, n);
        set_blend(batch_semi_mode);
        glUniform1i(ud.pass, 2);
        glDrawArrays(GL_TRIANGLES, 0, n);
        break;
    default: break;
    }
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    batch.clear();
    batch_boxes.clear();
    batch_kind = B_NONE;
}

static uint32_t mode_bits(const DrawCtx& d, bool rect) {
    uint32_t m = 0;
    if (d.textured) m |= 1u;
    if (d.raw) m |= 2u;
    if (d.do_dither && !rect) m |= 4u;
    if (d.semi) m |= 8u;
    if (d.mask_set) m |= 16u;
    m |= (uint32_t)(d.tp_depth & 3) << 5;
    m |= (uint32_t)((d.tp_x / 64) & 15) << 7;
    m |= (uint32_t)((d.tp_y / 256) & 1) << 11;
    m |= (uint32_t)((d.clut_x / 16) & 63) << 12;
    m |= (uint32_t)(d.clut_y & 511) << 18;
    if (rect) m |= 0x8000000u;
    m |= (uint32_t)(d.semi_mode & 3) << 28;
    if (d.mask_check) m |= 0x40000000u;  // (honoured with framebuffer fetch)
    return m;
}
static uint32_t tw_bits(const DrawCtx& d) {
    return (uint32_t)d.tw_mask_x | ((uint32_t)d.tw_mask_y << 5) | ((uint32_t)d.tw_off_x << 10) | ((uint32_t)d.tw_off_y << 15);
}

// texture page and CLUT area a textured primitive reads (as gpu.cpp's texture_reads)
static int tex_reads(const DrawCtx& d, int umin, int umax, int vmin, int vmax, Rgn* out) {
    if (!d.textured) return 0;
    int shift = d.tp_depth == 0 ? 2 : d.tp_depth == 1 ? 1 : 0;
    if (d.tw_mask_x || umin < 0 || umax > 255) { umin = 0; umax = 255; }
    if (d.tw_mask_y || vmin < 0 || vmax > 255) { vmin = 0; vmax = 255; }
    out[0] = Rgn{d.tp_x + (umin >> shift), d.tp_y + vmin, d.tp_x + (umax >> shift) + 1, d.tp_y + vmax + 1};
    if (d.tp_depth == 2) return 1;
    out[1] = Rgn{d.clut_x, d.clut_y, d.clut_x + (d.tp_depth == 0 ? 16 : 256), d.clut_y + 1};
    return 2;
}

// Queue a primitive: n vertices (whole triangles), the native area it covers (bounding box,
// clipped to the drawing area) and the texture areas it reads.
static void add_prim(const DrawCtx& d, const GV* v, int n, Rgn box, const Rgn* reads, int nreads) {
    // drawing area -> scissor (internal pixels)
    int x0 = std::max<int>(d.da_x1, 0), y0 = std::max<int>(d.da_y1, 0);
    int x1 = std::min<int>(d.da_x2 + 1, 1024), y1 = std::min<int>(d.da_y2 + 1, 512);
    if (x1 <= x0 || y1 <= y0) return;
    box = Rgn{std::max(box.x0, x0), std::max(box.y0, y0), std::min(box.x1, x1), std::min(box.y1, y1)};
    if (box.x1 <= box.x0 || box.y1 <= box.y0) return;
    if (d.mask_check && !g_fetch && !g_mask_check++) LOGW("GL: mask-checked drawing is not supported yet (ignored)");
    // a texture read of something drawn since the last refresh of the read copy
    for (int i = 0; i < nreads; i++)
        if (dirty_test(reads[i])) { sync_read(); break; }
    BatchKind kind = g_fetch || !(d.semi && d.semi_mode == 2) ? B_MAIN : d.textured ? B_SEMI_TEX : B_SEMI;
    bool same_kind = batch_kind == kind && (kind == B_OPAQUE || kind == B_MAIN || batch_semi_mode == d.semi_mode);
    bool same_sc = sc_x0 == x0 << gsh && sc_y0 == y0 << gsh && sc_x1 == x1 << gsh && sc_y1 == y1 << gsh;
    bool same = same_kind && same_sc;
    if (same && kind == B_SEMI_TEX) {
        // the two passes reorder overlapping primitives: keep those in separate batches
        for (const Rgn& b : batch_boxes)
            if (b.x0 < box.x1 && box.x0 < b.x1 && b.y0 < box.y1 && box.y0 < b.y1) { same = false; break; }
    }
    if (!same && batch_kind != B_NONE) g_break_why[!same_kind ? 0 : !same_sc ? 1 : 2]++;
    if (!same) {
        flush();
        batch_kind = kind;
        batch_semi_mode = d.semi_mode;
        sc_x0 = x0 << gsh; sc_y0 = y0 << gsh; sc_x1 = x1 << gsh; sc_y1 = y1 << gsh;
    }
    if (kind == B_SEMI_TEX) batch_boxes.push_back(box);
    batch.insert(batch.end(), v, v + n);
    dirty_mark(box);
    g_prims++;
}

static GV gv(float x, float y, int r, int g, int b, float u, float v, uint32_t mode, uint32_t tw) {
    GV o;
    o.x = x; o.y = y;
    o.r = (uint8_t)r; o.g = (uint8_t)g; o.b = (uint8_t)b; o.a = 0;
    o.u = u; o.v = v;
    o.mode = mode; o.tw = tw;
    return o;
}

static void draw_tri(const Job& j) {
    const DrawCtx& d = j.d;
    const Vertex* v = j.v;
    // internal-resolution position + 0.5: GL samples pixel centres, the PS1 integer positions
    float k = (float)S / (1 << SUB);
    uint32_t m = mode_bits(d, false), tw = tw_bits(d);
    GV t[3];
    for (int i = 0; i < 3; i++) t[i] = gv(v[i].x * k + 0.5f, v[i].y * k + 0.5f, v[i].r, v[i].g, v[i].b, (float)v[i].u, (float)v[i].v, m, tw);
    // native bounding box (sub-pixel vertices can lie up to 2 pixels from the integer ones)
    Rgn box{std::min({v[0].nx, v[1].nx, v[2].nx}) - 2, std::min({v[0].ny, v[1].ny, v[2].ny}) - 2,
            std::max({v[0].nx, v[1].nx, v[2].nx}) + 2, std::max({v[0].ny, v[1].ny, v[2].ny}) + 2};
    Rgn rd[2];
    int nr = tex_reads(d, std::min({v[0].u, v[1].u, v[2].u}), std::max({v[0].u, v[1].u, v[2].u}),
                       std::min({v[0].v, v[1].v, v[2].v}), std::max({v[0].v, v[1].v, v[2].v}), rd);
    add_prim(d, t, 3, box, rd, nr);
}

static void draw_rect(const Job& j) {
    const DrawCtx& d = j.d;
    int x = j.v[0].nx, y = j.v[0].ny, w = j.v[1].nx, h = j.v[1].ny;
    int r = j.v[0].r, g = j.v[0].g, b = j.v[0].b, u0 = j.v[0].u, v0 = j.v[0].v;
    if (w <= 0 || h <= 0) return;
    uint32_t m = mode_bits(d, true), tw = tw_bits(d);
    float fx0 = (float)(x * S), fy0 = (float)(y * S), fx1 = (float)((x + w) * S), fy1 = (float)((y + h) * S);
    float uu0 = (float)u0, vv0 = (float)v0, uu1 = (float)(u0 + w), vv1 = (float)(v0 + h);
    GV q[6] = {gv(fx0, fy0, r, g, b, uu0, vv0, m, tw), gv(fx1, fy0, r, g, b, uu1, vv0, m, tw), gv(fx0, fy1, r, g, b, uu0, vv1, m, tw),
               gv(fx1, fy0, r, g, b, uu1, vv0, m, tw), gv(fx1, fy1, r, g, b, uu1, vv1, m, tw), gv(fx0, fy1, r, g, b, uu0, vv1, m, tw)};
    Rgn rd[2];
    int nr = tex_reads(d, u0, u0 + w - 1, v0, v0 + h - 1, rd);
    add_prim(d, q, 6, Rgn{x, y, x + w, y + h}, rd, nr);
}

// lines: the software renderer's DDA, one native pixel (a quad) per step
static void draw_line(const Job& j) {
    const DrawCtx& d = j.d;
    const Vertex &a = j.v[0], &b = j.v[1];
    int dx = b.nx - a.nx, dy = b.ny - a.ny;
    if (std::abs(dx) >= 1024 || std::abs(dy) >= 512) return;
    int n = std::max(std::abs(dx), std::abs(dy));
    uint32_t m = mode_bits(d, false), tw = tw_bits(d);
    std::vector<GV> q;
    q.reserve((size_t)(n + 1) * 6);
    for (int i = 0; i <= n; i++) {
        int x = n ? a.nx + dx * i / n : a.nx;
        int y = n ? a.ny + dy * i / n : a.ny;
        int r = a.r, g = a.g, bb = a.b;
        if (d.gouraud && n) {
            r = a.r + (b.r - a.r) * i / n;
            g = a.g + (b.g - a.g) * i / n;
            bb = a.b + (b.b - a.b) * i / n;
        }
        float x0 = (float)(x * S), y0 = (float)(y * S), x1 = x0 + S, y1 = y0 + S;
        GV p[6] = {gv(x0, y0, r, g, bb, 0, 0, m, tw), gv(x1, y0, r, g, bb, 0, 0, m, tw), gv(x0, y1, r, g, bb, 0, 0, m, tw),
                   gv(x1, y0, r, g, bb, 0, 0, m, tw), gv(x1, y1, r, g, bb, 0, 0, m, tw), gv(x0, y1, r, g, bb, 0, 0, m, tw)};
        q.insert(q.end(), p, p + 6);
    }
    Rgn box{std::min(a.nx, b.nx), std::min(a.ny, b.ny), std::max(a.nx, b.nx) + 1, std::max(a.ny, b.ny) + 1};
    add_prim(d, q.data(), (int)q.size(), box, nullptr, 0);
}

// ---- fills, copies, uploads, readback -----------------------------------------------------------
// Split [p, p+n) at the VRAM edge `size`: calls f(offset, start, length) per piece.
template <typename F>
static void split_wrap(int p, int n, int size, F f) {
    p &= size - 1;
    int first = std::min(n, size - p);
    f(0, p, first);
    if (n > first) f(first, 0, n - first);
}

static void fill(const Job& j) {
    flush();
    int x = j.v[0].nx, y = j.v[0].ny, w = j.v[1].nx, h = j.v[1].ny;
    if (w <= 0 || h <= 0) return;
    uint16_t c = (uint16_t)j.v[0].r;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
    glEnable(GL_SCISSOR_TEST);
    glClearColor((c & 31) * 8 / 255.0f, ((c >> 5) & 31) * 8 / 255.0f, ((c >> 10) & 31) * 8 / 255.0f, 0);
    split_wrap(x, w, 1024, [&](int, int px0, int pw) {
        split_wrap(y, h, 512, [&](int, int py0, int ph) {
            glScissor(px0 << gsh, VH - ((py0 + ph) << gsh), pw << gsh, ph << gsh);
            glClear(GL_COLOR_BUFFER_BIT);
        });
    });
    glDisable(GL_SCISSOR_TEST);
    dirty_mark_wrap(x, y, w, h);
}

// draw a destination rectangle (internal pixels) with the copy program
static void copy_quad(int x0, int y0, int x1, int y1) {
    float q[12] = {(float)x0, (float)y0, (float)x1, (float)y0, (float)x0, (float)y1,
                   (float)x1, (float)y0, (float)x1, (float)y1, (float)x0, (float)y1};
    GV v[6];
    for (int i = 0; i < 6; i++) v[i] = gv(q[2 * i], q[2 * i + 1], 0, 0, 0, 0, 0, 0, 0);
    bind_vertices(v, sizeof v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void begin_copy(GLuint src, int div, bool mask, bool src_flip, int src_h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
    glViewport(0, 0, VW, VH);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(prog_copy);
    glUniform2f(uc.size, (float)VW, (float)VH);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform1i(uc.src, 0);
    glUniform1i(uc.div, div);
    glUniform1i(uc.mask, mask ? 1 : 0);
    glUniform1i(uc.flip, VH - 1);
    glUniform1i(uc.src_flip, src_flip ? 1 : 0);
    glUniform1i(uc.src_h, src_h);
}

static void vram_copy(const Job& j) {
    flush();
    int sx = j.v[0].nx, sy = j.v[0].ny, dx = j.v[1].nx, dy = j.v[1].ny, w = j.v[2].nx, h = j.v[2].ny;
    if (j.d.mask_check && !g_mask_check++) LOGW("GL: mask-checked drawing is not supported yet (ignored)");
    // the source must be current in the read copy
    if (dirty_test(Rgn{sx, sy, sx + w, sy + h}) || dirty_test(Rgn{0, 0, sx + w - 1024, sy + h}) ||
        dirty_test(Rgn{sx, 0, sx + w, sy + h - 512}) || dirty_test(Rgn{0, 0, sx + w - 1024, sy + h - 512}))
        sync_read();
    begin_copy(tex_read, 0, j.d.mask_set, true, VH);
    // pieces where neither the source nor the destination wraps
    int xs[4] = {0, w, 0, 0}, nx = 2, ys[4] = {0, h, 0, 0}, ny = 2;
    auto cut = [](int* c, int& n, int p, int len, int size) {
        int k = size - (p & (size - 1));
        if (k > 0 && k < len) c[n++] = k;
    };
    cut(xs, nx, sx, w, 1024); cut(xs, nx, dx, w, 1024);
    cut(ys, ny, sy, h, 512); cut(ys, ny, dy, h, 512);
    std::sort(xs, xs + nx);
    std::sort(ys, ys + ny);
    for (int iy = 0; iy + 1 < ny; iy++)
        for (int ix = 0; ix + 1 < nx; ix++) {
            int ox = xs[ix], oy = ys[iy], pw = xs[ix + 1] - ox, ph = ys[iy + 1] - oy;
            if (pw <= 0 || ph <= 0) continue;
            int px0 = (dx + ox) & 1023, py0 = (dy + oy) & 511;
            glUniform2i(uc.dst0, px0 << gsh, py0 << gsh);
            glUniform2i(uc.src0, ((sx + ox) & 1023) << gsh, ((sy + oy) & 511) << gsh);
            copy_quad(px0 << gsh, py0 << gsh, (px0 + pw) << gsh, (py0 + ph) << gsh);
        }
    dirty_mark_wrap(dx, dy, w, h);
}

static std::vector<uint8_t> stage_buf;
static void upload(const Job& j) {
    flush();
    int x = j.v[0].nx, y = j.v[0].ny, w = j.v[1].nx, h = j.v[1].ny;
    const std::vector<uint16_t>& src = *j.data;
    if (j.d.mask_check && !g_mask_check++) LOGW("GL: mask-checked drawing is not supported yet (ignored)");
    stage_buf.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint16_t p = src[i];
        stage_buf[i * 4 + 0] = (uint8_t)((p & 31) << 3);
        stage_buf[i * 4 + 1] = (uint8_t)(((p >> 5) & 31) << 3);
        stage_buf[i * 4 + 2] = (uint8_t)(((p >> 10) & 31) << 3);
        stage_buf[i * 4 + 3] = (p & 0x8000) ? 255 : 0;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_stage);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, stage_buf.data());
    begin_copy(tex_stage, gsh, j.d.mask_set, false, 512);
    split_wrap(x, w, 1024, [&](int ox, int px0, int pw) {
        split_wrap(y, h, 512, [&](int oy, int py0, int ph) {
            glUniform2i(uc.dst0, px0 << gsh, py0 << gsh);
            glUniform2i(uc.src0, ox, oy);
            copy_quad(px0 << gsh, py0 << gsh, (px0 + pw) << gsh, (py0 + ph) << gsh);
        });
    });
    dirty_mark_wrap(x, y, w, h);
    delete j.data;
}

static std::vector<uint8_t> read_buf;
// read a native rectangle (no wrapping) of the drawn VRAM: calls f(x, y, pixel) per native pixel
template <typename F>
static void read_native(int x, int y, int w, int h, F f) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_draw);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    int iw = w << gsh, ih = h << gsh;
    read_buf.resize((size_t)iw * ih * 4);
    glReadPixels(x << gsh, VH - ((y + h) << gsh), iw, ih, GL_RGBA, GL_UNSIGNED_BYTE, read_buf.data());
    for (int yy = 0; yy < h; yy++) {
        // texture rows are upside down: the native row's top sample is the last of its block
        const uint8_t* row = &read_buf[(size_t)(ih - 1 - (yy << gsh)) * iw * 4];
        for (int xx = 0; xx < w; xx++) {
            const uint8_t* p = row + (size_t)(xx << gsh) * 4;
            uint16_t v = (uint16_t)((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) | (p[3] > 127 ? 0x8000 : 0));
            f(x + xx, y + yy, v);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
}

static void readback(const Job& j) {
    flush();
    int x = j.v[0].nx, y = j.v[0].ny, w = j.v[1].nx, h = j.v[1].ny;
    split_wrap(x, w, 1024, [&](int, int px0, int pw) {
        split_wrap(y, h, 512, [&](int, int py0, int ph) {
            read_native(px0, py0, pw, ph, [](int xx, int yy, uint16_t v) { g_gl_mirror[yy * 1024 + xx] = v; });
        });
    });
}

// ---- presenting -----------------------------------------------------------------------------------
static uint32_t expand15(uint32_t r5, uint32_t g5, uint32_t b5) {
    uint32_t r = (r5 << 3) | (r5 >> 2), g = (g5 << 3) | (g5 >> 2), b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// ARGB copy of the display area for screenshots (as the software frontend's convert_display)
static void screenshot(const PresentInfo& p) {
    const DisplayInfo& d = p.disp;
    uint32_t* dst = p.shot;
    if (!d.enabled) {
        memset(dst, 0, (size_t)p.shot_w * p.shot_h * 4);
        return;
    }
    if (d.rgb24) {
        static std::vector<uint16_t> words;
        words.assign((size_t)1024 * 512, 0);
        read_native(0, 0, 1024, 512, [](int x, int y, uint16_t v) { words[(size_t)y * 1024 + x] = v; });
        for (int y = 0; y < p.shot_h; y++) {
            int vy = (d.y + y) & 511;
            auto byte = [&](int bx) {
                uint16_t w = words[(size_t)vy * 1024 + ((bx >> 1) & 1023)];
                return (uint32_t)(uint8_t)(bx & 1 ? w >> 8 : w);
            };
            for (int x = 0; x < p.shot_w; x++) {
                int bx = (d.x * 2 + x * 3) % 2048;
                dst[(size_t)y * p.shot_w + x] = 0xFF000000u | (byte(bx) << 16) | (byte((bx + 1) % 2048) << 8) | byte((bx + 2) % 2048);
            }
        }
        return;
    }
    // internal resolution, every sample
    int iw = p.shot_w, ih = p.shot_h;
    int x0 = (d.x << gsh) % VW, y0 = (d.y << gsh) % VH;
    iw = std::min(iw, VW - x0);
    ih = std::min(ih, VH - y0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_draw);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    read_buf.resize((size_t)iw * ih * 4);
    glReadPixels(x0, VH - y0 - ih, iw, ih, GL_RGBA, GL_UNSIGNED_BYTE, read_buf.data());
    for (int y = 0; y < ih; y++) {
        const uint8_t* row = &read_buf[(size_t)(ih - 1 - y) * iw * 4];
        for (int x = 0; x < iw; x++)
            dst[(size_t)y * p.shot_w + x] = expand15(row[x * 4] >> 3, row[x * 4 + 1] >> 3, row[x * 4 + 2] >> 3);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
}

static void present(const PresentInfo& p) {
    flush();
    if (p.shot) screenshot(p);
    if (!p.swap) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    static int logged_w = 0, logged_h = 0;
    if (p.win_w != logged_w || p.win_h != logged_h) {
        logged_w = p.win_w; logged_h = p.win_h;
        LOGI("GL: output %dx%d, picture %dx%d", p.win_w, p.win_h, p.dst_w, p.dst_h);
    }
    glViewport(0, 0, p.win_w, p.win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (p.disp.enabled) {
        glViewport(p.dst_x, p.win_h - p.dst_y - p.dst_h, p.dst_w, p.dst_h);
        glUseProgram(prog_present);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_draw);
        glUniform1i(up.vram, 0);
        glUniform1i(up.shift, gsh);
        glUniform2i(up.origin, p.disp.x, p.disp.y);
        glUniform2i(up.disp, p.disp.w, p.disp.h);
        glUniform1i(up.rgb24, p.disp.rgb24 ? 1 : 0);
        glUniform1i(up.flip, VH - 1);
        glUniform1i(up.cheap, (g_gl_debug & 2) ? 1 : 0);
        float w = (float)p.disp.w, h = (float)p.disp.h;
        GV v[6] = {gv(-1, 1, 0, 0, 0, 0, 0, 0, 0), gv(1, 1, 0, 0, 0, w, 0, 0, 0), gv(-1, -1, 0, 0, 0, 0, h, 0, 0),
                   gv(1, 1, 0, 0, 0, w, 0, 0, 0), gv(1, -1, 0, 0, 0, w, h, 0, 0), gv(-1, -1, 0, 0, 0, 0, h, 0, 0)};
        bind_vertices(v, sizeof v);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    SDL_GL_SwapWindow(g_gl_window);
    // at most one frame queued on the GPU: wait for the previous one to finish (otherwise a
    // GPU that is slower than real time builds up frames, and input delay with them)
    static GLsync prev = nullptr;
    GLsync f = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (prev) {
        glClientWaitSync(prev, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);  // 100 ms at most
        glDeleteSync(prev);
    }
    prev = f;
    g_gl_presents_done.fetch_add(1, std::memory_order_release);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw);
}

// ---- thread entry points -------------------------------------------------------------------------
bool gl_thread_init() {
    if (SDL_GL_MakeCurrent(g_gl_window, g_gl_context) != 0) {
        LOGE("GL: cannot make the context current: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetSwapInterval(0);  // the emulation paces itself
    if (!gl_load_functions()) return false;
    LOGI("GL: %s / %s / %s", (const char*)glGetString(GL_VENDOR), (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));
    if (const char* ext = (const char*)glGetString(GL_EXTENSIONS)) {
        LOGI("GL: blend_func_extended %d, framebuffer_fetch %d", strstr(ext, "blend_func_extended") != nullptr,
             strstr(ext, "framebuffer_fetch") != nullptr);
    }
    const char* ext = (const char*)glGetString(GL_EXTENSIONS);
    if (ext && !(g_gl_debug & 16)) {
        if (strstr(ext, "GL_EXT_shader_framebuffer_fetch")) prog_draw = link(kDrawVS, kDrawFS, kHeaderFetchExt);
        if (!prog_draw && strstr(ext, "GL_ARM_shader_framebuffer_fetch")) prog_draw = link(kDrawVS, kDrawFS, kHeaderFetchArm);
        g_fetch = prog_draw != 0;
    }
    if (!prog_draw && ext && strstr(ext, "GL_EXT_blend_func_extended") && !(g_gl_debug & 8)) {
        prog_draw = link(kDrawVS, kDrawFS, kHeaderDual);
        g_dual = prog_draw != 0;
    }
    if (!prog_draw) prog_draw = link(kDrawVS, kDrawFS);
    LOGI("GL: %s", g_fetch ? "framebuffer fetch (blending in the shader)"
                   : g_dual ? "dual-source blending (one pass)" : "colour + mask passes");
    prog_copy = link(kCopyVS, kCopyFS);
    prog_present = link(kPresentVS, kPresentFS);
    if (!prog_draw || !prog_copy || !prog_present) return false;
    ud.size = glGetUniformLocation(prog_draw, "u_size");
    ud.vram = glGetUniformLocation(prog_draw, "u_nat");
    ud.shift = glGetUniformLocation(prog_draw, "u_shift");
    ud.pass = glGetUniformLocation(prog_draw, "u_pass");
    ud.flip = glGetUniformLocation(prog_draw, "u_flip");
    uc.size = glGetUniformLocation(prog_copy, "u_size");
    uc.src = glGetUniformLocation(prog_copy, "u_src");
    uc.dst0 = glGetUniformLocation(prog_copy, "u_dst0");
    uc.src0 = glGetUniformLocation(prog_copy, "u_src0");
    uc.div = glGetUniformLocation(prog_copy, "u_div");
    uc.mask = glGetUniformLocation(prog_copy, "u_mask");
    uc.flip = glGetUniformLocation(prog_copy, "u_flip");
    uc.src_flip = glGetUniformLocation(prog_copy, "u_src_flip");
    uc.src_h = glGetUniformLocation(prog_copy, "u_src_h");
    up.vram = glGetUniformLocation(prog_present, "u_vram");
    up.shift = glGetUniformLocation(prog_present, "u_shift");
    up.origin = glGetUniformLocation(prog_present, "u_origin");
    up.disp = glGetUniformLocation(prog_present, "u_disp");
    up.rgb24 = glGetUniformLocation(prog_present, "u_rgb24");
    up.flip = glGetUniformLocation(prog_present, "u_flip");
    up.cheap = glGetUniformLocation(prog_present, "u_cheap");

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(GV), (const void*)offsetof(GV, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, 0, sizeof(GV), (const void*)offsetof(GV, r));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(GV), (const void*)offsetof(GV, u));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 2, GL_UNSIGNED_INT, sizeof(GV), (const void*)offsetof(GV, mode));

    glDisable(GL_DITHER);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    tex_stage = make_tex(1024, 512);
    tex_nat = make_tex(1024, 512, GL_R16UI);
    fbo_nat = make_fbo(tex_nat);
    prog_pack = link(kPackVS, kPackFS);
    if (!prog_pack) return false;
    upk.src = glGetUniformLocation(prog_pack, "u_src");
    upk.shift = glGetUniformLocation(prog_pack, "u_shift");
    upk.flip = glGetUniformLocation(prog_pack, "u_flip");
    set_scale(g_gl_shift);
    return true;
}

static void gl_run_job_inner(const Job& j);
static uint64_t g_present_ns = 0, g_jobs = 0;
static inline uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void gl_run_job(const Job& j) {
    uint64_t t0 = g_spu_profile ? now_ns() : 0;
    gl_run_job_inner(j);
    if (g_spu_profile) {
        uint64_t dt = now_ns() - t0;
        g_busy_ns += dt;
        if (j.kind == JOB_PRESENT) g_present_ns += dt;
        g_jobs++;
    }
}

static void gl_run_job_inner(const Job& j) {
    switch (j.kind) {
    case JOB_TRI: draw_tri(j); break;
    case JOB_RECT: draw_rect(j); break;
    case JOB_LINE: draw_line(j); break;
    case JOB_FILL: fill(j); break;
    case JOB_COPY: vram_copy(j); break;
    case JOB_UPLOAD: upload(j); break;
    case JOB_READBACK: readback(j); break;
    case JOB_PRESENT: present(j.present); break;
    case JOB_SCALE:
        flush();
        sync_read();
        set_scale(j.present.scale_shift);
        break;
    case JOB_CONVERT: break;  // software renderer only
    }
}

void gl_idle() {
    if (batch.empty()) return;
    flush();
    glFlush();
}

// one profile line (per-frame averages over `frames`), counters reset; racy, statistics only
void gl_stats_line(char* out, size_t size, int frames) {
    double f = frames;
    snprintf(out, size,
             "GL thread busy %.2f ms/frame (present %.2f), %.0f jobs, %.0f prims, %.0f batches (new: kind %.0f, area %.0f, "
             "overlap %.0f), %.1f read copies",
             g_busy_ns / 1e6 / f, g_present_ns / 1e6 / f, g_jobs / f, g_prims / f, g_batches / f, g_break_why[0] / f,
             g_break_why[1] / f, g_break_why[2] / f, g_read_syncs / f);
    g_busy_ns = g_present_ns = g_jobs = g_prims = g_batches = g_read_syncs = 0;
    memset(g_break_why, 0, sizeof g_break_why);
}

}  // namespace psx
