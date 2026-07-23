#include "config.h"

#include <gio/gio.h>
#include <glib-unix.h>
#include <libseat.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#if HAVE_LIBUDEV
#include <libudev.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <xf86drm.h>
#endif
#if HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif
#if HAVE_SYS_VT_H
#include <sys/vt.h>
#elif HAVE_SYS_CONSIO_H
#include <sys/consio.h>
#endif

static constexpr char SESSION_PATH[] = "/org/freedesktop/login1/session/1";
static constexpr char SEAT_PATH[] = "/org/freedesktop/login1/seat/seat0";
static constexpr char SESSION_IFACE[] = "org.freedesktop.login1.Session";
static constexpr uid_t SESSION_UID = 1000;

static const char *user_name = "user";
static const char *session_type = "wayland";

static const gchar introspection_xml[] = {
#embed "login1-introspection.xml"
    , '\0'};

static GDBusNodeInfo *node_info = nullptr;

struct taken_device {
    guint32 major, minor;
    char *path;
    int device_id; // libseat device id, -1 while not opened through seatd
    int fd;
    bool drm;
    bool pause_pending;
};

static struct {
    GDBusConnection *conn;
    char *controller;   // unique bus name of the session controller
    char *session_path; // object path the controller addressed (session/1 or session/auto)
    guint watch_id;
    struct libseat *seat;
    guint seat_watch_id;
    bool seat_active;     // what seatd last told us
    bool reported_active; // what we last told D-Bus
    guint vtnr;
    GList *devices;
    guint ack_timeout_id;
} ctl = {.vtnr = 1, .reported_active = true};

#if HAVE_LIBUDEV

static struct udev *udev_ctx = nullptr;

static char *
resolve_devnode(guint32 maj, guint32 min, bool *is_drm)
{
    if (!udev_ctx)
        return nullptr;
    struct udev_device *dev = udev_device_new_from_devnum(udev_ctx, 'c', makedev(maj, min));
    if (!dev)
        return nullptr;
    char *node = g_strdup(udev_device_get_devnode(dev));
    if (is_drm)
        *is_drm = g_strcmp0(udev_device_get_subsystem(dev), "drm") == 0;
    udev_device_unref(dev);
    return node;
}

#else

static char *
scan_devdir(const char *dir, dev_t devnum, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return nullptr;
    struct dirent *e;
    char *found = nullptr;
    while (!found && (e = readdir(d))) {
        if (g_strcmp0(e->d_name, ".") == 0 || g_strcmp0(e->d_name, "..") == 0)
            continue;
        char *path = g_build_filename(dir, e->d_name, nullptr);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (S_ISCHR(st.st_mode) && st.st_rdev == devnum)
                found = g_steal_pointer(&path);
            else if (S_ISDIR(st.st_mode) && depth > 0)
                found = scan_devdir(path, devnum, depth - 1);
        }
        g_free(path);
    }
    closedir(d);
    return found;
}

static char *
resolve_devnode(guint32 maj, guint32 min, bool *is_drm)
{
    char *node = scan_devdir("/dev", makedev(maj, min), 4);
    if (is_drm)
        *is_drm = node && g_str_has_prefix(node, DRM_DIR_NAME "/");
    return node;
}

#endif

