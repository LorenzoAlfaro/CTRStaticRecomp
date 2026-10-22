"""Compiled interpreter for the code that must not be recompiled as C functions.

CTR's hand-written render assembly (and the per-player level renderers in overlays
226-229) use register continuations, loops that return several call levels at once, and
ra/sp as data, so they run on the interpreter (runtime/cpu.cpp: interp_run), which follows
MIPS control flow exactly. This module compiles each such region into one C function that
does *exactly* what interp_run does for every instruction - same cycle charges, same
c->pc updates, same stop/depth checks, same event-check points, same transfers - but with
the decoding done at build time. Every instruction gets a label; branches and jumps
inside the region are gotos, indirect jumps go through a switch on the address, and
leaving the region hands the pc back to the interpreter.

Generated function signature (see FlatRegion in runtime/recomp.h):
    uint32_t fn(CPU* c, uint32_t pc, uint32_t stop, int* depth, int* done)
returns the pc where execution left the region, or sets *done when interp_run would have
returned (stop reached at depth 0).
"""
import re
import struct


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def insn_cycles(w):
    """MUST match runtime/cycles.h insn_cycles()."""
    op = w >> 26
    if (0x20 <= op <= 0x26) or op == 0x32:
        return 5
    if op == 0:
        f = w & 63
        if f in (0x18, 0x19):
            return 7
        if f in (0x1A, 0x1B):
            return 36
    if op == 0x12 and (w & (1 << 25)):
        return {0x01: 15, 0x06: 8, 0x0C: 6, 0x10: 8, 0x11: 8, 0x12: 8, 0x13: 19, 0x14: 13,
                0x16: 44, 0x1B: 17, 0x1C: 11, 0x1E: 14, 0x20: 30, 0x28: 5, 0x29: 8, 0x2A: 17,
                0x2D: 5, 0x2E: 6, 0x30: 23, 0x3D: 5, 0x3E: 5, 0x3F: 39}.get(w & 0x3F, 8)
    return 1


def is_control(w):
    """Instructions interp_run handles itself (with a delay slot)."""
    op = w >> 26
    if op == 0 and (w & 63) in (0x08, 0x09):
        return True
    return op in (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07)


