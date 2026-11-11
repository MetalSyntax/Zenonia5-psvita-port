cmake_minimum_required(VERSION 3.14)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for legacy SDK scripts")

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Define VITASDK (ej. export VITASDK=$HOME/vitasdk).")
endif ()
set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(zenonia5ios C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

# --- datos del port (el resto de source/ es común a los ports iOS de Gameloft 2009-2010)
set(VITA_APP_NAME "Zenonia 5")
set(VITA_TITLEID "PSVZ5I001")
set(VITA_VPKNAME "zenonia5ios")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_LOADER)
endif()

## vitaGL vendorizada: SOFTFP_ABI=1 es OBLIGATORIO (iOS y nuestro código pasan floats en r0-r3).
set(VITAGL_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/vitaGL)
set(VITAGL_MAKE_FLAGS_STR "SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1 HAVE_GLSL_UBOS=1 SAMPLERS_SPEEDHACK=1 DRAW_SPEEDHACK=2")
separate_arguments(VITAGL_MAKE_FLAGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS_STR}")
include(ExternalProject)
ExternalProject_Add(
  vitaGL_vendored
  SOURCE_DIR ${VITAGL_VENDOR_DIR}
  BUILD_IN_SOURCE 1
  CONFIGURE_COMMAND ""
  # libvitaGL.a ya compilada (la misma fuente que Inotia2-vita): no recompilar si existe
  BUILD_COMMAND sh -c "test -f libvitaGL.a || make ${VITAGL_MAKE_FLAGS_STR}"
  BUILD_BYPRODUCTS ${VITAGL_VENDOR_DIR}/libvitaGL.a
  INSTALL_COMMAND ""
)
add_library(vitaGL_vendored_lib STATIC IMPORTED)
set_target_properties(vitaGL_vendored_lib PROPERTIES IMPORTED_LOCATION ${VITAGL_VENDOR_DIR}/libvitaGL.a)
add_dependencies(vitaGL_vendored_lib vitaGL_vendored)

include_directories(
  ${VITAGL_VENDOR_DIR}/source
  $ENV{VITASDK}/arm-vita-eabi/include/freetype2
)

# -ffixed-r9: Darwin usa r9 como temporal sin guardarlo; nuestro código no debe tocarlo.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O2 -g -ffixed-r9 -mfloat-abi=softfp -DSKIP_SPLASHSCREEN -Wall -Wno-unused-function -Wno-pointer-sign -D_GNU_SOURCE")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -ffixed-r9 -mfloat-abi=softfp")

add_executable(${CMAKE_PROJECT_NAME}
  source/main.c
  source/log.c
  source/macho.c
  source/asm.S
  source/imports.c
  source/darwin.c
  source/darwin_printf.c
  source/objc.c
  source/foundation.c
  source/uikit.c
  source/coretext.c
  source/gles.c
  source/vitagl_no_splash.c
  source/openal.c
  source/audio.c
  source/input.c
  source/cgbi.c
  source/cxx_imports.c
  source/ios5.c
  source/patch.c
)

target_link_libraries(${CMAKE_PROJECT_NAME}
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  vitaGL_vendored_lib
  vitashark
  SceShaccCgExt
  openal
  vorbisfile
  vorbis
  ogg
  stdc++
  supc++
  taihen_stub
  SceShaccCg_stub
  mathneon
  freetype
  png16
  z
  bz2
  m
  kubridge_stub
  SceAppMgr_stub
  SceCtrl_stub
  SceTouch_stub
  SceDisplay_stub
  SceSysmodule_stub
  ScePower_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudioIn_stub
  SceAvPlayer_stub
  SceIofilemgr_stub
  SceIme_stub
  SceNet_stub
  SceSqlite_stub
)

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME "${VITA_APP_NAME}"
  FILE ${CMAKE_SOURCE_DIR}/font.ttf font.ttf
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
)
