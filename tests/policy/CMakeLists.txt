# SPDX-License-Identifier: GPL-3.0-or-later
# M1 gate driver. The top-level CMakeLists.txt adds this directory after defining the riley_core library
# (src/core: config.h, policy.h) and finding Qt6 Core and Test.
find_package(Qt6 REQUIRED COMPONENTS Core Test)

add_executable(tst_policy tst_policy.cpp)
set_target_properties(tst_policy PROPERTIES AUTOMOC ON)
target_link_libraries(tst_policy PRIVATE riley_core Qt6::Core Qt6::Test)
target_compile_definitions(tst_policy PRIVATE RILEY_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME policy COMMAND tst_policy)
