# SPDX-License-Identifier: GPL-3.0-or-later
# riley_core: config loading (libxml2 + the compiled-in schema), the permission engine, the key-like-text
# detector, the log record and the bar state file. No desktop access.
add_library(riley_core STATIC
    config.cpp config.h
    glob.cpp glob.h
    policy.cpp policy.h
    textcheck.cpp textcheck.h
    log.cpp log.h
    barstate.cpp barstate.h
)
target_include_directories(riley_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(riley_core PUBLIC Qt6::Core PRIVATE LibXml2::LibXml2)
target_compile_options(riley_core PRIVATE -Wall -Wextra)

# The schema is compiled into every binary; each config load is validated against this copy.
qt_add_resources(riley_core riley_schema
    PREFIX /riley
    BASE ${PROJECT_SOURCE_DIR}
    FILES ${PROJECT_SOURCE_DIR}/schema/config.xsd
)
