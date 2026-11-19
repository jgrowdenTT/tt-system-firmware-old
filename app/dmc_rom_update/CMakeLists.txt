# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Which DMC bootloader this update image is built for:
#   legacy - the legacy bootloader (18-19), slot0 at 0xc000 (overlays/legacy.overlay)
#   new    - the new bootloader (19-20), slot0 at 0x10000 (overlays/new.overlay)
set(ROM_UPDATE_BL "legacy" CACHE STRING "DMC bootloader layout: legacy or new")
set_property(CACHE ROM_UPDATE_BL PROPERTY STRINGS legacy new)

if(NOT DEFINED DTC_OVERLAY_FILE)
  if(ROM_UPDATE_BL STREQUAL "legacy")
    set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/overlays/legacy.overlay)
  elseif(ROM_UPDATE_BL STREQUAL "new")
    set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/overlays/new.overlay)
  else()
    message(FATAL_ERROR "ROM_UPDATE_BL must be 'legacy' or 'new', got '${ROM_UPDATE_BL}'")
  endif()
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dmc_rom_update)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
