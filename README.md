# CTR Static Recomp

A static recompilation of **Crash Team Racing (USA, SCUS-94426)** for Windows. The game's
MIPS R3000A code is translated to C at build time and linked with a runtime that emulates
the PS1 hardware underneath it. No game data or code is included: everything is read from
your own disc image when you build and run.

## Layout

| Path | What |
|---|---|
| `tools/extract.py` | pulls `SCUS_944.26` and code overlays 221–233 out of the disc image |
| `gen/recomp.py` | the recompiler: function discovery, jump tables, MIPS → C |
| `gen/mips.py` | instruction decoder shared by the tools |
| `runtime/` | CPU glue, interpreter fallback, HLE BIOS, GTE, GPU, SPU, CD-ROM, SIO, SDL frontend |
| `tools/disasm.py` | disassembler for the EXE and overlays |
| `tools/lockstep.ps1` | recompiled vs. interpreter lockstep comparison (see *Debugging*) |
| `tools/build_variant.ps1` | builds debug variants (precise timing, checks, native render asm) |

## How it works

* **Recompiled code.** Every function reachable from the entry point, the symbol list
  (`CTR-ModSDK/symbols/syms926.txt`), `jal` targets, and code pointers found in data becomes a
  C function operating on a `CPU` struct. Jump tables become `switch` statements; indirect
  calls go through `rt_call`, which looks the target up at runtime.
* **Overlays.** Overlays share load addresses (three regions). Calls into an overlay region
  check which overlay is actually resident (by comparing the function's code bytes with RAM).
* **Interpreter.** CTR's hand-written render assembly (register continuations, loops that
  return several levels at once, `ra`/`sp` used as data) and anything not recompiled runs on a
  MIPS interpreter that follows exact MIPS control flow. It is fast enough for full speed.
* **CTR's thread system.** `ThTick_SetAndExec` / `ThTick_FastRET` abandon the current tick and
  resume `ThTick_RunBucket`; the runtime models this with an unwind point at that call site.
  Non-local jumps use `rt_setjmp`/`rt_longjmp` (`runtime/jmp.c`), which preserve all Win64
  callee-saved registers including XMM6–15.
* **HLE BIOS.** The ~50 BIOS functions CTR uses are implemented natively, including the
  interrupt chain (run on a kernel stack) and PsyQ's `HookEntryInt` exit, events, and memory
  cards (`memcard1.mcd`, standard 128 KiB format).
* **Hardware.** Software GPU rasterizer, GTE, SPU (ADPCM voices, ADSR, reverb, XA input,
  capture buffers/IRQ), CD-ROM controller with XA-ADPCM, timers, DMA, controller port.

## Building

Requires Python 3, CMake, Ninja and llvm-mingw (`winget install Kitware.CMake
Ninja-build.Ninja MartinStorsjo.LLVM-MinGW.UCRT`). SDL2 is downloaded by CMake.

```
.\build.ps1 -Disc "D:\path\CTR - Crash Team Racing (USA).cue"
.\ctr.exe
```

The disc path is remembered in `ctr_disc.txt`. The log is written to `ctr.log`.

## Controls

Keyboard: arrows = D-pad, C = Cross, V = Circle, X = Square, Z = Triangle, Enter = Start,
Space = Select, LShift/RShift = L1/R1, LCtrl/RCtrl = L2/R2. F11 or Alt+Enter = fullscreen,
Tab = fast forward, Pause = pause, F9 = toggle 16:9 widescreen (saved in `ctr.cfg`).

Game controllers with an SDL mapping (Xbox/XInput, DualShock 4/DualSense, most common pads)
use the standard layout (A = Cross, B = Circle, X = Square, Y = Triangle). Joysticks without a
mapping (e.g. vJoy) use a generic layout: button 0 = Cross, 1 = Circle, 2 = Square,
3 = Triangle, 4/5 = L1/R1, 6/7 = L2/R2, 8 = Select, 9 = Start, stick/hat = D-pad.
Detected devices are listed in `ctr.log`.

## Widescreen

`--widescreen` or F9 switches to 16:9. It works like the CTR-ModSDK 16BY9 mod: the X row of the
camera's view-projection matrix is scaled by 3/4 so a wider field of view fits in the game's
512x216 buffer, which is then shown stretched to 16:9. The culling frustum and far-clip
distance are widened to match, so nothing pops in at the new edges. These patches are runtime
hooks on three instructions in `PushBuffer_SetMatrixVP` / `PushBuffer_UpdateFrustum`
(`runtime/hooks.cpp`, `HOOKS` in `gen/recomp.py`). 2D elements (HUD, menus, text) are not
corrected and appear 33% wider.

## Debugging tools

* `--frames N`, `--shot N file.bmp`, `--shot-every N dir`, `--turbo`, `--keys` (scripted
  input, e.g. `"2700:down,2780:cross,3950:cross:600"`): headless test runs.
* `--interp`: run everything on the interpreter (reference behaviour).
* `tools/lockstep.ps1`: builds with per-instruction cycle accounting, runs recompiled and
  interpreted execution, and reports the first frame whose RAM differs. `--trace A B file`
  logs every function entry with cycle counts for frames A..B to find the exact divergence.
* `--verify`: differential check of dynamic calls (native vs interpreter from same state).
* `-DRECOMP_CHECK_CALLS=ON` builds report callee-saved register clobbers and support
  `--watch-addr` / `--watch-value` store watchpoints.
* `--prim-log`: per-frame primitive buffer usage. `-v`: verbose log (CD, BIOS, SPU levels).
* Crashes print a host backtrace (`tools/symbolize.sh build/run_err.txt`) and dump RAM to
  `build/crash_ram.bin`.
