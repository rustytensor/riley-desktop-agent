# SPDX-License-Identifier: GPL-3.0-or-later
# riley-window-harness: the M4 gate's stand-in for a person at riley-agent-window (tests/run-loop.sh, check f). It
# builds the window exactly as riley-agent-window does (riley::createWindow from the riley_window library), offscreen,
# and presses its Re-enable button through Qt Test. Test use only: never installed, not part of the product, not
# built by default (the gate builds it by name).
find_package(Qt6 6.8 REQUIRED COMPONENTS Gui Qml Quick Test)

add_executable(riley-window-harness main.cpp)
target_link_libraries(riley-window-harness PRIVATE riley_window Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test)
target_compile_options(riley-window-harness PRIVATE -Wall -Wextra)
