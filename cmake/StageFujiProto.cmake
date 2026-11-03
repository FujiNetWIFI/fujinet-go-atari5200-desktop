# Provide the fujinet-firmware checkout (see cmake/Dependencies.cmake) and
# stage the FujiNet Atari 5200 cartridge's own sources into
# core/cart/fuji-generated, which is what core/CMakeLists.txt compiles into
# the a52_cart library.
#
# These are the cartridge firmware's OWN sources, from two roots:
#
#   pico/atari-5200/firmware  a52map.c / a52map.h / a52map_db.c / a52map_db.h
#                      (the mapper engine and image planner: ROM, 2 chips,
#                      Bounty Bob, Super Cart; the CRC database; the .car
#                      header; the FUJI claim), fuji_load.c / fuji_load.h
#                      (the two-buffer staging and the swap), fuji_mailbox.h
#                      (the register and status-page layout) and
#                      baked/fujiconfigrom.h (the baked CONFIG)
#   pico/common        fujimail.c / fujimail.h (the mailbox service),
#                      fujibus.c / fujibus.h (SLIP + FujiBus wire codec),
#                      fuji_cfg.c / fuji_cfg.h (the .cfg sibling's mapper=),
#                      fuji_crc.c / fuji_crc.h, fuji_mailbox_proto.h /
#                      fuji_mailbox_check.h (the layout every cart shares) and
#                      the emulator-side SLIP-over-TCP transport (fujitcp.h)
#
# The firmware's pico/atari-5200/emu/apply.sh grafts the identical set into a
# MAME tree; this file is that graft for this repository. Nothing here is
# patched, only copied. apply.sh additionally prefixes shared symbols
# (several FujiNet carts share one MAME binary) and renames every .c to .cpp;
# only one FujiNet cart is ever linked here and the sources are C, so neither
# is reproduced. The one port-specific file, the cart device itself, is this
# repository's own core/cart/fujinet_cart.c (transposed from
# pico/atari-5200/emu/a5200_fujinet.cpp).
#
# The staged fujitcp.c is POSIX sockets only and is not compiled: every build
# uses core/cart/fujitcp_host.c, this app's portable, abortable twin, against
# the SAME staged fujitcp.h.
#
# Staging is automatic: it runs when the staged tree is missing, when the pin
# has moved, when this file changes, or on demand with -DFUJI_RESTAGE=ON.

set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/cart/fuji-generated")

option(FUJI_RESTAGE "Re-stage the FujiNet protocol sources from the firmware checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

a52_provide_dependency(
  NAME fujinet-firmware
  PATH third_party/fujinet-firmware
  URL "${FUJINET_URL}"
  COMMIT "${FUJINET_COMMIT}"
  SENTINEL build.sh
  OVERRIDE FUJINET_SRC
  RESULT FUJINET_DIR)

set(FUJI_PROTO_DIR "${FUJINET_DIR}/pico")
if(NOT EXISTS "${FUJI_PROTO_DIR}/atari-5200/firmware/include/fuji_mailbox.h"
   OR NOT EXISTS "${FUJI_PROTO_DIR}/atari-5200/firmware/include/a52map.h"
   OR NOT EXISTS "${FUJI_PROTO_DIR}/atari-5200/firmware/baked/fujiconfigrom.h")
  message(FATAL_ERROR
    "${FUJINET_DIR} has no pico/atari-5200/firmware/{include/fuji_mailbox.h, "
    "include/a52map.h, baked/fujiconfigrom.h} -- the pin must have the Atari "
    "5200 cart (pico/atari-5200) and its baked CONFIG; see "
    "cmake/Dependencies.cmake.")
endif()

# The exact file list apply.sh grafts into MAME, relative to pico/, plus the
# baked CONFIG (the cartridge firmware bakes the same bytes in).
set(FUJI_STAGE_FILES
  common/src/fujimail.c
  common/src/fujibus.c
  common/src/fuji_cfg.c
  common/src/fuji_crc.c
  common/include/fujimail.h
  common/include/fujibus.h
  common/include/fuji_cfg.h
  common/include/fuji_crc.h
  common/include/fuji_mailbox_proto.h
  common/include/fuji_mailbox_check.h
  common/emu/fujitcp.c
  common/emu/fujitcp.h
  atari-5200/firmware/src/a52map.c
  atari-5200/firmware/src/a52map_db.c
  atari-5200/firmware/src/fuji_load.c
  atari-5200/firmware/include/fuji_mailbox.h
  atari-5200/firmware/include/a52map.h
  atari-5200/firmware/include/a52map_db.h
  atari-5200/firmware/include/fuji_load.h
  atari-5200/firmware/baked/fujiconfigrom.h)

# What the staged tree was made from: the source identity plus a hash of this
# file (the only transform), so that editing the stage re-stages rather than
# silently leaving the old result in place.
set(_fuji_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${FUJINET_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _fuji_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _fuji_head)
  set(_fuji_head "${FUJINET_COMMIT} ${FUJINET_DIR}")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _fuji_stage_hash)
set(_fuji_want "${_fuji_head} ${_fuji_stage_hash}")

set(_fuji_staged "")
if(EXISTS "${FUJI_GEN}/.source-info")
  file(READ "${FUJI_GEN}/.source-info" _fuji_staged)
  string(STRIP "${_fuji_staged}" _fuji_staged)
endif()

if(FUJI_RESTAGE OR NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h"
   OR NOT _fuji_staged STREQUAL _fuji_want)
  message(STATUS "Staging FujiNet protocol sources from ${FUJI_PROTO_DIR}")
  file(REMOVE_RECURSE "${FUJI_GEN}")
  file(MAKE_DIRECTORY "${FUJI_GEN}")

  foreach(_f IN LISTS FUJI_STAGE_FILES)
    if(NOT EXISTS "${FUJI_PROTO_DIR}/${_f}")
      file(REMOVE_RECURSE "${FUJI_GEN}")
      message(FATAL_ERROR
        "FujiNet protocol staging failed: ${FUJI_PROTO_DIR}/${_f} is missing "
        "(pin drift? see cmake/Dependencies.cmake).")
    endif()
    get_filename_component(_base "${_f}" NAME)
    file(COPY_FILE "${FUJI_PROTO_DIR}/${_f}" "${FUJI_GEN}/${_base}")
  endforeach()

  file(WRITE "${FUJI_GEN}/.source-info" "${_fuji_want}\n")
endif()

message(STATUS "FujiNet protocol sources staged from ${FUJI_PROTO_DIR}")
