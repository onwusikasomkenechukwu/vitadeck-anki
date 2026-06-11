cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain")
  else()
    message(FATAL_ERROR "VITASDK env not set")
  endif()
endif()

project(vitadeck_p3 C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -Wall -O2 -fno-exceptions -fno-rtti")

# freetype headers live under .../include/freetype2 (ft2build.h + freetype/).
set(FT_INCLUDE "$ENV{VITASDK}/arm-vita-eabi/include/freetype2")

# Shared link set. freetype pulls png/z/bz2 (freetype2.pc: Requires zlib,
# bzip2, libpng); freetype must precede them in link order.
set(VD_LIBS
  vita2d
  freetype png jpeg z bz2
  SceCtrl_stub SceTouch_stub SceDisplay_stub SceGxm_stub SceSysmodule_stub
  SceCommonDialog_stub SceAppMgr_stub ScePgf_stub ScePvf_stub
  SceLibKernel_stub SceIofilemgr_stub ScePower_stub SceSqlite_stub
  SceRtc_stub SceAudio_stub m c)

# Fonts packaged into every vpk at app0:assets/fonts/.
# NOTE: vita_create_vpk does NOT quote/escape FILE *source* paths, and this
# project lives under a path containing spaces ("anki psvita project"), so a
# `-a <src-with-spaces>=<dst>` token gets split by the shell and the pack
# fails. Work around it by staging the fonts (the in-repo assets/ are the
# source of truth) into a space-free sibling dir and referencing those
# absolute, space-free paths. The two `..` collapse past the spaced project
# folder, so the result is /.../Downloads/vitadeck_fonts (no spaces).
get_filename_component(VD_FONT_STAGE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../vitadeck_fonts" ABSOLUTE)
file(MAKE_DIRECTORY "${VD_FONT_STAGE}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/SpaceGrotesk-Bold.ttf"
     DESTINATION "${VD_FONT_STAGE}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/SpaceGrotesk-SemiBold.ttf"
     DESTINATION "${VD_FONT_STAGE}")
set(VD_FONT_FILES
  FILE ${VD_FONT_STAGE}/SpaceGrotesk-Bold.ttf     assets/fonts/SpaceGrotesk-Bold.ttf
  FILE ${VD_FONT_STAGE}/SpaceGrotesk-SemiBold.ttf assets/fonts/SpaceGrotesk-SemiBold.ttf)

# SFX (raw 48 kHz stereo s16le, converted from .wav). Same space-free staging.
# bgm.raw is NOT packaged (~37MB) -- it streams from ux0:data/vitadeck/bgm.raw.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/scroll.raw" DESTINATION "${VD_FONT_STAGE}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/select.raw" DESTINATION "${VD_FONT_STAGE}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/assets/audio/back.raw"   DESTINATION "${VD_FONT_STAGE}")
set(VD_AUDIO_FILES
  FILE ${VD_FONT_STAGE}/scroll.raw assets/audio/scroll.raw
  FILE ${VD_FONT_STAGE}/select.raw assets/audio/select.raw
  FILE ${VD_FONT_STAGE}/back.raw   assets/audio/back.raw)

# LiveArea / bubble assets (icon0 + custom LiveArea background, gate, template).
# Same space-free staging as the fonts (vita_create_vpk FILE doesn't escape
# spaces). Sources live in vita_app2/sce_sys/ (resized: icon0 128x128,
# bg0 840x500, startup 280x158).
get_filename_component(VD_ASSET_STAGE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../vitadeck_assets" ABSOLUTE)
file(MAKE_DIRECTORY "${VD_ASSET_STAGE}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png" DESTINATION "${VD_ASSET_STAGE}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg0.png" DESTINATION "${VD_ASSET_STAGE}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png" DESTINATION "${VD_ASSET_STAGE}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml" DESTINATION "${VD_ASSET_STAGE}")
set(VD_SCE_FILES
  FILE ${VD_ASSET_STAGE}/icon0.png    sce_sys/icon0.png
  FILE ${VD_ASSET_STAGE}/bg0.png      sce_sys/livearea/contents/bg0.png
  FILE ${VD_ASSET_STAGE}/startup.png  sce_sys/livearea/contents/startup.png
  FILE ${VD_ASSET_STAGE}/template.xml sce_sys/livearea/contents/template.xml)

# ---- Main reviewer app ----
add_executable(vitadeck_p3 main.cpp)
target_include_directories(vitadeck_p3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FT_INCLUDE})
target_link_libraries(vitadeck_p3 ${VD_LIBS})
vita_create_self(eboot.bin vitadeck_p3)
vita_create_vpk(vitadeck_p3.vpk VDCKREV01 eboot.bin
  VERSION ${VITA_VERSION}
  NAME    "VitaDeck"
  ${VD_FONT_FILES}
  ${VD_AUDIO_FILES}
  ${VD_SCE_FILES})

# ---- Phase 8 Step 1 primitives test (separate vpk; leaves the app intact) ----
add_executable(vitadeck_test test_primitives.cpp)
target_include_directories(vitadeck_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FT_INCLUDE})
target_link_libraries(vitadeck_test ${VD_LIBS})
vita_create_self(eboot_test.bin vitadeck_test)
vita_create_vpk(vitadeck_test.vpk VDCKTEST1 eboot_test.bin
  VERSION ${VITA_VERSION}
  NAME    "VitaDeck Primitives Test"
  ${VD_FONT_FILES})