static guint
read_active_vt(void)
{
    static const char *const consoles[] = {"/dev/tty0", "/dev/ttyv0", "/dev/console"};
    guint vt = 1;
    for (gsize i = 0; i < G_N_ELEMENTS(consoles); i++) {
        int fd = open(consoles[i], O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (fd < 0)
            continue;
        bool ok = false;
#if defined(VT_GETSTATE)
        struct vt_stat st;
        if ((ok = ioctl(fd, VT_GETSTATE, &st) == 0))
            vt = st.v_active;
#elif defined(VT_GETACTIVE)
        int active;
        if ((ok = ioctl(fd, VT_GETACTIVE, &active) == 0 && active > 0))
            vt = (guint)active;
#endif
        close(fd);
        if (ok)
            break;
    }
    return vt;
}

static bool
is_controller(const char *sender)
{
    return ctl.controller && g_strcmp0(sender, ctl.controller) == 0;
}

static struct taken_device *
find_device(guint32 maj, guint32 min)
{
    for (GList *l = ctl.devices; l; l = l->next) {
        struct taken_device *dev = l->data;
        if (dev->major == maj && dev->minor == min)
            return dev;
    }
    return nullptr;
}

static void
taken_device_destroy(struct taken_device *dev)
{
    if (dev->device_id >= 0 && ctl.seat)
        libseat_close_device(ctl.seat, dev->device_id);
    if (dev->fd >= 0)
        close(dev->fd);
    g_free(dev->path);
    g_free(dev);
}

static void
signal_controller(const char *member, GVariant *body, int fd)
{
    if (!ctl.controller) {
        g_variant_unref(g_variant_ref_sink(body));
        return;
    }
    GDBusMessage *msg = g_dbus_message_new_signal(ctl.session_path, SESSION_IFACE, member);
    g_dbus_message_set_destination(msg, ctl.controller);
    if (fd >= 0) {
        GUnixFDList *fdlist = g_unix_fd_list_new();
        g_unix_fd_list_append(fdlist, fd, nullptr);
        g_dbus_message_set_unix_fd_list(msg, fdlist);
        g_object_unref(fdlist);
    }
    g_dbus_message_set_body(msg, body);
    g_dbus_connection_send_message(ctl.conn, msg, G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, nullptr);
    g_object_unref(msg);
}

static void
set_session_active(bool active)
{
    if (ctl.reported_active == active)
        return;
    ctl.reported_active = active;
    GVariantBuilder props, invalidated;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "Active", g_variant_new_boolean(active));
    g_variant_builder_add(&props, "{sv}", "State",
                          g_variant_new_string(active ? "active" : "online"));
    g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));
    g_dbus_connection_emit_signal(
        ctl.conn, nullptr, ctl.session_path ? ctl.session_path : SESSION_PATH,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", SESSION_IFACE, &props, &invalidated), nullptr);
}

static guint
pending_acks(void)
{
    guint n = 0;
    for (GList *l = ctl.devices; l; l = l->next)
        if (((struct taken_device *)l->data)->pause_pending)
            n++;
    return n;
}

static void
finish_disable(void)
{
    g_clear_handle_id(&ctl.ack_timeout_id, g_source_remove);
    for (GList *l = ctl.devices; l; l = l->next) {
        struct taken_device *dev = l->data;
        dev->pause_pending = false;

        if (dev->device_id >= 0 && !dev->drm) {
            libseat_close_device(ctl.seat, dev->device_id);
            dev->device_id = -1;
            close(dev->fd);
            dev->fd = -1;
        }
    }
    libseat_disable_seat(ctl.seat);
}

static gboolean
ack_timeout_cb(gpointer user_data)
{
    g_warning("minilogind: controller did not ack PauseDevice in time, forcing disable");
    ctl.ack_timeout_id = 0;
    finish_disable();
    return G_SOURCE_REMOVE;
}

static void
on_seat_disable(struct libseat *seat, void *userdata)
{
    ctl.seat_active = false;
    guint pending = 0;
    for (GList *l = ctl.devices; l; l = l->next) {
        struct taken_device *dev = l->data;
        if (dev->device_id < 0)
            continue; /* taken while inactive, already paused */
        dev->pause_pending = true;
        pending++;
        signal_controller("PauseDevice", g_variant_new("(uus)", dev->major, dev->minor, "pause"),
                          -1);
    }
    set_session_active(false);
    if (pending == 0)
        finish_disable();
    else
        ctl.ack_timeout_id = g_timeout_add(2000, ack_timeout_cb, nullptr);
}

static void
on_seat_enable(struct libseat *seat, void *userdata)
{
    ctl.seat_active = true;
    for (GList *l = ctl.devices; l; l = l->next) {
        struct taken_device *dev = l->data;
        if (dev->device_id < 0) {
            int fd = -1;
            int id = libseat_open_device(ctl.seat, dev->path, &fd);
            if (id < 0) {
                g_warning("minilogind: reopen of %s failed: %s", dev->path, g_strerror(errno));
                signal_controller("PauseDevice",
                                  g_variant_new("(uus)", dev->major, dev->minor, "gone"), -1);
                continue;
            }
            if (dev->fd >= 0)
                close(dev->fd);
            dev->device_id = id;
            dev->fd = fd;
        }
        signal_controller("ResumeDevice", g_variant_new("(uuh)", dev->major, dev->minor, 0),
                          dev->fd);
    }
    set_session_active(true);
}

static const struct libseat_seat_listener seat_listener = {
    .enable_seat = on_seat_enable,
    .disable_seat = on_seat_disable,
};

