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
add_subdirectory(tests/policy)
add_subdirectory(tests/app)
