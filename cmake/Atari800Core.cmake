# atari800_core: the staged atari800 emulator compiled as a static library
# with core/atari800/config.h and the host layer in core/atari800/host.
#
# a52_add_core(<target> <staged dir> [STOCK]) -- STOCK builds the unpatched
# tree for the A/B tests (deterministic, unthrottled).

# Static on Windows: the shipped folder carries no zlib1.dll (release.yml
# checks the exe's imports)
if(WIN32)
  set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# The 5200 needs none of the computers' peripherals (XEP80, AF80, Bit3, MIO,
# Black Box, Proto80, Voice Box, MyIDE, R: and the rest), their NTSC/PAL
# filters or NetSIO: they are neither defined in core/atari800/config.h nor
# compiled. What remains is the CPU, ANTIC, GTIA, POKEY, the memory map, the
# monitor and the Altirra 5200 OS (the XL/XE and 400/800 OS images stay only
# because sysrom.c's table names them).
set(ATARI800_CORE_FILES
  acidtest.c afile.c antic.c artifact.c atari.c binload.c
  cartridge.c cartridge_info.c cassette.c cfg.c colours.c colours_external.c
  colours_ntsc.c colours_pal.c compfile.c cpu.c crc32.c cycle_map.c devices.c
  esc.c gtia.c img_tape.c input.c log.c memory.c
  monitor.c mzpokeysnd.c pbi.c pia.c pokey.c pokeysnd.c
  remez.c rtime.c screen.c sio.c sound.c statesav.c sysrom.c util.c
  videomode.c
  roms/altirra_5200_charset.c roms/altirra_5200_os.c roms/altirra_basic.c
  roms/altirraos_800.c roms/altirraos_xl.c)

set(ATARI800_HOST_DIR "${CMAKE_SOURCE_DIR}/core/atari800")

function(a52_add_core target gen)
  cmake_parse_arguments(C "STOCK" "" "" ${ARGN})
  set(_src "")
  foreach(_f IN LISTS ATARI800_CORE_FILES)
    list(APPEND _src "${gen}/src/${_f}")
  endforeach()
  list(APPEND _src
    "${ATARI800_HOST_DIR}/host/platform.c"
    "${ATARI800_HOST_DIR}/host/video.c"
    "${ATARI800_HOST_DIR}/host/ui_stub.c"
    "${ATARI800_HOST_DIR}/host/file_export_stub.c")
  add_library(${target} STATIC ${_src})
  set_target_properties(${target} PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
  # config.h must win over any stray one in the staged tree
  target_include_directories(${target} BEFORE PUBLIC
    "${ATARI800_HOST_DIR}" "${ATARI800_HOST_DIR}/host" "${gen}/src")
  target_compile_definitions(${target} PUBLIC _GNU_SOURCE _DEFAULT_SOURCE)
  if(C_STOCK)
    target_compile_definitions(${target} PUBLIC A52_STOCK A52_DETERMINISTIC)
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /w)
  else()
    # upstream code: its own warnings are not ours to fix
    target_compile_options(${target} PRIVATE -w)
  endif()
  target_link_libraries(${target} PUBLIC ZLIB::ZLIB Threads::Threads)
  if(WIN32)
    target_link_libraries(${target} PUBLIC ws2_32 winmm)
  else()
    target_link_libraries(${target} PUBLIC m)
  endif()
endfunction()
