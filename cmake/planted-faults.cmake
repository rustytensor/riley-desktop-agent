# SPDX-License-Identifier: GPL-3.0-or-later
#
# Planted faults for the gates (tests/run-*.sh --plant <fault>). Compile time only: a fault exists only in a
# binary built with -DRILEY_PLANTED_FAULT=<name> into a build directory whose name ends in "-fault". There is no
# runtime switch, environment variable or config field that turns one on.
#
# Each fault <name> defines RILEY_FAULT_<NAME> (upper case, "-" as "_") for the code to test with #ifdef.
#   remote-allowlist-ignored  (M1)  the engine skips the remote allow-list check (rule R12)
#   focus-recheck-removed     (M2)  the service skips the focus re-check (hyprctl activewindow) before each key
#                                   event of type and key (RILEY_FAULT_FOCUS_RECHECK_REMOVED)
#   unlisted-client-accepted  (M3)  the service treats a caller with no recognised client (no coding agent and not
#                                   the loop among its ancestors) as the tool's own loop instead of refusing it
#                                   (RILEY_FAULT_UNLISTED_CLIENT_ACCEPTED)
#
# Include this from the top-level CMakeLists.txt before any target is defined.

set(RILEY_PLANTED_FAULT "" CACHE STRING "Gate use only: build a planted fault. Never set by hand.")
set(RILEY_KNOWN_PLANTED_FAULTS remote-allowlist-ignored focus-recheck-removed unlisted-client-accepted)

if(RILEY_PLANTED_FAULT)
    if(NOT RILEY_PLANTED_FAULT IN_LIST RILEY_KNOWN_PLANTED_FAULTS)
        message(FATAL_ERROR "Unknown planted fault '${RILEY_PLANTED_FAULT}'. Known: ${RILEY_KNOWN_PLANTED_FAULTS}")
    endif()
    if(NOT CMAKE_BINARY_DIR MATCHES "-fault$")
        message(FATAL_ERROR "A planted fault builds only into a build directory ending in -fault, not ${CMAKE_BINARY_DIR}")
    endif()
    string(TOUPPER "${RILEY_PLANTED_FAULT}" _riley_fault_id)
    string(MAKE_C_IDENTIFIER "${_riley_fault_id}" _riley_fault_id)
    add_compile_definitions(RILEY_FAULT_${_riley_fault_id}=1)
    message(WARNING "Building with planted fault ${RILEY_PLANTED_FAULT} (RILEY_FAULT_${_riley_fault_id}). Not for use.")
endif()
