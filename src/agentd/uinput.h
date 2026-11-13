// SPDX-License-Identifier: GPL-3.0-or-later
//
// A virtual mouse on /dev/uinput, made once when the service starts and kept open: the compositor needs about half a
// second before it uses a new device. It only sends button events; the pointer is placed by Hyprland's own
// hl.dsp.cursor.move, so no pointer acceleration applies.
#pragma once

#include <QString>

#include <chrono>
#include <mutex>

namespace riley {

class VirtualMouse
{
public:
    VirtualMouse() = default;
    ~VirtualMouse();
    VirtualMouse(const VirtualMouse &) = delete;
    VirtualMouse &operator=(const VirtualMouse &) = delete;

    bool open(QString *error);
    void close();
    bool isOpen() const { return m_fd >= 0; }

    // Milliseconds still to wait before the compositor uses the device (0 when ready).
    int warmupLeftMs() const;

    // One press and release of BTN_LEFT, BTN_RIGHT or BTN_MIDDLE (button 1, 2, 3).
    bool click(int button, QString *error);

private:
    bool sendEvent(int type, int code, int value);

    int m_fd = -1;
    std::chrono::steady_clock::time_point m_created;
    std::mutex m_mutex;
};

} // namespace riley
