# SPDX-License-Identifier: GPL-3.0-or-later
# riley-agentd: the service. It owns every action and is the only path to the desktop.
find_package(PkgConfig REQUIRED)
pkg_check_modules(ATSPI REQUIRED IMPORTED_TARGET atspi-2>=2.50 gobject-2.0 glib-2.0 gio-2.0)

add_executable(riley-agentd
    main.cpp
    service.cpp service.h
    hypr.cpp hypr.h
    procs.cpp procs.h
    run.cpp run.h
    uinput.cpp uinput.h
    a11y.cpp a11y.h
)
target_link_libraries(riley-agentd PRIVATE riley_core Qt6::Core PkgConfig::ATSPI)
target_compile_options(riley-agentd PRIVATE -Wall -Wextra)
