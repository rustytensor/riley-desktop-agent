// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hyprland's IPC socket ($XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock), the same requests
// hyprctl sends: "j/clients", "j/activewindow", "j/monitors" and "/dispatch <Lua dispatch>". The service only reads
// the window list and dispatches against one window address or the cursor; it never sets keywords or reloads.
#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace riley {

struct HyprWindow {
    QString address;          // 0x...
    QString windowClass, title;
    qint64 pid = 0;
    int x = 0, y = 0, width = 0, height = 0;   // global layout (logical) coordinates
    int monitorId = -1;
    QString monitor;          // the monitor's name, e.g. DP-2
    int workspaceId = 0;
    bool mapped = true, hidden = false, floating = false, pinned = false, fullscreen = false;
};

struct HyprMonitor {
    int id = -1;
    QString name;
    int x = 0, y = 0, width = 0, height = 0;   // width/height in pixels
    double scale = 1.0;
};

struct Desktop {
    bool ok = false;
    QString error;
    QList<HyprWindow> windows;
    QHash<int, HyprMonitor> monitors;
    QString activeAddress;

    const HyprWindow *window(const QString &address) const;
};

// One request; the whole reply. Empty with *error set on failure.
QByteArray hyprRequest(const QByteArray &request, QString *error = nullptr, int timeoutMs = 2000);

// Windows, monitors and the active window, read now.
Desktop readDesktop();

// The address of the active window now ("" when none or on error).
QString activeWindowAddress(bool *ok = nullptr);

// "/dispatch <expr>"; true when Hyprland answers ok.
bool hyprDispatch(const QString &expr, QString *error = nullptr);

} // namespace riley
