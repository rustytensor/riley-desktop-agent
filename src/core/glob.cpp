// SPDX-License-Identifier: GPL-3.0-or-later
#include "glob.h"

#include <QList>

namespace riley {

bool globMatch(const QString &pattern, const QString &text)
{
    // Compare by code point after case folding, so ? stands for one character even outside the BMP.
    const QList<uint> p = pattern.toCaseFolded().toUcs4();
    const QList<uint> t = text.toCaseFolded().toUcs4();
    qsizetype pi = 0, ti = 0, star = -1, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && p[pi] == U'*') {
            star = pi++;
            mark = ti;
        } else if (pi < p.size() && (p[pi] == U'?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (star >= 0) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == U'*')
        ++pi;
    return pi == p.size();
}

} // namespace riley
