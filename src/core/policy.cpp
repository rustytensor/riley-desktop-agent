// SPDX-License-Identifier: GPL-3.0-or-later
#include "policy.h"

#include "glob.h"
#include "textcheck.h"

#include <QRegularExpression>

namespace riley {

namespace {

Decision deny(const char *reason, const QString &rule)
{
    return Decision{Verdict::Deny, QString::fromLatin1(reason), rule};
}

Decision deny(const char *reason, const char *rule)
{
    return deny(reason, QString::fromLatin1(rule));
}

bool classIn(const QString &windowClass, const QStringList &classes)
{
    const QString c = windowClass.toCaseFolded();
    for (const QString &entry : classes)
        if (entry.toCaseFolded() == c)
            return true;
    return false;
}

bool containsCi(const QString &haystack, const char *needle)
{
    return haystack.contains(QLatin1String(needle), Qt::CaseInsensitive);
}

// Whole words (or phrases), case-insensitive.
QRegularExpression wordList(const QStringList &words)
{
    QStringList escaped;
    for (const QString &w : words)
        escaped << QRegularExpression::escape(w);
    return QRegularExpression(QStringLiteral("\\b(?:") + escaped.join(QLatin1Char('|')) + QStringLiteral(")\\b"),
                              QRegularExpression::CaseInsensitiveOption
                                  | QRegularExpression::UseUnicodePropertiesOption);
}

// R6: built-in deny-always. No config entry can lift these. More entries may be added; none removed.
bool isLockScreen(const WindowInfo &w)
{
    static const QStringList classes = {QStringLiteral("hyprlock"), QStringLiteral("swaylock"),
                                        QStringLiteral("gtklock")};
    return classIn(w.windowClass, classes);
}

bool isOwnTool(const WindowInfo &w)
{
    return w.windowClass.compare(QLatin1String("riley-desktop-agent"), Qt::CaseInsensitive) == 0
           || containsCi(w.title, ".config/riley-desktop-agent");
}

bool isAgentTerminal(const WindowInfo &w)
{
    static const QStringList agents = {QStringLiteral("claude-code"), QStringLiteral("pi"), QStringLiteral("codex")};
    static const QStringList terminals = {
        QStringLiteral("kitty"),         QStringLiteral("foot"),
        QStringLiteral("footclient"),    QStringLiteral("alacritty"),
        QStringLiteral("com.mitchellh.ghostty"), QStringLiteral("org.wezfurlong.wezterm"),
        QStringLiteral("org.kde.konsole"), QStringLiteral("org.gnome.terminal"),
        QStringLiteral("org.gnome.ptyxis"), QStringLiteral("xterm"),
    };
    if (agents.contains(w.agent))
        return true;
    return classIn(w.windowClass, terminals) && (containsCi(w.title, "claude") || containsCi(w.title, "codex"));
}

bool isPasswordManager(const WindowInfo &w)
{
    static const QStringList classes = {
        QStringLiteral("org.keepassxc.keepassxc"), QStringLiteral("keepassxc"), QStringLiteral("1password"),
        QStringLiteral("bitwarden"), QStringLiteral("enpass"), QStringLiteral("org.gnome.seahorse.application"),
        QStringLiteral("seahorse"), QStringLiteral("org.kde.kwalletmanager5"), QStringLiteral("kwalletmanager5"),
        QStringLiteral("proton-pass"),
    };
    static const QRegularExpression words = wordList({
        QStringLiteral("1password"), QStringLiteral("bitwarden"), QStringLiteral("keepassxc"),
        QStringLiteral("lastpass"), QStringLiteral("dashlane"), QStringLiteral("proton pass"),
    });
    return classIn(w.windowClass, classes) || words.match(w.title).hasMatch();
}

bool isFinance(const WindowInfo &w)
{
    static const QStringList classes = {
        QStringLiteral("electrum"), QStringLiteral("org.electrum.electrum"), QStringLiteral("exodus"),
        QStringLiteral("ledger live"), QStringLiteral("monero-wallet-gui"), QStringLiteral("feather"),
        QStringLiteral("com.sparrowwallet.sparrow"), QStringLiteral("bisq"),
    };
    static const QRegularExpression words = wordList({
        QStringLiteral("bank"), QStringLiteral("banking"), QStringLiteral("paypal"), QStringLiteral("stripe"),
        QStringLiteral("venmo"), QStringLiteral("revolut"), QStringLiteral("coinbase"), QStringLiteral("binance"),
        QStringLiteral("kraken"), QStringLiteral("metamask"), QStringLiteral("checkout"), QStringLiteral("payment"),
        QStringLiteral("payments"),
    });
    return classIn(w.windowClass, classes) || words.match(w.title).hasMatch();
}

bool isSystemSettings(const WindowInfo &w)
{
    static const QStringList classes = {
        QStringLiteral("org.gnome.settings"), QStringLiteral("gnome-control-center"),
        QStringLiteral("systemsettings"), QStringLiteral("org.kde.systemsettings"),
        QStringLiteral("hyprpolkitagent"), QStringLiteral("polkit-gnome-authentication-agent-1"),
        QStringLiteral("org.kde.polkit-kde-authentication-agent-1"),
    };
    return classIn(w.windowClass, classes);
}

// R8: editors that load a coding agent's hooks when they start.
bool isHookEditor(const WindowInfo &w)
{
    static const QStringList classes = {
        QStringLiteral("cursor"), QStringLiteral("code"), QStringLiteral("code-oss"), QStringLiteral("codium"),
        QStringLiteral("vscodium"), QStringLiteral("windsurf"), QStringLiteral("dev.zed.zed"),
    };
    return classIn(w.windowClass, classes);
}

bool typesKeys(const QString &action)
{
    return action == QLatin1String("type") || action == QLatin1String("key");
}

bool showsContent(const QString &action)
{
    return action == QLatin1String("see") || action == QLatin1String("tree") || action == QLatin1String("windows");
}

} // namespace

const QStringList &knownActions()
{
    static const QStringList actions = {
        QStringLiteral("see"),   QStringLiteral("tree"), QStringLiteral("windows"),
        QStringLiteral("point"), QStringLiteral("click"), QStringLiteral("type"),
        QStringLiteral("key"),   QStringLiteral("launch"), QStringLiteral("close"),
    };
    return actions;
}

bool windowMatches(const WindowPattern &pattern, const WindowInfo &window)
{
    if (!globMatch(pattern.windowClass, window.windowClass))
        return false;
    if (!pattern.title.isNull() && !globMatch(pattern.title, window.title))
        return false;
    if (!pattern.monitor.isNull() && !globMatch(pattern.monitor, window.monitor))
        return false;
    return true;
}

Decision decide(const Request &request, const ConfigLoad &load, const State &state)
{
    // R1, R2: no valid config, no permission at all.
    if (load.status == ConfigStatus::Missing)
        return deny("no-config", "R1");
    if (load.status != ConfigStatus::Ok || !load.config)
        return deny("invalid-config", "R2");
    const Config &config = *load.config;

    // R3: stopped until resumed.
    if (state.stopped)
        return deny("stopped", "R3");

    // R4, R5.
    if (request.client.isEmpty() || !config.client(request.client))
        return deny("unknown-client", "R4");
    if (!knownActions().contains(request.action))
        return deny("unknown-action", "R5");

    const WindowInfo &w = request.window;

    // R6: built-in deny-always, in this order.
    if (isLockScreen(w))
        return deny("deny-lock-screen", "R6a");
    if (isOwnTool(w))
        return deny("deny-own-tool", "R6b");
    if (isAgentTerminal(w))
        return deny("deny-agent-terminal", "R6c");
    if (isPasswordManager(w))
        return deny("deny-password-manager", "R6d");
    if (isFinance(w))
        return deny("deny-finance", "R6e");
    if (isSystemSettings(w))
        return deny("deny-system-settings", "R6f");

    // R7: the config's own deny-always list.
    for (const WindowPattern &entry : config.denyAlways)
        if (windowMatches(entry, w))
            return deny("deny-config-list", entry.id);

    // R8: editors that load agent hooks, unless the config names them.
    if (isHookEditor(w) && !classIn(w.windowClass, config.namedHookEditors))
        return deny("deny-agent-hook-editor", "R8");

    // R9 to R11: typing.
    if (typesKeys(request.action) && request.passwordField)
        return deny("password-field", "R9");
    if (request.action == QLatin1String("type")) {
        const TextVerdict text = checkText(request.text);
        if (isKeyLike(text))
            return deny("key-like-text", "R10");
        if (text == TextVerdict::CardNumber)
            return deny("card-like-text", "R10");
    }
    if (typesKeys(request.action)) {
        if (!w.focused)
            return deny("target-not-focused", "R11");
        if (request.focusMoved)
            return deny("focus-moved", "R11");
    }

    // R12: a remote client sees only windows on the remote allow-list.
#ifndef RILEY_FAULT_REMOTE_ALLOWLIST_IGNORED
    if (showsContent(request.action) && isRemoteClient(config, request.client)) {
        bool listed = false;
        for (const WindowPattern &entry : config.remoteAllow)
            listed = listed || windowMatches(entry, w);
        if (!listed)
            return deny("remote-not-allowed", "R12");
    }
#endif

    // R13: the first permission in document order that covers this client, action and window.
    for (const Permission &p : config.permissions) {
        if (p.client != request.client || !p.actions.contains(request.action))
            continue;
        for (const WindowPattern &pattern : p.windows)
            if (windowMatches(pattern, w))
                return Decision{Verdict::Allow, QStringLiteral("allowed"), p.id};
    }

    // R14.
    return deny("no-permission", "R14");
}

} // namespace riley
