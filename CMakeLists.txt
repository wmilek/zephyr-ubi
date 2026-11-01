# Copyright (c) 2026 Kamil Kiełbasa
# SPDX-License-Identifier: MIT

if(CONFIG_UBI)

  include(${CMAKE_CURRENT_LIST_DIR}/cmake/warnings.cmake)

  zephyr_include_directories(include)

  zephyr_library_named(ubi)
  zephyr_library_include_directories(library)

  zephyr_library_sources(
    library/ubi_api.c
    library/ubi_attach.c
    library/ubi_device.c
    library/ubi_header.c
    library/ubi_io.c
    library/ubi_key.c
    library/ubi_leb.c
    library/ubi_maintenance.c
    library/ubi_peb.c
    library/ubi_relocate.c
    library/ubi_scan.c
    library/ubi_state.c
    library/ubi_volume.c
    library/ubi_volume_table.c
  )

  zephyr_library_sources_ifdef(CONFIG_UBI_SHELL
    library/ubi_shell.c
    library/ubi_shell_volume.c
  )

  ubi_target_warnings(ubi)

endif()
