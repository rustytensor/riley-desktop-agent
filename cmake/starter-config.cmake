# SPDX-License-Identifier: GPL-3.0-or-later
#
# Copies examples/starter-config.xml to <build>/share/riley-desktop-agent/starter-config.xml, the copy
# tools/install.sh installs. Run at build time by the starter_config target:
#   cmake -DIN=<source> -DOUT=<copy> -DPLANT=<planted fault or empty> -P starter-config.cmake
#
# Planted fault starter-config-too-wide (gate use only, built only into a -fault build dir, see
# cmake/planted-faults.cmake): the copy also allows pi click on windows on DP-2.

file(READ "${IN}" text)
if(PLANT STREQUAL "starter-config-too-wide")
    set(extra "    <allow id=\"starter-pi-click\" client=\"pi\" actions=\"click\">\n      <window class=\"*\" monitor=\"DP-2\"/>\n    </allow>\n  </permissions>")
    string(REPLACE "  </permissions>" "${extra}" text "${text}")
endif()
get_filename_component(dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${dir}")
file(WRITE "${OUT}.tmp" "${text}")
file(RENAME "${OUT}.tmp" "${OUT}")
