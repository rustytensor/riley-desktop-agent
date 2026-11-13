// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace riley {

// Matches the whole of text against a glob: * is any run of characters, ? exactly one, everything else is itself.
// Case-insensitive (Unicode case folding).
bool globMatch(const QString &pattern, const QString &text);

} // namespace riley
