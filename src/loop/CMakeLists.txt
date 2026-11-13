# SPDX-License-Identifier: GPL-3.0-or-later
#
# riley-agent-loop: the agent loop, one task per run, no window. A task in, then ask the model, act through
# riley-agentd, look, repeat. The service knows it as own-loop by this executable.
#
# riley-agent-window: the small Qt Quick window. It starts riley-agent-loop for each task, relays Stop and the safe
# word to it, and is the only part of the tool that may end a stop (its Re-enable button). The window itself is the
# library riley_window (with its QML module), so the M4 gate's test harness can build exactly the same window.
find_package(Qt6 6.8 REQUIRED COMPONENTS Gui Network Qml Quick QuickControls2)
qt_policy(SET QTP0001 NEW)

qt_add_executable(riley-agent-loop
    main.cpp
    agentloop.cpp agentloop.h
    servicecall.cpp servicecall.h
    endpoint.cpp endpoint.h
    safeword.h
)
set_target_properties(riley-agent-loop PROPERTIES AUTOMOC ON)
target_link_libraries(riley-agent-loop PRIVATE riley_core Qt6::Core Qt6::Network)
target_compile_options(riley-agent-loop PRIVATE -Wall -Wextra)

qt_add_library(riley_window STATIC
    window.cpp window.h
    controller.cpp controller.h
    servicecall.cpp servicecall.h
    safeword.h
)
set_target_properties(riley_window PROPERTIES AUTOMOC ON)
qt_add_qml_module(riley_window
    URI Riley.Loop
    VERSION 1.0
    QML_FILES Main.qml
)
target_include_directories(riley_window PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(riley_window PUBLIC riley_windowplugin PRIVATE riley_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick
    Qt6::QuickControls2)
target_compile_options(riley_window PRIVATE -Wall -Wextra)

qt_add_executable(riley-agent-window window_main.cpp)
target_link_libraries(riley-agent-window PRIVATE riley_window Qt6::Gui Qt6::Qml Qt6::Quick)
target_compile_options(riley-agent-window PRIVATE -Wall -Wextra)
