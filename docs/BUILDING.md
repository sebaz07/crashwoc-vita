# Building from source

The repository holds only the port. The decompilation it runs on comes from
its own repository, at a pinned commit (`patches/DECOMP_COMMIT`), with the
port's changes applied as `patches/decomp-port.patch` (all under `#ifdef PORT`,
so the GameCube matching build is unchanged). The build also needs **your own**
`main.dol` from the USA disc (`GCBE7D`, SHA-1
`c9cbd49a9eb0006f55533eb7d0fb5ebe2a73b72f`): part of the game code that is not
decompiled yet is translated from it, and its data tables are converted from it.

Linux or WSL is recommended.

## Requirements

- Git, CMake ≥ 3.16, Ninja, Python 3 with `pyelftools` (`pip install pyelftools`).
- PS Vita: [VitaSDK](https://vitasdk.org) with the packages `vitaGL`,
  `vitashark`, `SceShaccCgExt`, `libmathneon`, `taihen` and `sdl2`
  (`vdpm` installs them). For best performance rebuild vitaGL from source with
  `make NO_DEBUG=1 HAVE_SHADER_CACHE=1` (tested: vitaGL commit `3dddc43`).
- PC (optional, for development): 32-bit `gcc-multilib`, SDL2 and OpenGL
  development packages (i386).

## Steps

```sh
# 1. decompilation + port patch -> ./decomp
./scripts/setup_decomp.sh

# 2. your main.dol (from your disc image)
mkdir -p decomp/orig/GCBE7D/sys
./dist/crashwoc-extract --dol decomp/orig/GCBE7D/sys/main.dol "Crash (USA).iso"
#    (or copy sys/main.dol extracted with Dolphin)

# 3. VPK
export VITASDK=/usr/local/vitasdk
./scripts/build_vita.sh            # -> build-vita/crashwoc.vpk
```

`build_vita.sh` runs the decompilation's `configure.py` and
`ninja build/GCBE7D/config.json` (decomp-toolkit splits the DOL into assembly),
then configures `port/` with CMake. LiveArea art: set
`CWOC_LIVEAREA_DIR=<folder>` (see `port/README.md`).

PC build (development):

```sh
cmake -S port -B build-pc -G Ninja \
  -DCWOC_DOL=decomp/orig/GCBE7D/sys/main.dol -DCWOC_ASM_DIR=decomp/build/GCBE7D/asm
ninja -C build-pc
CWOC_DATA=/path/to/crashwoc/files ./build-pc/crashwoc
```

## The extractor

```sh
cd extractor
GOOS=windows GOARCH=amd64 go build -trimpath -ldflags "-s -w" -o ../dist/crashwoc-extract.exe .
GOOS=linux   GOARCH=amd64 go build -trimpath -ldflags "-s -w" -o ../dist/crashwoc-extract-linux .
GOOS=darwin  GOARCH=arm64 go build -trimpath -ldflags "-s -w" -o ../dist/crashwoc-extract-macos .
```

`extractor/manifest.csv` lists the file names, sizes and SHA-256 of a good USA
dump (no game data) to verify what is extracted.

## Updating the patch

Work inside `decomp/` (a git checkout), then regenerate the patch with LF line
endings:

```sh
cd decomp && git diff > ../patches/decomp-port.patch
```

Technical notes: [port/README.md](../port/README.md).
