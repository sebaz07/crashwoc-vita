# Crash WOC native port target (PC 32-bit for development, PS Vita via VitaSDK).
# Separate from the matching GameCube build (configure.py/ninja); it compiles the
# decompiled C with -DPORT and links it against port/src platform code.
cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{VITASDK} AND PORT_VITA)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
endif()

project(crashwoc_port C)

# decompilation tree (src/, include/, build/GCBE7D/asm): ../decomp when the
# port lives in its own repository (scripts/setup_decomp), else the parent
if(NOT CWOC_DECOMP_DIR)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../decomp/src)
    set(CWOC_DECOMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../decomp)
  else()
    set(CWOC_DECOMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
  endif()
endif()
set(ROOT ${CWOC_DECOMP_DIR})
set(SRC ${ROOT}/src)

set(GAME_SOURCES
  gamecode/ai.c gamecode/bug.c gamecode/camera.c gamecode/chase.c gamecode/cloudfx.c
  gamecode/crate.c gamecode/creature.c gamecode/credits.c gamecode/cut.c gamecode/deb3.c
  gamecode/font3d.c gamecode/game.c gamecode/game_deb.c gamecode/game_obj.c gamecode/inst.c
  gamecode/jeep.c gamecode/lights.c gamecode/listman.c gamecode/loadsave.c gamecode/main.c
  gamecode/move.c gamecode/panel.c gamecode/text.c gamecode/vehicle.c gamecode/vehsupp.c
  gamecode/vehterr.c gamecode/visi.c gamecode/visidat.c
  gamelib/debris.c gamelib/edanim.c gamelib/edbits.c gamelib/edfile.c gamelib/edgra.c
  gamelib/edobj.c gamelib/edptl.c gamelib/gcutscn.c gamelib/glutils.c gamelib/nubridge.c
  gamelib/nuwind.c gamelib/terrain.c
  nu3dx/nuanim.c nu3dx/nucamera.c nu3dx/nucvtskn.c nu3dx/nuglass.c nu3dx/nugobj.c
  nu3dx/nuhaze.c nu3dx/nuhgobj.c nu3dx/nulight.c nu3dx/numtl.c nu3dx/nurndr.c
  nu3dx/nuscene.c nu3dx/nutex.c nu3dx/nutexanm.c nu3dx/nuvport.c nu3dx/nuwater.c
  nucore/nuerror.c nucore/nufile.c nucore/nufpar.c nucore/numem.c
  numath/nu_asm.c numath/nufloat.c numath/numtx.c numath/nuplane.c numath/nuquat.c
  numath/nurand.c numath/nutrig.c numath/nuvec.c numath/nuvec4.c
  nuraster/dxframe.c
  nusound/nusound.c nusound/sfx.c
  system/crashlib.c system/gsmatrix.c system/memcard.c system/pointspr.c system/port.c
  system/skinning.c system/syserror.c system/Version.c system/xform.c
  system/gs/gs.c system/gs/gsinit.c system/gs/gslight.c
  system/gs/gsprim.c system/gs/gstex.c
  system/ss/ss.c system/ss/ssam.c system/ss/ssdtk.c system/ss/ssmix.c system/ss/sssfx.c
  system/ss/sssp.c system/ss/sstrack.c
  system/gc/stats.c system/gc/gcStats.c system/gc/syfont.c
  # system/gc/fs.c, system/gc/pad.c: replaced by port/src/port_fs.c, port_pad.c
  system/gc/gcInit.c system/gc/gcPuts.c
  nuxbox/crash_xbox.c nuxbox/dummyfunc.c nuxbox/gba.c
  system/xboxlibs.c
)
list(TRANSFORM GAME_SOURCES PREPEND ${SRC}/)

set(FULL_PRELUDE_SOURCES ${GAME_SOURCES})
list(FILTER FULL_PRELUDE_SOURCES INCLUDE REGEX "/(gamecode|gamelib|nu3dx)/")
# These units carry private struct/prototype copies that clash with main.h.
list(FILTER FULL_PRELUDE_SOURCES EXCLUDE REGEX "/(edbits|edgra|gcutscn|nutexanm)\\.c$")
set_source_files_properties(${FULL_PRELUDE_SOURCES} PROPERTIES COMPILE_DEFINITIONS PORT_FULL_PRELUDE=1)

set(SYS_PRELUDE_SOURCES ${GAME_SOURCES})
list(FILTER SYS_PRELUDE_SOURCES INCLUDE REGEX "/(system|nucore|numath|nuraster|nusound|nuxbox)/")
set_source_files_properties(${SYS_PRELUDE_SOURCES} PROPERTIES COMPILE_DEFINITIONS PORT_SYS_PRELUDE=1)

file(GLOB PORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
# GPU profiler capture (port_gpuprof.c): only in a -DCWOC_GPUPROF=ON build
option(CWOC_GPUPROF "Vita: build the psp2-cex-gpues4-prof capture in (measurement build)" OFF)
if(NOT CWOC_GPUPROF)
  list(REMOVE_ITEM PORT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/port_gpuprof.c)
endif()
set_source_files_properties(${PORT_SOURCES} PROPERTIES COMPILE_DEFINITIONS PORT_PLATFORM_SOURCE=1)
# Game-side reimplementations written for the port (missing asm functions).
file(GLOB PORT_GAME_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/game/*.c)
set_source_files_properties(${PORT_GAME_SOURCES} PROPERTIES COMPILE_DEFINITIONS PORT_FULL_PRELUDE=1)
list(APPEND PORT_SOURCES ${PORT_GAME_SOURCES})

# Game + platform code is compiled once as an object library; the DOL data
# generator inspects these objects (undefined symbols + DWARF types) and emits
# port_dol_data.c, which is then linked into the final executable.
add_library(cwoc_objs OBJECT ${GAME_SOURCES} ${PORT_SOURCES})
set_source_files_properties(${SRC}/gamecode/main.c PROPERTIES COMPILE_DEFINITIONS "PORT_FULL_PRELUDE=1;main=cwoc_game_main")

set(CWOC_DOL "${ROOT}/orig/GCBE7D/sys/main.dol" CACHE FILEPATH "User-supplied GCBE7D main.dol")
set(CWOC_ASM_DIR "${ROOT}/build/GCBE7D/asm" CACHE PATH "dtk asm output of the matching build")
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Functions translated from the original PowerPC code (tools/ppc2c.py). Their
# decompiled C (or asm-only stub) is made weak so the generated wrapper wins.
set(RECOMP_LIST ${CMAKE_CURRENT_SOURCE_DIR}/recomp.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_LIST})
file(STRINGS ${RECOMP_LIST} _recomp_lines)
set(_weak "/* generated from port/recomp.txt */\n")
foreach(_l ${_recomp_lines})
  string(REGEX REPLACE "#.*" "" _l "${_l}")
  string(STRIP "${_l}" _l)
  if(_l STREQUAL "main")
    set(_l cwoc_game_main)
  endif()
  if(_l AND NOT _l MATCHES "[@!]")
    string(APPEND _weak "#pragma weak ${_l}\n")
  endif()
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/port_recomp_weak.h.tmp "${_weak}")
file(COPY_FILE ${CMAKE_CURRENT_BINARY_DIR}/port_recomp_weak.h.tmp ${CMAKE_CURRENT_BINARY_DIR}/port_recomp_weak.h ONLY_IF_DIFFERENT)
target_compile_options(cwoc_objs PRIVATE "SHELL:-include ${CMAKE_CURRENT_BINARY_DIR}/port_recomp_weak.h")

set(RECOMP_C ${CMAKE_CURRENT_BINARY_DIR}/port_recomp.c)
set(RECOMP_WANTED ${CMAKE_CURRENT_BINARY_DIR}/port_recomp_wanted.txt)
add_custom_command(
  OUTPUT ${RECOMP_C} ${RECOMP_WANTED}
  COMMAND ${CMAKE_COMMAND} -E env NM=${CMAKE_NM} ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/ppc2c.py
          --asm ${CWOC_ASM_DIR} --dol ${CWOC_DOL} --list ${RECOMP_LIST}
          --out ${RECOMP_C} --wanted ${RECOMP_WANTED}
          --report ${CMAKE_CURRENT_BINARY_DIR}/port_recomp_report.txt
          $<TARGET_OBJECTS:cwoc_objs>
  DEPENDS cwoc_objs $<TARGET_OBJECTS:cwoc_objs> ${RECOMP_LIST}
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/ppc2c.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_dol_data.py
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/ppc_types.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/elf_compat.py
  COMMAND_EXPAND_LISTS
  COMMENT "Translating selected PowerPC functions (ppc2c)")

set(DOL_DATA_C ${CMAKE_CURRENT_BINARY_DIR}/port_dol_data.c)
add_custom_command(
  OUTPUT ${DOL_DATA_C}
  COMMAND ${CMAKE_COMMAND} -E env NM=${CMAKE_NM} ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_dol_data.py
          --dol ${CWOC_DOL} --asm ${CWOC_ASM_DIR} --out ${DOL_DATA_C}
          --report ${CMAKE_CURRENT_BINARY_DIR}/port_dol_data_report.txt
          --extra ${RECOMP_WANTED} --types ${CMAKE_CURRENT_SOURCE_DIR}/dol_types.txt
          $<TARGET_OBJECTS:cwoc_objs>
  DEPENDS cwoc_objs $<TARGET_OBJECTS:cwoc_objs> ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_dol_data.py ${RECOMP_WANTED}
          ${CMAKE_CURRENT_SOURCE_DIR}/dol_types.txt ${CMAKE_CURRENT_SOURCE_DIR}/tools/elf_compat.py
  COMMAND_EXPAND_LISTS
  COMMENT "Generating native data tables from main.dol")

add_executable(crashwoc $<TARGET_OBJECTS:cwoc_objs> ${DOL_DATA_C} ${RECOMP_C})
target_include_directories(crashwoc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_source_files_properties(${RECOMP_C} PROPERTIES COMPILE_OPTIONS
  "-fwrapv;-fno-strict-aliasing;-Wno-unused-label;-Wno-unused-variable;-Wno-unused-but-set-variable")

target_include_directories(cwoc_objs PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${SRC} ${SRC}/gamecode ${SRC}/gamelib ${SRC}/nu3dx ${SRC}/nucore ${SRC}/numath
  ${SRC}/nuraster ${SRC}/nusound ${SRC}/system ${SRC}/system/gs ${SRC}/system/ss
)
target_compile_options(cwoc_objs PRIVATE -idirafter ${ROOT}/include)

target_compile_definitions(cwoc_objs PRIVATE PORT=1 VERSION_GCBE7D BUILD_VERSION=0)
# Match the original target's ABI where it matters for data layout:
#  - plain 'char' is signed in the original (ProDG GCC: lbz + extsb), unsigned by default on ARM; - double/long long 8-byte aligned (ARM EABI).
target_compile_options(cwoc_objs PRIVATE
  -std=gnu99 -fgnu89-inline -fsigned-char -fcommon -fno-strict-aliasing -fwrapv
  "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/port_prelude.h"
  -Wno-int-conversion -Wno-incompatible-pointer-types
  -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-implicit-int -Wno-return-mismatch
  -Wno-builtin-declaration-mismatch -fpermissive)

if(PORT_VITA)
  # PS Vita (VitaSDK): vitaGL for rendering (GLSL translated at runtime, needs
  # libshacccg.suprx on the console), sceCtrl for input, SDL2 only for timers.
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
  target_compile_definitions(cwoc_objs PRIVATE PORT_VITA=1)
  # PORT_VITAGL_DIR: a vitaGL built by scripts/build_vitagl.sh (lib/ and
  # include/), linked instead of the VitaSDK one (adds the shader disk cache)
  set(PORT_VITAGL_DIR "" CACHE PATH "vitaGL from scripts/build_vitagl.sh (empty: the VitaSDK one)")
  if(PORT_VITAGL_DIR)
    set(_vgl_lib "${PORT_VITAGL_DIR}/lib/libvitaGL.a")
    target_include_directories(cwoc_objs BEFORE PRIVATE "${PORT_VITAGL_DIR}/include")
    target_include_directories(crashwoc BEFORE PRIVATE "${PORT_VITAGL_DIR}/include")
    set(_vgl_link "${_vgl_lib}")
    message(STATUS "vitaGL: ${_vgl_lib}")
  else()
    set(_vgl_lib "$ENV{VITASDK}/arm-vita-eabi/lib/libvitaGL.a")
    set(_vgl_link vitaGL)
  endif()
  # identity of the linked vitaGL: part of the shader program cache key (gx_gl.c).
  # A vitaGL from build_vitagl.sh is named by its stamp (commit, make flags,
  # patch hash): a rebuild of the same vitaGL keeps the id (the archive's own
  # bytes carry timestamps), so shipped program binaries stay valid.
  if(PORT_VITAGL_DIR AND EXISTS "${PORT_VITAGL_DIR}/lib/.built")
    file(READ "${PORT_VITAGL_DIR}/lib/.built" _vgl_stamp)
    string(SHA256 _vgl_sha "${_vgl_stamp}")
    string(SUBSTRING "${_vgl_sha}" 0 12 PORT_VITAGL_ID)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PORT_VITAGL_DIR}/lib/.built")
  elseif(EXISTS "${_vgl_lib}")
    file(SHA256 "${_vgl_lib}" _vgl_sha)
    string(SUBSTRING "${_vgl_sha}" 0 12 PORT_VITAGL_ID)
  else()
    set(PORT_VITAGL_ID "unknown")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vgl_lib}")
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/gx_gl.c PROPERTIES COMPILE_DEFINITIONS
    "PORT_PLATFORM_SOURCE=1;PORT_VITAGL_ID=\"${PORT_VITAGL_ID}\"")
  message(STATUS "vitaGL id for the shader cache: ${PORT_VITAGL_ID}")
  set(PORT_VITA_TEXT_PAD 4096 CACHE STRING "bytes of padding at the end of .rodata (see port_main.c)")
  target_compile_definitions(crashwoc PRIVATE PORT_VITA_TEXT_PAD=${PORT_VITA_TEXT_PAD})
  if(NOT PORT_OPT)
    set(PORT_OPT -O2)
  endif()
  # -g: the data generator and ppc2c read the DWARF of these objects
  # -fno-short-enums: the GameCube ABI (and the data layouts) use 4-byte enums
  target_compile_options(cwoc_objs PRIVATE -g ${PORT_OPT} -fno-short-enums)
  target_compile_options(crashwoc PRIVATE -g ${PORT_OPT} -fno-short-enums)
  target_link_options(crashwoc PRIVATE -Wl,--no-enum-size-warning)
  # vita-elf-create appends ~4 KB of module data after the text segment and
  # fails ("segment 1 overlaps") when the text ends just below the 64 KB
  # boundary of the data segment. The SDK linker script (armvita.x) reserves
  # __sce_headroom bytes there; 8 KB always leaves room, whatever the code
  # size (the .rodata pad in port_main.c only moved the boundary).
  target_link_options(crashwoc PRIVATE -Wl,--defsym,__sce_headroom=0x2000)
  target_link_libraries(crashwoc
    ${_vgl_link} vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
    SDL2 SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCtrl_stub SceSysmem_stub SceAppMgr_stub
    SceAppUtil_stub SceSysmodule_stub SceCommonDialog_stub SceIme_stub SceAudio_stub
    SceAudioIn_stub SceTouch_stub SceHid_stub SceMotion_stub ScePower_stub SceProcessmgr_stub
    SceRtc_stub stdc++ pthread m c)
  if(CWOC_GPUPROF)
    # client libraries from psp2-cex-gpues4-prof (CMAKE_PREFIX_PATH: its install
    # prefix). Only the GXM observer: its --wrap options put every sceGxm call of
    # vitaGL through it, so it goes after vitaGL on the link line.
    find_package(psp2_gpuprof CONFIG REQUIRED NO_CMAKE_FIND_ROOT_PATH)
    find_library(GPUPROF_STUB NAMES psp2_gpuprof_stub REQUIRED NO_CMAKE_FIND_ROOT_PATH)
    target_compile_definitions(cwoc_objs PRIVATE PORT_GPUPROF=1)
    target_compile_definitions(crashwoc PRIVATE PORT_GPUPROF=1)
    get_target_property(_gp_inc psp2_gpuprof::client INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(cwoc_objs PRIVATE ${_gp_inc})
    target_link_libraries(crashwoc psp2_gpuprof::client psp2_gpuprof::gxm ${GPUPROF_STUB} ScePower_stub)
    message(STATUS "GPU profiler capture: on (${GPUPROF_STUB})")
  endif()
  set(VITA_APP_NAME "Crash WOC")
  set(VITA_TITLEID "CRWC00001")
  set(VITA_VERSION "00.10")
  vita_create_self(eboot.bin crashwoc UNSAFE)
  # Optional LiveArea art (personal build, not in the repo): a folder with
  # icon0.png (128x128), bg.png (840x500), startup.png (280x158), all 8-bit
  # palette PNGs, and template.xml
  set(CWOC_LIVEAREA_DIR "" CACHE PATH "folder with the LiveArea bubble images")
  set(CWOC_VPK_FILES)
  if(CWOC_LIVEAREA_DIR AND EXISTS "${CWOC_LIVEAREA_DIR}/icon0.png")
    list(APPEND CWOC_VPK_FILES FILE "${CWOC_LIVEAREA_DIR}/icon0.png" sce_sys/icon0.png)
    if(EXISTS "${CWOC_LIVEAREA_DIR}/pic0.png") # 960x544 shown while the game starts
      list(APPEND CWOC_VPK_FILES FILE "${CWOC_LIVEAREA_DIR}/pic0.png" sce_sys/pic0.png)
    endif()
    if(EXISTS "${CWOC_LIVEAREA_DIR}/splash.rgba") # raw 960x544 RGBA boot splash (port_splash.c)
      list(APPEND CWOC_VPK_FILES FILE "${CWOC_LIVEAREA_DIR}/splash.rgba" splash.rgba)
    endif()
    foreach(f bg.png startup.png template.xml)
      if(EXISTS "${CWOC_LIVEAREA_DIR}/${f}")
        list(APPEND CWOC_VPK_FILES FILE "${CWOC_LIVEAREA_DIR}/${f}" sce_sys/livearea/contents/${f})
      endif()
    endforeach()
  endif()
  # Optional shader seed (scripts/shader_seed.py; not in the repo): GPU
  # programs compiled on a console, packed as app0:shaders/ so a first start
  # compiles nothing that the seed has (gx_gl.c, port_video.c). Layout:
  # vgl/v2/{v,f}/<hash>.gxp (vitaGL's shader cache) and prog/<hash>.bin (the
  # port's program cache, valid only for the same PORT_VITAGL_ID).
  set(CWOC_SHADER_SEED "" CACHE PATH "folder with a shader seed (scripts/shader_seed.py)")
  if(CWOC_SHADER_SEED AND IS_DIRECTORY "${CWOC_SHADER_SEED}")
    file(GLOB_RECURSE _seed_files RELATIVE "${CWOC_SHADER_SEED}" "${CWOC_SHADER_SEED}/*.gxp" "${CWOC_SHADER_SEED}/*.bin")
    list(LENGTH _seed_files _seed_n)
    message(STATUS "shader seed: ${_seed_n} files from ${CWOC_SHADER_SEED}")
    foreach(f ${_seed_files})
      list(APPEND CWOC_VPK_FILES FILE "${CWOC_SHADER_SEED}/${f}" shaders/${f})
    endforeach()
  endif()
  vita_create_vpk(crashwoc.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    ${CWOC_VPK_FILES})
else()
  target_compile_definitions(cwoc_objs PRIVATE PORT_PC=1)
  if(NOT PORT_OPT)
    set(PORT_OPT -O1)
  endif()
  target_compile_options(cwoc_objs PRIVATE -m32 -malign-double -msse2 -mfpmath=sse -g ${PORT_OPT})
  target_link_options(crashwoc PRIVATE -m32)
  set_source_files_properties(${DOL_DATA_C} PROPERTIES COMPILE_OPTIONS "-m32")
  target_compile_options(crashwoc PRIVATE -m32 -msse2 -mfpmath=sse -g ${PORT_OPT})
  target_link_libraries(crashwoc SDL2 GL m)
endif()
