// SPDX-License-Identifier: GPL-3.0-or-later
#include "textcheck.h"

#include <QHash>
#include <QRegularExpression>

#include <cmath>

namespace riley {

namespace {

bool isTokenChar(QChar c)   // K2: [A-Za-z0-9_-]
{
    const char16_t u = c.unicode();
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '_' || u == '-';
}

bool isRunChar(QChar c)   // K3: [A-Za-z0-9_+=-]
{
    return isTokenChar(c) || c == QLatin1Char('+') || c == QLatin1Char('=');
}

bool isUpperOrDigit(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9');
}

bool isAsciiDigit(QChar c)
{
    return c.unicode() >= '0' && c.unicode() <= '9';
}

bool isAsciiLetter(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
}

// K1: names such as OPENROUTER_API_KEY, VALYU_API_KEY, THUNDERC_TOKEN, GH_TOKEN, GITHUB_AUTH_TOKEN.
bool hasKeyName(const QString &text)
{
    static const QRegularExpression re(
        QStringLiteral("\\b[A-Z0-9]+(?:_[A-Z0-9]+)*_(?:API_KEY|KEY|TOKEN|SECRET|PASSWORD)\\b"));
    return re.match(text).hasMatch();
}

// K2: known token prefixes, anywhere in the text.
bool hasTokenPrefix(const QString &text)
{
    static const QStringList prefixes = {
        QStringLiteral("sk-or-v1-"), QStringLiteral("sk-ant-"), QStringLiteral("sk-proj-"), QStringLiteral("sk-"),
        QStringLiteral("ghp_"),      QStringLiteral("gho_"),    QStringLiteral("ghu_"),     QStringLiteral("ghs_"),
        QStringLiteral("ghr_"),      QStringLiteral("github_pat_"), QStringLiteral("glpat-"), QStringLiteral("hf_"),
        QStringLiteral("rpa_"),      QStringLiteral("xoxa-"),   QStringLiteral("xoxb-"),    QStringLiteral("xoxp-"),
        QStringLiteral("xoxr-"),     QStringLiteral("xoxs-"),
    };
    for (const QString &prefix : prefixes) {
        for (qsizetype at = text.indexOf(prefix); at >= 0; at = text.indexOf(prefix, at + 1)) {
            qsizetype n = 0;
            for (qsizetype i = at + prefix.size(); i < text.size() && isTokenChar(text.at(i)); ++i)
                ++n;
            if (n >= 16)
                return true;
        }
    }
    const QString aws = QStringLiteral("AKIA");
    for (qsizetype at = text.indexOf(aws); at >= 0; at = text.indexOf(aws, at + 1)) {
        qsizetype n = 0;
        for (qsizetype i = at + aws.size(); i < text.size() && isUpperOrDigit(text.at(i)); ++i)
            ++n;
        if (n >= 16)
            return true;
    }
    return false;
}

// K3: a maximal run of at least 32 run characters with a letter, a digit and at least 3.5 bits per character.
bool hasHighEntropyRun(const QString &text)
{
    qsizetype i = 0;
    while (i < text.size()) {
        if (!isRunChar(text.at(i))) {
            ++i;
            continue;
        }
        const qsizetype start = i;
        bool letter = false, digit = false;
        while (i < text.size() && isRunChar(text.at(i))) {
            letter = letter || isAsciiLetter(text.at(i));
            digit = digit || isAsciiDigit(text.at(i));
            ++i;
        }
        const QStringView run = QStringView(text).mid(start, i - start);
        if (run.size() >= 32 && letter && digit && shannonEntropy(run) >= 3.5)
            return true;
    }
    return false;
}

// C: digits with at most one space or dash between neighbours form a group; any 13 to 19 consecutive digits of a
// group that pass Luhn count as a card number.
bool hasCardNumber(const QString &text)
{
    qsizetype i = 0;
    while (i < text.size()) {
        if (!isAsciiDigit(text.at(i))) {
            ++i;
            continue;
        }
        QString digits;
        digits += text.at(i++);
        while (i < text.size()) {
            if (isAsciiDigit(text.at(i))) {
                digits += text.at(i++);
            } else if ((text.at(i) == QLatin1Char(' ') || text.at(i) == QLatin1Char('-')) && i + 1 < text.size()
                       && isAsciiDigit(text.at(i + 1))) {
                ++i;
            } else {
                break;
            }
        }
        for (qsizetype len = 13; len <= 19 && len <= digits.size(); ++len)
            for (qsizetype from = 0; from + len <= digits.size(); ++from)
                if (luhnValid(QStringView(digits).mid(from, len)))
                    return true;
    }
    return false;
}

} // namespace

double shannonEntropy(QStringView s)
{
    if (s.isEmpty())
        return 0.0;
    QHash<char16_t, int> counts;
    for (QChar c : s)
        ++counts[c.unicode()];
    double h = 0.0;
    const double n = double(s.size());
    for (int count : std::as_const(counts)) {
        const double p = count / n;
        h -= p * std::log2(p);
    }
    return h;
}

bool luhnValid(QStringView digits)
{
    if (digits.isEmpty())
        return false;
    int sum = 0;
    bool doubleIt = false;
    for (qsizetype i = digits.size() - 1; i >= 0; --i) {
        const QChar c = digits.at(i);
        if (!isAsciiDigit(c))
            return false;
        int d = c.unicode() - '0';
        if (doubleIt) {
            d *= 2;
            if (d > 9)
                d -= 9;
        }
        sum += d;
        doubleIt = !doubleIt;
    }
    return sum % 10 == 0;
}

TextVerdict checkText(const QString &text)
{
    if (hasKeyName(text))
        return TextVerdict::KeyName;
    if (hasTokenPrefix(text))
        return TextVerdict::TokenPrefix;
    if (hasHighEntropyRun(text))
        return TextVerdict::HighEntropy;
    if (hasCardNumber(text))
        return TextVerdict::CardNumber;
    return TextVerdict::Clean;
}

} // namespace riley
