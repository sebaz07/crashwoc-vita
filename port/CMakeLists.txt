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
  target_link_libraries(crashwoc
    vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub
    SDL2 SceKernelDmacMgr_stub SceGxm_stub SceDisplay_stub SceCtrl_stub SceAppMgr_stub
    SceAppUtil_stub SceSysmodule_stub SceCommonDialog_stub SceIme_stub SceAudio_stub
    SceAudioIn_stub SceTouch_stub SceHid_stub SceMotion_stub ScePower_stub SceProcessmgr_stub
    SceRtc_stub stdc++ pthread m c)
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
