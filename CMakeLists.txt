cmake_minimum_required(VERSION 3.16)
project(arcade_foundry_cores_chihiro C CXX)   # C++ only for Cores-Common's zip reader

# The recompiler (and the core interface header), shared with the other core
# repos (submodule common/, Arcade-Foundry-Cores-Common).
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/common/tools/x86recomp/recomp.py)
  message(FATAL_ERROR "common/ is empty: run git submodule update --init")
endif()

# The game's XBE is translated at build time; nothing derived from it is committed.
set(GAME "vcop3" CACHE STRING "game directory under games/")
set(GAME_GEN_COUNT 32)
set(GAME_TITLE "Virtua Cop 3")
set(GAME_WIDTH 640)
set(GAME_HEIGHT 480)
set(GAME_ONE_CORE 0)   # 1: one guest thread runs at a time (runtime/kthread.c)
set(GAME_DRIVE 0)      # 1: a wheel, pedals and shifter instead of guns (core_api.c, input_sdl.c)
set(GAME_PUMP 0)       # 1: the gun reloads by its pump (the grip switch 0x0080, pulled twice), not by a shot off screen (core_api.c)
set(GAME_TEST_XBE "")  # the game's own test program beside its XBE (Crazy Taxi High Roller: ctx_ac_t.xbe), translated too
set(GAME_TEST_GEN_COUNT 8)
if(EXISTS ${CMAKE_SOURCE_DIR}/games/${GAME}/game.cmake)
  include(${CMAKE_SOURCE_DIR}/games/${GAME}/game.cmake)
endif()
# games.toml's games (tools/gen_games.py writes cmake/games.cmake): their title
include(${CMAKE_SOURCE_DIR}/cmake/games.cmake)
if(DEFINED CH_TITLE_${GAME})
  set(GAME_TITLE "${CH_TITLE_${GAME}}")
endif()
set(GAME_XBE "" CACHE FILEPATH "path to the game's XBE (e.g. vc3/vc3.xbe), in the folder holding its files")
if(NOT GAME_XBE)
  message(FATAL_ERROR "set -DGAME_XBE=/path/to/game.xbe")
endif()

option(CH_RUN "build chihiro-run (Linux with OpenGL, macOS with Metal; for development)" ON)
option(CH_CORE "build the game's core library for Arcade Foundry: Insert Coin (afcore_<set>)" OFF)
# iOS (iPadOS): the core only, on the same Metal renderer as macOS. The 4 GB
# guest window needs the app's extended virtual addressing entitlement, which
# tvOS does not offer.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(CH_RUN OFF)
  set(CH_CORE ON)
endif()
set(CORE_ROMSET "${GAME}" CACHE STRING "the core's romset name: afcore_api_<romset>")

# Python: the one shared venv (Cores-Common tools/afic-venv: AFIC_VENV,
# default ~/venvs/afic) when there is one and no Python3_EXECUTABLE is given
if(NOT Python3_EXECUTABLE)
  if(DEFINED ENV{AFIC_VENV})
    set(AFIC_VENV "$ENV{AFIC_VENV}")
  else()
    set(AFIC_VENV "$ENV{HOME}/venvs/afic")
  endif()
  if(EXISTS "${AFIC_VENV}/bin/python3")
    set(Python3_EXECUTABLE "${AFIC_VENV}/bin/python3" CACHE FILEPATH "Python (the shared afic venv)")
  endif()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(GEN_COUNT ${GAME_GEN_COUNT} CACHE STRING "number of gen_NNN.c files")

