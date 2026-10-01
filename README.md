# CTR Static Recomp

A static recompilation of **Crash Team Racing** (PlayStation, USA, SCUS-94426) for Windows.
The game's original MIPS R3000A machine code is translated to C at build time and linked with
a runtime that emulates the PS1 hardware underneath it, so the game runs as a native program
rather than in an emulator.

**No game code or data is included.** You need your own disc image: the build extracts the
code from it, and the game reads its data from it at run time.

This is not a decompilation and does not use decompiled source. The game logic comes from the
original binary (see *Credits* for what the CTR-ModSDK decomp project contributed).
[`docs/RECOMP_LESSONS.md`](docs/RECOMP_LESSONS.md) records the problems hit while building it,
for anyone writing a recompiler.

## Status

* Boots through the intros, title and menus and plays races at full speed with music, sound
  effects and XA audio. Tested so far: the menus, single races in arcade mode, and entering
  adventure mode. Other modes, multiplayer and memory card saves (`memcard1.mcd`) have not
  been tested yet.
* 4x internal resolution with sub-pixel vertices (no wobble) by default, optional 16:9
  widescreen, keyboard and controller support (see below).
* Not implemented: FMV playback (MDEC), the demo disc (`LoadExec`), the analog-pad protocol
  (analog sticks are mapped to the D-pad), more than one controller port.
* Windows x64 only for now (`runtime/jmp.c` is Win64 assembly; everything else is portable
  C/C++ with SDL2).

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

* **Recompiled code.** Every function reachable from the entry point, the symbol map
  (CTR-ModSDK `symbols/syms926.txt`), `jal` targets, and code pointers found in data becomes a
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

Run `build.ps1` from PowerShell 7 (`pwsh`). The disc path is remembered in `ctr_disc.txt`; the
log is written to `ctr.log`. The build downloads the CTR-ModSDK symbol map from a pinned commit
into `data/`; if the download fails it builds without it. Only the USA release (SCUS-94426) is
supported. At startup the executable on the disc is checked against the one that was
recompiled, so a build only runs with the disc it was built from; a modified disc needs its own
build.

## Controls

Keyboard: arrows = D-pad, C = Cross, V = Circle, X = Square, Z = Triangle, Enter = Start,
Space = Select, LShift/RShift = L1/R1, LCtrl/RCtrl = L2/R2. F11 or Alt+Enter = fullscreen,
Tab = fast forward, Pause = pause, F9 = 16:9 widescreen, F10 = internal resolution
(1x/2x/4x), F8 = PGXP sub-pixel vertices, F7 = dithering. Settings are saved in `ctr.cfg`.

Game controllers with an SDL mapping (Xbox/XInput, DualShock 4/DualSense, most common pads)
use the standard layout (A = Cross, B = Circle, X = Square, Y = Triangle). Joysticks without a
mapping (e.g. vJoy) use a generic layout: button 0 = Cross, 1 = Circle, 2 = Square,
3 = Triangle, 4/5 = L1/R1, 6/7 = L2/R2, 8 = Select, 9 = Start, stick/hat = D-pad.
All connected controllers are read at once (their inputs are combined), so a virtual device like vJoy
does not block a real pad. Detected devices are listed in `ctr.log`; controllers keep working
when the window is not focused.

## HD rendering

The GPU renders at 4x the native resolution by default (2048x864 for CTR's 512x216 screen).
`--scale 1|2|4|8` or F10 changes it; `--scale 1 --no-pgxp` gives the original output exactly.

* **Upscaled VRAM.** VRAM is kept at the internal resolution, each native pixel an NxN
  block. CPU transfers, texture and CLUT fetches use the top-left sample of each block, so the
  game sees the native 1024x512 VRAM while polygons, sprites and lines are drawn at full
  resolution (`runtime/gpu.cpp`).
* **PGXP.** The GTE rounds projected vertices to whole pixels, which is what makes PS1 geometry
  wobble. RTPS/RTPT record the exact position of every vertex they output, keyed by the rounded
  value; the GPU looks vertices up when they arrive and rasterizes with 1/256-pixel precision
  (`runtime/pgxp.cpp`). A wrong match is off by less than one native pixel. `--no-pgxp` or F8
  turns it off.
* **Multithreaded.** Primitives are queued and rendered by worker threads (each owns
  interleaved 8-line bands) while the emulated CPU keeps running. Hazard tracking on 16x8 tiles
  drains the queue only when a primitive reads VRAM that queued work writes (render-to-texture)
  or writes VRAM that queued work reads, so results are identical to single-threaded
  rendering. Display conversion runs on the workers too.
* **Dithering** uses the native 4x4 pattern scaled up; `--no-dither` or F7 turns it off.

Performance on a Ryzen 5 3600 during a race: about 8 ms per frame at 4x (well within the
16.7 ms of a 60 Hz frame). `CTR_PROFILE=1` logs a per-frame breakdown every 300 frames,
`CTR_GPU_THREADS=N` sets the number of render threads.

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

## Credits

* [CTR-ModSDK](https://github.com/CTR-tools/CTR-ModSDK) (decompilation project, GPL-3.0): the
  symbol map used for function names and extra discovery seeds (downloaded at build time, not
  redistributed), documentation of the game's systems, and the 16BY9 widescreen mod that
  showed which camera functions to patch.
* [psx-spx](https://psx-spx.consoledev.net/) (PlayStation hardware documentation): GTE, GPU,
  SPU, CD-ROM and timing behaviour.
* [SDL2](https://www.libsdl.org/) for video, audio and input.

Crash Team Racing is a trademark of its respective owners. This project is not affiliated with
or endorsed by them, and is intended for use with a legally owned copy of the game.

## License

The code in this repository is released under the [MIT License](LICENSE).