def simple(w, a):
    """C for one non-control instruction, mirroring interp_exec_simple() exactly.
    Unusual instructions call the interpreter's own routine."""
    op = w >> 26
    rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    sa, imm = (w >> 6) & 31, w & 0xFFFF
    simm = s16(imm) & 0xFFFFFFFF
    # pending cycles must be in c->cycles before anything that can look at them
    fallback = f"FLUSH(); c->pc = {a:#010x}u; SPILL(); rt_flat_exec(c, {w:#010x}u, {a:#010x}u); RELOAD();"

    def wr(dst, expr):
        return f"r[{dst}] = {expr};" if dst else f"(void)({expr});"

    # memory operands: RAM and scratchpad never look at the cycle counter, hardware registers
    # (timers, CD, GPU status...) do, so flush first only when the address is not memory
    addr = "ea"
    mem = f"uint32_t ea = r[{rs}] + {simm:#x}u; if (!FLAT_FAST(ea)) FLUSH(); "
    if op == 0:
        f = w & 63
        ops = {
            0x00: f"r[{rt}] << {sa}", 0x02: f"r[{rt}] >> {sa}",
            0x03: f"(uint32_t)((int32_t)r[{rt}] >> {sa})",
            0x04: f"r[{rt}] << (r[{rs}] & 31)", 0x06: f"r[{rt}] >> (r[{rs}] & 31)",
            0x07: f"(uint32_t)((int32_t)r[{rt}] >> (r[{rs}] & 31))",
            0x10: "c->hi", 0x12: "c->lo",
            0x20: f"r[{rs}] + r[{rt}]", 0x21: f"r[{rs}] + r[{rt}]",
            0x22: f"r[{rs}] - r[{rt}]", 0x23: f"r[{rs}] - r[{rt}]",
            0x24: f"r[{rs}] & r[{rt}]", 0x25: f"r[{rs}] | r[{rt}]", 0x26: f"r[{rs}] ^ r[{rt}]",
            0x27: f"~(r[{rs}] | r[{rt}])",
            0x2A: f"(uint32_t)((int32_t)r[{rs}] < (int32_t)r[{rt}])", 0x2B: f"(uint32_t)(r[{rs}] < r[{rt}])",
        }
        if f in ops:
            return f"r[{rd}] = {ops[f]};" if rd else ";"
        if f == 0x11:
            return f"c->hi = r[{rs}];"
        if f == 0x13:
            return f"c->lo = r[{rs}];"
        if f == 0x18:
            return f"{{ int64_t p = (int64_t)(int32_t)r[{rs}] * (int32_t)r[{rt}]; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}"
        if f == 0x19:
            return f"{{ uint64_t p = (uint64_t)r[{rs}] * r[{rt}]; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}"
        if f == 0x1A:
            return f"DIV(c, r[{rs}], r[{rt}]);"
        if f == 0x1B:
            return f"DIVU(c, r[{rs}], r[{rt}]);"
        return fallback  # syscall, break, invalid
    if op in (0x08, 0x09):
        return f"r[{rt}] = r[{rs}] + {simm:#x}u;" if rt else ";"
    if op == 0x0A:
        return f"r[{rt}] = (uint32_t)((int32_t)r[{rs}] < (int32_t){simm:#x}u);" if rt else ";"
    if op == 0x0B:
        return f"r[{rt}] = (uint32_t)(r[{rs}] < {simm:#x}u);" if rt else ";"
    if op == 0x0C:
        return f"r[{rt}] = r[{rs}] & {imm:#x}u;" if rt else ";"
    if op == 0x0D:
        return f"r[{rt}] = r[{rs}] | {imm:#x}u;" if rt else ";"
    if op == 0x0E:
        return f"r[{rt}] = r[{rs}] ^ {imm:#x}u;" if rt else ";"
    if op == 0x0F:
        return f"r[{rt}] = {(imm << 16) & 0xFFFFFFFF:#x}u;" if rt else ";"
    if op == 0x12:  # COP2 (GTE)
        if rs & 0x10:
            return f"gte_command(c, {w & 0x1FFFFFF:#x}u);"
        if rs == 0:
            return wr(rt, f"gte_read_data(c, {rd})")
        if rs == 2:
            return wr(rt, f"gte_read_ctrl(c, {rd})")
        if rs == 4:
            return f"gte_write_data(c, {rd}, r[{rt}]);"
        if rs == 6:
            return f"gte_write_ctrl(c, {rd}, r[{rt}]);"
        return ";"
    loads = {
        0x20: f"(uint32_t)(int32_t)(int8_t)MEM_LB({addr})",
        0x21: f"(uint32_t)(int32_t)(int16_t)MEM_LH({addr})",
        0x22: f"LWL(r[{rt}], {addr})",
        0x23: f"MEM_LW({addr})",
        0x24: f"MEM_LB({addr})",
        0x25: f"MEM_LH({addr})",
        0x26: f"LWR(r[{rt}], {addr})",
    }
    if op in loads:
        return "{ " + mem + wr(rt, loads[op]) + " }"
    stores = {0x28: "MEM_SB", 0x29: "MEM_SH", 0x2A: "SWL", 0x2B: "MEM_SW", 0x2E: "SWR"}
    if op in stores:
        return "{ " + mem + f"{stores[op]}({addr}, r[{rt}]); }}"
    if op == 0x32:
        return "{ " + mem + f"gte_write_data(c, {rt}, MEM_LW({addr})); }}"
    if op == 0x3A:
        return "{ " + mem + f"MEM_SW({addr}, gte_read_data(c, {rt})); }}"
    return fallback  # COP0, invalid


CHUNK = 256  # instructions per generated C function (huge functions compile very slowly)


