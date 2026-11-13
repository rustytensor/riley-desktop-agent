// SPDX-License-Identifier: GPL-3.0-or-later
#include "uinput.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace riley {

namespace {
constexpr int kWarmupMs = 600;
}

VirtualMouse::~VirtualMouse()
{
    close();
}

bool VirtualMouse::open(QString *error)
{
    std::lock_guard lock(m_mutex);
    if (m_fd >= 0)
        return true;
    const int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (error)
            *error = QStringLiteral("cannot open /dev/uinput: ") + QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    bool ok = ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(fd, UI_SET_EVBIT, EV_REL) == 0
              && ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0 && ioctl(fd, UI_SET_KEYBIT, BTN_LEFT) == 0
              && ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT) == 0 && ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE) == 0
              && ioctl(fd, UI_SET_RELBIT, REL_X) == 0 && ioctl(fd, UI_SET_RELBIT, REL_Y) == 0;
    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1d6b;
    setup.id.product = 0x0104;
    std::strncpy(setup.name, "riley-desktop-agent pointer", UINPUT_MAX_NAME_SIZE - 1);
    ok = ok && ioctl(fd, UI_DEV_SETUP, &setup) == 0 && ioctl(fd, UI_DEV_CREATE) == 0;
    if (!ok) {
        if (error)
            *error = QStringLiteral("cannot set up the uinput device: ") + QString::fromLocal8Bit(std::strerror(errno));
        ::close(fd);
        return false;
    }
    m_fd = fd;
    m_created = std::chrono::steady_clock::now();
    return true;
}

void VirtualMouse::close()
{
    std::lock_guard lock(m_mutex);
    if (m_fd < 0)
        return;
    ioctl(m_fd, UI_DEV_DESTROY);
    ::close(m_fd);
    m_fd = -1;
}

int VirtualMouse::warmupLeftMs() const
{
    if (m_fd < 0)
        return 0;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_created);
    return age.count() >= kWarmupMs ? 0 : int(kWarmupMs - age.count());
}

bool VirtualMouse::sendEvent(int type, int code, int value)
{
    input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return ::write(m_fd, &ev, sizeof ev) == ssize_t(sizeof ev);
}

bool VirtualMouse::click(int button, QString *error)
{
    std::lock_guard lock(m_mutex);
    if (m_fd < 0) {
        if (error)
            *error = QStringLiteral("no uinput device");
        return false;
    }
    const int code = button == 2 ? BTN_RIGHT : button == 3 ? BTN_MIDDLE : BTN_LEFT;
    bool ok = sendEvent(EV_KEY, code, 1) && sendEvent(EV_SYN, SYN_REPORT, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    ok = sendEvent(EV_KEY, code, 0) && sendEvent(EV_SYN, SYN_REPORT, 0) && ok;   // always release
    if (!ok && error)
        *error = QStringLiteral("uinput write failed");
    return ok;
}

} // namespace riley
