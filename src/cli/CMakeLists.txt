# SPDX-License-Identifier: GPL-3.0-or-later
# riley-agent: the command line coding agents use. It only talks to riley-agentd.
add_executable(riley-agent main.cpp)
target_link_libraries(riley-agent PRIVATE Qt6::Core)
target_compile_options(riley-agent PRIVATE -Wall -Wextra)
