# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(ubi_integration)

include(${ZEPHYR_UBI_MODULE_DIR}/cmake/warnings.cmake)

target_sources(app PRIVATE
  ../common/forge.c
  ../common/partition.c
  ../common/suite.c
  ../common/table_copies.c
  src/test_attach.c
  src/test_concurrency.c
  src/test_contract.c
  src/test_integrity_block.c
  src/test_integrity_table.c
  src/test_leb.c
  src/test_leb_copies.c
  src/test_maintenance.c
  src/test_relocation.c
  src/test_trust.c
  src/test_volume.c
  src/test_volume_resize.c
  src/workload.c
)

# Reads the flash simulator's operation counters and injects its faults.
if(CONFIG_FLASH_SIMULATOR)
  target_sources(app PRIVATE
    ../common/flash_faults.c
    ../common/flash_shim.c
    ../common/flash_stats.c
    src/sweep.c
    src/test_cost.c
    src/test_erase_failure.c
    src/test_power_loss_device.c
    src/test_power_loss_leb.c
    src/test_write_failure.c
  )
endif()

# The diagnostic shell, through the dummy backend.
if(CONFIG_UBI_SHELL)
  target_sources(app PRIVATE src/test_shell.c)
endif()

# The on-flash layout and key derivation, for tests that forge blocks.
target_include_directories(app PRIVATE
  ${ZEPHYR_UBI_MODULE_DIR}/library
  ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

ubi_target_warnings(app)
