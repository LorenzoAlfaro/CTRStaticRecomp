// PS1 GPU: command processing and a software rasterizer into 1 MiB VRAM.
#include <algorithm>
#include <cstring>

#include "psx.h"

namespace psx {

static uint16_t vram[512 * 1024];

// ---- state -----------------------------------------------------------------------
static uint32_t stat_texpage = 0;   // GP0 E1 bits 0..10 (+ texture disable bit 11)
static bool tex_disable = false;
static bool allow_tex_disable = false;  // GP1(09h): texture disable only works when allowed
static bool dither = false;
static bool draw_to_display = false;
static bool mask_set = false, mask_check = false;
static uint32_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
static int da_x1, da_y1, da_x2, da_y2;  // drawing area (inclusive)
static int off_x, off_y;
static bool display_disabled = true;
static int dma_dir = 0;
static int disp_x = 0, disp_y = 0;
static int h_start = 0x200, h_end = 0xC00, v_start = 0x10, v_end = 0x100;
static uint32_t disp_mode = 0;
static bool irq_flag = false;
static bool vblank_now = false;

// command FIFO
static uint32_t cmd[64];
static int cmd_len = 0, cmd_need = 0;
static bool polyline = false;
static struct {
    uint32_t op, prev_v, prev_c, cur_c;
    bool gour, have_prev, want_color;
} pl;

// VRAM transfers
static struct {
    bool active = false;
    int x, y, w, h, cx, cy;
} upload, download;

// ---- helpers -----------------------------------------------------------------------
static inline uint16_t& px(int x, int y) { return vram[((y & 511) << 10) | (x & 1023)]; }

struct Vertex {
    int x, y;
    int r, g, b;
    int u, v;
};

struct DrawCtx {
    bool textured, raw, semi, gouraud;
    int semi_mode;
    int tp_x, tp_y, tp_depth;  // texpage base (pixels) and depth 0=4bit 1=8bit 2=15bit
    int clut_x, clut_y;
    bool do_dither;
};

static const int dither_tab[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline uint16_t sample_tex(const DrawCtx& d, int u, int v) {
    u &= 0xFF;
    v &= 0xFF;
    u = (int)((u & ~(tw_mask_x * 8)) | ((tw_off_x & tw_mask_x) * 8));
    v = (int)((v & ~(tw_mask_y * 8)) | ((tw_off_y & tw_mask_y) * 8));
    switch (d.tp_depth) {
    case 0: {
        uint16_t w = px(d.tp_x + (u >> 2), d.tp_y + v);
        int idx = (w >> ((u & 3) * 4)) & 0xF;
        return px(d.clut_x + idx, d.clut_y);
    }
    case 1: {
        uint16_t w = px(d.tp_x + (u >> 1), d.tp_y + v);
        int idx = (w >> ((u & 1) * 8)) & 0xFF;
        return px(d.clut_x + idx, d.clut_y);
    }
    default:
        return px(d.tp_x + u, d.tp_y + v);
    }
}

static inline void plot(const DrawCtx& d, int x, int y, int r, int g, int b, int u, int v) {
    uint16_t& dst = px(x, y);
    if (mask_check && (dst & 0x8000)) return;
    uint16_t out;
    bool semi_px = d.semi;
    if (d.textured) {
        uint16_t t = sample_tex(d, u, v);
        if (t == 0) return;
        semi_px = d.semi && (t & 0x8000);
        int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
        if (!d.raw) {
            if (d.do_dither) {
                int dt = dither_tab[y & 3][x & 3];
                int rr = std::clamp(((tr << 3) * r >> 7) + dt, 0, 255);
                int gg = std::clamp(((tg << 3) * g >> 7) + dt, 0, 255);
                int bb = std::clamp(((tb << 3) * b >> 7) + dt, 0, 255);
                tr = rr >> 3; tg = gg >> 3; tb = bb >> 3;
            } else {
                tr = std::min(31, tr * r >> 7);
                tg = std::min(31, tg * g >> 7);
                tb = std::min(31, tb * b >> 7);
            }
        }
        out = (uint16_t)(tr | (tg << 5) | (tb << 10) | (t & 0x8000));
    } else {
        if (d.do_dither) {
            int dt = dither_tab[y & 3][x & 3];
            r = std::clamp(r + dt, 0, 255);
            g = std::clamp(g + dt, 0, 255);
            b = std::clamp(b + dt, 0, 255);
        }
        out = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    }
    if (semi_px) {
        int br = dst & 31, bg = (dst >> 5) & 31, bb = (dst >> 10) & 31;
        int fr = out & 31, fg = (out >> 5) & 31, fb = (out >> 10) & 31;
        switch (d.semi_mode) {
        case 0: fr = (br + fr) >> 1; fg = (bg + fg) >> 1; fb = (bb + fb) >> 1; break;
        case 1: fr = std::min(31, br + fr); fg = std::min(31, bg + fg); fb = std::min(31, bb + fb); break;
        case 2: fr = std::max(0, br - fr); fg = std::max(0, bg - fg); fb = std::max(0, bb - fb); break;
        default: fr = std::min(31, br + (fr >> 2)); fg = std::min(31, bg + (fg >> 2)); fb = std::min(31, bb + (fb >> 2)); break;
        }
        out = (uint16_t)(fr | (fg << 5) | (fb << 10) | (out & 0x8000));
    }
    if (mask_set) out |= 0x8000;
    dst = out;
}

static DrawCtx make_ctx(uint32_t op, uint32_t texpage_word, uint32_t clut_word, bool textured) {
    DrawCtx d{};
    d.textured = textured && !tex_disable;
    d.raw = d.textured && (op & 1);
    d.semi = op & 2;
    uint32_t tp = textured ? texpage_word : stat_texpage;
    d.semi_mode = (tp >> 5) & 3;
    d.tp_x = (tp & 0xF) * 64;
    d.tp_y = ((tp >> 4) & 1) * 256;
    d.tp_depth = std::min<int>((tp >> 7) & 3, 2);
    d.clut_x = (clut_word & 0x3F) * 16;
    d.clut_y = (clut_word >> 6) & 0x1FF;
    return d;
}

// ---- triangles ------------------------------------------------------------------------
static inline int64_t edge(const Vertex& a, const Vertex& b, int x, int y) {
    return (int64_t)(b.x - a.x) * (y - a.y) - (int64_t)(b.y - a.y) * (x - a.x);
}

static void draw_triangle(const DrawCtx& d, Vertex v0, Vertex v1, Vertex v2) {
    int64_t area = edge(v0, v1, v2.x, v2.y);
    if (area == 0) return;
    if (area < 0) { std::swap(v1, v2); area = -area; }
    int minx = std::min({v0.x, v1.x, v2.x}), maxx = std::max({v0.x, v1.x, v2.x});
    int miny = std::min({v0.y, v1.y, v2.y}), maxy = std::max({v0.y, v1.y, v2.y});
    if (maxx - minx >= 1024 || maxy - miny >= 512) return;
    minx = std::max(minx, da_x1); maxx = std::min(maxx, da_x2 + 1);
    miny = std::max(miny, da_y1); maxy = std::min(maxy, da_y2 + 1);
    if (minx >= maxx || miny >= maxy) return;

    // attribute gradients (fixed 16.16)
    auto grad = [&](int a0, int a1, int a2, int64_t& ddx, int64_t& ddy) {
        // plane through (x_i, y_i, a_i)
        int64_t dx1 = v1.x - v0.x, dy1 = v1.y - v0.y, dx2 = v2.x - v0.x, dy2 = v2.y - v0.y;
        int64_t da1 = a1 - a0, da2 = a2 - a0;
        int64_t den = dx1 * dy2 - dx2 * dy1;
        ddx = ((da1 * dy2 - da2 * dy1) << 16) / den;
        ddy = ((da2 * dx1 - da1 * dx2) << 16) / den;
    };
    int64_t rdx = 0, rdy = 0, gdx = 0, gdy = 0, bdx = 0, bdy = 0, udx = 0, udy = 0, vdx = 0, vdy = 0;
    if (d.gouraud) {
        grad(v0.r, v1.r, v2.r, rdx, rdy);
        grad(v0.g, v1.g, v2.g, gdx, gdy);
        grad(v0.b, v1.b, v2.b, bdx, bdy);
    }
    if (d.textured) {
        grad(v0.u, v1.u, v2.u, udx, udy);
        grad(v0.v, v1.v, v2.v, vdx, vdy);
    }
    // top-left rule: an edge is "top" or "left" if ... (with CCW positive area in y-down space)
    auto is_tl = [](const Vertex& a, const Vertex& b) {
        int dy = b.y - a.y, dx = b.x - a.x;
        return dy < 0 || (dy == 0 && dx > 0);
    };
    int bias0 = is_tl(v1, v2) ? 0 : -1;
    int bias1 = is_tl(v2, v0) ? 0 : -1;
    int bias2 = is_tl(v0, v1) ? 0 : -1;

    for (int y = miny; y < maxy; y++) {
        int64_t w0 = edge(v1, v2, minx, y), w1 = edge(v2, v0, minx, y), w2 = edge(v0, v1, minx, y);
        int64_t s0 = -(int64_t)(v2.y - v1.y), s1 = -(int64_t)(v0.y - v2.y), s2 = -(int64_t)(v1.y - v0.y);
        for (int x = minx; x < maxx; x++, w0 += s0, w1 += s1, w2 += s2) {
            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) continue;
            int64_t fx = x - v0.x, fy = y - v0.y;
            int r = v0.r, g = v0.g, b = v0.b, u = 0, v = 0;
            if (d.gouraud) {
                r = (int)((((int64_t)v0.r << 16) + rdx * fx + rdy * fy + 0x8000) >> 16);
                g = (int)((((int64_t)v0.g << 16) + gdx * fx + gdy * fy + 0x8000) >> 16);
                b = (int)((((int64_t)v0.b << 16) + bdx * fx + bdy * fy + 0x8000) >> 16);
                r = std::clamp(r, 0, 255); g = std::clamp(g, 0, 255); b = std::clamp(b, 0, 255);
            }
            if (d.textured) {
                u = (int)((((int64_t)v0.u << 16) + udx * fx + udy * fy + 0x8000) >> 16);
                v = (int)((((int64_t)v0.v << 16) + vdx * fx + vdy * fy + 0x8000) >> 16);
                u = std::clamp(u, 0, 255); v = std::clamp(v, 0, 255);
            }
            plot(d, x, y, r, g, b, u, v);
        }
    }
}

// ---- rectangles / lines ---------------------------------------------------------------
static void draw_rect(const DrawCtx& d, int x, int y, int w, int h, int r, int g, int b, int u0, int v0) {
    int x0 = std::max(x, da_x1), y0 = std::max(y, da_y1);
    int x1 = std::min(x + w, da_x2 + 1), y1 = std::min(y + h, da_y2 + 1);
    DrawCtx dd = d;
    dd.do_dither = false;  // rectangles are never dithered
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            plot(dd, xx, yy, r, g, b, u0 + (xx - x), v0 + (yy - y));
}

static void draw_line(const DrawCtx& d, Vertex a, Vertex b) {
    int dx = b.x - a.x, dy = b.y - a.y;
    if (std::abs(dx) >= 1024 || std::abs(dy) >= 512) return;
    int n = std::max(std::abs(dx), std::abs(dy));
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + dx * i / n : a.x;
        int y = n ? a.y + dy * i / n : a.y;
        if (x < da_x1 || x > da_x2 || y < da_y1 || y > da_y2) continue;
        int r = a.r, g = a.g, bb = a.b;
        if (d.gouraud && n) {
            r = a.r + (b.r - a.r) * i / n;
            g = a.g + (b.g - a.g) * i / n;
            bb = a.b + (b.b - a.b) * i / n;
        }
        plot(d, x, y, r, g, bb, 0, 0);
    }
}

