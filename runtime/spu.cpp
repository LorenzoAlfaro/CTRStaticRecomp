// SPU: 24 ADPCM voices, ADSR, noise, pitch modulation, reverb, CD input, capture + IRQ.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "psx.h"

namespace psx {

bool g_spu_profile = false;
std::atomic<int64_t> g_spu_ns{0};
FILE* g_audio_dump = nullptr;
bool g_spu_thread = true;

static uint8_t spu_ram[512 * 1024];
static uint16_t regs[0x200];   // raw register file (offsets 0..0x3FF / 2), as the mixer sees it
static uint16_t mregs[0x200];  // the same as the CPU wrote it (CPU side)
static bool threaded = false;  // mixing on the audio thread (see below)
static uint32_t mix_irq_addr = 0;
static void probe_irq(uint32_t addr);
static void irq_hit(uint32_t addr);

static inline int16_t ram16(uint32_t a) { return *(int16_t*)(spu_ram + (a & 0x7FFFE)); }
static inline void ram16_set(uint32_t a, int16_t v) { *(int16_t*)(spu_ram + (a & 0x7FFFE)) = v; }

enum AdsrPhase { OFF, ATTACK, DECAY, SUSTAIN, RELEASE };

struct Voice {
    uint32_t cur_addr = 0;
    uint32_t counter = 0;
    int16_t decoded[28];
    int16_t prev[3] = {0, 0, 0};  // history for interpolation (last samples of previous block)
    int16_t hist[2] = {0, 0};     // ADPCM filter history
    bool block_valid = false;
    uint8_t block_flags = 0;
    AdsrPhase phase = OFF;
    int32_t level = 0;
    int32_t adsr_wait = 0;
    int16_t last_out = 0;
};
static Voice voices[24];
static uint32_t endx = 0;
static uint32_t transfer_addr = 0;
static uint32_t irq_addr = 0;
static bool irq_flag = false;
static uint32_t capture_pos = 0;
static int64_t last_cycles = 0;
static int64_t cycle_accum = 0;
static uint32_t noise_level = 1;
static int32_t noise_timer = 0;
static uint32_t reverb_addr = 0;
static bool reverb_odd = false;
static int32_t reverb_out_l = 0, reverb_out_r = 0;

// CD audio input ring buffer (44.1 kHz stereo)
struct CdRing {
    std::vector<int16_t> buf = std::vector<int16_t>(44100 * 2 * 2);
    size_t rd = 0, wr = 0;
    double frac = 0;
    int16_t last_l = 0, last_r = 0;
    void push(const int16_t* stereo, int frames, int rate);
    void pop(int32_t& l, int32_t& r) {
        l = r = 0;
        if (rd == wr) return;
        l = buf[rd];
        r = buf[rd + 1];
        rd += 2;
        if (rd == buf.size()) rd = 0;
    }
};
// The mixer's ring, and with the audio thread an identical one on the CPU side: CTR reads the
// CD capture buffers (SPU RAM 0x000-0x7FF) back by DMA a few times per frame, and their
// contents depend only on the CD input and the sample clock, so the CPU side keeps its own
// copy of them (cap_cpu) and answers those reads without waiting for the mixer.
static CdRing cd_mix, cd_cpu;
static int16_t cap_cpu[0x400];  // SPU RAM 0x000..0x7FF (CD left, CD right capture buffers)
// set for good once reverb writes may reach that area (the only other mixer writes there)
static bool cap_unsafe = false;
static inline void cap_set(uint32_t a, int16_t v) {  // a CPU write to SPU RAM
    a &= 0x7FFFE;
    if (a < 0x800) cap_cpu[a >> 1] = v;
}

static std::vector<int16_t> out_buf;

static inline uint16_t reg(uint32_t off) { return regs[off >> 1]; }
static inline uint16_t spucnt() { return reg(0x1AA); }

// IRQ check for the mixer's own SPU RAM accesses
static inline void check_irq(uint32_t addr) {
    if (threaded) probe_irq(addr);
    else irq_hit(addr);
}

// ---- ADPCM ----------------------------------------------------------------------------------
static void decode_block(Voice& v) {
    static const int K0[5] = {0, 60, 115, 98, 122}, K1[5] = {0, 0, -52, -55, -60};
    uint32_t a = v.cur_addr & 0x7FFF8;
    check_irq(a);
    uint8_t hdr = spu_ram[a], flags = spu_ram[a + 1];
    int shift = hdr & 0xF;
    if (shift > 12) shift = 9;
    int f = std::min((hdr >> 4) & 7, 4);
    for (int i = 0; i < 28; i++) {
        uint8_t b = spu_ram[a + 2 + i / 2];
        int nib = (i & 1) ? (b >> 4) : (b & 0xF);
        int s = (int16_t)(nib << 12) >> shift;
        s += (v.hist[0] * K0[f] + v.hist[1] * K1[f] + 32) >> 6;
        s = std::clamp(s, -32768, 32767);
        v.hist[1] = v.hist[0];
        v.hist[0] = (int16_t)s;
        v.decoded[i] = (int16_t)s;
    }
    v.block_flags = flags;
    v.block_valid = true;
}

// ---- ADSR -----------------------------------------------------------------------------------
static void adsr_tick(int vi) {
    Voice& v = voices[vi];
    if (v.phase == OFF) return;
    if (v.adsr_wait > 0) { v.adsr_wait--; return; }
    uint32_t lo = reg(vi * 16 + 8), hi = reg(vi * 16 + 10);
    bool exp = false, dec = false;
    int shift = 0, step = 0;
    switch (v.phase) {
    case ATTACK:
        exp = lo & 0x8000; dec = false; shift = (lo >> 10) & 31; step = 7 - ((lo >> 8) & 3);
        break;
    case DECAY:
        exp = true; dec = true; shift = (lo >> 4) & 15; step = -8;
        break;
    case SUSTAIN:
        exp = hi & 0x8000; dec = hi & 0x4000; shift = (hi >> 8) & 31;
        step = dec ? -8 + ((hi >> 6) & 3) : 7 - ((hi >> 6) & 3);
        break;
    case RELEASE:
        exp = hi & 0x20; dec = true; shift = hi & 31; step = -8;
        break;
    default: break;
    }
    int cycles = 1 << std::max(0, shift - 11);
    int32_t s = step << std::max(0, 11 - shift);
    if (exp && !dec && v.level > 0x6000) cycles *= 4;
    if (exp && dec) s = (int32_t)((int64_t)s * v.level >> 15);
    v.adsr_wait = cycles - 1;
    v.level = std::clamp(v.level + s, 0, 0x7FFF);
    switch (v.phase) {
    case ATTACK:
        if (v.level >= 0x7FFF) { v.phase = DECAY; v.adsr_wait = 0; }
        break;
    case DECAY: {
        int32_t sl = (int32_t)(((lo & 15) + 1) * 0x800);
        if (v.level <= sl) { v.phase = SUSTAIN; v.adsr_wait = 0; }
        break;
    }
    case RELEASE:
        if (v.level <= 0) { v.phase = OFF; v.level = 0; }
        break;
    default: break;
    }
}

static void key_on(int vi) {
    Voice& v = voices[vi];
    v.cur_addr = (uint32_t)reg(vi * 16 + 6) * 8;
    v.counter = 0;
    v.hist[0] = v.hist[1] = 0;
    v.prev[0] = v.prev[1] = v.prev[2] = 0;
    v.block_valid = false;
    v.phase = ATTACK;
    v.level = 0;
    v.adsr_wait = 0;
    endx &= ~(1u << vi);
}
static void key_off(int vi) {
    if (voices[vi].phase != OFF) { voices[vi].phase = RELEASE; voices[vi].adsr_wait = 0; }
}

// ---- volume ---------------------------------------------------------------------------------
static inline int32_t fixed_vol(uint16_t r) {
    if (r & 0x8000) return (int16_t)(r << 1) >= 0 ? 0x7FFF : 0;
    return (int32_t)(int16_t)(r << 1);  // 15-bit volume -> 16-bit signed range
}

// ---- reverb ---------------------------------------------------------------------------------
static inline int16_t sat16(int32_t v) { return (int16_t)std::clamp(v, -32768, 32767); }
// reverb work area: base address (mBASE) to the end of SPU RAM, read once per step
static uint32_t rv_base, rv_size;
static bool rv_writes;
static inline uint32_t rv_addr(uint32_t off) {
    uint32_t a = reverb_addr + off;
    if (rv_size == 0) return rv_base;
    while (a >= 0x80000) a = rv_base + (a - 0x80000) % rv_size;
    return a & 0x7FFFE;
}
static inline int16_t rv_read(uint32_t off, int32_t delta = 0) { return ram16(rv_addr((off + (uint32_t)delta) & 0x7FFFF)); }
static inline void rv_write(uint32_t off, int32_t v) {
    if (rv_writes) {
        uint32_t a = rv_addr(off);
        check_irq(a);
        ram16_set(a, sat16(v));
    }
}

static void reverb_step(int32_t in_l, int32_t in_r) {
    rv_base = (uint32_t)reg(0x1A2) * 8;
    rv_size = 0x80000 - rv_base;
    rv_writes = spucnt() & 0x80;
    auto R = [](int i) { return (int32_t)(int16_t)regs[(0x1C0 >> 1) + i]; };
    auto A = [](int i) { return (uint32_t)regs[(0x1C0 >> 1) + i] * 8; };
    int32_t dAPF1 = (int32_t)A(0), dAPF2 = (int32_t)A(1);
    int32_t vIIR = R(2), vCOMB1 = R(3), vCOMB2 = R(4), vCOMB3 = R(5), vCOMB4 = R(6), vWALL = R(7);
    int32_t vAPF1 = R(8), vAPF2 = R(9);
    uint32_t mLSAME = A(10), mRSAME = A(11), mLCOMB1 = A(12), mRCOMB1 = A(13), mLCOMB2 = A(14), mRCOMB2 = A(15);
    uint32_t dLSAME = A(16), dRSAME = A(17), mLDIFF = A(18), mRDIFF = A(19), mLCOMB3 = A(20), mRCOMB3 = A(21);
    uint32_t mLCOMB4 = A(22), mRCOMB4 = A(23), dLDIFF = A(24), dRDIFF = A(25);
    uint32_t mLAPF1 = A(26), mRAPF1 = A(27), mLAPF2 = A(28), mRAPF2 = A(29);
    int32_t vLIN = R(30), vRIN = R(31);

    int32_t Lin = (vLIN * in_l) >> 15, Rin = (vRIN * in_r) >> 15;
    auto same = [&](uint32_t m, uint32_t d, int32_t in) {
        int32_t prev = rv_read(m, -2);
        int32_t v = (((in + ((rv_read(d) * vWALL) >> 15) - prev) * vIIR) >> 15) + prev;
        rv_write(m, v);
    };
    same(mLSAME, dLSAME, Lin);
    same(mRSAME, dRSAME, Rin);
    same(mLDIFF, dRDIFF, Lin);
    same(mRDIFF, dLDIFF, Rin);
    int32_t Lout = ((vCOMB1 * rv_read(mLCOMB1)) >> 15) + ((vCOMB2 * rv_read(mLCOMB2)) >> 15) +
                   ((vCOMB3 * rv_read(mLCOMB3)) >> 15) + ((vCOMB4 * rv_read(mLCOMB4)) >> 15);
    int32_t Rout = ((vCOMB1 * rv_read(mRCOMB1)) >> 15) + ((vCOMB2 * rv_read(mRCOMB2)) >> 15) +
                   ((vCOMB3 * rv_read(mRCOMB3)) >> 15) + ((vCOMB4 * rv_read(mRCOMB4)) >> 15);
    auto apf = [&](int32_t x, uint32_t m, int32_t d, int32_t v) {
        int32_t old = rv_read(m, -d);
        int32_t t = sat16(x - ((v * old) >> 15));
        rv_write(m, t);
        return sat16(((t * v) >> 15) + old);
    };
    Lout = apf(Lout, mLAPF1, dAPF1, vAPF1);
    Rout = apf(Rout, mRAPF1, dAPF1, vAPF1);
    Lout = apf(Lout, mLAPF2, dAPF2, vAPF2);
    Rout = apf(Rout, mRAPF2, dAPF2, vAPF2);
    reverb_out_l = (Lout * (int16_t)reg(0x184)) >> 15;
    reverb_out_r = (Rout * (int16_t)reg(0x186)) >> 15;
    uint32_t base = (uint32_t)reg(0x1A2) * 8;
    reverb_addr = std::max(base, (reverb_addr + 2) & 0x7FFFE);
    if (reverb_addr < base) reverb_addr = base;
}

// ---- one output sample ---------------------------------------------------------------------
static void noise_tick() {
    uint16_t cnt = spucnt();
    int shift = (cnt >> 10) & 15, step = ((cnt >> 8) & 3) + 4;
    noise_timer -= step;
    int period = 0x20000 >> shift;
    if (noise_timer < 0) {
        noise_timer += period;
        if (noise_timer < 0) noise_timer += period;
        uint32_t parity = ((noise_level >> 15) ^ (noise_level >> 12) ^ (noise_level >> 11) ^ (noise_level >> 10) ^ 1) & 1;
        noise_level = ((noise_level << 1) | parity) & 0xFFFF;
    }
}

static void sample_tick() {
    uint16_t cnt = spucnt();
    int32_t dry_l = 0, dry_r = 0, rev_l = 0, rev_r = 0;
    uint32_t pmon = reg(0x190) | ((uint32_t)reg(0x192) << 16);
    uint32_t non = reg(0x194) | ((uint32_t)reg(0x196) << 16);
    uint32_t eon = reg(0x198) | ((uint32_t)reg(0x19A) << 16);
    noise_tick();
    int16_t voice_out[24] = {};
    for (int vi = 0; vi < 24; vi++) {
        Voice& v = voices[vi];
        if (v.phase == OFF && v.level == 0) {
            regs[(vi * 16 + 12) >> 1] = 0;
            continue;
        }
        if (!v.block_valid) decode_block(v);
        int idx = v.counter >> 12;  // 0..27: the advance below keeps it inside the block
        int frac = (v.counter >> 4) & 0xFF;
        int32_t s0 = v.decoded[idx], s1 = v.decoded[idx < 27 ? idx + 1 : 27];
        int32_t s = s0 + (((s1 - s0) * frac) >> 8);
        if (non & (1u << vi)) s = (int16_t)noise_level;
        s = (s * v.level) >> 15;
        voice_out[vi] = (int16_t)s;
        v.last_out = (int16_t)s;
        regs[(vi * 16 + 12) >> 1] = (uint16_t)v.level;
        int32_t l = (s * fixed_vol(reg(vi * 16 + 0))) >> 15;
        int32_t r = (s * fixed_vol(reg(vi * 16 + 2))) >> 15;
        dry_l += l;
        dry_r += r;
        if (eon & (1u << vi)) { rev_l += l; rev_r += r; }

        // advance
        uint32_t step = reg(vi * 16 + 4);
        if ((pmon & (1u << vi)) && vi > 0) {
            int32_t f = voice_out[vi - 1] + 0x8000;
            step = (uint32_t)((int32_t)(int16_t)step * f >> 15) & 0xFFFF;
        }
        step = std::min<uint32_t>(step, 0x3FFF);
        v.counter += step;
        while ((v.counter >> 12) >= 28) {
            v.counter -= 28 << 12;
            v.prev[0] = v.decoded[25]; v.prev[1] = v.decoded[26]; v.prev[2] = v.decoded[27];
            uint8_t flags = v.block_flags;
            if (flags & 4) regs[(vi * 16 + 14) >> 1] = (uint16_t)((v.cur_addr & 0x7FFF8) / 8);
            if (flags & 1) {
                endx |= 1u << vi;
                if (flags & 2) v.cur_addr = (uint32_t)reg(vi * 16 + 14) * 8;
                else { v.phase = OFF; v.level = 0; v.cur_addr += 16; }
            } else {
                v.cur_addr += 16;
            }
            v.cur_addr &= 0x7FFFF;
            decode_block(v);
            // the loop-start flag of the *new* block sets the repeat address
            if (v.block_flags & 4) regs[(vi * 16 + 14) >> 1] = (uint16_t)((v.cur_addr & 0x7FFF8) / 8);
        }
        adsr_tick(vi);
    }

    // CD audio input
    int32_t cd_l = 0, cd_r = 0;
    cd_mix.pop(cd_l, cd_r);
    // capture buffers (CD L/R, voice 1, voice 3)
    ram16_set(0x000 + capture_pos * 2, (int16_t)cd_l);
    ram16_set(0x400 + capture_pos * 2, (int16_t)cd_r);
    ram16_set(0x800 + capture_pos * 2, voice_out[1]);
    ram16_set(0xC00 + capture_pos * 2, voice_out[3]);
    if (!threaded) {  // otherwise checked on the CPU side (spu_run_until)
        irq_hit(0x000 + capture_pos * 2);
        irq_hit(0x400 + capture_pos * 2);
        irq_hit(0x800 + capture_pos * 2);
        irq_hit(0xC00 + capture_pos * 2);
    }
    capture_pos = (capture_pos + 1) & 0x1FF;
    if (cnt & 1) {
        int32_t cl = (cd_l * (int16_t)reg(0x1B0)) >> 15, cr = (cd_r * (int16_t)reg(0x1B2)) >> 15;
        dry_l += cl;
        dry_r += cr;
        if (cnt & 4) { rev_l += cl; rev_r += cr; }
    }

    // reverb at 22.05 kHz
    reverb_odd = !reverb_odd;
    if (reverb_odd) reverb_step(sat16(rev_l), sat16(rev_r));

    int32_t out_l = dry_l + reverb_out_l, out_r = dry_r + reverb_out_r;
    out_l = (sat16(out_l) * fixed_vol(reg(0x180))) >> 15;
    out_r = (sat16(out_r) * fixed_vol(reg(0x182))) >> 15;
    if (!(cnt & 0x4000) || !(cnt & 0x8000)) out_l = out_r = 0;  // muted / disabled
    out_buf.push_back(sat16(out_l));
    out_buf.push_back(sat16(out_r));
    if (g_log_level >= LOG_DEBUG) {
        static int64_t n = 0, peak = 0, cd_peak = 0, voices_on = 0;
        peak = std::max<int64_t>(peak, std::abs(sat16(out_l)));
        cd_peak = std::max<int64_t>(cd_peak, std::abs(cd_l));
        int on = 0;
        for (auto& v : voices) on += v.phase != OFF;
        voices_on = std::max<int64_t>(voices_on, on);
        if (++n == 44100 * 2) {
            LOGD("SPU: peak %lld, CD input peak %lld, max active voices %lld, spucnt %04x", (long long)peak,
                 (long long)cd_peak, (long long)voices_on, cnt);
            n = peak = cd_peak = voices_on = 0;
        }
    }
    if (out_buf.size() >= 1024) {
        if (g_audio_dump) fwrite(out_buf.data(), 2, out_buf.size(), g_audio_dump);
        frontend_audio(out_buf.data(), (int)out_buf.size() / 2);
        out_buf.clear();
    }
}


// ---- audio thread -----------------------------------------------------------------------------
// With the audio thread on, the CPU side (main thread) keeps only what the game can observe
// without the mixer: the register file as written, the sample clock, the capture position and
// the IRQ (CTR points it at the capture buffer, whose position follows the clock alone). The
// mixer runs behind on its own thread: each register / SPU RAM / CD input write is queued
// with the sample count at which it happened and applied between the same two samples as
// when mixing inline, so the output is identical. Reads of mixer state (voice registers,
// ENDX, voice output) and SPU RAM DMA reads wait for the mixer to catch up first.
// IRQ hits from the mixer's own accesses (voice blocks, reverb writes) can't be delivered at
// the right moment from the audio thread; they are counted and reported (none in CTR).
static uint64_t m_samples = 0;     // samples elapsed (CPU side)
static uint32_t m_capture_pos = 0;  // capture position (CPU side)

enum CmdKind { CMD_REG, CMD_RAM16, CMD_DMA, CMD_CD };
struct Cmd {
    uint64_t at;  // apply after this many samples
    CmdKind kind;
    uint32_t a, b;
    std::vector<int16_t> data;
};
static std::mutex mu;
static std::condition_variable cv_work, cv_idle;
static std::deque<Cmd> cmds;
static uint64_t target = 0, w_samples = 0;  // under mu
static bool w_waiting = false;
static std::atomic<uint64_t> probe_hits{0};

static void probe_irq(uint32_t addr) {
    if ((spucnt() & 0x40) && (addr & 0x7FFF8) == (mix_irq_addr & 0x7FFF8) &&
        probe_hits.fetch_add(1, std::memory_order_relaxed) == 0)
        LOGW("SPU: IRQ address %05x hit by the mixer on the audio thread; its timing may be off (use --no-spu-thread)",
             mix_irq_addr);
}

static void mixer_write(uint32_t off, uint16_t v) {
    regs[off >> 1] = v;
    switch (off) {
    case 0x188: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_on(i); break;
    case 0x18A: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_on(16 + i); break;
    case 0x18C: for (int i = 0; i < 16; i++) if (v & (1 << i)) key_off(i); break;
    case 0x18E: for (int i = 0; i < 8; i++) if (v & (1 << i)) key_off(16 + i); break;
    case 0x1A2: reverb_addr = (uint32_t)v * 8; break;
    case 0x1A4: mix_irq_addr = (uint32_t)v * 8; break;
    default: break;
    }
}

void CdRing::push(const int16_t* stereo, int frames, int rate) {
    // resample to 44.1 kHz (linear) into the ring buffer
    double step = (double)rate / 44100.0;
    double pos = frac;
    while (pos < frames) {
        int i = (int)pos;
        double f = pos - i;
        int16_t l0 = i == 0 ? last_l : stereo[2 * (i - 1)], r0 = i == 0 ? last_r : stereo[2 * (i - 1) + 1];
        int16_t l1 = stereo[2 * i], r1 = stereo[2 * i + 1];
        size_t next = (wr + 2) % buf.size();
        if (next == rd) break;  // full
        buf[wr] = (int16_t)(l0 + (l1 - l0) * f);
        buf[wr + 1] = (int16_t)(r0 + (r1 - r0) * f);
        wr = next;
        pos += step;
    }
    frac = pos - frames;
    last_l = stereo[2 * (frames - 1)];
    last_r = stereo[2 * (frames - 1) + 1];
}
static void cd_push(const int16_t* stereo, int frames, int rate) { cd_mix.push(stereo, frames, rate); }

static void apply(const Cmd& c) {
    switch (c.kind) {
    case CMD_REG: mixer_write(c.a, (uint16_t)c.b); break;
    case CMD_RAM16: ram16_set(c.a, (int16_t)c.b); break;
    case CMD_DMA: {
        uint32_t a = c.a;
        for (size_t i = 0; i < c.data.size(); i += 2) {
            ram16_set(a, c.data[i]);
            ram16_set(a + 2, c.data[i + 1]);
            a = (a + 4) & 0x7FFFF;
        }
        break;
    }
    case CMD_CD: cd_push(c.data.data(), (int)c.data.size() / 2, (int)c.a); break;
    }
}

static void post(Cmd&& c) {
    if (!threaded) { apply(c); return; }
    c.at = m_samples;
    std::lock_guard<std::mutex> lk(mu);
    cmds.push_back(std::move(c));
}

static void mix_samples(uint64_t n) {
    auto t0 = g_spu_profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    for (uint64_t i = 0; i < n; i++) sample_tick();
    if (g_spu_profile)
        g_spu_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

static void audio_thread() {
    std::unique_lock<std::mutex> lk(mu);
    for (;;) {
        if (!cmds.empty() && cmds.front().at <= w_samples) {
            Cmd c = std::move(cmds.front());
            cmds.pop_front();
            lk.unlock();
            apply(c);
            lk.lock();
            continue;
        }
        uint64_t end = target;
        if (!cmds.empty()) end = std::min(end, cmds.front().at);
        if (w_samples < end) {
            uint64_t n = end - w_samples;
            lk.unlock();
            mix_samples(n);
            lk.lock();
            w_samples += n;
            continue;
        }
        w_waiting = true;
        cv_idle.notify_all();
        cv_work.wait(lk);
        w_waiting = false;
    }
}

// wait until the mixer has caught up with the CPU side
uint64_t g_spu_drains = 0;     // waits for the audio thread to catch up (profiling)
int64_t g_spu_drain_ns = 0;
static void drain() {
    if (!threaded) return;
    std::unique_lock<std::mutex> lk(mu);
    auto caught_up = [] { return w_waiting && w_samples == target && cmds.empty(); };
    if (caught_up()) return;
    auto t0 = std::chrono::steady_clock::now();
    g_spu_drains++;
    cv_work.notify_one();
    cv_idle.wait(lk, caught_up);
    g_spu_drain_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}
void spu_flush() { drain(); }
// frame end: wake the mixer for whatever is pending, so no sound waits through the pause until
// the next frame (the batching in spu_run_until would leave up to 256 samples, and the audio
// device runs dry)
void spu_kick() {
    if (!threaded) return;
    std::lock_guard<std::mutex> lk(mu);
    if (w_waiting && target > w_samples) cv_work.notify_one();
}

// CPU-side IRQ check (transfers; the capture buffer when the mixer is threaded)
static void irq_hit(uint32_t addr) {
    if (!(mregs[0x1AA >> 1] & 0x40) || irq_flag) return;
    if ((addr & 0x7FFF8) == (irq_addr & 0x7FFF8)) {
        irq_flag = true;
        irq_raise(IRQ_SPU);
    }
}

void spu_run_until(int64_t cycles) {
    int64_t delta = cycles - last_cycles;
    if (delta <= 0) return;
    last_cycles = cycles;
    cycle_accum += delta;
    if (cycle_accum < 768) return;
    uint64_t n = (uint64_t)(cycle_accum / 768);
    cycle_accum -= (int64_t)n * 768;
    if (!threaded) {
        mix_samples(n);
        m_samples += n;
        m_capture_pos = (m_capture_pos + (uint32_t)n) & 0x1FF;
        return;
    }
    for (uint64_t i = 0; i < n; i++) {
        uint32_t p = m_capture_pos * 2;
        int32_t cl, cr;  // the CD input capture, as the mixer will write it for this sample
        cd_cpu.pop(cl, cr);
        cap_cpu[m_capture_pos] = (int16_t)cl;
        cap_cpu[0x200 + m_capture_pos] = (int16_t)cr;
        irq_hit(p);
        irq_hit(0x400 + p);
        irq_hit(0x800 + p);
        irq_hit(0xC00 + p);
        m_capture_pos = (m_capture_pos + 1) & 0x1FF;
    }
    m_samples += n;
    std::lock_guard<std::mutex> lk(mu);
    target = m_samples;
    if (w_waiting && target - w_samples >= 256) cv_work.notify_one();  // wake in batches
}

static void ev_spu(int64_t t) {
    spu_run_until(t);
    schedule(EV_SPU, t + 768 * 32);
}

// ---- register interface --------------------------------------------------------------------
uint16_t spu_read16(uint32_t off) {
    spu_run_until(now());
    off &= 0x3FE;
    if (off < 0x180 || off == 0x19C || off == 0x19E || (off >= 0x200 && off < 0x260)) {
        // mixer state: voice registers (envelope, repeat address), ENDX, voice output
        static int logged = 0;
        if (threaded && logged < 8) { logged++; LOGI("SPU: the game reads mixer register %03x (audio thread waits)", off); }
        drain();
        if (off == 0x19C) return (uint16_t)endx;
        if (off == 0x19E) return (uint16_t)(endx >> 16);
        if (off >= 0x200) return (uint16_t)voices[(off - 0x200) / 4].last_out;
        return reg(off);
    }
    if (off == 0x1AE) {
        uint16_t cnt = mregs[0x1AA >> 1];
        uint16_t s = cnt & 0x3F;
        if (irq_flag) s |= 0x40;
        uint32_t mode = (cnt >> 4) & 3;
        if (mode == 2) s |= 0x80 | 0x100;
        if (mode == 3) s |= 0x80 | 0x200;
        if (m_capture_pos >= 0x100) s |= 0x800;
        return s;
    }
    if (off == 0x1A8) return 0;
    return mregs[off >> 1];
}

void spu_write16(uint32_t off, uint16_t v) {
    spu_run_until(now());
    off &= 0x3FE;
    mregs[off >> 1] = v;
    if (!cap_unsafe && (mregs[0x1AA >> 1] & 0x80) && (uint32_t)mregs[0x1A2 >> 1] * 8 < 0x800) {
        cap_unsafe = true;
        LOGI("SPU: reverb area %05x reaches the capture buffers: capture reads wait for the mixer", mregs[0x1A2 >> 1] * 8);
    }
    switch (off) {
    case 0x1A4: irq_addr = (uint32_t)v * 8; break;
    case 0x1A6: transfer_addr = (uint32_t)v * 8; break;
    case 0x1A8:
        irq_hit(transfer_addr);
        cap_set(transfer_addr, (int16_t)v);
        post({0, CMD_RAM16, transfer_addr, v, {}});
        transfer_addr = (transfer_addr + 2) & 0x7FFFF;
        break;
    case 0x1AA:
        if (!(v & 0x40)) irq_flag = false;
        break;
    default: break;
    }
    post({0, CMD_REG, off, v, {}});
}

void spu_dma_write(const uint32_t* src, int words) {
    spu_run_until(now());
    Cmd c{0, CMD_DMA, transfer_addr, 0, std::vector<int16_t>((size_t)words * 2)};
    for (int i = 0; i < words; i++) {
        irq_hit(transfer_addr);
        c.data[2 * i] = (int16_t)src[i];
        c.data[2 * i + 1] = (int16_t)(src[i] >> 16);
        cap_set(transfer_addr, c.data[2 * i]);
        cap_set(transfer_addr + 2, c.data[2 * i + 1]);
        transfer_addr = (transfer_addr + 4) & 0x7FFFF;
    }
    post(std::move(c));
}

void spu_dma_read(uint32_t* dst, int words) {
    if (threaded && !cap_unsafe && (transfer_addr & 0x7FFFF) + (uint32_t)words * 4 <= 0x800) {
        // the CD capture buffers: the CPU side's copy is current (no waiting for the mixer)
        for (int i = 0; i < words; i++) {
            irq_hit(transfer_addr);
            uint32_t lo = (transfer_addr & 0x7FE) >> 1, hi = ((transfer_addr + 2) & 0x7FE) >> 1;
            dst[i] = (uint16_t)cap_cpu[lo] | ((uint32_t)(uint16_t)cap_cpu[hi] << 16);
            transfer_addr = (transfer_addr + 4) & 0x7FFFF;
        }
        return;
    }
    static int logged = 0;
    if (threaded && logged < 8) { logged++; LOGI("SPU: DMA read of %d words at %05x (audio thread waits)", words, transfer_addr); }
    drain();
    for (int i = 0; i < words; i++) {
        irq_hit(transfer_addr);
        dst[i] = (uint16_t)ram16(transfer_addr) | ((uint32_t)(uint16_t)ram16(transfer_addr + 2) << 16);
        transfer_addr = (transfer_addr + 4) & 0x7FFFF;
    }
}

void spu_cd_audio(const int16_t* stereo, int frames, int rate) {
    if (threaded) cd_cpu.push(stereo, frames, rate);  // the mixer applies the same push at this sample
    post({0, CMD_CD, (uint32_t)rate, 0, std::vector<int16_t>(stereo, stereo + (size_t)frames * 2)});
}

void spu_init() {
    memset(spu_ram, 0, sizeof spu_ram);
    memset(regs, 0, sizeof regs);
    memset(mregs, 0, sizeof mregs);
    register_event(EV_SPU, ev_spu);
    schedule(EV_SPU, 768 * 32);
    threaded = g_spu_thread && std::thread::hardware_concurrency() >= 4;
    if (threaded) std::thread(audio_thread).detach();
    LOGI("SPU: mixing on %s", threaded ? "its own thread" : "the CPU thread");
}

}  // namespace psx