static void
release_controller(void)
{
    g_clear_handle_id(&ctl.ack_timeout_id, g_source_remove);
    g_clear_handle_id(&ctl.seat_watch_id, g_source_remove);
    g_list_free_full(g_steal_pointer(&ctl.devices), (GDestroyNotify)taken_device_destroy);
    if (ctl.seat) {
        libseat_close_seat(ctl.seat);
        ctl.seat = nullptr;
    }
    if (ctl.watch_id) {
        g_bus_unwatch_name(ctl.watch_id);
        ctl.watch_id = 0;
    }
    g_clear_pointer(&ctl.controller, g_free);
    ctl.seat_active = false;
    set_session_active(true); /* back to the always-active stub default */
    g_clear_pointer(&ctl.session_path, g_free);
}

static void
on_controller_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    g_message("minilogind: session controller %s vanished", name);
    release_controller();
}

static gboolean
seat_fd_cb(gint fd, GIOCondition condition, gpointer user_data)
{
    if (libseat_dispatch(ctl.seat, 0) < 0) {
        g_warning("minilogind: lost connection to seatd");
        ctl.seat_watch_id = 0;
        release_controller();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void
take_control(GDBusConnection *conn, const char *sender, const char *obj_path)
{
    ctl.vtnr = read_active_vt();
    ctl.controller = g_strdup(sender);
    ctl.session_path = g_strdup(obj_path);
    ctl.watch_id =
        g_bus_watch_name_on_connection(conn, sender, G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr,
                                       on_controller_vanished, nullptr, nullptr);
    ctl.seat = libseat_open_seat(&seat_listener, nullptr);
    if (!ctl.seat) {
        g_warning("minilogind: libseat_open_seat failed (%s), using direct device access",
                  g_strerror(errno));
        return;
    }
    for (int i = 0; i < 20 && !ctl.seat_active; i++)
        if (libseat_dispatch(ctl.seat, 50) < 0)
            break;
    ctl.seat_watch_id =
        g_unix_fd_add(libseat_get_fd(ctl.seat), G_IO_IN | G_IO_HUP | G_IO_ERR, seat_fd_cb, nullptr);
    if (!ctl.seat_active)
        g_message("minilogind: seat not active yet (VT %u not focused?), devices start paused",
                  ctl.vtnr);
    set_session_active(ctl.seat_active);
}

static void
manager_method(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
               const gchar *method, GVariant *params, GDBusMethodInvocation *invocation,
               gpointer user_data)
{
    if (g_str_has_prefix(method, "Can")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "yes"));
    } else if (g_strcmp0(method, "GetSession") == 0 || g_strcmp0(method, "GetSessionByPID") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", SESSION_PATH));
    } else if (g_strcmp0(method, "GetSeat") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", SEAT_PATH));
    } else if (g_strcmp0(method, "GetUser") == 0) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(o)", "/org/freedesktop/login1/user/_1000"));
    } else if (g_strcmp0(method, "ListSessions") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(susso)"));
        g_variant_builder_add(&b, "(susso)", "1", (guint32)SESSION_UID, user_name, "seat0",
                              SESSION_PATH);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(susso))", &b));
    } else if (g_strcmp0(method, "ListSeats") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(so)"));
        g_variant_builder_add(&b, "(so)", "seat0", SEAT_PATH);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(so))", &b));
    } else if (g_strcmp0(method, "ListUsers") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(uso)"));
        g_variant_builder_add(&b, "(uso)", (guint32)SESSION_UID, user_name,
                              "/org/freedesktop/login1/user/_1000");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(uso))", &b));
    } else if (g_strcmp0(method, "PowerOff") == 0) {
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", "poweroff || halt -p", nullptr);
            _exit(1);
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (g_strcmp0(method, "Reboot") == 0) {
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", "reboot", nullptr);
            _exit(1);
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (g_strcmp0(method, "Suspend") == 0 || g_strcmp0(method, "Hibernate") == 0) {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    } else if (g_strcmp0(method, "Inhibit") == 0) {
        int fds[2];
        if (pipe(fds) != 0) {
            g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR,
                                                          G_DBUS_ERROR_FAILED, "pipe failed");
            return;
        }
        close(fds[1]);
        GUnixFDList *fdlist = g_unix_fd_list_new();
        gint idx = g_unix_fd_list_append(fdlist, fds[0], nullptr);
        close(fds[0]);
        g_dbus_method_invocation_return_value_with_unix_fd_list(invocation,
                                                                g_variant_new("(h)", idx), fdlist);
        g_object_unref(fdlist);
    } else {
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }
}

