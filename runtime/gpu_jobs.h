// Render jobs: what the GPU command decoder (gpu.cpp, emulation thread) hands to the
// renderers, the software rasterizer's worker threads or the OpenGL ES thread (gpu_gl.cpp).
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

#include "psx.h"

namespace psx {

constexpr int SUB = 8;  // sub-pixel bits of vertex positions (internal-resolution pixels)

struct Vertex {
    int32_t x, y;  // internal-resolution position, SUB fractional bits
    int nx, ny;    // native integer position (for the GPU's size limits)
    int r, g, b;
    int u, v;
};

// Everything a primitive needs, captured when it is queued (workers render it later).
struct DrawCtx {
    bool textured, raw, semi, gouraud;
    int semi_mode;
    int tp_x, tp_y, tp_depth;  // texpage base (pixels) and depth 0=4bit 1=8bit 2=15bit
    int clut_x, clut_y;
    bool do_dither;
    bool mask_set, mask_check;
    uint8_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int16_t da_x1, da_y1, da_x2, da_y2;
};

enum JobKind : uint8_t {
    JOB_TRI, JOB_RECT, JOB_LINE, JOB_FILL, JOB_CONVERT, JOB_COPY,
    // OpenGL renderer only
    JOB_UPLOAD,    // CPU -> VRAM: v[0] position, v[1] size (native), data (w*h pixels, owned)
    JOB_READBACK,  // VRAM -> the native mirror (gpu.cpp's vram): v[0] position, v[1] size
    JOB_PRESENT,   // show the display area: present
    JOB_SCALE,     // change the internal resolution: present.scale_shift
};

// JOB_PRESENT: where the frame goes, and optionally a copy for screenshots
struct PresentInfo {
    DisplayInfo disp;
    int win_w, win_h;              // drawable size
    int dst_x, dst_y, dst_w, dst_h;  // picture rectangle in the drawable (top-left origin)
    uint32_t* shot;                // if set: ARGB copy of the display area at internal resolution
    int shot_w, shot_h;
    bool swap;                     // present on screen (false: screenshot only)
    int scale_shift;               // JOB_SCALE
};

struct Job {
    JobKind kind;
    DrawCtx d;
    Vertex v[3];  // triangle / line vertices; rect: v[0].nx/ny pos, v[1].nx/ny size, v[0].u/v
    uint32_t* dst;  // JOB_CONVERT: ARGB output (v[0] = internal-res origin, v[1] = size)
    int pitch;      // JOB_COPY: v[0] = src, v[1] = dst, v[2] = size (native, nx/ny)
    std::vector<uint16_t>* data;  // JOB_UPLOAD
    PresentInfo present;
};

struct Rgn { int x0, y0, x1, y1; };  // native, exclusive end

// OpenGL ES renderer (gpu_gl.cpp), run on its own thread
bool gl_thread_init();            // make the context current, create resources; false on failure
void gl_run_job(const Job& j);
void gl_idle();                   // queue empty: send pending drawing to the GPU
void gl_stats_line(char* out, size_t size, int frames);  // profile line, resets the counters
extern uint16_t* g_gl_mirror;     // JOB_READBACK target (native 1024x512)
extern std::atomic<uint64_t> g_gl_presents_done;  // frames shown by the GL thread
extern int g_gl_shift;            // log2 of the GL renderer's internal resolution

// frontend side (gpu.cpp)
void gpu_use_gl();                // before gpu_init: render with OpenGL ES (main sets the context)
bool gpu_gl();
void gpu_gl_present(const PresentInfo& p);  // the display area is filled in; waits if p.shot

}  // namespace psx
