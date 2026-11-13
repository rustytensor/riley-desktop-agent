# SPDX-License-Identifier: GPL-3.0-or-later
# riley-test-app: the window the desktop gate (tests/run-desktop.sh) drives. Test use only; never installed.
find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(riley-test-app main.cpp)
target_link_libraries(riley-test-app PRIVATE Qt6::Widgets)
target_compile_options(riley-test-app PRIVATE -Wall -Wextra)
