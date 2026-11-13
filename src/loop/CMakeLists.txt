# SPDX-License-Identifier: GPL-3.0-or-later
# riley-agent-loop: the agent loop. A task in, then ask the model, act through riley-agentd, look, repeat. A small
# Qt Quick window, or --headless for scripts and the gate. The service knows it as own-loop by this executable.
find_package(Qt6 6.8 REQUIRED COMPONENTS Gui Network Qml Quick QuickControls2)
qt_policy(SET QTP0001 NEW)

qt_add_executable(riley-agent-loop
    main.cpp
    agentloop.cpp agentloop.h
    servicecall.cpp servicecall.h
    endpoint.cpp endpoint.h
    controller.cpp controller.h
)
set_target_properties(riley-agent-loop PROPERTIES AUTOMOC ON)
qt_add_qml_module(riley-agent-loop
    URI Riley.Loop
    VERSION 1.0
    QML_FILES Main.qml
)
target_link_libraries(riley-agent-loop PRIVATE riley_core Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick
    Qt6::QuickControls2)
target_compile_options(riley-agent-loop PRIVATE -Wall -Wextra)