// ---- command decoding ------------------------------------------------------------------
static inline int sx11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

static Vertex read_vertex(const uint32_t* w, uint32_t color) {
    Vertex v;
    v.x = sx11(w[0] & 0xFFFF) + off_x;
    v.y = sx11(w[0] >> 16) + off_y;
    v.r = color & 0xFF;
    v.g = (color >> 8) & 0xFF;
    v.b = (color >> 16) & 0xFF;
    v.u = v.v = 0;
    return v;
}

static int poly_words(uint32_t op) {
    bool quad = op & 8, tex = op & 4, gour = op & 0x10;
    int n = quad ? 4 : 3;
    return 1 + n * (1 + (tex ? 1 : 0) + (gour ? 1 : 0)) - (gour ? 1 : 0);
}

static void exec_polygon() {
    uint32_t op = cmd[0] >> 24;
    bool quad = op & 8, tex = op & 4, gour = op & 0x10;
    int n = quad ? 4 : 3;
    Vertex v[4];
    uint32_t clut = 0, tpage = 0;
    int i = 0;
    uint32_t color = cmd[0] & 0xFFFFFF;
    for (int k = 0; k < n; k++) {
        if (gour && k > 0) color = cmd[i] & 0xFFFFFF;
        if (k == 0 || gour) i++;
        v[k] = read_vertex(&cmd[i++], color);
        if (tex) {
            uint32_t t = cmd[i++];
            v[k].u = t & 0xFF;
            v[k].v = (t >> 8) & 0xFF;
            if (k == 0) clut = t >> 16;
            if (k == 1) tpage = t >> 16;
        }
    }
    DrawCtx d = make_ctx(op, tpage, clut, tex);
    d.gouraud = gour;
    d.do_dither = dither && (gour || (d.textured && !d.raw));
    if (tex) {
        // polygon texpage updates the global texpage (bits 0-8, 11)
        stat_texpage = (stat_texpage & ~0x9FFu) | (tpage & 0x9FF);
    }
    if (!gour) for (int k = 1; k < n; k++) { v[k].r = v[0].r; v[k].g = v[0].g; v[k].b = v[0].b; }
    if (d.raw) for (int k = 0; k < n; k++) v[k].r = v[k].g = v[k].b = 128;
    draw_triangle(d, v[0], v[1], v[2]);
    if (quad) draw_triangle(d, v[1], v[2], v[3]);
}

