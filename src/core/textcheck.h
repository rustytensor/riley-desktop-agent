// SPDX-License-Identifier: GPL-3.0-or-later
//
// Text the tool refuses to type: anything that looks like a key or token (rule K), or a card number (rule C).
// The rules are written out in tests/policy/cases.xml.
#pragma once

#include <QString>

namespace riley {

enum class TextVerdict {
    Clean,
    KeyName,       // K1: an environment variable name for a key or token, such as GH_TOKEN
    TokenPrefix,   // K2: a known token prefix followed by at least 16 token characters
    HighEntropy,   // K3: a run of 32 or more token characters with letters and digits and >= 3.5 bits per char
    CardNumber,    // C: 13 to 19 digits, single spaces or dashes allowed between them, passing the Luhn check
};

TextVerdict checkText(const QString &text);

inline bool isKeyLike(TextVerdict v)
{
    return v == TextVerdict::KeyName || v == TextVerdict::TokenPrefix || v == TextVerdict::HighEntropy;
}

// Shannon entropy in bits per character of s (log2, over the characters of s). 0 for an empty string.
double shannonEntropy(QStringView s);

// The Luhn check over a string of decimal digits.
bool luhnValid(QStringView digits);

} // namespace riley