file(GLOB RECOMP_PY ${CMAKE_SOURCE_DIR}/common/tools/x86recomp/*.py)
set(WARN -O2 -g -Wall -Wno-unused-function -fno-strict-aliasing -fwrapv)

# Translate an XBE: an object library gen_<name> whose tables are gen_program_<name>
# (the game, and its test program when it has one of its own)
function(translate name xbe count dir)
  set(gen ${CMAKE_BINARY_DIR}/gen_${name})
  if(name STREQUAL "game")
    set(gen ${CMAKE_BINARY_DIR}/gen)
  endif()
  math(EXPR last "${count} - 1")
  set(srcs ${gen}/gen_tables.c)
  foreach(i RANGE ${last})
    string(LENGTH "${i}" n)
    if(n EQUAL 1)
      set(i "00${i}")
    elseif(n EQUAL 2)
      set(i "0${i}")
    endif()
    list(APPEND srcs ${gen}/gen_${i}.c)
  endforeach()
  foreach(f overrides patches)
    if(NOT EXISTS ${dir}/${f}.txt)
      message(FATAL_ERROR "${dir}/${f}.txt is missing")
    endif()
  endforeach()
  # starts.txt (optional): function starts discovery misses, such as one reached only through a jump table
  set(starts "")
  set(starts_dep "")
  if(EXISTS ${dir}/starts.txt)
    set(starts --starts ${dir}/starts.txt)
    set(starts_dep ${dir}/starts.txt)
  endif()
  add_custom_command(
    OUTPUT ${srcs} ${gen}/gen.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/common/tools/x86recomp/recomp.py ${xbe} ${gen} --name ${name}
            --files ${count} --overrides ${dir}/overrides.txt --patches ${dir}/patches.txt ${starts}
    DEPENDS ${RECOMP_PY} ${xbe} ${dir}/overrides.txt ${dir}/patches.txt ${starts_dep}
    COMMENT "Translating ${xbe}")
  add_library(gen_${name} OBJECT ${srcs})
  target_include_directories(gen_${name} PRIVATE runtime ${gen})
  # translated code: modest optimisation, exact IEEE behaviour, no aliasing assumptions
  target_compile_options(gen_${name} PRIVATE -O1 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
endfunction()

set(GAME_DIR ${CMAKE_SOURCE_DIR}/games/${GAME})
translate(game ${GAME_XBE} ${GEN_COUNT} ${GAME_DIR})
set(GEN_OBJECTS $<TARGET_OBJECTS:gen_game>)
set(TEST_DEFS "")
if(GAME_TEST_XBE)
  # games/<game>/test/ holds the test program's overrides and patches
  get_filename_component(game_folder "${GAME_XBE}" DIRECTORY)
  set(TEST_XBE_PATH ${game_folder}/${GAME_TEST_XBE})
  if(NOT EXISTS ${TEST_XBE_PATH})
    message(FATAL_ERROR "${GAME} has a test program of its own: ${TEST_XBE_PATH} is missing")
  endif()
  translate(test ${TEST_XBE_PATH} ${GAME_TEST_GEN_COUNT} ${GAME_DIR}/test)
  list(APPEND GEN_OBJECTS $<TARGET_OBJECTS:gen_test>)
  file(SIZE ${TEST_XBE_PATH} test_xbe_size)
  set(TEST_DEFS "CH_TEST_XBE=\"${GAME_TEST_XBE}\"")
  set(CORE_TEST_DEFS "CORE_TEST_XBE=\"${GAME_TEST_XBE}\"" CORE_TEST_XBE_SIZE=${test_xbe_size})
endif()

# Cores-Common's archive readers: chihiro.zip's EEPROM dumps (afcommon/zip_c.h)
add_subdirectory(common EXCLUDE_FROM_ALL)

add_library(ch_runtime OBJECT
  runtime/clock.c runtime/core.c runtime/kernel.c runtime/kthread.c runtime/kfile.c runtime/mmio.c runtime/nv2a.c
  runtime/apu.c runtime/chihiro.c runtime/ohci.c runtime/baseboard.c runtime/pgraph.c runtime/vsh.c runtime/vsh_ff.c runtime/psh.c runtime/tex.c runtime/perf.c)
target_include_directories(ch_runtime PRIVATE runtime ${CMAKE_SOURCE_DIR}/common/include)
target_compile_options(ch_runtime PRIVATE ${WARN})

if(CH_RUN)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED sdl2)
  if(APPLE)
    # the Metal renderer; the window on the main thread (main_linux.c)
    enable_language(OBJC)
    add_executable(chihiro-run runtime/main_linux.c runtime/gpu_metal.m runtime/input_sdl.c $<TARGET_OBJECTS:ch_runtime> ${GEN_OBJECTS})
    set_source_files_properties(runtime/gpu_metal.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_directories(chihiro-run PRIVATE ${SDL2_LIBRARY_DIRS})
    target_link_libraries(chihiro-run PRIVATE afc::afcommon ${SDL2_LIBRARIES} "-framework Metal" "-framework QuartzCore"
                          "-framework Foundation" m Threads::Threads)
  else()
    add_executable(chihiro-run runtime/main_linux.c runtime/gpu_gl.c runtime/input_sdl.c $<TARGET_OBJECTS:ch_runtime> ${GEN_OBJECTS})
    target_link_libraries(chihiro-run PRIVATE afc::afcommon ${SDL2_LIBRARIES} GL m Threads::Threads)
  endif()
  target_include_directories(chihiro-run PRIVATE runtime ${CMAKE_SOURCE_DIR}/common/include)
  target_compile_definitions(chihiro-run PRIVATE CH_PROGRAM=gen_program_game CH_ONE_CORE=${GAME_ONE_CORE} CH_DRIVE=${GAME_DRIVE} ${TEST_DEFS})
  target_compile_options(chihiro-run PRIVATE ${WARN})
  target_include_directories(chihiro-run PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

if(CH_CORE)
  set(core afcore_${CORE_ROMSET})
  # the game's core version (afcore_api.core_version), from games.toml
  set(GAME_CORE_VERSION 1.0.0)
  if(DEFINED CH_CORE_VERSION_${GAME})
    set(GAME_CORE_VERSION ${CH_CORE_VERSION_${GAME}})
  endif()
  get_filename_component(core_xbe "${GAME_XBE}" NAME)
  file(SIZE ${GAME_XBE} core_xbe_size)
  string(REPLACE "." ";" core_version "${GAME_CORE_VERSION}")
  list(GET core_version 0 core_major)
  list(GET core_version 1 core_minor)
  list(GET core_version 2 core_patch)
  if(APPLE)
    enable_language(OBJC)
    set(core_gpu runtime/gpu_metal.m)
    set_source_files_properties(runtime/gpu_metal.m PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  else()
    set(core_gpu runtime/gpu_gl.c)
  endif()
  add_library(${core} SHARED runtime/core_api.c ${core_gpu} $<TARGET_OBJECTS:ch_runtime> ${GEN_OBJECTS})
  target_include_directories(${core} PRIVATE runtime ${CMAKE_SOURCE_DIR}/common/include)
  # the core checks the game folder has the very XBE it was translated from
  target_compile_definitions(${core} PRIVATE AFCORE_BUILD CH_CORE CORE_ROMSET=${CORE_ROMSET} "CORE_TITLE=\"${GAME_TITLE}\""
    CORE_VERSION_MAJOR=${core_major} CORE_VERSION_MINOR=${core_minor} CORE_VERSION_PATCH=${core_patch}
    CORE_WIDTH=${GAME_WIDTH} CORE_HEIGHT=${GAME_HEIGHT} "CORE_XBE=\"${core_xbe}\"" CORE_XBE_SIZE=${core_xbe_size}
    CORE_ONE_CORE=${GAME_ONE_CORE} CORE_DRIVE=${GAME_DRIVE} CORE_PUMP=${GAME_PUMP} ${CORE_TEST_DEFS})
  # filelists/<set>.h (tools/filelist/make_filelist.py in common/): check_roms
  set(filelist ${CMAKE_SOURCE_DIR}/filelists/${CORE_ROMSET}.h)
  if(EXISTS ${filelist})
    configure_file(${filelist} ${CMAKE_CURRENT_BINARY_DIR}/filelist_${CORE_ROMSET}/afc_filelist.h COPYONLY)
    target_include_directories(${core} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/filelist_${CORE_ROMSET})
    target_compile_definitions(${core} PRIVATE CORE_FILELIST)
  endif()
  target_compile_options(${core} PRIVATE ${WARN})
  if(APPLE)
    set_target_properties(${core} PROPERTIES FRAMEWORK TRUE
      MACOSX_FRAMEWORK_IDENTIFIER com.thebrinkoftomorrow.arcadefoundry.insertcoin.${core})
    target_link_libraries(${core} PRIVATE afc::afcommon "-framework Metal" "-framework QuartzCore" "-framework Foundation" m Threads::Threads)
  else()
    find_library(EGL_LIBRARY EGL REQUIRED)
    find_library(GL_LIBRARY GL REQUIRED)
    target_link_libraries(${core} PRIVATE afc::afcommon ${EGL_LIBRARY} ${GL_LIBRARY} m Threads::Threads)
  endif()
  # drives the core as the front end does, without a window (not on iOS)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(core_test tools/core_test.c)
    target_include_directories(core_test PRIVATE ${CMAKE_SOURCE_DIR}/common/include)
    target_link_libraries(core_test PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()
