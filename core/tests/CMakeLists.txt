# ---- the core on its own -------------------------------------------------
# The Altirra 5200 OS is built in: the console boots with no ROM and no
# cartridge (a picture appears: its own screen).
add_test(NAME boot_5200 COMMAND a52_headless --frames 300 --expect-colours 2)

# Patched vs unpatched atari800, both deterministic: identical frames and
# audio, so the patches and our config change nothing about emulation.
if(A52_AB_TEST)
  add_test(NAME ab_5200
           COMMAND ${CMAKE_COMMAND}
                   -DA=$<TARGET_FILE:a52_headless_det>
                   -DB=$<TARGET_FILE:a52_headless_stock>
                   "-DARGS=--frames|900|--hash"
                   -P "${CMAKE_CURRENT_SOURCE_DIR}/ab_compare.cmake")
endif()

# ---- the FujiNet cartridge, on its own (no SDL) ---------------------------
add_executable(cart_test cart_test.c)
target_link_libraries(cart_test PRIVATE a52_cart)
add_test(NAME cart COMMAND cart_test)
add_executable(fujitrace_test fujitrace_test.c "${CMAKE_SOURCE_DIR}/core/debugger/fujitrace.c")
target_include_directories(fujitrace_test PRIVATE "${CMAKE_SOURCE_DIR}/core/include"
                           "${CMAKE_SOURCE_DIR}/core/debugger")
add_test(NAME fujitrace COMMAND fujitrace_test)
add_executable(cart_live_test cart_live_test.c)
target_link_libraries(cart_live_test PRIVATE a52_cart ${CMAKE_DL_LIBS})
target_include_directories(cart_live_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src"
                           "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(cart_live_test PRIVATE
  A52_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out")
add_test(NAME cart_live COMMAND cart_live_test)
set_tests_properties(cart_live PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# ---- the session layer ----------------------------------------------------
if(TARGET a52_session)
  target_include_directories(a52_session PUBLIC "${CMAKE_SOURCE_DIR}/core/src")
  # name  timeout  [serial]
  foreach(_spec
      "session;180;0" "roms;120;0" "prefs;120;0" "input;120;0" "media;180;1"
      "hostport;60;0" "external;120;1" "debug;180;0" "video;120;0"
      "gamepad;120;0" "gamepad_hotplug;120;0" "config_boot;180;0" "help;60;0")
    list(GET _spec 0 _t)
    list(GET _spec 1 _timeout)
    list(GET _spec 2 _serial)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_t}_test.c")
      add_executable(${_t}_test ${_t}_test.c)
      target_link_libraries(${_t}_test PRIVATE a52_session)
      add_test(NAME ${_t} COMMAND ${_t}_test)
      set_tests_properties(${_t} PROPERTIES TIMEOUT ${_timeout} SKIP_RETURN_CODE 77)
      if(_serial)
        set_tests_properties(${_t} PROPERTIES RUN_SERIAL ON)
      endif()
    endif()
  endforeach()

  # the live FujiNet: CONFIG mounts a game and the cartridge swaps it in
  foreach(_t netboot)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_t}_test.c")
      add_executable(${_t}_test ${_t}_test.c)
      target_link_libraries(${_t}_test PRIVATE a52_session)
      if(WIN32)
        target_link_libraries(${_t}_test PRIVATE ws2_32)
      endif()
      add_test(NAME ${_t} COMMAND ${_t}_test)
      set_tests_properties(${_t} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)
    endif()
  endforeach()
endif()

find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM AND WITH_FUJINET)
  add_test(NAME fujinet_update COMMAND "${BASH_PROGRAM}" "${CMAKE_CURRENT_SOURCE_DIR}/update_test.sh")
  set_tests_properties(fujinet_update PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 LABELS network)
endif()
