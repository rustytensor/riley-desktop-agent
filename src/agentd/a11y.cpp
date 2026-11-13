// SPDX-License-Identifier: GPL-3.0-or-later
#include "a11y.h"

#include <atspi/atspi.h>
#include <gio/gio.h>

#include <mutex>

namespace riley::a11y {

namespace {

std::mutex lock;
bool initialised = false;

// Lets libatspi handle what the bus sent since the last call (nothing else runs this context: the service starts
// Qt without its GLib event loop).
void drain()
{
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
}

// Whether an accessibility bus can be reached, asked over D-Bus without libatspi: libatspi ends the whole process
// (a fatal GLib error) when it needs the bus and cannot connect, so it is started only once this says yes. Without a
// bus, typing answers password-unknown and tree fails; the service keeps running.
bool busReachable()
{
    if (const char *direct = g_getenv("AT_SPI_BUS_ADDRESS"); direct && *direct) {
        GError *error = nullptr;
        GDBusConnection *c = g_dbus_connection_new_for_address_sync(
            direct, GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                         | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, &error);
        if (error)
            g_error_free(error);
        if (!c)
            return false;
        g_dbus_connection_close_sync(c, nullptr, nullptr);
        g_object_unref(c);
        return true;
    }
    GError *error = nullptr;
    gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (error)
        g_error_free(error);
    if (!address)
        return false;
    error = nullptr;
    GDBusConnection *session = g_dbus_connection_new_for_address_sync(
        address, GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                      | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, nullptr, &error);
    g_free(address);
    if (error)
        g_error_free(error);
    if (!session)
        return false;
    error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus",
                                                  "GetAddress", nullptr, G_VARIANT_TYPE("(s)"),
                                                  G_DBUS_CALL_FLAGS_NO_AUTO_START, 1500, nullptr, &error);
    if (error)
        g_error_free(error);
    bool ok = false;
    if (reply) {
        const gchar *a11yAddress = nullptr;
        g_variant_get(reply, "(&s)", &a11yAddress);
        if (a11yAddress && *a11yAddress) {
            error = nullptr;
            GDBusConnection *a11y = g_dbus_connection_new_for_address_sync(
                a11yAddress, GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT
                                                  | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                nullptr, nullptr, &error);
            if (error)
                g_error_free(error);
            if (a11y) {
                ok = true;
                g_dbus_connection_close_sync(a11y, nullptr, nullptr);
                g_object_unref(a11y);
            }
        }
        g_variant_unref(reply);
    }
    g_dbus_connection_close_sync(session, nullptr, nullptr);
    g_object_unref(session);
    return ok;
}

bool init()
{
    if (!initialised) {
        // Asked again on every call until a bus is there (the service may start before the session's bus).
        if (!busReachable() || atspi_init() != 0)
            return false;
        atspi_set_timeout(1500, 3000);
        initialised = true;
    }
    drain();
    return true;
}

std::string take(gchar *s)
{
    std::string out = s ? s : "";
    g_free(s);
    return out;
}

void clear(GError **error)
{
    if (*error) {
        g_error_free(*error);
        *error = nullptr;
    }
}

// The app with this process ID, with its cache cleared so states are read fresh. A new reference, or null.
AtspiAccessible *findApp(long pid)
{
    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (!desktop)
        return nullptr;
    GError *error = nullptr;
    const gint n = atspi_accessible_get_child_count(desktop, &error);
    clear(&error);
    AtspiAccessible *found = nullptr;
    for (gint i = 0; i < n && i < 512 && !found; ++i) {
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, &error);
        clear(&error);
        if (!app)
            continue;
        const guint appPid = atspi_accessible_get_process_id(app, &error);
        clear(&error);
        if (long(appPid) == pid)
            found = app;
        else
            g_object_unref(app);
    }
    g_object_unref(desktop);
    if (found) {
        atspi_accessible_set_cache_mask(found, ATSPI_CACHE_NONE);
        atspi_accessible_clear_cache(found);
    }
    return found;
}