class FlatRegion:
    def __init__(self, name, lo, hi, words, ovr):
        self.name, self.lo, self.hi, self.words, self.ovr = name, lo, hi, words, ovr
        self.clo, self.chi = lo, hi  # the chunk being emitted

    def word(self, a):
        return self.words[(a - self.lo) >> 2]

    def inside(self, a):
        """Target reachable by goto: inside the chunk being emitted."""
        return self.clo <= a < self.chi and not (a & 3)

    def goto(self, t):
        """Continue at constant target t: a goto inside the chunk, else via the dispatcher."""
        return f"goto L_{t:08X};" if self.inside(t) else f"{{ pc = {t:#010x}u; goto dispatch; }}"

    def delay(self, a):
        """The delay slot at a + 4, as interp_run's exec_delay."""
        if a + 4 >= self.hi:
            return f"FLUSH(); SPILL(); rt_flat_exec_delay(c, {a:#010x}u); RELOAD();"
        w = self.word(a + 4)
        if is_control(w):  # interp_exec_simple treats it as invalid
            return (f"n += {insn_cycles(w)}; FLUSH(); c->pc = {a + 4:#010x}u; "
                    f"SPILL(); rt_flat_exec(c, {w:#010x}u, {a + 4:#010x}u); RELOAD();")
        return f"c->pc = {a + 4:#010x}u; n += {insn_cycles(w)}; {simple(w, a + 4)}"

    def in_region(self, t):
        return self.lo <= t < self.hi and not (t & 3)

    def xfer(self, target, ret, link):
        """interp_run's transfer(): BIOS vectors and recompiled functions run to completion,
        anything else continues there. The regions contain no recompiled functions or BIOS
        vectors, so a target inside this region only needs transfer's depth bookkeeping and
        can be reached directly (unless tracing, which transfer also does)."""
        slow = (f"SPILL(); pc = rt_flat_transfer(c, {target}, {ret:#010x}u, {1 if link else 0}, stop, &depth); "
                "RELOAD(); goto dispatch;")
        depth = f"if ({ret:#010x}u == stop) depth++; " if link else ""
        if isinstance(target, int):
            if self.in_region(target):
                return f"if (!g_trace_on) {{ {depth}{self.goto(target)} }} {slow}"
            return slow
        return (f"if (t - {self.lo:#010x}u < {self.hi - self.lo:#x}u && !(t & 3) && !g_trace_on) "
                f"{{ {depth}pc = t; goto dispatch; }} {slow}")

    def control(self, a, w):
        op = w >> 26
        rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        ret = a + 8
        if op == 0:  # jr / jalr
            link = (w & 63) == 0x09
            out = [f"t = r[{rs}];"]
            if link and rd:
                out.append(f"r[{rd}] = {ret:#010x}u;")
            out.append(self.delay(a))
            out.append("FLUSH();")
            if not link:
                out.append("if (t == stop && depth == 0) FINISH();")
                # returns to an outer native frame longjmp there: registers must be in c->r
                out.append("if (rt_flat_nonlocal_needed(t)) { SPILL(); rt_flat_nonlocal(t); }")
            else:
                out.append("CHK();")
            out.append(self.xfer("t", ret, link))
            return " ".join(out)
        if op in (0x02, 0x03):  # j / jal
            t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            jal = op == 0x03
            out = []
            if jal:
                out.append(f"r[31] = {ret:#010x}u;")
            out.append(self.delay(a))
            out.append("FLUSH();")
            if jal or t <= a:
                out.append("CHK();")
            elif not self.in_region(t):  # (no function entries inside the regions)
                out.append(f"if (rt_is_func_entry({t:#010x}u)) CHK();")
            out.append(self.xfer(t, ret, jal))
            return " ".join(out)
        # conditional branches: the condition is read before the delay slot runs
        simm = s16(w & 0xFFFF)
        target = (a + 4 + (simm << 2)) & 0xFFFFFFFF
        link = False
        if op == 0x01:
            cond = f"(int32_t)r[{rs}] >= 0" if rt & 1 else f"(int32_t)r[{rs}] < 0"
            link = (rt & 0x1E) == 0x10
        elif op == 0x04:
            cond = f"r[{rs}] == r[{rt}]"
        elif op == 0x05:
            cond = f"r[{rs}] != r[{rt}]"
        elif op == 0x06:
            cond = f"(int32_t)r[{rs}] <= 0"
        else:
            cond = f"(int32_t)r[{rs}] > 0"
        out = [f"taken = {cond};"]
        if link:
            out.append(f"r[31] = {ret:#010x}u;")
        out.append(self.delay(a))
        out.append(f"if (!taken) {self.goto(ret)}")
        if link:
            out.append("FLUSH(); CHK();")
            out.append(self.xfer(target, ret, True))
        else:
            if target <= a:
                out.append("FLUSH(); CHK();")
            out.append(self.goto(target))
        return " ".join(out)

    def emit_chunk(self, ci):
        """One chunk: instructions [clo, chi) with gotos between them. Returns the pc where
        execution leaves the chunk (*done set if interp_run would have returned)."""
        self.clo = self.lo + ci * CHUNK * 4
        self.chi = min(self.hi, self.clo + CHUNK * 4)
        n = (self.chi - self.clo) >> 2
        # instruction bodies first: the register set the chunk uses decides its prologue
        reg_ref = re.compile(r"\br\[(\d+)\]")
        lines = []
        for k in range(n):
            a = self.clo + 4 * k
            w = self.word(a)
            lines.append(f"L_{a:08X}:")
            lines.append(f"    if (sic && stop == {a:#010x}u) {{ if (depth == 0) FINISH(); depth--; }}")
            body = self.control(a, w) if is_control(w) else simple(w, a)
            pc_set = f"c->pc = {a:#010x}u; " if is_control(w) else ""
            lines.append(reg_ref.sub(r"R\1", f"    {pc_set}n += {insn_cycles(w)}; {body}"))
        text = "\n".join(lines)
        used = sorted({int(m) for m in re.findall(r"\bR(\d+)\b", text)} - {0})
        written = sorted({int(m) for m in re.findall(r"\bR(\d+) = ", text)} - {0})

        o = []
        o.append(f"static uint32_t {self.name}_{ci}(CPU* c, uint32_t pc, uint32_t stop, int* dp, int* done) {{")
        o.append("    int depth = *dp;")
        o.append("    uint32_t t; int taken; (void)t; (void)taken;")
        # The MIPS registers this chunk uses live in locals so the compiler can keep them in
        # host registers: c->r would have to be reloaded after every store, since any store
        # may alias it. The ones it writes go back to c->r (SPILL) before anything that can
        # look at or change c->r - calls, events, fallback instructions, longjmps, leaving
        # the chunk - and all of them are read again (RELOAD) after it.
        o.append("    const uint32_t R0 = 0; (void)R0;")
        if used:
            o.append("    uint32_t " + ", ".join(f"R{k} = c->r[{k}]" for k in used) + ";")
        o.append("#define SPILL() do { " + " ".join(f"c->r[{k}] = R{k};" for k in written) + " } while (0)")
        o.append("#define RELOAD() do { " + " ".join(f"R{k} = c->r[{k}];" for k in used) + " } while (0)")
        o.append("#define CHK() do { if (c->cycles >= c->next_event) { SPILL(); rt_check_events(c); RELOAD(); } } while (0)")
        # Cycles are counted in n and added to c->cycles (FLUSH) only where something can look
        # at them: hardware accesses, event checks, calls, leaving the chunk. Adding the same
        # total in one step gives the same result as interp_run's per-instruction rt_cyc.
        o.append("    uint32_t n = 0;")
        # the stop address can only match inside this chunk if it lies in it
        o.append(f"    const int sic = stop - {self.clo:#010x}u < {self.chi - self.clo:#x}u;")
        o.append("#define FLUSH() do { if (n) { rt_cyc(c, n); n = 0; } } while (0)")
        o.append("#define FINISH() do { FLUSH(); SPILL(); *dp = depth; *done = 1; return 0; } while (0)")
        o.append("    goto dispatch;")
        o.append(text)
        o.append(f"    pc = {self.chi:#010x}u;")
        o.append("dispatch:")
        o.append(f"    if (pc - {self.clo:#010x}u < {self.chi - self.clo:#x}u && !(pc & 3)) {{")
        o.append(f"        switch ((pc - {self.clo:#010x}u) >> 2) {{")
        for k in range(n):
            o.append(f"        case {k}: goto L_{self.clo + 4 * k:08X};")
        o.append("        }")
        o.append("    }")
        o.append("    FLUSH(); SPILL(); *dp = depth; *done = 0; return pc;")
        o.append("#undef FINISH")
        o.append("#undef FLUSH")
        o.append("#undef CHK")
        o.append("#undef RELOAD")
        o.append("#undef SPILL")
        o.append("}")
        return "\n".join(o)

    def emit(self):
        nchunks = ((self.hi - self.lo) // 4 + CHUNK - 1) // CHUNK
        o = [f"// {self.name}: {self.lo:08x}..{self.hi:08x}, {nchunks} chunks"]
        for ci in range(nchunks):
            o.append(self.emit_chunk(ci))
        o.append(f"static const FlatFunc {self.name}_chunks[] = {{")
        o.append("    " + ", ".join(f"{self.name}_{ci}" for ci in range(nchunks)))
        o.append("};")
        # region entry: run chunks until execution leaves the region
        o.append(f"uint32_t {self.name}(CPU* c, uint32_t pc, uint32_t stop, int* dp, int* done) {{")
        o.append(f"    while (pc - {self.lo:#010x}u < {self.hi - self.lo:#x}u && !(pc & 3)) {{")
        o.append(f"        pc = {self.name}_chunks[(pc - {self.lo:#010x}u) / {CHUNK * 4}](c, pc, stop, dp, done);")
        o.append("        if (*done) return 0;")
        o.append("    }")
        o.append("    *done = 0;")
        o.append("    return pc;")
        o.append("}")
        return "\n".join(o)


def regions(main_mod, modules, interp_only_main, interp_only_overlays):
    out = []
    for lo, hi in interp_only_main:
        words = [struct.unpack_from("<I", main_mod.data, a - main_mod.base)[0] for a in range(lo, hi, 4)]
        out.append(FlatRegion(f"FLAT_main_{lo:08X}", lo, hi, words, 0))
    for m in modules:
        if m.ovr in interp_only_overlays:
            lo, hi = m.base, m.base + (len(m.data) & ~3)
            words = [struct.unpack_from("<I", m.data, a - m.base)[0] for a in range(lo, hi, 4)]
            out.append(FlatRegion(f"FLAT_{m.ovr}_{lo:08X}", lo, hi, words, m.ovr))
    return out


def write(f, regs):
    f.write("// generated by gen/recomp.py (gen/flat.py) - do not edit\n#include \"recomp.h\"\n\n")
    for rg in regs:
        f.write(f"static const uint32_t {rg.name}_words[] = {{\n")
        for k in range(0, len(rg.words), 8):
            f.write("    " + ", ".join(f"{x:#010x}u" for x in rg.words[k:k + 8]) + ",\n")
        f.write("};\n\n")
        f.write(rg.emit())
        f.write("\n\n")
    f.write("const FlatRegion g_flat_regions[] = {\n")
    for rg in regs:
        f.write(f"    {{ {rg.lo:#010x}u, {rg.hi:#010x}u, {rg.ovr}, {rg.name}, {rg.name}_words }},\n")
    if not regs:
        f.write("    { 0, 0, 0, 0, 0 },\n")
    f.write("};\n")
    f.write(f"const int g_flat_region_count = {len(regs)};\n")