static void
take_device(const gchar *sender, GVariant *params, GDBusMethodInvocation *invocation)
{
    guint32 maj, min;
    g_variant_get(params, "(uu)", &maj, &min);
    bool is_drm = false;
    char *path = resolve_devnode(maj, min, &is_drm);
    if (!path) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                              "TakeDevice: no device node for %u:%u", maj, min);
        return;
    }

    int reply_fd;
    bool paused = false;
    struct taken_device *dev = nullptr;

    if (is_controller(sender) && ctl.seat) {
        if (find_device(maj, min)) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "TakeDevice: %u:%u already taken", maj, min);
            g_free(path);
            return;
        }
        dev = g_new0(struct taken_device, 1);
        dev->major = maj;
        dev->minor = min;
        dev->path = path;
        dev->device_id = -1;
        dev->fd = -1;
        dev->drm = is_drm;
        if (ctl.seat_active) {
            dev->device_id = libseat_open_device(ctl.seat, path, &dev->fd);
            if (dev->device_id < 0) {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "TakeDevice: libseat_open_device %s: %s",
                                                      path, g_strerror(errno));
                taken_device_destroy(dev);
                return;
            }
        } else {
            dev->fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
            if (dev->fd < 0) {
                g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "TakeDevice: %s: %s", path,
                                                      g_strerror(errno));
                taken_device_destroy(dev);
                return;
            }
            paused = true;
        }
        ctl.devices = g_list_append(ctl.devices, dev);
        reply_fd = dev->fd;
    } else {
        reply_fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
        g_free(path);
        if (reply_fd < 0) {
            g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "TakeDevice: open %u:%u: %s", maj, min,
                                                  g_strerror(errno));
            return;
        }
    }

    GUnixFDList *fdlist = g_unix_fd_list_new();
    gint idx = g_unix_fd_list_append(fdlist, reply_fd, nullptr);
    if (!dev)
        close(reply_fd);
    g_dbus_method_invocation_return_value_with_unix_fd_list(
        invocation, g_variant_new("(hb)", idx, paused), fdlist);
    g_object_unref(fdlist);
}

