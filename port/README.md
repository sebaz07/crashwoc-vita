# Crash WOC native port (PC dev target + PS Vita)

This directory is a **separate build** from the matching GameCube decomp
(`configure.py` / `ninja`). It compiles the decompiled C with `-DPORT`, adds a
platform layer, and runs the game natively. The GameCube matching build is not
touched: every change in `src/` is under `#ifdef PORT` (or `PORT_W64`, which is
the identity outside the port).

> **Personal builds only.** The build reads *your* `main.dol` and dtk
> disassembly and generates `port_dol_data.c` (the DOL's data tables) and
> `port_recomp.c` (machine-translated game functions). Both end up inside the
> executable/VPK. Never commit them, never share the binaries or the VPK.

## Status (2026-10-28)

| Area | State |
|---|---|
| PC build (Linux x86-32, SDL2 + GL 2.1) | builds, boots, renders the hub, Crash/Coco animated, controllable; warps into level 1 (`a\volcano`) and plays |
| Vita build (VitaSDK, vitaGL) | runs on hardware: logos → hub → levels, controls work; hub 8–12 fps, levels 20–30 fps (VPK `980a2306`) |
| Audio | sound effects (translated sssfx.s on an emulated AX/AM/ARAM) and DTK music/voice streams, SDL output 48 kHz (`src/port_audio.c`) |
| Intro/title cut movie | plays on PC (logos, title menu, name entry); `.cut` byte-swapped in `gcutscn.c`; `CWOC_INTRO=0` skips |
| Memory card / saves | memcard.s translated on a file-backed CARD API (`src/port_card.c`): `<save dir>/card_a/` |
| Pause menu | works (RESUME / OPTIONS / QUIT, 3D text) |
| Performance | GX blocks batched into GL draws, vertices pre-transformed on the CPU, FTZ; `prof ms/frame` in the log |

## How it works

1. **Decompiled C, compiled natively** (`cwoc_objs`), with a force-included
   prelude (`include/port_prelude.h`) that supplies prototypes the decomp
   omitted (implicit declarations break float arguments on x86/ARM).
2. **Big-endian file data** is converted at load time: `NuFileRead*`,
   scene/hgobj/anim/terrain/vis loaders (`port_swap_*`, `port/src/port_endian.c`).
3. **DOL data** (`tools/gen_dol_data.py`): every data object the C code needs
   but does not define is emitted from `main.dol`, byte-swapped per C type
   (DWARF of the port objects), with relocations applied at startup.
4. **ppc2c** (`tools/ppc2c.py`, `tools/ppc_types.py`, `include/ppc_rt.h`,
   `src/ppc_rt.c`): functions whose decompiled C does not match the DOL
   (permuter output, commented-out code, asm-only functions such as `MoveCRASH`)
   are **translated instruction by instruction from the dtk assembly into C**.
   The list is `recomp.txt` (≈250 functions: all non-matching `gamecode`, the
   input chain, animation evaluation, `main`). The decompiled versions are made
   weak (`#pragma weak`) and the generated wrappers win.
   File-local (asm `local`) functions get a `recw_<name>_<unit>` wrapper that
   is also exported under the C name (`alias`) so it replaces the weak C
   version; C callbacks handed to translated code need shims (ppc2c warns
   when such a callback is `static` in C).
   * registers are C locals; translated↔translated calls pass arguments in a
     global context (PPC SysV ABI); calls to native C are typed from DWARF;
     indirect calls go through a shim table; `sprintf` & co. are formatted from
     the PPC varargs registers.
   * memory is the port's own (native byte order, same struct layout).
     Things that differ between big- and little-endian are made identical:
     - **bitfields** are declared in reverse order under `PORT` so a storage
       unit has the same *value* as on GameCube;
     - **64-bit integers** (`LBIT`, `*.levbits`, `gate_bits`) are stored in
       PowerPC word order; C code reads/writes them through `PORT_W64()`;
     - **narrow accesses** (`lbz 0x53(r5)` reading the low byte of an int at
       0x50) are remapped using register type inference over DWARF types
       (`ppc_types.py`), and directly for globals;
     - doubles on the emulated stack keep PPC word order (int↔float idioms);
     - plain `char` is **signed** (the ProDG compiler emitted `lbz`+`extsb`).
   * read-only float constants are inlined from the DOL.
   * `PPC2C_TRACE=Func1,Func2` + `CWOC_PPCTRACE=1` logs basic blocks (debugging).
   * hooks (`HOOKS` in ppc2c, `src/game/port_hooks.c`) can skip the logos / intro.
5. **GX → OpenGL** (`src/gx_gl.c`): FIFO decoding, TEV → generated GLSL,
   texture decoding, lighting channels. On Vita, vitaGL translates the GLSL.

## Building

PC (32-bit, needs `gcc-multilib`, 32-bit SDL2 and GL dev packages, Python 3 with
`pyelftools`):

```sh
cmake -S port -B port/build -G Ninja \
  -DCWOC_DOL=/path/to/GCBE7D/sys/main.dol -DCWOC_ASM_DIR=/path/to/build/GCBE7D/asm
ninja -C port/build
```

`CWOC_ASM_DIR` is the dtk asm of the matching build (`build/GCBE7D/asm`).

PS Vita (VitaSDK with the `vitaGL`, `vitashark`, `SceShaccCgExt`, `libmathneon`,
`taihen`, `sdl2` packages; for speed rebuild vitaGL from source with
`make NO_DEBUG=1 HAVE_SHADER_CACHE=1` at a commit matching the vitashark package):

```sh
export VITASDK=/usr/local/vitasdk PATH=$VITASDK/bin:$PATH
cmake -S port -B port/build-vita -G Ninja -DPORT_VITA=ON \
  -DCWOC_DOL=... -DCWOC_ASM_DIR=...
ninja -C port/build-vita        # -> crashwoc.vpk
```

When `ppc2c` reports `static in C` errors (after adding functions to
`recomp.txt`), run `tools/fix_statics.sh <build>/port_recomp_report.txt` and
rebuild.

LiveArea art (Vita, personal build): configure with
`-DCWOC_LIVEAREA_DIR=<folder>` holding `icon0.png` (128x128), optional
`pic0.png` (960x544), `splash.rgba` (raw 960x544 RGBA: boot splash shown by
`port_splash.c` before vitaGL starts, at least 2 s), `bg.png` (840x500), `startup.png` (280x158) as 8-bit palette PNGs (`pngquant 256`) and
`template.xml`.

## Running

PC: `CWOC_DATA=<extracted disc>/files ./crashwoc`. Keys: arrows = stick,
X/Space = A (jump), Z = B (spin), C = X, V = Y, Enter = Start, A/S/D = L/R/Z.

Useful environment variables: `CWOC_SKIP_LOGOS=1`, `CWOC_INTRO=0` (skip the cut movie),
`CWOC_LOG=file`, `CWOC_FRAMES=n` / `--frames n` (exit after n frames),
`CWOC_SHOT_DIR` + `CWOC_SHOT_EVERY` (PPM screenshots),
`CWOC_INPUT="START@100-103,SUP@150-400,A@200"` (scripted input),
`CWOC_GXTRACE`, `CWOC_TEXDUMP`, `CWOC_DEBUGFS`, `CWOC_DUMPSHADERS`,
`CWOC_WARP=level@frame` (request a level change), `CWOC_BATCHSTAT=1` (why GL
batches break), `CWOC_IEEE=1` (PC: keep denormals),
`CWOC_FIXEDTIME=1` (deterministic clock: 1/60 s per frame), `CWOC_AUDIO_DUMP=file.wav`, `CWOC_NOAUDIO=1`, `CWOC_DUMPSHADERS=<dir>`,
`CWOC_MIPMAPS=0` (no mipmaps), `CWOC_LONGFRAME=ms` (log frames longer than ms
with file-read time), `CWOC_CRELOG=1` (per-frame creature positions/AI state),
`CWOC_DISPLAY=4:3|stretch|wide` + `CWOC_WINSIZE=960x544` (display mode; on the
Vita SELECT + D-pad up cycles it, saved in `<save dir>/display.txt`),
`CWOC_TELEPORT=x,y,z@frame;bonus@frame` (move the player; `bonus` = the bonus round platform), `CWOC_BONUSLOG=1` (bonus/fall state), `CWOC_STRETCHLOG=1` (joint
scale/extent of every skinned model drawn), `CWOC_NOPACE=1` (no frame pacing), `CWOC_PACESLEEP=1` (frame lock: hold frames to whole 1/60 s; Vita: SELECT + D-pad down),
`CWOC_PACELOG=1` (log the pacing per frame), `CWOC_NOFUSE=1` (original two-pass
skinning), `CWOC_RTXF=0` (view transforms on the game thread), `CWOC_RTSKIN=0` (skinning on the game thread), `CWOC_RTCOPY=0` (indexed ranges and static skins converted on the game thread), `CWOC_IDXCACHE=0` (no index-list cache), `CWOC_GPURING=0` (Vita: vitaGL client arrays instead of the GPU vertex ring; PC `=1` tests the ring); build time: `PPC2C_FLOAT=0` (no single-precision shadows in ppc2c); the log prints `tb ms/frame:` (game phases), `CWOC_JUMPLOG=1`,
`CWOC_DWALOG=1` (glitch hunting), `CWOC_SHOT_FROM=n` (screenshots from frame n).

Float-cast checking: `-DCMAKE_C_FLAGS="-fsanitize=float-cast-overflow -g -O1"`
(+ the same linker flag) reports float→integer conversions that differ
between x86/PPC (wrap) and ARM (saturate to 0 for unsigned).

Memory checking: configure another build dir with
`-DCMAKE_C_FLAGS="-fsanitize=address -fsanitize-recover=address"` and run it
with `ASAN_OPTIONS=halt_on_error=0:detect_leaks=0`.

Vita: install `crashwoc.vpk`, copy the disc's `files/` directory to
`ux0:data/crashwoc/files/` and make sure `ur0:data/libshacccg.suprx` exists
(vitaGL compiles shaders at runtime; extract it with ShaRKBrew). Log:
`ux0:data/crashwoc/crashwoc.log`. Controls: Cross = A (jump), Square = B (spin),
Circle = X, Triangle = Y, L/R, Select = Z, Start, left stick.

## Known issues / next steps

- Audio: hardware check (PC-verified by WAV dump); title logo
  brightness, portal-entry animation check, hub performance on Vita.
- numath non-matching functions were checked against the original code
  (differential test); remaining non-matching `nu3dx` / `nuxbox` functions
  are still the decompiled C — translate them if they misbehave.
- ppc2c reports "static in C" when translated code shares a C `static`
  (also for dtk `name_ADDR` names): make it non-static under `PORT`
  (`tools/unstatic.py FILE NAME`).
- Narrow accesses through untyped pointers can still read the wrong byte.
- `dol_types.txt`: C type hints for untyped DOL objects, and `@follow NAME`
  for lists the original reads past their end (into the next DOL object).
