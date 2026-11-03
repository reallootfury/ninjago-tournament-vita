cmake_minimum_required(VERSION 3.14)

if(NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR
    "Set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE=<VITASDK>/share/vita.toolchain.cmake")
endif()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
    CACHE PATH "VitaSDK toolchain file")

project(ninjago_vita C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Ninjago Tournament")
set(VITA_TITLEID "NINJA0001")
set(VITA_VPKNAME "ninjago-vita")
set(VITA_VERSION "01.00")
option(NINJAGO_ENABLE_LOGGING "Enable the optional logging VPK variant" OFF)
if(NINJAGO_ENABLE_LOGGING)
  set(VITA_VPKNAME "ninjago-vita-debug")
  set(NINJAGO_BUILD_REVISION "1.00 Logging")
  set(NINJAGO_LOGGING_MODE "enabled")
  add_compile_definitions(NINJAGO_RUNTIME_LOGGING)
else()
  set(NINJAGO_BUILD_REVISION "1.00")
  set(NINJAGO_LOGGING_MODE "disabled")
endif()
# Request the Vita's expanded application-memory budget.  Without this the
# process can fail before _start while newlib creates its per-thread state.
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(DATA_PATH "ux0:data/ninjago/" CACHE STRING
    "Runtime directory containing the user-supplied game files")
set(SO_PATH "${DATA_PATH}libLEGO_M1.so" CACHE STRING
    "Path to the Android ARMv7 executable")
set(OBB_PATH "${DATA_PATH}main.1057.com.lego.ninjago.toe.obb" CACHE STRING
    "Path to the matching Android expansion file")
add_compile_definitions(
  DATA_PATH="${DATA_PATH}"
  SO_PATH="${SO_PATH}"
  OBB_PATH="${OBB_PATH}")

set(SOFTFP_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps-softfp" CACHE PATH
    "Directory containing Android soft-float ABI dependency archives")
foreach(archive libOpenSLES.a libz.a)
  if(NOT EXISTS "${SOFTFP_DEPS_DIR}/${archive}")
    message(FATAL_ERROR
      "Missing ${SOFTFP_DEPS_DIR}/${archive}. "
      "Run python3 scripts/build_softfp_deps.py before configuring.")
  endif()
endforeach()

set(MINIMP3_DIR "${CMAKE_SOURCE_DIR}/.deps-src/minimp3")
set(LIBSNDFILE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/.deps-src/libsndfile/include")
foreach(header
    "${MINIMP3_DIR}/minimp3.h"
    "${MINIMP3_DIR}/minimp3_ex.h"
    "${LIBSNDFILE_INCLUDE_DIR}/sndfile.h")
  if(NOT EXISTS "${header}")
    message(FATAL_ERROR
      "Missing ${header}. Run python3 scripts/build_softfp_deps.py before configuring.")
  endif()
endforeach()

option(USE_SCELIBC_IO "Use SceLibcBridge for Android-compatible I/O" ON)
if(USE_SCELIBC_IO)
  add_compile_definitions(USE_SCELIBC_IO)
endif()

# Pinned renderer with scissor guards and uniform-binding restoration.
option(USE_PINNED_VITAGL "Link the pinned soft-float vitaGL renderer" ON)
set(VITAGL_LIBRARY "${SOFTFP_DEPS_DIR}/libvitaGL.a" CACHE FILEPATH
    "Immutable scissor-corrected soft-float vitaGL archive")
if(USE_PINNED_VITAGL)
  if(NOT EXISTS "${VITAGL_LIBRARY}")
    message(FATAL_ERROR "Missing pinned vitaGL archive: ${VITAGL_LIBRARY}")
  endif()
  file(SHA256 "${VITAGL_LIBRARY}" VITAGL_ARCHIVE_SHA256)
  if(NOT VITAGL_ARCHIVE_SHA256 STREQUAL
      "316ed3fd7a0ba0833a9ba84218a4b845465ba8fa2ab44ffbdb03187059f29300")
    message(FATAL_ERROR "Expected the pinned release vitaGL archive; see scripts/build_vitagl_scissor.sh")
  endif()
  set(NINJAGO_VITAGL_LIBRARY "${VITAGL_LIBRARY}")
  set(NINJAGO_RENDERER_REVISION "vitaGL")
else()
  set(NINJAGO_VITAGL_LIBRARY vitaGL)
  set(NINJAGO_RENDERER_REVISION "VitaSDK-vitaGL")
endif()

add_compile_definitions(
  NINJAGO_BUILD_REVISION="${NINJAGO_BUILD_REVISION}"
  NINJAGO_RENDERER_REVISION="${NINJAGO_RENDERER_REVISION}")

add_compile_definitions(_GNU_SOURCE __POSIX_VISIBLE=999999 USE_GLSL_SHADERS)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(DEBUG_SOLOADER)
endif()

set(CMAKE_C_FLAGS
    "${CMAKE_C_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu11 -Wno-deprecated")
set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -Wl,-q -mfloat-abi=softfp -std=gnu++20 -Wno-write-strings -Wno-psabi")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g3")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g3")
else()
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g0")
endif()

add_executable(${CMAKE_PROJECT_NAME}
  source/main.c
  source/game.c
  source/obb.c
  source/dynlib.c
  source/java.c
  source/patch.c
  source/reimpl/bits/_ctype.c
  source/reimpl/bionic.c
  source/reimpl/controls.c
  source/reimpl/egl.c
  source/reimpl/errno.c
  source/reimpl/io.c
  source/reimpl/jmp.c
  source/reimpl/log.c
  source/reimpl/mem.c
  source/reimpl/opensles_diag.c
  source/reimpl/pthr.c
  source/reimpl/sys.c
  source/reimpl/time64.c
  source/reimpl/window.c
  source/reimpl/sndfile_stub.c
  source/reimpl/asset_manager.cpp
  source/utils/dialog.c
  source/utils/glutil.c
  source/utils/init.c
  source/utils/logger.c
  source/utils/hotcache.c
  source/utils/pagecache.c
  source/utils/flashclip.c
  source/utils/sha256.c
  source/utils/livearea.c
  source/utils/settings.c
  source/utils/utils.c
  lib/falso_jni/FalsoJNI.c
  lib/falso_jni/FalsoJNI_ImplBridge.c
  lib/falso_jni/FalsoJNI_Logger.c
  lib/falso_jni/converter.c
  lib/sha1/sha1.c
  lib/fios/fios.c
  lib/so_util/so_util.c)

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

target_include_directories(${CMAKE_PROJECT_NAME}
  SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/lib)
target_include_directories(${CMAKE_PROJECT_NAME}
  PUBLIC
    ${CMAKE_SOURCE_DIR}/source
    ${MINIMP3_DIR}
    ${LIBSNDFILE_INCLUDE_DIR})

target_link_libraries(${CMAKE_PROJECT_NAME}
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  ${SOFTFP_DEPS_DIR}/libOpenSLES.a
  stdc++
  ${NINJAGO_VITAGL_LIBRARY}
  vitashark
  SceShaccCgExt
  taihen_stub
  SceShaccCg_stub
  ${SOFTFP_DEPS_DIR}/libz.a
  m
  ScePgf_stub
  mathneon
  kubridge_stub
  ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
  SceHid_stub
  SceTouch_stub
  SceMotion_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceSysmem_stub
  SceFios2_stub
  SceCtrl_stub
  SceGxm_stub
  ScePower_stub
  SceVshBridge_stub)

# VitaGL also exports partial EGL symbols. The Android compatibility layer is
# intentionally authoritative for the loaded Fusion executable.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
  -Wl,--allow-multiple-definition)

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(NINJAGO_BUILD_INFO "${CMAKE_BINARY_DIR}/build-info.txt")
add_custom_command(
  OUTPUT "${NINJAGO_BUILD_INFO}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/scripts/generate_build_info.py"
          --output "${NINJAGO_BUILD_INFO}"
          --revision "${NINJAGO_BUILD_REVISION}"
          --renderer "${NINJAGO_RENDERER_REVISION}"
          --logging "${NINJAGO_LOGGING_MODE}"
          --version "${VITA_VERSION}"
          --title-id "${VITA_TITLEID}"
          --eboot "${CMAKE_BINARY_DIR}/eboot.bin"
          --asset "icon0=${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png"
          --asset "pic0=${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png"
          --asset "startup=${CMAKE_SOURCE_DIR}/extras/livearea/startup.png"
          --asset "bg0=${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png"
  DEPENDS
    eboot.bin
    "${CMAKE_SOURCE_DIR}/scripts/generate_build_info.py"
    "${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png"
    "${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png"
    "${CMAKE_SOURCE_DIR}/extras/livearea/startup.png"
    "${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png"
  COMMENT "Generating verifiable Ninjago package provenance"
  VERBATIM)

file(GLOB NINJAGO_LICENSE_TEXTS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/licenses/*.txt")
set(NINJAGO_PACKAGED_LICENSES)
foreach(license_file IN LISTS NINJAGO_LICENSE_TEXTS)
  get_filename_component(license_name "${license_file}" NAME)
  list(APPEND NINJAGO_PACKAGED_LICENSES
       "${license_file}" "licenses/${license_name}")
endforeach()

vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE
    ${CMAKE_SOURCE_DIR}/LICENSE licenses/LICENSE
    ${CMAKE_SOURCE_DIR}/COPYING licenses/COPYING
    ${CMAKE_SOURCE_DIR}/THIRD_PARTY.md licenses/THIRD_PARTY.md
    ${NINJAGO_PACKAGED_LICENSES}
    ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
    ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
    ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
    ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
    ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
    ${NINJAGO_BUILD_INFO} build-info.txt
    ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
    ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
    ${CMAKE_SOURCE_DIR}/extras/urandom urandom)