static void
session_method(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
               const gchar *method, GVariant *params, GDBusMethodInvocation *invocation,
               gpointer user_data)
{
    if (g_strcmp0(method, "TakeControl") == 0) {
        if (ctl.controller && !is_controller(sender)) {
            g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR,
                                                          G_DBUS_ERROR_ACCESS_DENIED,
                                                          "Session already has a controller");
            return;
        }
        if (!ctl.controller)
            take_control(conn, sender, path);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    if (g_strcmp0(method, "ReleaseControl") == 0) {
        if (is_controller(sender))
            release_controller();
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    if (g_strcmp0(method, "TakeDevice") == 0) {
        take_device(sender, params, invocation);
        return;
    }

    if (g_strcmp0(method, "ReleaseDevice") == 0) {
        guint32 maj, min;
        g_variant_get(params, "(uu)", &maj, &min);
        struct taken_device *dev;
        if (is_controller(sender) && (dev = find_device(maj, min))) {
            ctl.devices = g_list_remove(ctl.devices, dev);
            taken_device_destroy(dev);
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    if (g_strcmp0(method, "PauseDeviceComplete") == 0) {
        guint32 maj, min;
        g_variant_get(params, "(uu)", &maj, &min);
        struct taken_device *dev;
        if (is_controller(sender) && (dev = find_device(maj, min)) && dev->pause_pending) {
            dev->pause_pending = false;
            if (pending_acks() == 0 && ctl.ack_timeout_id)
                finish_disable();
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    if (g_strcmp0(method, "Activate") == 0) {
        if (ctl.seat)
            libseat_switch_session(ctl.seat, (int)ctl.vtnr);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    if (g_strcmp0(method, "SetType") == 0) {
        const gchar *type;
        g_variant_get(params, "(&s)", &type);
        session_type = g_strdup(type);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
    }

    if (g_strcmp0(method, "Lock") == 0 || g_strcmp0(method, "Unlock") == 0)
        g_dbus_connection_emit_signal(conn, nullptr, path, SESSION_IFACE, method, nullptr, nullptr);

    g_dbus_method_invocation_return_value(invocation, nullptr);
}

static void
seat_method(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
            const gchar *method, GVariant *params, GDBusMethodInvocation *invocation,
            gpointer user_data)
{
    if (g_strcmp0(method, "SwitchTo") == 0) {
        guint32 vtnr;
        g_variant_get(params, "(u)", &vtnr);
        if (ctl.seat)
            libseat_switch_session(ctl.seat, (int)vtnr);
    } else if (g_strcmp0(method, "ActivateSession") == 0) {
        if (ctl.seat)
            libseat_switch_session(ctl.seat, (int)ctl.vtnr);
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

static GVariant *
session_get_property(GDBusConnection *conn, const gchar *sender, const gchar *path,
                     const gchar *iface, const gchar *prop, GError **error, gpointer user_data)
{
    if (g_strcmp0(prop, "Id") == 0)
        return g_variant_new_string("1");
    if (g_strcmp0(prop, "Name") == 0)
        return g_variant_new_string(user_name);
    if (g_strcmp0(prop, "Seat") == 0)
        return g_variant_new("(so)", "seat0", SEAT_PATH);
    if (g_strcmp0(prop, "VTNr") == 0)
        return g_variant_new_uint32(ctl.vtnr);
    if (g_strcmp0(prop, "Active") == 0)
        return g_variant_new_boolean(ctl.reported_active);
    if (g_strcmp0(prop, "State") == 0)
        return g_variant_new_string(ctl.reported_active ? "active" : "online");
    if (g_strcmp0(prop, "Remote") == 0)
        return g_variant_new_boolean(false);
    if (g_strcmp0(prop, "Class") == 0)
        return g_variant_new_string("user");
    if (g_strcmp0(prop, "Type") == 0)
        return g_variant_new_string(session_type);
    if (g_strcmp0(prop, "IdleHint") == 0)
        return g_variant_new_boolean(false);
    if (g_strcmp0(prop, "LockedHint") == 0)
        return g_variant_new_boolean(false);
    return nullptr;
}

static GVariant *
seat_get_property(GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                  const gchar *prop, GError **error, gpointer user_data)
{
    if (g_strcmp0(prop, "Id") == 0)
        return g_variant_new_string("seat0");
    if (g_strcmp0(prop, "ActiveSession") == 0)
        return g_variant_new("(so)", "1", SESSION_PATH);
    if (g_strcmp0(prop, "CanGraphical") == 0)
        return g_variant_new_boolean(true);
    if (g_strcmp0(prop, "CanTTY") == 0)
        return g_variant_new_boolean(true);
    if (g_strcmp0(prop, "Sessions") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(so)"));
        g_variant_builder_add(&b, "(so)", "1", SESSION_PATH);
        return g_variant_builder_end(&b);
    }
    return nullptr;
}

static const GDBusInterfaceVTable manager_vtable = {
    manager_method,
    nullptr,
    nullptr,
    {0},
};
static const GDBusInterfaceVTable session_vtable = {
    session_method,
    session_get_property,
    nullptr,
    {0},
};
static const GDBusInterfaceVTable seat_vtable = {
    seat_method,
    seat_get_property,
    nullptr,
    {0},
};

static GDBusInterfaceInfo *
find_iface(const char *name)
{
    return g_dbus_node_info_lookup_interface(node_info, name);
}

static void
on_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    ctl.conn = conn;
    g_dbus_connection_register_object(conn, "/org/freedesktop/login1",
                                      find_iface("org.freedesktop.login1.Manager"), &manager_vtable,
                                      nullptr, nullptr, nullptr);
    g_dbus_connection_register_object(conn, SESSION_PATH, find_iface(SESSION_IFACE),
                                      &session_vtable, nullptr, nullptr, nullptr);
    g_dbus_connection_register_object(conn, "/org/freedesktop/login1/session/auto",
                                      find_iface(SESSION_IFACE), &session_vtable, nullptr, nullptr,
                                      nullptr);
    g_dbus_connection_register_object(conn, SEAT_PATH, find_iface("org.freedesktop.login1.Seat"),
                                      &seat_vtable, nullptr, nullptr, nullptr);
}

static void
on_name_lost(GDBusConnection *conn, const gchar *name, gpointer user_data)
{
    g_warning("minilogind: could not own name %s (is real logind running?)", name);
    exit(1);
}

int
main(void)
{
    struct passwd *pw = getpwuid(SESSION_UID);
    if (pw)
        user_name = g_strdup(pw->pw_name);

#if HAVE_LIBUDEV
    udev_ctx = udev_new();
    if (!udev_ctx)
        g_warning("minilogind: udev_new failed, TakeDevice will not work");
#endif

    // Never let libseat autodetect its logind backend.
    g_setenv("LIBSEAT_BACKEND", "seatd", false);
    libseat_set_log_level(LIBSEAT_LOG_LEVEL_ERROR);

    GError *error = nullptr;
    node_info = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (!node_info) {
        g_printerr("minilogind: bad introspection xml: %s\n", error->message);
        return 1;
    }

    g_bus_own_name(G_BUS_TYPE_SYSTEM, "org.freedesktop.login1", G_BUS_NAME_OWNER_FLAGS_NONE,
                   on_bus_acquired, nullptr, on_name_lost, nullptr, nullptr);

    GMainLoop *loop = g_main_loop_new(nullptr, false);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    return 0;
}
