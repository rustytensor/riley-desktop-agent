// SPDX-License-Identifier: GPL-3.0-or-later
//
// The accessibility tree (AT-SPI, libatspi) of one app, found by its process ID. Kept free of Qt headers because
// GLib's headers and Qt's keywords do not mix. Every call is serialised by one lock; libatspi is not thread-safe.
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace riley::a11y {

struct Node {
    int depth = 0;
    std::string role, name;
};

enum class Field { Unknown, Password, Other };

// Walks the app with process `pid`, starting at its top-level object whose name is `title` when there is one, else
// at the app. At most maxDepth levels and maxNodes nodes. Ends early (false) when *cancel becomes true.
// False with *error set when the app is not on the accessibility bus.
bool tree(long pid, const std::string &title, int maxDepth, int maxNodes, const std::atomic<bool> *cancel,
          std::vector<Node> *out, std::string *error);

// The kind of the control that has keyboard focus in the app with process `pid`: Password when its role is
// "password text"; Unknown when the app is not on the bus or nothing in it has focus.
Field focusedField(long pid);

} // namespace riley::a11y
