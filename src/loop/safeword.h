// SPDX-License-Identifier: GPL-3.0-or-later
//
// The safe word: "segfault" (any case, surrounding spaces ignored) stops everything at once.
#pragma once

#include <QString>

namespace riley {

inline bool isSafeWord(const QString &text)
{
    return text.trimmed().compare(QLatin1String("segfault"), Qt::CaseInsensitive) == 0;
}

} // namespace riley