static void exec_rect() {
    uint32_t op = cmd[0] >> 24;
    bool tex = op & 4;
    int size = (op >> 3) & 3;
    uint32_t color = cmd[0] & 0xFFFFFF;
    int i = 1;
    Vertex p = read_vertex(&cmd[i++], color);
    int u = 0, v = 0;
    uint32_t clut = 0;
    if (tex) {
        uint32_t t = cmd[i++];
        u = t & 0xFF;
        v = (t >> 8) & 0xFF;
        clut = t >> 16;
    }
    int w, h;
    if (size == 0) { w = cmd[i] & 0x3FF; h = (cmd[i] >> 16) & 0x1FF; }
    else if (size == 1) w = h = 1;
    else if (size == 2) w = h = 8;
    else w = h = 16;
    DrawCtx d = make_ctx(op, stat_texpage, clut, tex);
    d.gouraud = false;
    int r = p.r, g = p.g, b = p.b;
    if (d.raw) r = g = b = 128;
    draw_rect(d, p.x, p.y, w, h, r, g, b, u, v);
}

static void exec_line_segment(const uint32_t* w0, uint32_t c0, const uint32_t* w1, uint32_t c1, uint32_t op) {
    DrawCtx d = make_ctx(op, stat_texpage, 0, false);
    d.gouraud = op & 0x10;
    d.do_dither = dither && d.gouraud;
    draw_line(d, read_vertex(w0, c0), read_vertex(w1, c1));
}

