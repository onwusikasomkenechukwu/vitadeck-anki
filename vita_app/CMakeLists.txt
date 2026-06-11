cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK env not set; source the MSYS2 shell with .bashrc that exports it.")
  endif()
endif()

project(vitadeck_boot_test C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

# Per VITA_TITLEID convention: 9 characters, [A-Z0-9]
set(VITA_APP_NAME "VitaDeck Boot Test")
set(VITA_TITLEID  "VDCKBOOT1")
set(VITA_VERSION  "01.00")

# Flags pulled from the vita2d sample makefile in vdpm repo. -Wl,-q is
# REQUIRED for the vita-elf-create step (keeps relocation info in the elf).
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q -Wall -O2 -fno-exceptions -fno-rtti")

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME}
  vita2d
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  ScePgf_stub
  ScePvf_stub
  SceLibKernel_stub
  SceIofilemgr_stub
  ScePower_stub
  freetype
  png
  jpeg
  z
  m
  c
)

vita_create_self(eboot.bin ${PROJECT_NAME})

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME    ${VITA_APP_NAME}
)
