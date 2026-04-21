# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# Include application-specific Kconfig before finding Zephyr
set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/Kconfig)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf54lm20b_wakeword_gated_kws)

# Note: The nrf_edgeai library is linked directly in zephyr/CMakeLists.txt using zephyr_link_libraries(),
# and the axon-ml SDK sources are added directly to Zephyr via zephyr_sources() in the same CMakeLists.txt.

target_sources(app PRIVATE src/main.cpp
    src/models/wakeword/nrf_edgeai_generated/nrf_edgeai_user_model.c
    src/models/kws/nrf_edgeai_generated/nrf_edgeai_user_model.c
    src/dmic.c
)