static void fill_rect() {
    uint32_t c = cmd[0];
    int x = cmd[1] & 0x3F0, y = (cmd[1] >> 16) & 0x1FF;
    int w = ((cmd[2] & 0x3FF) + 0xF) & ~0xF, h = (cmd[2] >> 16) & 0x1FF;
    uint16_t col = (uint16_t)(((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10));
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) px(x + xx, y + yy) = col;
}

static void vram_copy() {
    int sx = cmd[1] & 0x3FF, sy = (cmd[1] >> 16) & 0x1FF;
    int dx = cmd[2] & 0x3FF, dy = (cmd[2] >> 16) & 0x1FF;
    int w = cmd[3] & 0x3FF, h = (cmd[3] >> 16) & 0x1FF;
    if (!w) w = 0x400;
    if (!h) h = 0x200;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            uint16_t s = px(sx + xx, sy + yy);
            uint16_t& d = px(dx + xx, dy + yy);
            if (mask_check && (d & 0x8000)) continue;
            d = s | (mask_set ? 0x8000 : 0);
        }
}

static int command_length(uint32_t w) {
    uint32_t op = w >> 24;
    switch (op >> 5) {
    case 1: return poly_words(op);
    case 2: return (op & 8) ? -1 : ((op & 0x10) ? 4 : 3);  // polyline: variable
    case 3: {
        int n = 2;
        if (op & 4) n++;
        if (((op >> 3) & 3) == 0) n++;
        return n;
    }
    case 4: return 4;
    case 5: case 6: return 3;
    default: return op == 0x02 ? 3 : 1;
    }
}