void walk(AtspiAccessible *o, int depth, int maxDepth, int maxNodes, const std::atomic<bool> *cancel,
          std::vector<Node> *out)
{
    if (!o || depth > maxDepth || int(out->size()) >= maxNodes || (cancel && cancel->load()))
        return;
    GError *error = nullptr;
    Node node;
    node.depth = depth;
    node.role = take(atspi_accessible_get_role_name(o, &error));
    clear(&error);
    node.name = take(atspi_accessible_get_name(o, &error));
    clear(&error);
    out->push_back(std::move(node));
    const gint n = atspi_accessible_get_child_count(o, &error);
    clear(&error);
    for (gint i = 0; i < n && i < 4096; ++i) {
        AtspiAccessible *child = atspi_accessible_get_child_at_index(o, i, &error);
        clear(&error);
        if (!child)
            continue;
        walk(child, depth + 1, maxDepth, maxNodes, cancel, out);
        g_object_unref(child);
        if (int(out->size()) >= maxNodes || (cancel && cancel->load()))
            return;
    }
}

// The focused object below o (depth-first), a new reference or null.
AtspiAccessible *findFocused(AtspiAccessible *o, int depth, int *budget)
{
    if (!o || depth > 40 || --*budget < 0)
        return nullptr;
    AtspiStateSet *states = atspi_accessible_get_state_set(o);
    const bool focused = states && atspi_state_set_contains(states, ATSPI_STATE_FOCUSED);
    if (states)
        g_object_unref(states);
    if (focused)
        return ATSPI_ACCESSIBLE(g_object_ref(o));
    GError *error = nullptr;
    const gint n = atspi_accessible_get_child_count(o, &error);
    clear(&error);
    for (gint i = 0; i < n && i < 4096; ++i) {
        AtspiAccessible *child = atspi_accessible_get_child_at_index(o, i, &error);
        clear(&error);
        if (!child)
            continue;
        AtspiAccessible *hit = findFocused(child, depth + 1, budget);
        g_object_unref(child);
        if (hit)
            return hit;
    }
    return nullptr;
}

} // namespace

bool tree(long pid, const std::string &title, int maxDepth, int maxNodes, const std::atomic<bool> *cancel,
          std::vector<Node> *out, std::string *errorText)
{
    std::lock_guard guard(lock);
    if (!init()) {
        if (errorText)
            *errorText = "no accessibility bus";
        return false;
    }
    AtspiAccessible *app = findApp(pid);
    if (!app) {
        if (errorText)
            *errorText = "the app is not on the accessibility bus";
        return false;
    }
    // Start at the top-level object named like the window, when there is one.
    AtspiAccessible *start = nullptr;
    GError *error = nullptr;
    const gint n = atspi_accessible_get_child_count(app, &error);
    clear(&error);
    for (gint i = 0; i < n && i < 256 && !start && !title.empty(); ++i) {
        AtspiAccessible *child = atspi_accessible_get_child_at_index(app, i, &error);
        clear(&error);
        if (!child)
            continue;
        if (take(atspi_accessible_get_name(child, &error)) == title)
            start = child;
        else
            g_object_unref(child);
        clear(&error);
    }
    walk(start ? start : app, 0, maxDepth, maxNodes, cancel, out);
    if (start)
        g_object_unref(start);
    g_object_unref(app);
    return !(cancel && cancel->load());
}

Field focusedField(long pid)
{
    std::lock_guard guard(lock);
    if (!init())
        return Field::Unknown;
    AtspiAccessible *app = findApp(pid);
    if (!app)
        return Field::Unknown;
    int budget = 5000;
    AtspiAccessible *focused = findFocused(app, 0, &budget);
    g_object_unref(app);
    if (!focused)
        return Field::Unknown;
    GError *error = nullptr;
    const AtspiRole role = atspi_accessible_get_role(focused, &error);
    clear(&error);
    g_object_unref(focused);
    return role == ATSPI_ROLE_PASSWORD_TEXT ? Field::Password : Field::Other;
}

} // namespace riley::a11y
