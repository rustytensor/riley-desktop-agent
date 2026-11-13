# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(riley-desktop-agent VERSION 0.1.0 LANGUAGES CXX)

# Gate use only. Must come before any target.
include(cmake/planted-faults.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Test)
find_package(LibXml2 2.13 REQUIRED)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(src/agentd)
add_subdirectory(src/cli)
add_subdirectory(src/loop)
add_subdirectory(tests/policy)
add_subdirectory(tests/app)

# The starter config tools/install.sh installs, copied into the build dir (a planted fault can reach it there).
set(RILEY_STARTER_CONFIG "${CMAKE_BINARY_DIR}/share/riley-desktop-agent/starter-config.xml")
add_custom_command(OUTPUT "${RILEY_STARTER_CONFIG}"
    COMMAND "${CMAKE_COMMAND}" -DIN=${CMAKE_SOURCE_DIR}/examples/starter-config.xml -DOUT=${RILEY_STARTER_CONFIG}
            -DPLANT=${RILEY_PLANTED_FAULT} -P ${CMAKE_SOURCE_DIR}/cmake/starter-config.cmake
    DEPENDS "${CMAKE_SOURCE_DIR}/examples/starter-config.xml" "${CMAKE_SOURCE_DIR}/cmake/starter-config.cmake"
    COMMENT "Starter config")
add_custom_target(starter_config ALL DEPENDS "${RILEY_STARTER_CONFIG}")

# The M4 gate's window harness (tests/run-loop.sh, check f): test use only, never installed, built only by name.
# It needs the riley_window library (riley-agent-window's window, src/loop/window.h).
if(TARGET riley_window)
    add_subdirectory(tests/loop/harness EXCLUDE_FROM_ALL)
endif()