static void exec_command() {
    uint32_t op = cmd[0] >> 24;
    switch (op >> 5) {
    case 1: exec_polygon(); return;
    case 2: {
        bool gour = op & 0x10;
        uint32_t c0 = cmd[0] & 0xFFFFFF, c1 = gour ? cmd[2] & 0xFFFFFF : c0;
        exec_line_segment(&cmd[1], c0, gour ? &cmd[3] : &cmd[2], c1, op);
        return;
    }
    case 3: exec_rect(); return;
    case 4: vram_copy(); return;
    case 5: {
        upload.active = true;
        upload.x = cmd[1] & 0x3FF; upload.y = (cmd[1] >> 16) & 0x1FF;
        upload.w = ((cmd[2] & 0xFFFF) - 1) % 1024 + 1; upload.h = (((cmd[2] >> 16) & 0xFFFF) - 1) % 512 + 1;
        upload.cx = upload.cy = 0;
        return;
    }
    case 6: {
        download.active = true;
        download.x = cmd[1] & 0x3FF; download.y = (cmd[1] >> 16) & 0x1FF;
        download.w = ((cmd[2] & 0xFFFF) - 1) % 1024 + 1; download.h = (((cmd[2] >> 16) & 0xFFFF) - 1) % 512 + 1;
        download.cx = download.cy = 0;
        return;
    }
    default: break;
    }
    uint32_t v = cmd[0];
    switch (op) {
    case 0x02: fill_rect(); break;
    case 0x1F: irq_flag = true; irq_raise(IRQ_GPU); break;
    case 0xE1:
        stat_texpage = v & 0x9FF;
        tex_disable = allow_tex_disable && (v & 0x800);
        dither = v & 0x200;
        draw_to_display = v & 0x400;
        break;
    case 0xE2:
        tw_mask_x = v & 31; tw_mask_y = (v >> 5) & 31;
        tw_off_x = (v >> 10) & 31; tw_off_y = (v >> 15) & 31;
        break;
    case 0xE3: da_x1 = v & 0x3FF; da_y1 = (v >> 10) & 0x3FF; break;
    case 0xE4: da_x2 = v & 0x3FF; da_y2 = (v >> 10) & 0x3FF; break;
    case 0xE5: off_x = sx11(v & 0x7FF); off_y = sx11((v >> 11) & 0x7FF); break;
    case 0xE6: mask_set = v & 1; mask_check = v & 2; break;
    default: break;
    }
}

static void upload_word(uint32_t w) {
    for (int k = 0; k < 2 && upload.active; k++) {
        uint16_t p = (uint16_t)(k ? w >> 16 : w);
        uint16_t& d = px(upload.x + upload.cx, upload.y + upload.cy);
        if (!(mask_check && (d & 0x8000))) d = p | (mask_set ? 0x8000 : 0);
        if (++upload.cx >= upload.w) {
            upload.cx = 0;
            if (++upload.cy >= upload.h) upload.active = false;
        }
    }
}

void gpu_write_gp0(uint32_t w) {
    if (upload.active) { upload_word(w); return; }
    if (polyline) {
        if ((w & 0xF000F000u) == 0x50005000u && pl.have_prev) { polyline = false; return; }
        if (pl.gour && pl.want_color) { pl.cur_c = w & 0xFFFFFF; pl.want_color = false; return; }
        if (pl.have_prev) exec_line_segment(&pl.prev_v, pl.prev_c, &w, pl.cur_c, pl.op);
        pl.prev_v = w;
        pl.prev_c = pl.cur_c;
        pl.have_prev = true;
        pl.want_color = pl.gour;
        return;
    }
    if (cmd_len == 0) {
        int n = command_length(w);
        uint32_t op = w >> 24;
        if (n < 0) {  // polyline
            polyline = true;
            pl.op = op;
            pl.gour = op & 0x10;
            pl.cur_c = w & 0xFFFFFF;
            pl.have_prev = false;
            pl.want_color = false;
            return;
        }
        (void)op;
        cmd_need = n;
    }
    cmd[cmd_len++] = w;
    if (cmd_len >= cmd_need) {
        exec_command();
        cmd_len = 0;
    }
}

