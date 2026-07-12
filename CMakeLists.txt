# Copyright (c) 2026 asxeem
# Licensed under the MIT License - see LICENSE file in the repo root for details.
# Passive HRP-UWB frame-capture firmware for the Qorvo DWM3001CDK
# (DW3110 + nRF52833).  Out-of-tree Zephyr / nRF Connect SDK application that
# builds against the Qorvo DW3000 "decadriver" (dwt_uwb_driver) supplied as a
# Zephyr module at external/dw3000.

cmake_minimum_required(VERSION 3.20.0)

# Pull in the Qorvo/Decawave DW3000 decadriver as a Zephyr module.  It carries
# its own zephyr/module.yml + Kconfig (CONFIG_DW3000) and compiles only when
# CONFIG_DW3000=y (set in prj.conf).  Place the driver at external/dw3000 (see
# the README - e.g. `git submodule add <driver-repo> external/dw3000`).
set(DW3000_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/dw3000)
if(NOT EXISTS ${DW3000_MODULE_DIR}/dwt_uwb_driver/deca_device_api.h)
  message(FATAL_ERROR
    "DW3000 decadriver not found at ${DW3000_MODULE_DIR}. "
    "See the README: add the Qorvo dwt_uwb_driver module at external/dw3000.")
endif()
list(APPEND ZEPHYR_EXTRA_MODULES ${DW3000_MODULE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(uwb_capture LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/capture.c
  src/shell_cmds.c
)