void gpu_write_gp1(uint32_t v) {
    switch (v >> 24) {
    case 0x00:
        cmd_len = 0; polyline = false; upload.active = download.active = false;
        display_disabled = true; dma_dir = 0; irq_flag = false;
        stat_texpage = 0; tex_disable = dither = draw_to_display = false;
        mask_set = mask_check = false;
        tw_mask_x = tw_mask_y = tw_off_x = tw_off_y = 0;
        da_x1 = da_y1 = 0; da_x2 = da_y2 = 0; off_x = off_y = 0;
        disp_x = disp_y = 0; disp_mode = 0;
        break;
    case 0x01: cmd_len = 0; polyline = false; upload.active = false; break;
    case 0x02: irq_flag = false; break;
    case 0x03: display_disabled = v & 1; break;
    case 0x04: dma_dir = v & 3; break;
    case 0x05: disp_x = v & 0x3FE; disp_y = (v >> 10) & 0x1FF; break;
    case 0x06: h_start = v & 0xFFF; h_end = (v >> 12) & 0xFFF; break;
    case 0x07: v_start = v & 0x3FF; v_end = (v >> 10) & 0x3FF; break;
    case 0x08: disp_mode = v & 0xFF; break;
    case 0x09: allow_tex_disable = v & 1; break;
    case 0x10: {
        uint32_t r = 0;
        switch (v & 7) {
        case 2: r = tw_mask_x | (tw_mask_y << 5) | (tw_off_x << 10) | (tw_off_y << 15); break;
        case 3: r = da_x1 | (da_y1 << 10); break;
        case 4: r = da_x2 | (da_y2 << 10); break;
        case 5: r = (off_x & 0x7FF) | ((off_y & 0x7FF) << 11); break;
        case 7: r = 2; break;
        }
        download.active = false;
        extern uint32_t gpu_info_latch;
        gpu_info_latch = r;
        break;
    }
    default: break;
    }
}

uint32_t gpu_info_latch = 0;

uint32_t gpu_read() {
    if (!download.active) return gpu_info_latch;
    uint32_t out = 0;
    for (int k = 0; k < 2 && download.active; k++) {
        out |= (uint32_t)px(download.x + download.cx, download.y + download.cy) << (16 * k);
        if (++download.cx >= download.w) {
            download.cx = 0;
            if (++download.cy >= download.h) download.active = false;
        }
    }
    return out;
}

uint32_t gpu_stat() {
    uint32_t s = stat_texpage & 0x7FF;
    if (mask_set) s |= 1u << 11;
    if (mask_check) s |= 1u << 12;
    if (tex_disable) s |= 1u << 15;
    s |= ((disp_mode >> 6) & 1) << 16;   // hres2
    s |= (disp_mode & 3) << 17;          // hres1
    s |= ((disp_mode >> 2) & 1) << 19;   // vres
    s |= ((disp_mode >> 3) & 1) << 20;   // PAL
    s |= ((disp_mode >> 4) & 1) << 21;   // 24-bit
    s |= ((disp_mode >> 5) & 1) << 22;   // interlace
    if (display_disabled) s |= 1u << 23;
    if (irq_flag) s |= 1u << 24;
    s |= 1u << 26;                       // ready for command
    if (download.active) s |= 1u << 27;  // ready to send VRAM to CPU
    s |= 1u << 28;                       // ready for DMA block
    s |= (uint32_t)dma_dir << 29;
    switch (dma_dir) {
    case 1: case 2: s |= 1u << 25; break;
    case 3: if (download.active) s |= 1u << 25; break;
    default: break;
    }
    // odd/even line
    if (!vblank_now) {
        bool interlace = disp_mode & 0x20;
        if (interlace) s |= (uint32_t)(frame_count() & 1) << 31;
        else s |= (uint32_t)(hblank_count(now()) & 1) << 31;
    }
    if (disp_mode & 0x20) s |= (uint32_t)(frame_count() & 1) << 13;
    return s;
}

void gpu_vblank(bool in_vblank) { vblank_now = in_vblank; }

DisplayInfo gpu_display() {
    static const int hres[8] = {256, 320, 512, 640, 368, 368, 368, 368};
    DisplayInfo d;
    int idx = (disp_mode & 3) | ((disp_mode >> 4) & 4);
    d.w = (disp_mode & 0x40) ? 368 : hres[disp_mode & 3];
    (void)idx;
    int lines = v_end - v_start;
    if (lines <= 0 || lines > 256) lines = 240;
    d.h = (disp_mode & 0x24) == 0x24 ? lines * 2 : lines;
    d.x = disp_x;
    d.y = disp_y;
    d.rgb24 = disp_mode & 0x10;
    d.enabled = !display_disabled;
    return d;
}

const uint16_t* gpu_vram() { return vram; }

void gpu_init() {
    memset(vram, 0, sizeof vram);
    gpu_write_gp1(0);
}

}  // namespace psx
