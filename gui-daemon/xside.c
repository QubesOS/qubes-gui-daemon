/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
 * Copyright (C) 2010  Joanna Rutkowska <joanna@invisiblethingslab.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

/* high level documentation is here: https://www.qubes-os.org/doc/gui/ */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <err.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/queue.h>
#include <sys/random.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <execinfo.h>
#include <getopt.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/extensions/shmproto.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb_aux.h>
#include <libconfig.h>
#include <libnotify/notify.h>
#include <qubes-xorg-tray-defs.h>
#include <xen/grant_table.h>
#include <xen/gntdev.h>
#include "xside.h"
#include "txrx.h"
#include "double-buffer.h"
#include "list.h"
#include "error.h"
#include "png.h"
#include "trayicon.h"
#include "shm-args.h"
#include "util.h"
#include "unistd.h"
#include <qubes/pure.h>

/* Supported protocol version */

#define PROTOCOL_VERSION_MAJOR UINT32_C(1)
#define PROTOCOL_VERSION_MINOR UINT32_C(8)
#define PROTOCOL_VERSION(x, y) ((x) << 16 | (y))

#if !(PROTOCOL_VERSION_MAJOR == QUBES_GUID_PROTOCOL_VERSION_MAJOR && \
      3 <= QUBES_GUID_PROTOCOL_VERSION_MINOR)
#  error Incompatible qubes-gui-protocol.h.
#endif

/* some configuration */

static Ghandles ghandles;

/* macro used to verify data from VM */
#define VERIFY(x) do if (!(x) && ask_whether_verify_failed(g, __STRING(x))) return; while(0)

/* calculate virtual width */
#define XORG_DEFAULT_XINC 8
#define _VIRTUALX(x) ( (((x)+XORG_DEFAULT_XINC-1)/XORG_DEFAULT_XINC)*XORG_DEFAULT_XINC )

/* short macro for beginning of each xevent handling function
 * checks if this window is managed by guid and declares windowdata struct
 * pointer */
#define CHECK_NONMANAGED_WINDOW(g, id) struct windowdata *vm_window; \
    do if (!(vm_window=check_nonmanaged_window((g), (id)))) return; while (0)

#ifndef min
#define min(x,y) ({ \
    __typeof__(x) _x = (x); \
    __typeof__(y) _y = (y); \
    _x > _y ? _y : _x; \
})
#endif
#ifndef max
#define max(x,y) ({ \
    __typeof__(x) _x = (x); \
    __typeof__(y) _y = (y); \
    _x < _y ? _y : _x; \
})
#endif
#pragma GCC poison _x _y

#define ignore_result(x) do { __typeof__(x) __attribute__((unused)) _ignore=(x); } while (0)

static int (*default_x11_io_error_handler)(Display *dpy);
static void inter_appviewer_lock(Ghandles *g, int mode);
static void release_mapped_mfns(Ghandles * g, struct windowdata *vm_window);
static void print_backtrace(void);
static void parse_cmdline_prop(Ghandles *g);

static void show_message(Ghandles *g, const char *prefix, const char *msg,
                         gint timeout)
{
    char message[1024];
    NotifyNotification *notify;

    fprintf(stderr, "%s\n", msg);
    if (!notify_init("qubes-guid")) {
        fprintf(stderr, "Failed to init notification subsystem\n");
        return;
    }
    snprintf(message, sizeof message, "%s(%s): %s", prefix, g->vmname, msg);
    notify = notify_notification_new(prefix, message, g->cmdline_icon);
    notify_notification_set_timeout(notify, timeout);
    if (!notify_notification_show(notify, NULL)) {
        fprintf(stderr, "Failed to send notification\n");
    }
    g_object_unref (G_OBJECT (notify));
    // need to init/uninit every time because some notification daemons (namely
    // xfce4-notifyd) starts only on demand and connection reference become
    // stale after some idle period
    notify_uninit();
}

static void show_error_message(Ghandles *g, const char *msg)
{
    show_message(g, "ERROR", msg, NOTIFY_EXPIRES_DEFAULT);
}

/* ask user when VM sent invalid message */
static int ask_whether_verify_failed(Ghandles * g, const char *cond)
{
    char text[1024];
    char dontagain_param[128];
    int ret = 1;
    pid_t pid;
    fprintf(stderr, "Verify failed: %s\n", cond);
    /* to be enabled with KDE >= 4.6 in dom0 */
    //#define NEW_KDIALOG
#ifdef NEW_KDIALOG
    snprintf(text, sizeof(text),
            "The domain %s attempted to perform an invalid or suspicious GUI "
            "request. This might be a sign that the domain has been compromised "
            "and is attempting to compromise the GUI daemon (Dom0 domain). In "
            "rare cases, however, it might be possible that a legitimate "
            "application trigger such condition (check the guid logs for more "
            "information). \n\n"
            "Click “Terminate” to terminate this domain immediately, or "
            "“Ignore” to ignore this GUI request.",
         g->vmname);
#else
    snprintf(text, sizeof(text),
            "The domain %s attempted to perform an invalid or suspicious GUI "
            "request. This might be a sign that the domain has been compromised "
            "and is attempting to compromise the GUI daemon (Dom0 domain). In "
            "rare cases, however, it might be possible that a legitimate "
            "application trigger such condition (check the guid logs for more "
            "information). \n\n"
            "Do you allow this VM to continue running?",
         g->vmname);
#endif
    snprintf(dontagain_param, sizeof(dontagain_param), "qubes-quid-%s:%s", g->vmname, cond);

    pid = fork();
    switch (pid) {
        case 0:
            if (g->use_kdialog) {
#ifdef NEW_KDIALOG
                execlp(KDIALOG_PATH, "kdialog", "--dontagain", dontagain_param, "--no-label", "Terminate", "--yes-label", "Ignore", "--warningyesno", text, (char*)NULL);
#else
                execlp(KDIALOG_PATH, "kdialog", "--dontagain", dontagain_param, "--warningyesno", text, (char*)NULL);
#endif
            } else {
                execlp(ZENITY_PATH, "zenity", "--question", "--ok-label", "Terminate", "--cancel-label", "Ignore", "--text", text, (char*)NULL);
            }
            perror("execlp");
            _exit(1);
        case -1:
            perror("fork");
            exit(1);
        default:
            waitpid(pid, &ret, 0);
            ret = WEXITSTATUS(ret);
    }
    if (!g->use_kdialog) {
        // in zenity we use "OK" as "Terminate" to have it default
        // so invert the result
        ret ^= 1;
    }
    switch (ret) {
//    case 2:    /*cancel */
//        break;
    case 0:    /* YES */
        return 1;
    case 1:    /* NO */
        execl(QVM_KILL_PATH, "qvm-kill", g->vmname, (char*)NULL);
        perror("Problems executing qvm-kill");
        exit(1);
    default:
        fprintf(stderr, "Problems executing %s ?\n", g->use_kdialog ? "kdialog" : "zenity");
        exit(1);
    }
    /* should never happen */
    abort();
}

static void
qubes_xcb_handler(Ghandles *g, const char *msg, struct windowdata *vm_window,
                  xcb_generic_error_t *error) {
    fprintf(stderr,
        "%s failed for window 0x%lx(remote 0x%lx)\n",
        msg,
        vm_window->local_winid,
        vm_window->remote_winid);
    XErrorEvent err = {
       .type = error->response_type,
       .display = g->display,
       .error_code = error->error_code,
       .resourceid = error->resource_id,
       .serial = error->full_sequence,
       .request_code = error->major_code,
       .minor_code = error->minor_code,
    };
    dummy_handler(g->display, &err);
}

static int x11_error_handler(Display * dpy, XErrorEvent * ev)
{
    /* log the error */
    dummy_handler(dpy, ev);
    if ((ev->request_code == X_DestroyWindow
         || ev->request_code == X_UnmapWindow
         || ev->request_code == X_ConfigureWindow
         || ev->request_code == X_GetProperty)
            && ev->error_code == BadWindow) {
        fprintf(stderr, "  someone else already destroyed this window, ignoring\n");
        return 0;
    }
    /* Permit XGetWindowAttributes errors, as long as they're not for root_win */
    if (ev->request_code == X_GetWindowAttributes &&
        ev->error_code == BadWindow &&
        ev->resourceid != ghandles.root_win) {

        fprintf(stderr, "  someone else already destroyed this window, ignoring\n");
        return 0;
    }

#ifdef MAKE_X11_ERRORS_FATAL
    exit(1);
#endif
    return 0;
}

/*
 * The X11 IO error handler. It is supposed to cleanup things and do _not_
 * return (should terminate the process).
 */
static int x11_io_error_handler(Display * dpy)
{
    print_backtrace();
    if (default_x11_io_error_handler)
        default_x11_io_error_handler(dpy);
    exit(1);
}

/*
 * Infer a color value from the provided string using strtoul for
 * hex-like input or for a name Xlib by way of /etc/X11/rgb.txt.
 */
static XColor parse_color(const char *str, Display *dpy, int screen)
{
    XColor xcolor;
    Status status;
    char *trimmed = (char *) str;
    while (trimmed[0] == ' ') {  /* skip any leading spaces */
        trimmed++;
    }
    if (trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X')) {
        char *endptr;
        unsigned int rgb;
        errno = 0;
        rgb = strtoul(trimmed, &endptr, 16);
        if (errno) {
            perror("strtoul");
            fprintf(stderr, "Failed to parse color '%s'\n", trimmed);
            exit(1);
        } else if (endptr == trimmed ||
                   /* Note: this check incorrectly rejects the
                      specific case of trailing space on hex black,
                      i.e. '0x000000 ' */
                   (endptr[0] != '\0' && rgb == 0)) {
            fprintf(stderr, "Failed to parse color '%s'\n", trimmed);
            exit(1);
        }
        xcolor.blue = (rgb & 0xff) * 257;
        rgb >>= 8;
        xcolor.green = (rgb & 0xff) * 257;
        rgb >>= 8;
        xcolor.red = (rgb & 0xff) * 257;
        status = XAllocColor(dpy, XDefaultColormap(dpy, screen), &xcolor);
    } else {
        XColor dummy;
        status = XAllocNamedColor(dpy, XDefaultColormap(dpy, screen), trimmed,
                                  &xcolor, &dummy);
    }
    if (status == 0) {
        fprintf(stderr, "Failed to allocate color when parsing '%s'\n", trimmed);
        exit(1);
    }
    return xcolor;
}

/* prepare graphic context for painting colorful frame and set RGB value of the
 * color */
static void get_frame_gc(Ghandles * g, const char *name)
{
    XGCValues values;
    XColor fcolor = parse_color(name, g->display, g->screen);
    g->label_color_rgb =
        (fcolor.red >> 8) << 16 |
        (fcolor.green >> 8) << 8 |
        (fcolor.blue >> 8);
    values.foreground = fcolor.pixel;
    g->frame_gc =
        XCreateGC(g->display, g->root_win, GCForeground, &values);
}

/* create local window - on VM request.
 * parameters are sanitized already
 */
static Window mkwindow(Ghandles * g, struct windowdata *vm_window)
{
    char *gargv[1] = { NULL };
    Window child_win;
    XSizeHints my_size_hints;    /* hints for the window manager */
    int i;
    XSetWindowAttributes attr;

    my_size_hints.flags = PSize;
    my_size_hints.width = vm_window->width;
    my_size_hints.height = vm_window->height;

    attr.override_redirect = vm_window->override_redirect;
    attr.background_pixel = g->window_background_pixel;
    child_win = XCreateWindow(g->display, g->root_win,
                    vm_window->x, vm_window->y,
                    vm_window->width,
                    vm_window->height, 0,
                    CopyFromParent,
                    CopyFromParent,
                    CopyFromParent,
                    CWOverrideRedirect | CWBackPixel, &attr);
    /* pass my size hints to the window manager, along with window
       and icon names */
    (void) XSetStandardProperties(g->display, child_win,
                      "VMapp command", "Pixmap", None,
                      gargv, 0, &my_size_hints);
    if (g->time_win != None)
        XChangeProperty(g->display, child_win, g->wm_user_time_window,
               XA_WINDOW, 32, PropModeReplace,
               (const unsigned char *)&g->time_win,
               1);
    (void) XSelectInput(g->display, child_win,
                ExposureMask | KeyPressMask | KeyReleaseMask |
                ButtonPressMask | ButtonReleaseMask |
                PointerMotionMask | EnterWindowMask | LeaveWindowMask |
                FocusChangeMask | StructureNotifyMask | PropertyChangeMask);

    /* setting WM_CLIENT_MACHINE, _NET_WM_PID, _NET_WM_PING */
    XSetWMClientMachine(g->display, child_win, &g->hostname);
    XChangeProperty(g->display, child_win, g->wm_pid, XA_CARDINAL,
            32 /* bits */ , PropModeReplace,
            (unsigned char *) &g->pid, 1);
    Atom protocols[2];
    protocols[0] = g->wmDeleteMessage;
    protocols[1] = g->wm_ping;
    XSetWMProtocols(g->display, child_win, protocols, 2);

    if (g->icon_data) {
        XChangeProperty(g->display, child_win, g->net_wm_icon, XA_CARDINAL, 32,
                PropModeReplace, (unsigned char *) g->icon_data,
                g->icon_data_len);
        XClassHint class_hint =
            { g->vmname, g->vmname };
        XSetClassHint(g->display, child_win, &class_hint);
        // perhaps set also icon_pixmap property in WM_HINTS (two Pixmaps -
        // icon and the mask), but hopefully all window managers supports
        // _NET_WM_ICON
    } else if (g->cmdline_icon) {
        XClassHint class_hint =
            { g->cmdline_icon, g->cmdline_icon };
        XSetClassHint(g->display, child_win, &class_hint);
    }
    // Set '_QUBES_LABEL' property so that Window Manager can read it and draw proper decoration
    XChangeProperty(g->display, child_win, g->qubes_label, XA_CARDINAL,
            8 /* 8 bit is enough */ , PropModeReplace,
            (unsigned char *) &g->label_index, 1);

    // Set '_QUBES_LABEL_COLOR' property so that Window Manager can read it and draw proper decoration
    XChangeProperty(g->display, child_win, g->qubes_label_color, XA_CARDINAL,
            32 /* bits */ , PropModeReplace,
            (unsigned char *) &g->label_color_rgb, 1);

    // Set '_QUBES_VMNAME' property so that Window Manager can read it and nicely display it
    XChangeProperty(g->display, child_win, g->qubes_vmname, XA_STRING,
            8 /* 8 bit is enough */ , PropModeReplace,
            (const unsigned char *) g->vmname,
            strlen(g->vmname));

    // Set '_QUBES_VMWINDOWID' property so that additional plugins can
    // synchronize window state (icon etc)
    XChangeProperty(g->display, child_win, g->qubes_vmwindowid, XA_WINDOW,
            32, PropModeReplace,
            (const unsigned char *)&vm_window->remote_winid,
            1);

    /* extra properties from command line */
    for (i = 0; i < MAX_EXTRA_PROPS; i++) {
        if (g->extra_props[i].prop) {
            XChangeProperty(g->display, child_win, g->extra_props[i].prop,
                    g->extra_props[i].type, g->extra_props[i].format,
                    PropModeReplace,
                    (const unsigned char*)g->extra_props[i].data,
                    g->extra_props[i].nelements);
        }
    }

    if (vm_window->remote_winid == FULLSCREEN_WINDOW_ID) {
        /* whole screen window */
        g->screen_window = vm_window;
    }

    return child_win;
}

static const unsigned long desktop_coordinates_size = 4;
static const unsigned long max_display_width = 1UL << 20;

/*
 * Padding enforced between override-redirect windows and the edge of
 * the work area.
 */
static const int override_redirect_padding = 0;

/* update g when the current desktop changes */
static void update_work_area(Ghandles *g) {
    unsigned long *scratch = NULL;
    unsigned long nitems, bytesleft;
    Atom act_type;
    int ret, act_fmt;

    ret = XGetWindowProperty(g->display, g->root_win, g->net_current_desktop,
        0, 1, False, XA_CARDINAL, &act_type, &act_fmt, &nitems, &bytesleft,
        (unsigned char**)&scratch);
    if (ret != Success || nitems != 1 || act_fmt != 32 ||
        act_type != XA_CARDINAL || bytesleft) {
        if (ret == Success && None == act_fmt && !act_fmt && !bytesleft) {
            if (g->log_level > 0)
                fprintf(stderr, "Cannot obtain current desktop\n");
            g->work_x = 0;
            g->work_y = 0;
            g->work_width = g->root_width;
            g->work_height = g->root_height;
            goto check_width_height;
        }
        /* Panic!  Serious window manager problem. */
        fputs("PANIC: cannot obtain current desktop\n"
              "Instead of creating a security hole we will just exit.\n",
            stderr);
        exit(1);
    }
    if (*scratch > max_display_width) {
        fprintf(stderr, "Absurd current desktop (display width %lu exceeds "
                "limit %lu), exiting\n", *scratch, max_display_width);
        exit(1);
    }
    uint32_t current_desktop = (uint32_t)*scratch;
    XFree(scratch);
    scratch = NULL;
    bool bad_work_area = false;
    ret = XGetWindowProperty(g->display, g->root_win, g->wm_workarea,
            current_desktop * desktop_coordinates_size,
            desktop_coordinates_size, False, XA_CARDINAL, &act_type, &act_fmt,
            &nitems, &bytesleft, (unsigned char**)&scratch);
    if (ret != Success) {
        fprintf(stderr, "Cannot obtain work area (ret %d)\n", ret);
        exit(1);
    }
    if (act_fmt == 0) {
        scratch = NULL;
        if (g->log_level > 0)
            fprintf(stderr, "No _NET_WORKAREA on root window\n");
        g->work_x = 0;
        g->work_y = 0;
        g->work_width = g->root_width;
        g->work_height = g->root_height;
        goto check_width_height;
    }
    if (nitems != desktop_coordinates_size || act_fmt != 32 || act_type != XA_CARDINAL) {
        fprintf(stderr,
                "Invalid _NET_WORKAREA property (window manager bug?):\n"
                "   act_fmt %d (expected 32)\n"
                "   nitems %lu (expected %lu)\n"
                "   act_type %lu (expected %lu)\n"
                "Using old values:\n"
                "      x: %d\n"
                "      y: %d\n"
                "  width: %d\n"
                " height: %d\n",
                act_fmt, nitems, desktop_coordinates_size, act_type, XA_CARDINAL,
                g->work_x, g->work_y, g->work_width, g->work_height);
        XFree(scratch);
        scratch = NULL;
        goto check_width_height;
    }
    for (unsigned long s = 0; s < desktop_coordinates_size; ++s) {
        if (scratch[s] > max_display_width) {
            fprintf(stderr,
                    "WARNING: invalid work area (window manager bug?):\n"
                    "      x: %1$lu (limit %5$lu)\n"
                    "      y: %2$lu (limit %5$lu)\n"
                    "  width: %3$lu (limit %5$lu)\n"
                    " height: %4$lu (limit %5$lu)\n"
                    "Using old values:\n"
                    "      x: %6$d\n"
                    "      y: %7$d\n"
                    "  width: %8$d\n"
                    " height: %9$d\n",
                    scratch[0], scratch[1], scratch[2], scratch[3],
                    max_display_width,
                    g->work_x, g->work_y, g->work_width, g->work_height);
            bad_work_area = true;
            break;
        }
    }
    if (!bad_work_area) {
        g->work_x = scratch[0];
        g->work_y = scratch[1];
        g->work_width = scratch[2];
        g->work_height = scratch[3];
    }
    if (g->log_level > 0)
        fprintf(stderr, "work area %lu %lu %lu %lu\n",
                scratch[0], scratch[1], scratch[2], scratch[3]);
    if (scratch != NULL)
        XFree(scratch);

check_width_height:
    if (g->work_width <= 2 * override_redirect_padding ||
        g->work_height <= 2 * override_redirect_padding) {
        /* Work area too small for a border??? */
        fprintf(stderr, "Work area %dx%d smaller than 2 * border width %d???\n",
                g->work_width, g->work_height, override_redirect_padding);
        exit(1);
    }
}

/*
 * Internal all of the atoms we will use.  For performance reasons, we perform
 * all atom interning at startup, and do so using a single XInternAtoms() call.
 */
static void intern_global_atoms(Ghandles *const g) {
    char tray_sel_atom_name[64];
    if ((unsigned)snprintf(tray_sel_atom_name, sizeof(tray_sel_atom_name),
        "_NET_SYSTEM_TRAY_S%u", DefaultScreen(g->display)) >=
        sizeof(tray_sel_atom_name))
        abort();
    const struct {
        Atom *const dest;
        const char *const name;
    } atoms_to_intern[] = {
        { &g->tray_selection, tray_sel_atom_name },
        { &g->tray_opcode, "_NET_SYSTEM_TRAY_OPCODE" },
        { &g->xembed_message, "_XEMBED" },
        { &g->xembed_info, "_XEMBED_INFO" },
        { &g->wm_state, "_NET_WM_STATE" },
        { &g->wm_state_fullscreen, "_NET_WM_STATE_FULLSCREEN" },
        { &g->wm_state_demands_attention, "_NET_WM_STATE_DEMANDS_ATTENTION" },
        { &g->wm_state_hidden, "_NET_WM_STATE_HIDDEN" },
        { &g->wm_workarea, "_NET_WORKAREA" },
        { &g->frame_extents, "_NET_FRAME_EXTENTS" },
        { &g->wm_state_maximized_vert, "_NET_WM_STATE_MAXIMIZED_VERT" },
        { &g->wm_state_maximized_horz, "_NET_WM_STATE_MAXIMIZED_HORZ" },
        { &g->qubes_label, "_QUBES_LABEL" },
        { &g->qubes_label_color, "_QUBES_LABEL_COLOR" },
        { &g->qubes_vmname, "_QUBES_VMNAME" },
        { &g->qubes_vmwindowid, "_QUBES_VMWINDOWID" },
        { &g->net_wm_icon, "_NET_WM_ICON" },
        { &g->net_current_desktop, "_NET_CURRENT_DESKTOP" },
        { &g->wm_user_time_window, "_NET_WM_USER_TIME_WINDOW" },
        { &g->wm_user_time, "_NET_WM_USER_TIME" },
        { &g->wmDeleteMessage, "WM_DELETE_WINDOW" },
        { &g->net_supported, "_NET_SUPPORTED" },
        { &g->wm_pid, "_NET_WM_PID" },
        { &g->wm_ping, "_NET_WM_PING" },
        { &g->net_wm_name, "_NET_WM_NAME" },
        { &g->net_wm_icon_name, "_NET_WM_ICON_NAME" },
        { &g->utf8_string, "UTF8_STRING" },
    };
    Atom labels[QUBES_ARRAY_SIZE(atoms_to_intern)];
    const char *names[QUBES_ARRAY_SIZE(atoms_to_intern)];
    for (size_t i = 0; i < QUBES_ARRAY_SIZE(atoms_to_intern); ++i)
        names[i] = atoms_to_intern[i].name;
    if (!XInternAtoms(g->display, (char **)names, QUBES_ARRAY_SIZE(atoms_to_intern), False, labels)) {
        fputs("Could not intern global atoms\n", stderr);
        exit(1);
    }
    for (size_t i = 0; i < QUBES_ARRAY_SIZE(atoms_to_intern); ++i)
        *atoms_to_intern[i].dest = labels[i];
}

static bool qubes_get_all_atom_properties(Display *const display,
        Window const window, Atom property, long **state_list, size_t *items) {
    assert(state_list && "NULL state_list in qubes_get_all_atom_properties");
    assert(items && "NULL items in qubes_get_all_atom_properties");
    *items = 0;
    unsigned long nitems = 0, bytesleft = 0;
    Atom act_type = None;
    int act_fmt = 0, ret;
retry:
    /* Ensure we read all of the atoms */
    *state_list = NULL;
    ret = XGetWindowProperty(display, window, property,
            0, (10 * 4 + bytesleft + 3) / 4, False, XA_ATOM, &act_type, &act_fmt,
            &nitems, &bytesleft, (unsigned char**)state_list);
    if (ret != Success) {
        XFree(*state_list);
        *state_list = NULL;
        return false;
    }
    if (bytesleft) {
        XFree(*state_list);
        goto retry;
    }
    *items = nitems;
    return true;
}

/* prepare global variables content:
 * most of them are handles to local Xserver structures */
static void mkghandles(Ghandles * g)
{
    char buf[256];
    char *list[1] = { buf };
    if (gethostname(buf, sizeof(buf)) == -1) {
        fprintf(stderr, "Cannot get GUIVM hostname!\n");
        exit(1);
    }
    XStringListToTextProperty(list, 1, &g->hostname);
    g->pid = getpid();
    int ev_base, err_base; /* ignore */
    XWindowAttributes attr;
    int i;

    if (!(g->display = XOpenDisplay(NULL)))
        err(1, "XOpenDisplay");
    if (!(g->cb_connection = XGetXCBConnection(g->display)))
        err(1, "XGetXCBConnection");
    if ((g->xen_dir_fd = open("/dev/xen", O_DIRECTORY|O_CLOEXEC|O_NOCTTY|O_RDONLY)) == -1)
        err(1, "open /dev/xen");
    if ((g->xen_fd = openat(g->xen_dir_fd, "gntdev", O_PATH|O_CLOEXEC|O_NOCTTY)) == -1)
        err(1, "open /dev/xen/gntdev");
    g->screen = DefaultScreen(g->display);
    g->root_win = RootWindow(g->display, g->screen);
    g->gc = xcb_generate_id(g->cb_connection);
    const xcb_void_cookie_t cookie = check_xcb_void(
        xcb_create_gc_aux_checked(
            g->cb_connection, g->gc, g->root_win, 0, NULL),
        "xcb_create_gc_aux_checked");
    if (!XGetWindowAttributes(g->display, g->root_win, &attr)) {
        fprintf(stderr, "Cannot query window attributes!\n");
        exit(1);
    }
    g->root_width = _VIRTUALX(attr.width);
    g->root_height = attr.height;
    g->keyboard_grabbed = 0;
    g->keyboard_ungrab_evt = 0;
    g->context = XCreateGC(g->display, g->root_win, 0, NULL);
    g->clipboard_requested = 0;
    g->clipboard_xevent_time = 0;
    intern_global_atoms(g);
    if (!g->context || xcb_request_check(g->cb_connection, cookie)) {
        fprintf(stderr, "Failed to create global graphics context!\n");
        exit(1);
    }
    if (!XQueryExtension(g->display, "MIT-SHM",
                &g->shm_major_opcode, &ev_base, &err_base))
        fprintf(stderr, "MIT-SHM X extension missing!\n");
    /* get the work area */
    XSelectInput(g->display, g->root_win, PropertyChangeMask);
    update_work_area(g);
    /* create graphical contexts */
    get_frame_gc(g, g->cmdline_color ? g->cmdline_color : "red");
    if (g->trayicon_mode == TRAY_BACKGROUND)
        init_tray_bg(g);
    else if (g->trayicon_mode == TRAY_TINT)
        init_tray_tint(g);
    /* nothing extra needed for TRAY_BORDER */
    /* parse window background color */
    g->window_background_pixel = parse_color(g->window_background_color_pre_parse,
                                             g->display, g->screen).pixel;
    /* parse -p arguments now, as we have X server connection */
    parse_cmdline_prop(g);
    /* init window lists */
    g->remote2local = list_new();
    g->wid2windowdata = list_new();
    g->screen_window = NULL;
    /* use qrexec for clipboard operations when stubdom GUI is used */
    if (g->domid != g->target_domid)
        g->qrexec_clipboard = 1;
    g->use_kdialog = getenv("KDE_SESSION_UID") ? 1 : 0;
    g->icon_data = NULL;
    g->icon_data_len = 0;
    if (g->cmdline_icon && g->cmdline_icon[0] == '/') {
        /* in case of error g->icon_data will remain NULL so cmdline_icon will
         * be used instead (as icon label) */
        g->icon_data = load_png(g->cmdline_icon, &g->icon_data_len);
        if (g->icon_data)
            fprintf(stderr, "Icon size: %lux%lu\n", g->icon_data[0], g->icon_data[1]);
    }
    g->inter_appviewer_lock_fd = open("/run/qubes/appviewer.lock",
            O_RDWR | O_CREAT, 0660);
    if (g->inter_appviewer_lock_fd < 0) {
        perror("create lock");
        exit(1);
    }
    /* ignore possible errors */
    fchmod(g->inter_appviewer_lock_fd, 0660);

    g->cursors = malloc(sizeof(Cursor) * XC_num_glyphs);
    if (!g->cursors) {
        perror("malloc");
        exit(1);
    }
    for (i = 0; i < XC_num_glyphs; i++) {
        /* X font cursors have even numbers from 0 up to XC_num_glyphs.
         * Fill the rest with None.
         */
        g->cursors[i] = (i % 2 == 0) ? XCreateFontCursor(g->display, i) : None;
    }
    long *state_list;
    size_t nitems;
    /* Get the stub window for _NET_WM_USER_TIME */
    if (!qubes_get_all_atom_properties(g->display,
            g->root_win, g->net_supported, &state_list, &nitems)) {
        fputs("Cannot get properties for global window!\n", stderr);
        exit(1);
    }
    g->time_win = None;
    for (size_t i = 0; i < nitems; ++i) {
        if ((Atom)state_list[i] == g->wm_user_time_window && g->time_win == None) {
            XSetWindowAttributes sattr = { 0 };
            g->time_win = XCreateWindow(
                    /* display */ g->display,
                    /* parent */ g->root_win,
                    /* x */ -2,
                    /* y */ -2,
                    /* width */ 1,
                    /* height */ 1,
                    /* border_width */ 0,
                    /* depth */ 0,
                    /* class */ InputOnly,
                    /* visual */ CopyFromParent,
                    /* valuemask */ 0,
                    &sattr);
            if (g->time_win == None)
                fputs("Cannot create window!\n", stderr);
        }
    }
    if (g->time_win == None)
        fputs("Falling back to setting _NET_WM_USER_TIME on the root window\n", stderr);
}

/* reload X server parameters, especially after monitor/screen layout change */
static void reload(Ghandles * g) {
    XWindowAttributes attr;

    g->screen = DefaultScreen(g->display);
    g->root_win = RootWindow(g->display, g->screen);
    if (!XGetWindowAttributes(g->display, g->root_win, &attr))
        errx(1, "Cannot query root window attributes!");
    g->root_width = _VIRTUALX(attr.width);
    g->root_height = attr.height;
    update_work_area(g);
}

/* find if window (given by id) is managed by this guid */
static struct windowdata *check_nonmanaged_window(Ghandles * g, XID id)
{
    struct genlist *item = list_lookup(g->wid2windowdata, id);
    if (!item) {
        if (g->log_level > 0)
            fprintf(stderr, "cannot lookup 0x%x in wid2windowdata\n",
                    (int) id);
        return NULL;
    }
    return item->data;
}

/* caller must take inter_appviewer_lock first. for legacy applications */
static void save_clipboard_file_xevent_timestamp(Time timestamp) {
    FILE *file;
    mode_t old_umask;

    /* grant group read/write */
    old_umask = umask(0002);
    file = fopen(QUBES_CLIPBOARD_FILENAME ".xevent", "w");
    if (!file) {
        perror("open " QUBES_CLIPBOARD_FILENAME ".xevent");
        exit(1);
    }
    fprintf(file, "%lu\n", timestamp);
    fclose(file);
    umask(old_umask);
}

/* for legacy applications */
static void save_clipboard_source_vmname(const char *vmname) {
    FILE *file;
    mode_t old_umask;

    /* grant group write */
    old_umask = umask(0002);
    file = fopen(QUBES_CLIPBOARD_FILENAME ".source", "w");
    if (!file) {
        perror("open " QUBES_CLIPBOARD_FILENAME ".source");
        exit(1);
    }
    fwrite(vmname, strlen(vmname), 1, file);
    fclose(file);
    umask(old_umask);
}

/* caller must take inter_appviewer_lock first */
static void save_clipboard_metadata(struct clipboard_metadata *metadata) {
    FILE *file;
    mode_t old_umask;

    /* grant group write */
    old_umask = umask(0002);
    file = fopen(QUBES_CLIPBOARD_FILENAME ".metadata", "w");
    if (!file) {
        perror("Can not create " QUBES_CLIPBOARD_FILENAME ".metadata file");
        exit(1);
    }
    /* Save in JSON format in key,value pairs for easy parsing.
     * Keys always inside "" (double-quotes). vmname value is also inside "" */
    fprintf(file, "{\n");
    fprintf(file, "\"vmname\":\"%s\",\n", metadata->vmname);
    fprintf(file, "\"xevent_timestamp\":%lu,\n", metadata->xevent_timestamp);
    fprintf(file, "\"successful\":%d,\n", metadata->successful);
    fprintf(file, "\"copy_action\":%d,\n", metadata->copy_action);
    fprintf(file, "\"paste_action\":%d,\n", metadata->paste_action);
    fprintf(file, "\"malformed_request\":%d,\n", metadata->malformed_request);
    fprintf(file, "\"oversized_request\":%d,\n", metadata->oversized_request);
    fprintf(file, "\"cleared\":%d,\n", metadata->cleared);
    fprintf(file, "\"qrexec_clipboard\":%d,\n", metadata->qrexec_clipboard);
    fprintf(file, "\"sent_size\":%d,\n", metadata->sent_size);
    fprintf(file, "\"buffer_size\":%d,\n", metadata->buffer_size);
    fprintf(file, "\"protocol_version_xside\":%d,\n", metadata->protocol_version_xside);
    fprintf(file, "\"protocol_version_vmside\":%d\n", metadata->protocol_version_vmside);
    // other key,value pairs could be added if needed in future
    fprintf(file, "}\n");
    fclose(file);
    umask(old_umask);
}

static bool load_clipboard_metadata(struct clipboard_metadata *metadata, bool logging) {
    FILE *file;
    char line[256];
    char key[256] = {0};
    char value[256] = {0};

    file = fopen(QUBES_CLIPBOARD_FILENAME ".metadata", "r");
    if (!file) {
        if (logging)
            perror("Can not open " QUBES_CLIPBOARD_FILENAME ".metadata file");
        return false;
    }
    // Load JSON format
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strlen(line) == 0) continue;
        if (line[strlen(line) - 1] != '\n') {
            fprintf (stderr, "Clipboard metadata line over 255 characters: %s...\n", line);
            return false;
        }
        line[strlen(line) - 1] = 0;   // remove the trailing `\n` for easy parsing
        if (strcmp(line, "") == 0) continue;
        if (strcmp(line, "{") == 0) continue;
        if (strcmp(line, "}") == 0) continue;
        if (sscanf(line, "\"%[A-Za-z0-9_-]\":%[\"A-Za-z0-9_-]", key, value) != 2) {
            fprintf (stderr, "Failed to parse clipboard metadata line: %s\n", line);
            return false;
        }
        if (strcmp(key, "vmname") == 0) {
            /* value should be less than allowed maximum vmlenght + 2
             * considering the quotation marks */
            int vlen = strlen(value);
            if ((vlen >= 32 + 2) ||
                (vlen < 2) ||
                (value[0] != '"') ||
                (value[vlen - 1] != '"')) {
                    fprintf (stderr, "Clipboard vmname value should be less than 32 characters and between double-quotes: %s\n", line);
                    return false;
            }
            strncpy (metadata->vmname, value + 1, strlen(value) - 2);
            continue;
        }

        /* From this point on, all recognized values are essentially integers */
        long unsigned int dummy;
        if (sscanf(value, "%lu", &dummy) != 1) {
            fprintf (stderr, "Failed to parse clipboard metadata: key=%s, value=%s\n", key, value);
            return false;
        }

        if (strcmp(key, "xevent_timestamp") == 0) {
            metadata->xevent_timestamp = dummy;
        } else if (strcmp(key, "successful") == 0) {
            metadata->successful = dummy;
        } else if (strcmp(key, "copy_action") == 0) {
            metadata->copy_action = dummy;
        } else if (strcmp(key, "paste_action") == 0) {
            metadata->paste_action = dummy;
        } else if (strcmp(key, "malformed_request") == 0) {
            metadata->malformed_request = dummy;
        } else if (strcmp(key, "oversized_request") == 0) {
            metadata->oversized_request = dummy;
        } else if (strcmp(key, "cleared") == 0) {
            metadata->cleared = dummy;
        } else if (strcmp(key, "qrexec_clipboard") == 0) {
            metadata->qrexec_clipboard = dummy;
        } else if (strcmp(key, "sent_size") == 0) {
            metadata->sent_size = dummy;
        } else if (strcmp(key, "buffer_size") == 0) {
            metadata->buffer_size = dummy;
        } else if (strcmp(key, "protocol_version_vmside") == 0) {
            metadata->protocol_version_vmside = dummy;
        } else if (strcmp(key, "protocol_version_xside") == 0) {
            metadata->protocol_version_xside = dummy;
        }
    }
    fclose(file);
    return true;
}

/* caller must take inter_appviewer_lock first */
static void clear_clipboard(struct clipboard_metadata *metadata) {
    if (truncate(QUBES_CLIPBOARD_FILENAME, 0)) {
        perror("failed to truncate " QUBES_CLIPBOARD_FILENAME ", trying unlink instead\n");
        unlink(QUBES_CLIPBOARD_FILENAME);
    }
    metadata->cleared = true;
    save_clipboard_metadata(metadata);
    save_clipboard_source_vmname("");
}

/* caller must take inter_appviewer_lock first */
static Time get_clipboard_xevent_timestamp(bool logging) {
    struct clipboard_metadata metadata = {0};
    FILE *file;

    /* do not do a detailed logging for metadata parsing. handle non-existent
     * .metadata file independently here */
    file = fopen(QUBES_CLIPBOARD_FILENAME ".metadata", "r");
    if (!file) {
        if (logging)
            perror("Can not get xevent timestamp from non-existent " QUBES_CLIPBOARD_FILENAME ".metadata");
        return 0;
    } else {
       fclose(file);
    }

    if (!load_clipboard_metadata(&metadata, logging)) {
        if (logging)
            perror("Can not get xevent timestamp from " QUBES_CLIPBOARD_FILENAME ".metadata");
        return 0;
    } else {
        return metadata.xevent_timestamp;
    }
}

/* fetch clippboard content from file */
/* lock already taken in is_special_keypress() */
static void get_qubes_clipboard(Ghandles *g, char **data, int *len)
{
    FILE *file;
    *len = 0;
    struct clipboard_metadata metadata = {0};
    strcpy(metadata.vmname, g->vmname);
    metadata.paste_action = true;
    metadata.xevent_timestamp = g->clipboard_xevent_time;
    metadata.buffer_size = g->clipboard_buffer_size;
    metadata.protocol_version_vmside = g->protocol_version;
    metadata.protocol_version_xside = PROTOCOL_VERSION(
        PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
    metadata.successful = false;
    file = fopen(QUBES_CLIPBOARD_FILENAME, "r");
    if (!file)
        return;
    if (fseek(file, 0, SEEK_END) < 0) {
        show_error_message(g, "secure paste: failed to seek in " QUBES_CLIPBOARD_FILENAME);
        fclose(file);
        clear_clipboard(&metadata);
        return;
    }
    *len = ftell(file);
    if (*len < 0) {
        *len = 0;
        show_error_message(g, "secure paste: failed to determine size of "
            QUBES_CLIPBOARD_FILENAME);
        fclose(file);
        clear_clipboard(&metadata);
        return;
    }
    if (*len == 0) {
        fclose(file);
        clear_clipboard(&metadata);
        return;
    }
    *data = malloc(*len);
    if (!*data) {
        perror("malloc");
        exit(1);
    }
    if (fseek(file, 0, SEEK_SET) < 0) {
        free(*data);
        *data = NULL;
        *len = 0;
        show_error_message(g, "secure paste: failed to seek in "
            QUBES_CLIPBOARD_FILENAME);
        fclose(file);
        clear_clipboard(&metadata);
        return;
    }
    *len=fread(*data, 1, *len, file);
    if (*len < 0) {
        *len = 0;
        free(*data);
        *data=NULL;
        show_error_message(g, "secure paste: failed to read from "
            QUBES_CLIPBOARD_FILENAME);
        fclose(file);
        clear_clipboard(&metadata);
        return;
    }
    fclose(file);
    metadata.sent_size = *len;
    metadata.successful = true;
    clear_clipboard(&metadata);
}

/* This is specific to Microsoft Windows and non-X11 compliant OS */
static int run_clipboard_rpc(Ghandles * g, enum clipboard_op op) {
    char *path_stdin, *path_stdout, *service_call;
    pid_t pid;
    struct rlimit rl;
    int fd;
    char domid_str[16];
    int status;
    mode_t old_umask;

    switch (op) {
        case CLIPBOARD_COPY:
            path_stdin = "/dev/null";
            path_stdout = QUBES_CLIPBOARD_FILENAME;
            service_call = g->in_dom0 ? QREXEC_COMMAND_PREFIX QUBES_SERVICE_CLIPBOARD_COPY :
                QUBES_SERVICE_CLIPBOARD_COPY;
            break;
        case CLIPBOARD_PASTE:
            path_stdin = QUBES_CLIPBOARD_FILENAME;
            path_stdout = "/dev/null";
            service_call = g->in_dom0 ? QREXEC_COMMAND_PREFIX QUBES_SERVICE_CLIPBOARD_PASTE :
                QUBES_SERVICE_CLIPBOARD_PASTE;
            break;
        default:
            /* not reachable */
            abort();
    }
    switch (pid=fork()) {
        case -1:
            perror("fork");
            exit(1);
        case 0:
            /* in case of error do not use exit(1) in child to not fire
             * atexit() registered functions; use _exit() instead (which do not
             * fire that functions) */

            /* grant group write */
            old_umask = umask(0007);
            fd = open(path_stdout, O_WRONLY|O_CREAT|O_TRUNC|O_NOCTTY, 0644);
            if (fd < 0) {
                perror("open");
                _exit(1);
            }
            umask(old_umask);
            if (op == CLIPBOARD_COPY) {
                // TODO: Handle qrexec clipboard buffer size overflow gracefully
                rl.rlim_cur = g->clipboard_buffer_size;
                rl.rlim_max = g->clipboard_buffer_size;
                setrlimit(RLIMIT_FSIZE, &rl);
            }
            dup2(fd, 1);
            close(fd);
            fd = open(path_stdin, O_RDONLY|O_NOCTTY);
            if (fd < 0) {
                perror("open");
                _exit(1);
            }
            dup2(fd, 0);
            close(fd);
            if (g->in_dom0) {
                if ((unsigned)snprintf(domid_str, sizeof(domid_str), "%d", g->target_domid)
                    >= sizeof(domid_str))
                    abort();
                execl(QREXEC_CLIENT_PATH, QREXEC_CLIENT, "-T", "-d", domid_str, service_call, (char*)NULL);
            } else
                execl(QREXEC_CLIENT_VM_PATH, QREXEC_CLIENT_VM, "-T", "--", g->vmname, service_call, (char*)NULL);
            perror("execl");
            _exit(1);
        default:
            waitpid(pid, &status, 0);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* This is specific to Microsoft Windows and non-X11 compliant OS */
static int fetch_qubes_clipboard_using_qrexec(Ghandles * g) {
    int ret;
    struct clipboard_metadata metadata = {0};

    strcpy(metadata.vmname, g->vmname);
    metadata.copy_action = true;
    metadata.qrexec_clipboard = true;
    metadata.xevent_timestamp = g->clipboard_xevent_time;
    metadata.buffer_size = g->clipboard_buffer_size;
    metadata.protocol_version_vmside = g->protocol_version;
    metadata.protocol_version_xside = PROTOCOL_VERSION(
        PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);

    inter_appviewer_lock(g, 1);
    ret = run_clipboard_rpc(g, CLIPBOARD_COPY);
    if (ret) {
        save_clipboard_source_vmname(g->vmname);
        save_clipboard_file_xevent_timestamp(g->clipboard_xevent_time);
        metadata.successful = true;
        save_clipboard_metadata(&metadata);
    } else {
        metadata.successful = false;
        clear_clipboard(&metadata);
    }

    inter_appviewer_lock(g, 0);
    return ret;
}

/* lock already taken in is_special_keypress() */
static int paste_qubes_clipboard_using_qrexec(Ghandles * g) {
    struct stat statbuf;
    int ret;
    struct clipboard_metadata metadata = {0};
    strcpy(metadata.vmname, g->vmname);
    metadata.paste_action = true;
    metadata.qrexec_clipboard = true;
    metadata.xevent_timestamp = g->clipboard_xevent_time;
    metadata.buffer_size = g->clipboard_buffer_size;
    metadata.protocol_version_vmside = g->protocol_version;
    metadata.protocol_version_xside = PROTOCOL_VERSION(
        PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);
    metadata.successful = false;

    /* Query clipboard file stat to determine its size for metadata */
    if (stat(QUBES_CLIPBOARD_FILENAME, &statbuf)) {
        show_error_message(g, "secure paste: failed to get status of " QUBES_CLIPBOARD_FILENAME);
        clear_clipboard(&metadata);
        return -1;
    }
    metadata.sent_size = statbuf.st_size;

    ret = run_clipboard_rpc(g, CLIPBOARD_PASTE);
    if (ret) {
        metadata.successful = true;
    } else {
        metadata.successful = false;
    }

    clear_clipboard(&metadata);
    return ret;
}


/* handle VM message: MSG_CLIPBOARD_DATA
 *  - checks if clipboard data was requested
 *  - store it in file (+ its metadata)
 */
static void handle_clipboard_data(Ghandles * g, unsigned int untrusted_len)
{
    FILE *file;
    char *untrusted_data;
    size_t untrusted_data_sz;
    Time clipboard_file_xevent_time;
    mode_t old_umask;

    struct clipboard_metadata metadata = {0};
    strcpy(metadata.vmname, g->vmname);
    metadata.copy_action = true;
    metadata.xevent_timestamp = g->clipboard_xevent_time;
    metadata.sent_size = untrusted_len;
    metadata.buffer_size = g->clipboard_buffer_size;
    metadata.protocol_version_vmside = g->protocol_version;
    metadata.protocol_version_xside = PROTOCOL_VERSION(
                    PROTOCOL_VERSION_MAJOR, PROTOCOL_VERSION_MINOR);

    if (g->log_level > 0)
        fprintf(stderr, "handle_clipboard_data, len=0x%x\n",
            untrusted_len);
    if ((g->protocol_version < QUBES_GUID_MIN_CLIPBOARD_4X) &&
                    (untrusted_len > MAX_CLIPBOARD_SIZE)) {
        /* malformed clipboard sizes for GUI protocol 1.7 and older */
        metadata.malformed_request = true;
    } else if (untrusted_len > MAX_CLIPBOARD_BUFFER_SIZE + 1) {
        /* malformed clipboard sizes for GUI protocol 1.8 */
        metadata.malformed_request = true;
    } else if (untrusted_len > g->clipboard_buffer_size) {
        /* clipboard size over VM limit */
        metadata.oversized_request = true;
        if (g->log_level > 0)
            fprintf(stderr, "clipboard data len %d exceeds VM's allowed!\n",
                untrusted_len);
    }
    if (metadata.malformed_request) {
        fprintf(stderr, "clipboard data len 0x%x exceeds maximum allowed!\n",
            untrusted_len);
        exit(1);
    }
    /* now sanitized */
    untrusted_data_sz = untrusted_len;
    untrusted_data = malloc(untrusted_data_sz);
    if (!untrusted_data) {
        perror("malloc");
        exit(1);
    }
    read_data(g->vchan, untrusted_data, untrusted_data_sz);
    if (!g->clipboard_requested) {
        free(untrusted_data);
        fprintf(stderr,
            "received clipboard data when not requested\n");
        return;
    }
    if (metadata.oversized_request) {
        free(untrusted_data);
        metadata.successful = false;
        clear_clipboard(&metadata);
        return;
    }
    if (metadata.sent_size == 0) {
        /* source vm clipboard is empty. clear .data and update .metadata */
        free(untrusted_data);
        metadata.successful = true;
        clear_clipboard(&metadata);
        return;
    }
    inter_appviewer_lock(g, 1);
    clipboard_file_xevent_time = get_clipboard_xevent_timestamp(g->log_level > 0);
    /* X11 time is just 32-bit miliseconds counter, which make it wrap every
     * ~50 days - something that is realistic. Handle that wrapping too. */
    if (clipboard_file_xevent_time - g->clipboard_xevent_time < (1UL<<31)) {
        /* some other clipboard operation happened in the meantime, discard
         * request */
        inter_appviewer_lock(g, 0);
        fprintf(stderr,
            "received clipboard data after some other clipboard op, discarding\n");
        return;
    }
    /* grant group write */
    old_umask = umask(0007);
    file = fopen(QUBES_CLIPBOARD_FILENAME, "w");
    if (!file) {
        show_error_message(g, "secure copy: failed to open file " QUBES_CLIPBOARD_FILENAME);
        goto error;
    }
    if (fwrite(untrusted_data, 1, untrusted_data_sz, file) != untrusted_data_sz) {
        fclose(file);
        show_error_message(g, "secure copy: failed to write to file " QUBES_CLIPBOARD_FILENAME);
        goto error;
    }
    if (fclose(file) < 0) {
        show_error_message(g, "secure copy: failed to close file " QUBES_CLIPBOARD_FILENAME);
        goto error;
    }
    save_clipboard_source_vmname(g->vmname);
    save_clipboard_file_xevent_timestamp(g->clipboard_xevent_time);
    metadata.successful = true;
    save_clipboard_metadata(&metadata);
error:
    umask(old_umask);
    inter_appviewer_lock(g, 0);
    g->clipboard_requested = 0;
    free(untrusted_data);
}

static bool evaluate_clipboard_policy_socket(Ghandles *g, int socket, const char *const source_vm);

static bool evaluate_clipboard_policy_domU(Ghandles *g, const char *const source_vm) {
    int sockets[2]; /* 1 is for writing to dom0, 0 is for reading the result */
    pid_t pid;
    if (socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0, sockets)) {
        perror("socketpair");
        return false;
    }
    switch(pid=fork()) {
        case -1:
            perror("fork");
            return false;
        case 0:
            if (dup2(sockets[0], 0) == -1 || dup2(sockets[0], 1) == -1) {
                perror("dup2");
                _exit(1);
            }
            execl(QREXEC_CLIENT_VM_PATH, QREXEC_CLIENT_VM, "--",
                  "@adminvm", QUBES_SERVICE_EVAL_GUI "+"
                  QUBES_SERVICE_CLIPBOARD_PASTE, NULL);
            perror("execl");
            _exit(1);
        default:
            break;
    }
    close(sockets[0]);
    const bool policy_allowed = evaluate_clipboard_policy_socket(g, sockets[1], source_vm);
    close(sockets[1]);
    int status;
    /* this can only fail with EINTR, on which we retry */
    while (waitpid(pid, &status, 0) == -1)
        assert(errno == EINTR && "invalid return from kernel");
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, QREXEC_CLIENT_VM " failed\n");
        return false;
    } else {
        return policy_allowed;
    }
}

static bool write_all(int socket, void *buf, size_t len) {
    while (len) {
        ssize_t bytes_written_or_err = send(socket, buf, len, MSG_NOSIGNAL);
        if (bytes_written_or_err == -1) {
            if (errno == EINTR)
                continue;
            perror("send");
            return false;
        }
        assert((size_t)bytes_written_or_err <= len && "buffer overread");
        len -= (size_t)bytes_written_or_err;
        buf = (void *)((char *)buf + bytes_written_or_err);
    }
    return true;
}

static bool evaluate_clipboard_policy_dom0(Ghandles *g,
        const char *const source_vm) {
    const int sockfd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    bool status = false;
    if (sockfd < 0) {
        perror("socket");
        return false;
    }
    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
        .sun_path = QUBES_POLICY_EVAL_SIMPLE_SOCKET,
    };
    _Static_assert(sizeof addr - offsetof(struct sockaddr_un, sun_path) >=
                   sizeof QUBES_POLICY_EVAL_SIMPLE_SOCKET,
                   "struct sockaddr_un too small");
    if (connect(sockfd, (struct sockaddr *)&addr,
                sizeof QUBES_POLICY_EVAL_SIMPLE_SOCKET +
                offsetof(struct sockaddr_un, sun_path)) != 0) {
        perror("connect");
        goto fail;
    }
    if (!write_all(sockfd, QREXEC_PRELUDE_CLIPBOARD_PASTE, sizeof(QREXEC_PRELUDE_CLIPBOARD_PASTE)))
        goto fail;
    status = evaluate_clipboard_policy_socket(g, sockfd, source_vm);
fail:
    close(sockfd);
    return status;
}

static bool evaluate_clipboard_policy_socket(
        Ghandles *g, int socket, const char *const source_vm) {
    char buf[63];
    size_t const source_vm_len = strlen(source_vm);
    size_t const dst_vm_len = strlen(g->vmname);
    if (source_vm_len > 31 || dst_vm_len > 31) {
        fputs("VM name too long\n", stderr);
        exit(1); // this means something has gone horribly wrong elsewhere
    }
    memcpy(buf, source_vm, source_vm_len + 1);
    memcpy(buf + source_vm_len + 1, g->vmname, dst_vm_len);
    if (!write_all(socket, buf, source_vm_len + dst_vm_len + 1))
        return false;
    if (shutdown(socket, SHUT_WR)) {
        perror("shutdown");
        return false;
    }
    memset(buf, 0, sizeof buf);
    size_t bytes_received = 0;
    for (;;) {
        size_t const recv_len = sizeof buf - bytes_received - 1;
        if (recv_len == 0)
            break;
        ssize_t res = recv(socket, buf + bytes_received, recv_len, MSG_WAITALL);
        if (!res)
            break; /* EOF */
        if (res == -1) {
            if (errno == EINTR)
                continue;
            perror("recv");
            return false;
        }
        assert((size_t)res <= recv_len && "did we just overflow a buffer?");
        bytes_received += (size_t)res;
    }
    if (bytes_received >= QUBES_POLICY_ACCESS_ALLOWED_LEN &&
             !memcmp(buf, QUBES_POLICY_ACCESS_ALLOWED, QUBES_POLICY_ACCESS_ALLOWED_LEN))
        return true;
    else if (bytes_received >= QUBES_POLICY_ACCESS_DENIED_LEN &&
             !memcmp(buf, QUBES_POLICY_ACCESS_DENIED, QUBES_POLICY_ACCESS_DENIED_LEN)) {
        fprintf(stderr, "Operation refused by dom0\n");
        return false;
    } else {
        fprintf(stderr, "Received invalid response from dom0: '%s'\n", buf);
        return false;
    }
}

static int evaluate_clipboard_policy(Ghandles * g) {
    int fd;
    ssize_t len;
    char source_vm[255];
    bool result;

    fd = open(QUBES_CLIPBOARD_FILENAME ".source", O_RDONLY);
    if (fd < 0)
        return 0;

    len = read(fd, source_vm, sizeof(source_vm)-1);
    if (len < 0) {
        perror("read");
        close(fd);
        return 0;
    }
    close(fd);
    if (len == 0) {
        /* empty clipboard */
        return 0;
    }
    source_vm[len] = 0;
    if (g->in_dom0)
        result = evaluate_clipboard_policy_dom0(g, source_vm);
    else
        result = evaluate_clipboard_policy_domU(g, source_vm);
    if (!result) {
        char error_msg[1024];
        snprintf(error_msg, sizeof(error_msg),
            "Pasting from %s to %s is denied by policies",
            source_vm,
            g->vmname);
        show_message(g, "ERROR", error_msg, NOTIFY_EXPIRES_DEFAULT);
    }
    return result;
}

_Static_assert(CURSOR_X11_MAX == CURSOR_X11 + XC_num_glyphs, "protocol bug");

/* handle VM message: MSG_CURSOR */
static void handle_cursor(Ghandles *g, struct windowdata *vm_window)
{
    struct msg_cursor untrusted_msg;
    int cursor_id;
    Cursor cursor;

    read_struct(g->vchan, untrusted_msg);
    /* sanitize start */
    if (untrusted_msg.cursor & CURSOR_X11) {
        VERIFY(untrusted_msg.cursor < CURSOR_X11_MAX);
        cursor_id = untrusted_msg.cursor & ~CURSOR_X11;
    } else {
        VERIFY(untrusted_msg.cursor == CURSOR_DEFAULT);
        cursor_id = -1;
    }
    /* sanitize end */

    if (g->log_level > 0)
        fprintf(stderr, "handle_cursor, cursor = 0x%x\n",
                untrusted_msg.cursor);

    if (cursor_id < 0)
        cursor = None;
    else {
        /*
         * Should be true if CURSOR_X11_MAX == CURSOR_X11 + XC_num_glyphs,
         * but we don't want a protocol constant to depend on X headers.
         */
        assert(cursor_id < XC_num_glyphs);

        cursor = g->cursors[cursor_id];
    }
    XDefineCursor(g->display, vm_window->local_winid, cursor);
}

/* check and handle guid-special keys
 * currently only for inter-vm clipboard copy/paste
 */
static int is_special_keypress(Ghandles * g, const XKeyEvent * ev, XID local_winid, XID remote_winid)
{
    struct msg_hdr hdr;
    char *data;
    int len;
    Time clipboard_file_xevent_time;

    if (!g->keyboard_grabbed)
    {
        /* copy */
        if (((int)ev->state & SPECIAL_KEYS_MASK) == g->copy_seq_mask
            && ev->keycode == XKeysymToKeycode(g->display, g->copy_seq_key)) {
            if (ev->type != KeyPress)
                return 1;
            g->clipboard_xevent_time = ev->time;
            if (g->qrexec_clipboard) {
                int ret = fetch_qubes_clipboard_using_qrexec(g);
                if (g->log_level > 0)
                    fprintf(stderr, "secure copy: %s\n", ret?"success":"failed");
            } else {
                g->clipboard_requested = 1;
                hdr.type = MSG_CLIPBOARD_REQ;
                hdr.window = remote_winid;
                hdr.untrusted_len = 0;
                if (g->log_level > 0)
                    fprintf(stderr, "secure copy succeeded\n");
                write_struct(g->vchan, hdr);
            }
            return 1;
        }

        /* paste */
        if (((int)ev->state & SPECIAL_KEYS_MASK) == g->paste_seq_mask
            && ev->keycode == XKeysymToKeycode(g->display, g->paste_seq_key)) {
            if (ev->type != KeyPress)
                return 1;
            inter_appviewer_lock(g, 1);
            clipboard_file_xevent_time = get_clipboard_xevent_timestamp(g->log_level > 0);
            /* X11 time is just 32-bit miliseconds counter, which make it wrap every
             * ~50 days - something that is realistic. Handle that wrapping too. */
            if (clipboard_file_xevent_time - ev->time < (1UL<<31)) {
                /* some other clipboard operation happened in the meantime, discard
                 * request */
                inter_appviewer_lock(g, 0);
                fprintf(stderr,
                        "received clipboard xevent after some other clipboard op, discarding\n");
                return 1;
            }
            if (!evaluate_clipboard_policy(g)) {
                inter_appviewer_lock(g, 0);
                return 1;
            }
            if (g->qrexec_clipboard) {
                int ret = paste_qubes_clipboard_using_qrexec(g);
                if (g->log_level > 0)
                    fprintf(stderr, "secure paste: %s\n", ret?"success":"failed");
            } else {
                hdr.type = MSG_CLIPBOARD_DATA;
                if (g->log_level > 0)
                    fprintf(stderr, "secure paste\n");
                get_qubes_clipboard(g, &data, &len);
                if (len > 0) {
                    /* MSG_CLIPBOARD_DATA used to use the window field to pass the length
                       of the blob, be aware when working with old implementations. */
                    if (g->protocol_version < QUBES_GUID_MIN_CLIPBOARD_DATA_LEN_IN_LEN)
                        hdr.window = len;
                    else
                        hdr.window = remote_winid;
                    hdr.untrusted_len = len;
                    real_write_message(g->vchan, (char *) &hdr, sizeof(hdr),
                            data, len);
                    free(data);
                }
            }
            inter_appviewer_lock(g, 0);

            return 1;
        }
    }

    /* grab keyboard */
    if (((int)ev->state & SPECIAL_KEYS_MASK) == g->keyboard_grab_seq_mask
        && ev->keycode == XKeysymToKeycode(g->display, g->keyboard_grab_seq_key)) {
        if (ev->type != KeyPress)
            return 1;
        if (g->keyboard_grabbed)
        {
            XUngrabKeyboard(g->display, CurrentTime);
            g->keyboard_grabbed = 0;
            if (g->log_level > 0)
                fprintf(stderr, "keyboard ungrabbed\n");
        }
        else
        {
            int status = XGrabKeyboard(g->display, local_winid, True, GrabModeAsync, GrabModeAsync, CurrentTime);
            if (status == GrabSuccess)
            {
                g->keyboard_grabbed = 1;
                if (g->log_level > 0)
                    fprintf(stderr, "keyboard grabbed\n");
            }
        }
        return 1;
    }

    return 0;
}

static void update_wm_user_time(Ghandles *const g, const Window window,
        const Time time) {
    static_assert(sizeof time == sizeof(long), "Wrong size of X11 time");
    XChangeProperty(g->display, g->time_win != None ? g->time_win : window,
            g->wm_user_time, XA_CARDINAL,
            32, PropModeReplace,
            (const unsigned char *)&time,
            1);
}

/* handle local Xserver event: XKeyEvent
 * send it to relevant window in VM
 */
static void process_xevent_keypress(Ghandles * g, const XKeyEvent * ev)
{
    struct msg_hdr hdr;
    struct msg_keypress k;
    CHECK_NONMANAGED_WINDOW(g, ev->window);
    update_wm_user_time(g, ev->window, ev->time);
    if (is_special_keypress(g, ev, vm_window->local_winid, vm_window->remote_winid))
        return;
    k.type = ev->type;
    k.x = ev->x;
    k.y = ev->y;
    k.state = ev->state;
    k.keycode = ev->keycode;
    hdr.type = MSG_KEYPRESS;
    hdr.window = vm_window->remote_winid;
    write_message(g->vchan, hdr, k);
//      fprintf(stderr, "win 0x%x(0x%x) type=%d keycode=%d\n",
//              (int) ev->window, hdr.window, k.type, k.keycode);
}

// debug routine
#ifdef DEBUG
static void dump_mapped(Ghandles * g)
{
    struct genlist *item = g->wid2windowdata->next;
    for (; item != g->wid2windowdata; item = item->next) {
        struct windowdata *c = item->data;
        if (c->is_mapped) {
            if (g->log_level > 1)
                fprintf(stderr,
                    "id 0x%x(0x%x) w=0x%x h=0x%x rx=%d ry=%d ovr=%d\n",
                    (int) c->local_winid,
                    (int) c->remote_winid, c->width,
                    c->height, c->x, c->y,
                    c->override_redirect);
        }
    }
}
#endif

/* handle local Xserver event: XButtonEvent
 * same as XKeyEvent - send to relevant window in VM */
static void process_xevent_button(Ghandles * g, const XButtonEvent * ev)
{
    struct msg_hdr hdr;
    struct msg_button k;
    CHECK_NONMANAGED_WINDOW(g, ev->window);
    update_wm_user_time(g, ev->window, ev->time);

    k.type = ev->type;

    k.x = ev->x;
    k.y = ev->y;
    k.state = ev->state;
    k.button = ev->button;
    hdr.type = MSG_BUTTON;
    hdr.window = vm_window->remote_winid;
    write_message(g->vchan, hdr, k);
    if (g->log_level > 1)
        fprintf(stderr,
            "xside: win 0x%x(0x%x) type=%d button=%d x=%d, y=%d\n",
            (int) ev->window, hdr.window, k.type, k.button,
            k.x, k.y);
    if (vm_window->is_docked && ev->type == ButtonPress) {
        /* Take focus to that icon, to make possible keyboard nagivation
         * through the menu */
        XSetInputFocus(g->display, vm_window->local_winid, RevertToParent,
                CurrentTime);
    }
}

/* handle local Xserver event: XCloseEvent
 * send to relevant window in VM */
static void process_xevent_close(Ghandles * g, XID window)
{
    struct msg_hdr hdr;
    CHECK_NONMANAGED_WINDOW(g, window);
    hdr.type = MSG_CLOSE;
    hdr.window = vm_window->remote_winid;
    hdr.untrusted_len = 0;
    write_struct(g->vchan, hdr);
}

/* handle local Xserver event XReparentEvent
 * store information whether the window is reparented into some frame window */
static void process_xevent_reparent(Ghandles *g, XReparentEvent *ev) {
    CHECK_NONMANAGED_WINDOW(g, ev->window);

    /* check if current parent matches the one in the VM - this means the
     * window is reparented back into original structure (window manager
     * restart?)
     */
    if (ev->parent == g->root_win)
        vm_window->local_frame_winid = 0;
    else
        vm_window->local_frame_winid = ev->parent;
    if (g->log_level > 1)
        fprintf(stderr,
            "process_xevent_reparent(synth %d) local 0x%x remote 0x%x, "
            "local parent 0x%x, frame window 0x%x\n",
            ev->send_event,
            (int) vm_window->local_winid, (int) vm_window->remote_winid,
            (int)ev->parent, (int)vm_window->local_frame_winid);
}

/* send configure request for specified VM window */
static void send_configure(Ghandles * g, struct windowdata *vm_window, int x, int y,
        int w, int h)
{
    struct msg_hdr hdr;
    struct msg_configure msg = {0};
    hdr.type = MSG_CONFIGURE;
    hdr.window = vm_window->remote_winid;
    msg.height = h;
    msg.width = w;
    msg.x = x;
    msg.y = y;
    write_message(g->vchan, hdr, msg);
}

/* fix position of docked tray icon;
 * icon position is relative to embedder 0,0 so we must translate it to
 * absolute position */
static int fix_docked_xy(Ghandles * g, struct windowdata *vm_window, const char *caller)
{

    /* docked window is reparented to root_win on vmside */
    Window win;
    int x, y, ret = 0;
    if (XTranslateCoordinates
        (g->display, vm_window->local_winid, g->root_win,
         0, 0, &x, &y, &win) == True) {
        /* ignore offscreen coordinates */
        if (x < 0 || y < 0)
            x = y = 0;
        if (vm_window->x != x || vm_window->y != y)
            ret = 1;
        if (g->log_level > 1)
            fprintf(stderr,
                "fix_docked_xy(from %s), calculated xy %d/%d, was "
                "%d/%d\n", caller, x, y, vm_window->x,
                vm_window->y);
        vm_window->x = x;
        vm_window->y = y;
    }
    return ret;
}

/*
 * Undo the calculations that fix_docked_xy did, then perform move&resize.
 * Optionally apply vm_window->override_redirect. */
static void moveresize_vm_window(Ghandles * g, struct windowdata *vm_window,
                                 bool apply_override_redirect)
{
    int x = 0, y = 0;
    Window win;
    Atom act_type;
    long *frame_extents; // left, right, top, bottom
    unsigned long nitems, bytesleft;
    int ret, act_fmt;
    XSetWindowAttributes attr;

    assert(!(vm_window->is_docked && apply_override_redirect));

    if (!vm_window->is_docked) {
        /* we have window content coordinates, but XMoveResizeWindow requires
         * left top *border* pixel coordinates (if any border is present). */
        ret = XGetWindowProperty(g->display, vm_window->local_winid,
                g->frame_extents, 0, desktop_coordinates_size,
                False, XA_CARDINAL, &act_type, &act_fmt, &nitems, &bytesleft,
                (unsigned char**)&frame_extents);
        if (ret == Success && nitems == desktop_coordinates_size) {
            x = vm_window->x - frame_extents[0];
            y = vm_window->y - frame_extents[2];
            XFree(frame_extents);
        } else {
            /* assume no border */
            x = vm_window->x;
            y = vm_window->y;
        }
    } else
        if (!XTranslateCoordinates(g->display, g->root_win,
                      vm_window->local_winid, vm_window->x,
                      vm_window->y, &x, &y, &win))
            return;
    if (g->log_level > 1) {
        fprintf(stderr,
            "XMoveResizeWindow local 0x%x remote 0x%x, xy %d %d (vm_window is %d %d) wh %d %d\n",
            (int) vm_window->local_winid,
            (int) vm_window->remote_winid, x, y, vm_window->x,
            vm_window->y, vm_window->width, vm_window->height);
        if (apply_override_redirect)
            fprintf(stderr,
                "Setting override-redirect(%d) for the above%s\n",
                vm_window->override_redirect, vm_window->is_mapped ? ", with unmap+map" : "");
    }
    /* When changing override-redirect of a mapped window, unmap the window
     * first, to let the window manager notice the change */
    if (vm_window->is_mapped && apply_override_redirect)
        XUnmapWindow(g->display, vm_window->local_winid);
    XMoveResizeWindow(g->display, vm_window->local_winid, x, y,
              vm_window->width, vm_window->height);
    if (apply_override_redirect) {
        attr.override_redirect = vm_window->override_redirect;
        XChangeWindowAttributes(g->display, vm_window->local_winid,
                CWOverrideRedirect, &attr);
    }
    if (vm_window->is_mapped && apply_override_redirect)
        XMapWindow(g->display, vm_window->local_winid);
}

/* force window to not hide its frame
 * checks if at least border_width is from every screen edge (and fix if not)
 * Exception: allow window to be entirely off the screen */
static int force_on_screen(Ghandles * g, struct windowdata *vm_window,
            int border_width, const char *caller)
{
    int do_move = 0, reason = 0;

    /* Internal consistency checks */
    if (g->work_width < 2 * border_width ||
        g->work_height < 2 * border_width) {
        fputs("BUG: work_width or work_height too small in force_on_screen\n", stderr);
        abort(); // to get a core dump
    }

    if (vm_window->width > MAX_WINDOW_WIDTH ||
        vm_window->height > MAX_WINDOW_HEIGHT) {
        fputs("BUG: window width or height too large in force_on_screen\n", stderr);
        abort();
    }
    /* end consistency checks */

    int x = vm_window->x, y = vm_window->y,
        w = vm_window->width, h = vm_window->height;

    /* Check if the window is entirely off-screen */
    if (x >= g->root_width ||
        y >= g->root_height ||
        (x < 0 && x + w <= 0) ||
        (y < 0 && w + h <= 0)) {
        if (g->log_level > 0) {
            fprintf(stderr,
                    "force_on_screen(from %s) returns 0: window 0x%x, xy %d %d, wh %d %d, root window %d %d borderwidth %d is entirely off-screen\n",
            caller,
            (int) vm_window->local_winid, x, y, w, h,
            g->root_width, g->root_height,
            border_width);
        }

        return 0;
    }

    enum MOVE_REASONS {
        TOO_WIDE = (1 << 0),
        TOO_TALL = (1 << 1),
        LEFT_BORDER_OFF_SCREEN = (1 << 2),
        TOP_BORDER_OFF_SCREEN = (1 << 3),
        RIGHT_BORDER_OFF_SCREEN = (1 << 4),
        BOTTOM_BORDER_OFF_SCREEN = (1 << 5),
    };
    const int border_x = border_width + g->work_x,
        border_y = border_width + g->work_y,
        max_width = g->work_width - 2 * border_width,
        max_height = g->work_height - 2 * border_width;
    /* Sanitize width and height */
    if (w > max_width) {
        vm_window->width = max_width;
        do_move = 1;
        reason |= TOO_WIDE;
    }
    if (h > max_height) {
        vm_window->height = max_height;
        do_move = 1;
        reason |= TOO_TALL;
    }
    /* Sanitize left */
    if (x < border_x) {
        vm_window->x = border_x;
        do_move = 1;
        reason |= LEFT_BORDER_OFF_SCREEN;
    }
    /* Sanitize top */
    if (y < border_y) {
        vm_window->y = border_y;
        do_move = 1;
        reason |= TOP_BORDER_OFF_SCREEN;
    }
    /* Sanitize right */
    if (vm_window->x > border_x + (max_width - (int)vm_window->width)) {
        vm_window->x = border_x + (max_width - (int)vm_window->width);
        do_move = 1;
        reason |= RIGHT_BORDER_OFF_SCREEN;
    }
    /* Sanitize bottom */
    if (vm_window->y > border_y + (max_height - (int)vm_window->height)) {
        vm_window->y = border_y + (max_height - (int)vm_window->height);
        do_move = 1;
        reason |= BOTTOM_BORDER_OFF_SCREEN;
    }
    if ((do_move || g->log_level > 1) && g->log_level > 0)
        fprintf(stderr,
            "force_on_screen(from %s) returns %d (reason %x): window 0x%x, xy %d %d, wh %d %d, work area %d %d %d %d borderwidth %d\n",
            caller, do_move, reason,
            (int) vm_window->local_winid, x, y, w, h,
            g->work_x, g->work_y, g->work_width, g->work_height,
            border_width);
    return do_move;
}

static int validate_override_redirect(Ghandles * g, struct windowdata *vm_window,
                                      int req_override_redirect)
{
    static int warning_shown;
    uint64_t avail, desired;
    const char * warning_msg = "This VM has attempted to create a very large window "
        "in a manner that would have prevented you from closing it and regaining "
        "the control of Qubes OS\'s graphical user interface.\n\n"
        "As a protection measure, the \"override_redirect\" flag of the window "
        "in question has been unset. If this creates unexpected issues in the "
        "handling of this VM\'s windows, please set \"override_redirect_protection\" "
        "to \"false\" for this VM in /etc/qubes/guid.conf to disable this protection "
        "measure and restart the VM.\n\n"
        "This message will only appear once per VM per session. Please click on this "
        "notification to close it.";

    req_override_redirect = !!req_override_redirect;

    if (g->disable_override_redirect) {
        return 0;
    }

    /* do not allow override redirect for a docked window */
    if (vm_window->is_docked)
        return 0;

    /*
     * Do not allow changing override_redirect of a mapped window, but still
     * force it off if window is getting too big.
     */
    if (vm_window->is_mapped)
        req_override_redirect = vm_window->override_redirect;

    avail = (uint64_t) g->work_width * (uint64_t) g->work_height;
    desired = (uint64_t) vm_window->width * (uint64_t) vm_window->height;

    if (g->override_redirect_protection && req_override_redirect &&
            desired > ((avail * MAX_OVERRIDE_REDIRECT_PERCENTAGE) / 100)) {
        req_override_redirect = 0;

        if (g->log_level > 0)
            fprintf(stderr,
                    "%s unset override_redirect for "
                    "local 0x%x remote 0x%x, "
                    "window w=%u h=%u, work w=%d h=%d\n",
                    __func__,
                    (unsigned) vm_window->local_winid,
                    (unsigned) vm_window->remote_winid,
                    vm_window->width, vm_window->height,
                    g->work_width, g->work_height);

        /* Show a message to the user, but do this only once. */
        if (!warning_shown) {
            show_message(g, "WARNING", warning_msg,
                    NOTIFY_EXPIRES_NEVER);
            warning_shown = 1;
        }
    }

    return req_override_redirect;
}

/* handle local Xserver event: XConfigureEvent
 * after some checks/fixes send to relevant window in VM */
static void process_xevent_configure(Ghandles * g, const XConfigureEvent * ev)
{
    int x, y;
    CHECK_NONMANAGED_WINDOW(g, ev->window);
    if (g->log_level > 1)
        fprintf(stderr,
            "process_xevent_configure(synth %d) local 0x%x remote 0x%x, %d/%d, was "
            "%d/%d, xy %d/%d was %d/%d\n",
            ev->send_event,
            (int) vm_window->local_winid,
            (int) vm_window->remote_winid, ev->width,
            ev->height, vm_window->width, vm_window->height,
            ev->x, ev->y, vm_window->x, vm_window->y);
    /* non-synthetic events are about window position/size relative to the embeding
     * frame window (if applies), synthetic one (produced by window manager) are
     * about window position relative to original window parent.
     * Because synthetic one isn't generated in all the cases (for example
     * resize window without changing its position), process both of them and
     * possibly ignore if nothing have changed
     * See http://tronche.com/gui/x/icccm/sec-4.html#s-4.1.5 for details
     */
    if (!ev->send_event && vm_window->local_frame_winid) {
        /* needs to translate coordinates */
        Window child;
        XTranslateCoordinates(g->display, ev->window, g->root_win,
                0, 0, &x, &y, &child);
        if (g->log_level > 1)
            fprintf(stderr, "  translated to %d/%d\n", x, y);
    } else {
        x = ev->x;
        y = ev->y;
    }

    if ((int)vm_window->width == ev->width
        && (int)vm_window->height == ev->height && vm_window->x == x
        && vm_window->y == y)
        return;
    vm_window->width = ev->width;
    vm_window->height = ev->height;
    if (!vm_window->is_docked) {
        vm_window->x = x;
        vm_window->y = y;
    } else
        fix_docked_xy(g, vm_window, "process_xevent_configure");

    if (vm_window->override_redirect
        && force_on_screen(g, vm_window, override_redirect_padding,
                           "handle_map")) {
        if (g->log_level > 0)
            fprintf(stderr,
                    "Something moved/resized override-redirect window "
                    "0x%lx(0x%lx) outside of allowed area, moving it back\n",
                    vm_window->local_winid, vm_window->remote_winid);
        moveresize_vm_window(g, vm_window, false);
        XFlush(g->display);
    }

// if AppVM has not unacknowledged previous resize msg, do not send another one
    if (vm_window->have_queued_configure)
        return;
    if (vm_window->remote_winid != FULLSCREEN_WINDOW_ID)
        vm_window->have_queued_configure = 1;
    send_configure(g, vm_window, vm_window->x, vm_window->y,
               vm_window->width, vm_window->height);
}

/* handle VM message: MSG_CONFIGURE
 * check if we like new dimensions/position and move relevant window */
static void handle_configure_from_vm(Ghandles * g, struct windowdata *vm_window)
{
    struct msg_configure untrusted_conf;
    int x, y;
    unsigned width, height;
    int conf_changed, override_redirect;

    read_struct(g->vchan, untrusted_conf);
    if (g->log_level > 1)
        fprintf(stderr,
            "handle_configure_from_vm, local 0x%x remote 0x%x, %d/%d, was"
            " %d/%d, ovr=%d (ignored), xy %d/%d, was %d/%d\n",
            (int) vm_window->local_winid,
            (int) vm_window->remote_winid,
            untrusted_conf.width, untrusted_conf.height,
            vm_window->width, vm_window->height,
            untrusted_conf.override_redirect, untrusted_conf.x,
            untrusted_conf.y, vm_window->x, vm_window->y);
    /* sanitize start */
    if (untrusted_conf.width > MAX_WINDOW_WIDTH)
        untrusted_conf.width = MAX_WINDOW_WIDTH;
    if (untrusted_conf.height > MAX_WINDOW_HEIGHT)
        untrusted_conf.height = MAX_WINDOW_HEIGHT;
    width = untrusted_conf.width;
    height = untrusted_conf.height;
    VERIFY(width > 0 && height > 0);
    x = max(-MAX_WINDOW_WIDTH,
                min((int) untrusted_conf.x, MAX_WINDOW_WIDTH));
    y = max(-MAX_WINDOW_HEIGHT,
                min((int) untrusted_conf.y, MAX_WINDOW_HEIGHT));
    /* ignore override_redirect in MSG_CONFIGURE */
    untrusted_conf.override_redirect = vm_window->override_redirect;
    /* sanitize end */
    if (vm_window->width != width || vm_window->height != height ||
        vm_window->x != x || vm_window->y != y)
        conf_changed = 1;
    else
        conf_changed = 0;

    /* We do not allow a docked window to change its size, period. */
    if (vm_window->is_docked) {
        if (conf_changed)
            send_configure(g, vm_window, vm_window->x,
                       vm_window->y, vm_window->width,
                       vm_window->height);
        vm_window->have_queued_configure = 0;
        return;
    }


    if (vm_window->have_queued_configure) {
        if (conf_changed) {
            send_configure(g, vm_window, vm_window->x,
                       vm_window->y, vm_window->width,
                       vm_window->height);
            return;
        } else {
            // same dimensions; this is an ack for our previously sent configure req
            vm_window->have_queued_configure = 0;
        }
    }
    if (!conf_changed)
        return;
    vm_window->width = width;
    vm_window->height = height;
    vm_window->x = x;
    vm_window->y = y;
    /* verify if the window is (still) entitled for the override-redirect flag */
    override_redirect =
        validate_override_redirect(g, vm_window, vm_window->override_redirect);
    if (override_redirect)
        // do not let menu window hide its color frame by moving outside of the screen
        // if it is located offscreen, then allow negative x/y
        force_on_screen(g, vm_window, override_redirect_padding,
                        "handle_configure_from_vm");
    if (vm_window->override_redirect != override_redirect) {
        vm_window->override_redirect = override_redirect;
        moveresize_vm_window(g, vm_window, true);
    } else {
        moveresize_vm_window(g, vm_window, false);
    }
}

/* handle local Xserver event: EnterNotify, LeaveNotify
 * send it to VM, but alwo we use it to fix docked
 * window position */
static void process_xevent_crossing(Ghandles * g, const XCrossingEvent * ev)
{
    struct msg_hdr hdr;
    struct msg_crossing k;
    CHECK_NONMANAGED_WINDOW(g, ev->window);

    if (ev->type == EnterNotify) {
        char keys[32];
        XQueryKeymap(g->display, keys);
        hdr.type = MSG_KEYMAP_NOTIFY;
        hdr.window = 0;
        write_message(g->vchan, hdr, keys);
    }
    /* move tray to correct position in VM */
    if (vm_window->is_docked &&
        fix_docked_xy(g, vm_window, "process_xevent_crossing")) {
        send_configure(g, vm_window, vm_window->x, vm_window->y,
                   vm_window->width, vm_window->height);
    }

    hdr.type = MSG_CROSSING;
    hdr.window = vm_window->remote_winid;
    k.type = ev->type;
    k.x = ev->x;
    k.y = ev->y;
    k.state = ev->state;
    k.mode = ev->mode;
    k.detail = ev->detail;
    k.focus = ev->focus;
    write_message(g->vchan, hdr, k);
}

/* handle local Xserver event: XMotionEvent
 * send to relevant window in VM */
static void process_xevent_motion(Ghandles * g, const XMotionEvent * ev)
{
    struct msg_hdr hdr;
    struct msg_motion k;
    CHECK_NONMANAGED_WINDOW(g, ev->window);

    k.x = ev->x;
    k.y = ev->y;
    k.state = ev->state;
    k.is_hint = ev->is_hint;
    hdr.type = MSG_MOTION;
    hdr.window = vm_window->remote_winid;
    write_message(g->vchan, hdr, k);
//      fprintf(stderr, "motion in 0x%x", ev->window);
}

/* handle local Xserver event: FocusIn, FocusOut
 * send to relevant window in VM */
static void process_xevent_focus(Ghandles * g, const XFocusChangeEvent * ev)
{
    struct msg_hdr hdr;
    struct msg_focus k;
    CHECK_NONMANAGED_WINDOW(g, ev->window);

    if (ev->mode == NotifyWhileGrabbed && ev->type == FocusOut)
    {
        if (g->keyboard_ungrab_evt)
        {
            XSetInputFocus(g->display, vm_window->local_winid, RevertToParent, CurrentTime);
            g->keyboard_ungrab_evt = 0;
            return;
        }
        if (g->keyboard_grabbed)
        {
            Window focus_return;
            int revert_to_return;
            XGetInputFocus(g->display, &focus_return, &revert_to_return);
            XUngrabKeyboard(g->display, CurrentTime);
            g->keyboard_grabbed = 0;
            if (g->log_level > 0)
                fprintf(stderr, "keyboard ungrabbed\n");
            /* A hack to focus back on the previously grabbed window after the screen locker exit.
             * When the sceeen locker is triggered while a window is grabbed, then the grabbed
             * window is losing the focus and the focus is set to return to None after the screen
             * locker exit.
             */
            if (focus_return == None && revert_to_return == RevertToNone)
            {
                XSetInputFocus(g->display, vm_window->local_winid, RevertToParent, CurrentTime);
                g->keyboard_ungrab_evt = 1;
                return;
            }
        }
    }

    /* Ignore everything other than normal, non-temporary focus change. In
     * practice it ignores NotifyGrab and NotifyUngrab. VM does not have any
     * way to grab focus in dom0, so it shouldn't care about those events. Grab
     * is used by window managers during task switching (either classic task
     * switcher, or KDE "present windows" feature).
     */
    if (ev->mode != NotifyNormal && ev->mode != NotifyWhileGrabbed)
        return;

    if (ev->type == FocusIn) {
        char keys[32];
        XQueryKeymap(g->display, keys);
        hdr.type = MSG_KEYMAP_NOTIFY;
        hdr.window = 0;
        write_message(g->vchan, hdr, keys);
    }
    hdr.type = MSG_FOCUS;
    hdr.window = vm_window->remote_winid;
    k.type = ev->type;
    /* override NotifyWhileGrabbed with NotifyNormal b/c VM shouldn't care
     * about window manager details during focus switching
     */
    k.mode = NotifyNormal;
    k.detail = ev->detail;
    write_message(g->vchan, hdr, k);
}

/* update given fragment of window image
 * can be requested by VM (MSG_SHMIMAGE) and Xserver (XExposeEvent)
 * parameters are not sanitized earlier - we must check it carefully
 * also do not let to cover forced colorful frame (for undecoraded windows)
 */
static void do_shm_update(Ghandles * g, struct windowdata *vm_window,
           int untrusted_x, int untrusted_y, int untrusted_w,
           int untrusted_h)
{
    int border_width = BORDER_WIDTH;
    int border_padding = 0; /* start forced border x pixels from the edge */
    int x = 0, y = 0, w = 0, h = 0;
    ASSERT_WIDTH_UNSIGNED(vm_window->width);
    ASSERT_HEIGHT_UNSIGNED(vm_window->height);

    /* sanitize start */
    if (untrusted_x < 0 || untrusted_y < 0 || untrusted_w < 0 || untrusted_h < 0) {
        if (g->log_level > 1)
            fprintf(stderr,
                "do_shm_update for 0x%x(remote 0x%x), x=%d, y=%d, w=%d, h=%d ?\n",
                (int) vm_window->local_winid,
                (int) vm_window->remote_winid, untrusted_x,
                untrusted_y, untrusted_w, untrusted_h);
        return;
    }
    // checked in handle_create() and handle_configure_from_vm()
    assert(vm_window->x >= -MAX_WINDOW_WIDTH && vm_window->x <= MAX_WINDOW_WIDTH &&
           "vm_window->x should have been rejected earlier");
    assert(vm_window->y >= -MAX_WINDOW_HEIGHT && vm_window->y <= MAX_WINDOW_HEIGHT &&
           "vm_window->y should have been rejected earlier");
    // now known: untrusted_x, untrusted_y, untrusted_w, and untrusted_h
    // are not negative
    if (vm_window->shmseg != QUBES_NO_SHM_SEGMENT) {
        // image_width and image_height are not negative
        // (checked in handle_mfndump and handle_window_dump)
        ASSERT_WIDTH(vm_window->image_width);
        ASSERT_HEIGHT(vm_window->image_height);

        // now do actual calculations
        x = min(untrusted_x, vm_window->image_width);
        // now: x is not negative and not greater than vm_window->image_width
        y = min(untrusted_y, vm_window->image_height);
        // now: y is not negative and not greater than vm_window->image_height
        w = min(untrusted_w, vm_window->image_width - x);
        // now: w is not negative and not greater than vm_window->image_width
        h = min(untrusted_h, vm_window->image_height - y);
        // now: h is not negative and not greater than vm_window->image_height
    } else if (g->screen_window) {
        /* update only onscreen window part */
        // image_width and image_height are not negative
        // (checked in handle_mfndump and handle_window_dump)
        ASSERT_WIDTH(g->screen_window->image_width);
        ASSERT_HEIGHT(g->screen_window->image_height);

        // now do actual calculations
        if (vm_window->x >= g->screen_window->image_width ||
                vm_window->y >= g->screen_window->image_height) {
            // window is entirely off-screen
            return;
        }
        // now: vm_window->x is less than g->screen_window->image_width
        // now: vm_window->y is less than g->screen_window->image_height
        // now: g->screen_window->image_width - vm_window->x > 0
        // now: g->screen_window->image_height - vm_window->y > 0

        if (vm_window->x < 0 && vm_window->x+untrusted_x < 0) {
            // we know vm_window->x is not less than -MAX_WINDOW_WIDTH, so this
            // is not UB.
            untrusted_x = -vm_window->x;
        }
        // now: untrusted_x + vm_window->x is not negative and untrusted_x is not
        // negative

        if (vm_window->y < 0 && vm_window->y+untrusted_y < 0) {
            // we know vm_window->y is not less than -MAX_WINDOW_HEIGHT, so this
            // is not UB.
            untrusted_y = -vm_window->y;
        }
        // now: untrusted_y + vm_window->y is not negative and untrusted_y is
        // not negative

        // vm_window->x is greater than INT_MIN + MAX_WINDOW_WIDTH, so this is not UB
        x = min(untrusted_x, g->screen_window->image_width - vm_window->x);
        // now: x is not negative and x + vm_window->x is not negative
        // now: x + vm_window->x is not greater than g->screen_window->image_width

        // vm_window->y is greater than INT_MIN + MAX_WINDOW_HEIGHT, so this is not UB
        y = min(untrusted_y, g->screen_window->image_height - vm_window->y);
        // now: y is not negative and y + vm_window->y is not negative
        // now: y + vm_window->y is not greater than g->screen_window->image_height

        w = min(untrusted_w, g->screen_window->image_width - vm_window->x - x);
        // now: w is not negative and not greater than g->screen_window->image_width
        // also: g->screen_window->image_width >= (vm_window->x + x + w)
        // so if the code that forces the window on-screen makes vm_window->x + x
        // exceed g->screen_window->image_width, w will become negative, causing
        // the code to return early.

        h = min(untrusted_h, g->screen_window->image_height - vm_window->y - y);
        // now: h is not negative and not greater than g->screen_window->image_height
        // also: g->screen_window->image_height >= (vm_window->y + y + h)
        // so if the code that forces the window on-screen makes vm_window->y + y
        // exceed g->screen_window->image_height, h will become negative, causing
        // the code to return early.

        // if the requested area is outside of the window, this will be caught
        // by the code below that checks for frames being overwritten
    } else {
        /* no image to update, will return after possibly drawing a frame */
        // width and height are not negative
        // (checked in handle_mfndump and handle_window_dump)
        ASSERT_WIDTH(vm_window->image_width);
        ASSERT_HEIGHT(vm_window->image_height);

        x = min(untrusted_x, (int)vm_window->width);
        // now: x is not negative and not greater than vm_window->width
        y = min(untrusted_y, (int)vm_window->height);
        // now: y is not negative and not greater than vm_window->height
        w = min(untrusted_w, (int)vm_window->width - x);
        // now: w is not negative and not greater than vm_window->width
        h = min(untrusted_h, (int)vm_window->height - y);
        // now: h is not negative and not greater than vm_window->height
    }

    /* sanitize end */

    if (!vm_window->override_redirect) {
        // Window Manager will take care of the frame...
        border_width = 0;
    }

    if (vm_window->is_docked) {
        if (g->trayicon_border) {
            border_width = 1;
            border_padding = g->trayicon_border - 1;
        } else
            border_width = 0;
    }

    assert(x >= 0 && y >= 0 && w >= 0 && h >= 0);
    assert(border_width >= 0);

    bool do_border = false;
    if ((int)vm_window->width <= border_width * 2
        || (int)vm_window->height <= border_width * 2) {
        /* window contains only (forced) frame, so no content to update */
        XFillRectangle(g->display, vm_window->local_winid,
                   g->frame_gc, 0, 0,
                   vm_window->width,
                   vm_window->height);
        return;
    }

    const int right = x + w, bottom = y + h;
    /* force frame to be visible: */
    /*   * left */
    if (border_width > x) { // window would cover left border
        if (w <= border_width - x)
            return; /* nothing left to update */
        w -= border_width - x;
        x = border_width;
        do_border = 1;
    }
    /*   * right */
    const int right_allowed = vm_window->width - border_width;
    if (right > right_allowed) { // window would cover right border
        if (right_allowed <= x)
            return; /* nothing left to update */
        w = right_allowed - x;
        do_border = 1;
    }
    /*   * top */
    if (border_width > y) { // window would cover top border
        if (h <= border_width - y)
            return; /* nothing left to update */
        h -= (border_width - y);
        y = border_width;
        do_border = 1;
    }
    /*   * bottom */
    const int bottom_allowed = vm_window->height - border_width;
    if (bottom > bottom_allowed) { // window would cover bottom border
        if (bottom_allowed <= y)
            return; /* nothing left to update */
        h = bottom_allowed - y;
        do_border = 1;
    }

    /* again check if something left to update */
    if (w <= 0 || h <= 0)
        return;

    if (g->log_level > 1)
        fprintf(stderr,
                "  do_shm_update for 0x%x(remote 0x%x), after border calc: x=%d, y=%d, w=%d, h=%d\n",
                (int) vm_window->local_winid,
                (int) vm_window->remote_winid,
                x, y, w, h);

    if (vm_window->is_docked && g->trayicon_mode != TRAY_BORDER) {
        if (g->trayicon_mode == TRAY_BACKGROUND)
            fill_tray_bg_and_update(g, vm_window, x, y, w, h);
        else if (g->trayicon_mode == TRAY_TINT)
            tint_tray_and_update(g, vm_window, x, y, w, h);
        else
            assert(0 && "Invalid trayicon_mode in do_shm_update");
    } else {
        if (vm_window->shmseg != QUBES_NO_SHM_SEGMENT) {
            put_shm_image(g, vm_window->local_winid, vm_window, x, y, w, h, x, y);
        } else if (g->screen_window && g->screen_window->shmseg != QUBES_NO_SHM_SEGMENT) {
            // vm_window->x+x and vm_window->y+y are the position relative to
            // the screen, while x and y are the position relative to the window
            put_shm_image(g,
                          vm_window->local_winid,
                          g->screen_window,
                          vm_window->x + x,
                          vm_window->y + y,
                          w, h, x, y);
        }
        /* else no window content to update, but still draw a frame (if needed) */
    }
    if (!do_border)
        return;
    /* if border_width is 0 the loop body will not execute */
    for (int i = border_padding; i < border_padding + border_width; i++)
        XDrawRectangle(g->display, vm_window->local_winid,
                   g->frame_gc, i, i,
                   vm_window->width - 1 - 2 * i,
                   vm_window->height - 1 - 2 * i);

}

/* handle local Xserver event: XExposeEvent
 * update relevant part of window using stored image
 */
static void process_xevent_expose(Ghandles * g, const XExposeEvent * ev)
{
    CHECK_NONMANAGED_WINDOW(g, ev->window);
    do_shm_update(g, vm_window, ev->x, ev->y, ev->width, ev->height);
}

/* handle local Xserver event: XMapEvent
 * after some checks, send to relevant window in VM */
static void process_xevent_mapnotify(Ghandles * g, const XMapEvent * ev)
{
    int ret;
    XWindowAttributes attr;
    CHECK_NONMANAGED_WINDOW(g, ev->window);
    if (vm_window->is_mapped)
        return;
    ret = XGetWindowAttributes(g->display, vm_window->local_winid, &attr);
    if (!ret)
        return;
    if (attr.map_state != IsViewable && !vm_window->is_docked) {
        /* Unmap windows that are not visible on vmside.
         * WM may try to map non-viewable windows ie. when
         * switching desktops.
         */
        (void) XUnmapWindow(g->display, vm_window->local_winid);
        if (g->log_level > 1)
            fprintf(stderr, "WM tried to map 0x%x, revert\n",
                (int) vm_window->local_winid);
    } else {
        /* Tray windows shall be visible always */
        struct msg_hdr hdr;
        struct msg_map_info map_info;
        map_info.override_redirect = attr.override_redirect;
        hdr.type = MSG_MAP;
        hdr.window = vm_window->remote_winid;
        write_message(g->vchan, hdr, map_info);
        if (vm_window->is_docked
            && fix_docked_xy(g, vm_window,
                     "process_xevent_mapnotify"))
            send_configure(g, vm_window, vm_window->x,
                       vm_window->y, vm_window->width,
                       vm_window->height);
    }
}

static inline uint32_t flags_from_atom(Ghandles * g, Atom a) {
    if (a == g->wm_state_fullscreen)
        return WINDOW_FLAG_FULLSCREEN;
    else if (a == g->wm_state_demands_attention)
        return WINDOW_FLAG_DEMANDS_ATTENTION;
    else if (a == g->wm_state_hidden)
        return WINDOW_FLAG_MINIMIZE;
    else {
        /* ignore unsupported states */
    }
    return 0;
}

/* handle local Xserver event: XPropertyEvent
 * currently only _NET_WM_STATE is examined */
static void process_xevent_propertynotify(Ghandles *g, const XPropertyEvent *const ev)
{
    long *state_list;
    size_t nitems;
    unsigned long i;
    int maximize_flags_seen;
    uint32_t flags;
    struct msg_hdr hdr;
    struct msg_window_flags msg;

    if (ev->window == g->root_win) {
        if (ev->state == PropertyNewValue)
            update_work_area(g);
        return;
    }
    CHECK_NONMANAGED_WINDOW(g, ev->window);
    if (ev->atom == g->wm_state) {
        if (!vm_window->is_mapped)
            return;
        if (ev->state == PropertyNewValue) {
            if (!qubes_get_all_atom_properties(g->display,
                    vm_window->local_winid, g->wm_state, &state_list, &nitems)) {
                if (g->log_level > 0) {
                    fprintf(stderr, "Failed to get 0x%x window state details\n", (int)ev->window);
                    return;
                }
            }
            flags = 0;
            /* check if both VERT and HORZ states are set */
            maximize_flags_seen = 0;
            for (i = 0; i < nitems; i++) {
                const Atom state = (Atom)state_list[i];
                flags |= flags_from_atom(g, state);
                if (state == g->wm_state_maximized_vert)
                    maximize_flags_seen |= 1;
                if (state == g->wm_state_maximized_horz)
                    maximize_flags_seen |= 2;
            }
            if (flags & WINDOW_FLAG_FULLSCREEN) {
                /* if user triggered real fullscreen, forget about the fake
                 * one, otherwise application will not be notified when going
                 * back from fullscreen to maximize ("fake fullscreen") */
                vm_window->fullscreen_maximize_requested = 0;
            }
            if (vm_window->fullscreen_maximize_requested) {
                if (maximize_flags_seen == 3) {
                    /* if fullscreen request was converted to maximize request,
                     * then convert maximize ack to fullscreen ack */
                    flags |= WINDOW_FLAG_FULLSCREEN;
                } else {
                    /* going out of emulated fullscreen mode */
                    vm_window->fullscreen_maximize_requested = 0;
                }
            }
            XFree(state_list);
        } else { /* PropertyDelete */
            flags = 0;
        }
        if (flags == vm_window->flags_set) {
            /* no change */
            return;
        }
        hdr.type = MSG_WINDOW_FLAGS;
        hdr.window = vm_window->remote_winid;
        msg.flags_set = flags & ~vm_window->flags_set;
        msg.flags_unset = ~flags & vm_window->flags_set;
        write_message(g->vchan, hdr, msg);
        vm_window->flags_set = flags;
    }
}

/* handle local Xserver event: _XEMBED
 * if window isn't mapped already - map it now */
static void process_xevent_xembed(Ghandles * g, const XClientMessageEvent * ev)
{
    CHECK_NONMANAGED_WINDOW(g, ev->window);
    if (g->log_level > 1)
        fprintf(stderr, "_XEMBED message %ld\n", ev->data.l[1]);
    if (ev->data.l[1] == XEMBED_EMBEDDED_NOTIFY) {
        if (vm_window->is_docked < 2) {
            vm_window->is_docked = 2;
            if (!vm_window->is_mapped && !g->invisible)
                XMapWindow(g->display, ev->window);
            /* move tray to correct position in VM */
            if (fix_docked_xy
                (g, vm_window, "process_xevent_xembed")) {
                send_configure(g, vm_window, vm_window->x,
                           vm_window->y,
                           vm_window->width,
                           vm_window->height);
            }
        }
    } else if (ev->data.l[1] == XEMBED_FOCUS_IN) {
        struct msg_hdr hdr;
        struct msg_focus k;
        char keys[32];
        XQueryKeymap(g->display, keys);
        hdr.type = MSG_KEYMAP_NOTIFY;
        hdr.window = 0;
        write_message(g->vchan, hdr, keys);
        hdr.type = MSG_FOCUS;
        hdr.window = vm_window->remote_winid;
        k.type = FocusIn;
        k.mode = NotifyNormal;
        k.detail = NotifyNonlinear;
        write_message(g->vchan, hdr, k);
    }

}

/* get current time */
static int64_t ebuf_current_time_ms()
{
    int64_t timeval;
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    timeval = (((int64_t)spec.tv_sec) * 1000LL) + (((int64_t)spec.tv_nsec) / 1000000LL);
    return timeval;
}

/* get random delay value */
static uint32_t ebuf_random_delay(uint32_t upper_bound, uint32_t lower_bound)
{
    uint32_t maxval;
    uint32_t randval;
    size_t randsize;
    union ebuf_rand ebuf_rand_data;

    if (lower_bound >= upper_bound) {
        if (lower_bound > upper_bound) {
            fprintf(stderr,
                    "Bug detected - lower_bound > upper_bound, events may get briefly stuck");
        }
        return upper_bound;
    }

    maxval = upper_bound - lower_bound + 1;

    do {
        randsize = getrandom(ebuf_rand_data.raw, sizeof(uint32_t), 0);
        if (randsize != sizeof(uint32_t))
            continue;
    } while (ebuf_rand_data.val > UINT32_MAX - ((uint32_t)((((uint64_t)UINT32_MAX) + 1) % maxval)));

    randval = ebuf_rand_data.val % maxval;
    return lower_bound + randval;
}

/* queue input event */
static void ebuf_queue_xevent(Ghandles * g, XEvent xev)
{
    int64_t current_time;
    uint32_t random_delay;
    struct ebuf_entry *new_ebuf_entry;
    uint32_t lower_bound;

    current_time = ebuf_current_time_ms();

    /* 
     * Each event is scheduled by taking the current time and adding a delay.
     * We do not want events later in the queue having a release timestamp
     * that is *less* than an event earlier in the queue. This means that
     * whatever delay we add *must* be at least enough to give a release
     * timestamp larger than the one generated last time. To facilitate this,
     * we set lower_bound to the scheduled release time of the last event
     * minus the current time. Some sanity checks are included to make sure
     * lower_bound is never less than 0 or greater than ebuf_max_delay.
     */
    lower_bound = min(max(g->ebuf_prev_release_time - current_time, 0), g->ebuf_max_delay);

    random_delay = ebuf_random_delay(g->ebuf_max_delay, lower_bound);
    new_ebuf_entry = malloc(sizeof(struct ebuf_entry));
    if (new_ebuf_entry == NULL) {
        perror("Could not allocate ebuf_entry:");
        exit(1);
    }
    if (current_time > 0 && random_delay > (INT64_MAX - current_time)) {
        fprintf(stderr, "Event scheduler overflow detected, cannot continue");
        exit(1);
    }
    new_ebuf_entry->time = current_time + random_delay;
    new_ebuf_entry->xev = xev;
    TAILQ_INSERT_TAIL(&(g->ebuf_head), new_ebuf_entry, entries);
    g->ebuf_prev_release_time = new_ebuf_entry->time;
}

/* dispatch local Xserver event */
static void process_xevent_core(Ghandles * g, XEvent event_buffer)
{
    switch (event_buffer.type) {
    case KeyPress:
    case KeyRelease:
        process_xevent_keypress(g, (XKeyEvent *) & event_buffer);
        break;
    case ReparentNotify:
        process_xevent_reparent(g, (XReparentEvent *) &event_buffer);
        break;
    case ConfigureNotify:
        process_xevent_configure(g, (XConfigureEvent *) &
                     event_buffer);
        break;
    case ButtonPress:
    case ButtonRelease:
        process_xevent_button(g, (XButtonEvent *) & event_buffer);
        break;
    case MotionNotify:
        process_xevent_motion(g, (XMotionEvent *) & event_buffer);
        break;
    case EnterNotify:
    case LeaveNotify:
        process_xevent_crossing(g,
                    (XCrossingEvent *) & event_buffer);
        break;
    case FocusIn:
    case FocusOut:
        process_xevent_focus(g,
                     (XFocusChangeEvent *) & event_buffer);
        break;
    case Expose:
        process_xevent_expose(g, (XExposeEvent *) & event_buffer);
        break;
    case MapNotify:
        process_xevent_mapnotify(g, (XMapEvent *) & event_buffer);
        break;
    case PropertyNotify:
        process_xevent_propertynotify(g, (XPropertyEvent *) & event_buffer);
        break;
    case ClientMessage:
//              fprintf(stderr, "xclient, atom=%s\n",
//                      XGetAtomName(g->display,
//                                   event_buffer.xclient.message_type));
        if (event_buffer.xclient.message_type == g->xembed_message) {
            process_xevent_xembed(g, (XClientMessageEvent *) &
                          event_buffer);
        } else if ((Atom)event_buffer.xclient.data.l[0] ==
               g->wmDeleteMessage) {
            if (g->log_level > 0)
                fprintf(stderr, "close for 0x%x\n",
                    (int) event_buffer.xclient.window);
            process_xevent_close(g,
                         event_buffer.xclient.window);
        } else if ((Atom)event_buffer.xclient.data.l[0] ==
               g->wm_ping) {
            XClientMessageEvent *ev = (XClientMessageEvent *) &event_buffer;
            ev->window = g->root_win;
            XSendEvent(g->display, g->root_win, False,
                            (SubstructureNotifyMask|SubstructureRedirectMask),
                            &event_buffer);
            if (g->log_level > 1)
                fprintf(stderr, "Received ping request from Window Manager\n");
        }
        break;
    default:;
    }
}

/* dispatch queued events */
static void ebuf_release_xevents(Ghandles * g)
{
    int64_t current_time;
    struct ebuf_entry *current_ebuf_entry;

    current_time = ebuf_current_time_ms();
    while ((current_ebuf_entry = TAILQ_FIRST(&(g->ebuf_head)))
        && (current_time >= current_ebuf_entry->time)) {
        XEvent event_buffer = current_ebuf_entry->xev;
        process_xevent_core(g, event_buffer);
        TAILQ_REMOVE(&(g->ebuf_head), current_ebuf_entry, entries);
        free(current_ebuf_entry);
    }
    current_ebuf_entry = TAILQ_FIRST(&(g->ebuf_head));
    if (current_ebuf_entry == NULL) {
        g->ebuf_next_timeout = VCHAN_DEFAULT_POLL_DURATION;
    } else {
        g->ebuf_next_timeout = (int)(current_ebuf_entry->time - current_time);
    }
}

/* handle or queue local Xserver event */
static void process_xevent(Ghandles * g)
{
    XEvent event_buffer;
    XNextEvent(g->display, &event_buffer);
    if (g->ebuf_max_delay > 0) {
        ebuf_queue_xevent(g, event_buffer);
    } else {
        process_xevent_core(g, event_buffer);
    }
}


/* handle VM message: MSG_SHMIMAGE
 * pass message data to do_shm_update - there input validation will be done */
static void handle_shmimage(Ghandles * g, struct windowdata *vm_window)
{
    struct msg_shmimage untrusted_mx;

    read_struct(g->vchan, untrusted_mx);
    if (!vm_window->is_mapped && !vm_window->is_docked)
        return;
    if (g->log_level >= 2) {
        fprintf(stderr, "shmimage for 0x%x(remote 0x%x), x: %d, y: %d, w: %d, h: %d\n",
                (int) vm_window->local_winid, (int) vm_window->remote_winid,
                untrusted_mx.x, untrusted_mx.y, untrusted_mx.width,
                untrusted_mx.height);
    }
    /* WARNING: passing raw values, input validation is done inside of
     * do_shm_update */
    do_shm_update(g, vm_window, untrusted_mx.x, untrusted_mx.y,
              untrusted_mx.width, untrusted_mx.height);
}

/* handle VM message: MSG_CREATE
 * checks given attributes and create appropriate window in local Xserver
 * (using mkwindow) */
static void handle_create(Ghandles * g, XID window)
{
    struct windowdata *vm_window;
    struct msg_create untrusted_crt;

    vm_window =
        (struct windowdata *) calloc(1, sizeof(struct windowdata));
    if (!vm_window) {
        perror("calloc(vm_window in handle_create)");
        exit(1);
    }
    vm_window->shmseg = QUBES_NO_SHM_SEGMENT;
    /*
       vm_window->is_mapped = 0;
       vm_window->local_winid = 0;
       vm_window->dest = vm_window->src = vm_window->pix = 0;
     */
    read_struct(g->vchan, untrusted_crt);
    /* sanitize start */
    VERIFY((int) untrusted_crt.width >= 0
           && (int) untrusted_crt.height >= 0);
    vm_window->width =
        min((int) untrusted_crt.width, MAX_WINDOW_WIDTH);
    vm_window->height =
        min((int) untrusted_crt.height, MAX_WINDOW_HEIGHT);
    vm_window->x = max(-MAX_WINDOW_WIDTH,
                           min((int) untrusted_crt.x, MAX_WINDOW_WIDTH));
    vm_window->y = max(-MAX_WINDOW_HEIGHT,
                           min((int) untrusted_crt.y, MAX_WINDOW_HEIGHT));
    vm_window->override_redirect =
        validate_override_redirect(g, vm_window, !!(untrusted_crt.override_redirect));
    untrusted_crt.parent = 0; /* ignore the parent field */
    /* sanitize end */
    vm_window->remote_winid = window;
    if (!list_insert(g->remote2local, window, vm_window)) {
        fprintf(stderr, "list_insert(g->remote2local) failed\n");
        exit(1);
    }
    vm_window->transient_for = NULL;
    vm_window->local_winid = mkwindow(&ghandles, vm_window);
    if (g->log_level > 0)
        fprintf(stderr,
            "Created 0x%x(0x%x) ovr=%d x/y %d/%d w/h %d/%d\n",
            (int) vm_window->local_winid, (int) window,
            vm_window->override_redirect,
            vm_window->x, vm_window->y,
            vm_window->width, vm_window->height);
    if (!list_insert
        (g->wid2windowdata, vm_window->local_winid, vm_window)) {
        fprintf(stderr, "list_insert(g->wid2windowdata failed\n");
        exit(1);
    }

    /* do not allow to hide color frame off the screen */
    if (vm_window->override_redirect
        && force_on_screen(g, vm_window, override_redirect_padding,
                           "handle_create"))
        moveresize_vm_window(g, vm_window, false);
}

/* Check if window (to be destroyed) is not used anywhere. If it is, act
 * accordingly:
 *  - if it is a set as transient_for - clear that hint
 */
static void check_window_references(Ghandles * g, struct windowdata *vm_window)
{
    struct genlist *iter;

    list_for_each(iter, g->wid2windowdata) {
        struct windowdata *iter_window = iter->data;
        if (iter_window->transient_for == vm_window) {
            fprintf(stderr, "Window 0x%x is still set as transient_for "
                    "for a 0x%x window, but VM tried to destroy it\n",
                    (int)vm_window->local_winid, (int)iter_window->local_winid);
            XDeleteProperty(g->display, vm_window->local_winid,
                    XA_WM_TRANSIENT_FOR);
            iter_window->transient_for = NULL;
        }
    }
}

/* handle VM message: MSG_DESTROY
 * destroy window locally, as requested */
static void handle_destroy(Ghandles * g, struct genlist *l)
{
    struct genlist *l2;
    struct windowdata *vm_window = l->data;
    /* check if this window is referenced anywhere */
    check_window_references(g, vm_window);
    /* then destroy */
    XDestroyWindow(g->display, vm_window->local_winid);
    if (g->log_level > 0)
        fprintf(stderr, " XDestroyWindow 0x%x\n",
            (int) vm_window->local_winid);
    release_mapped_mfns(g, vm_window);
    l2 = list_lookup(g->wid2windowdata, vm_window->local_winid);
    list_remove(l);
    list_remove(l2);
    if (vm_window == g->screen_window)
        g->screen_window = NULL;
    /* Inform the agent that the window has been destroyed.
     * Mandatory in protocol version 1.5+, and harmless for older
     * versions. */
    if (g->protocol_version >= QUBES_GUID_MIN_BIDIRECTIONAL_MSG_DESTROY) {
        struct msg_hdr delete_id = {
            .type = MSG_DESTROY,
            .window = vm_window->remote_winid,
            .untrusted_len = 0,
        };
        write_struct(g->vchan, delete_id);
    }
    free(vm_window);
}

/* validate single UTF-8 character
 * return bytes count of this character, or 0 if the character is invalid */
static int validate_utf8_char(unsigned char *untrusted_c) {
    int tails_count = 0;
    int total_size = 0;
    /* it is safe to access byte pointed by the parameter and the next one
     * (which can be terminating NULL), but every next byte can access only if
     * neither of previous bytes was NULL
     */

    /* According to http://www.ietf.org/rfc/rfc3629.txt:
     *   UTF8-char   = UTF8-1 / UTF8-2 / UTF8-3 / UTF8-4
     *   UTF8-1      = %x00-7F
     *   UTF8-2      = %xC2-DF UTF8-tail
     *   UTF8-3      = %xE0 %xA0-BF UTF8-tail / %xE1-EC 2( UTF8-tail ) /
     *                 %xED %x80-9F UTF8-tail / %xEE-EF 2( UTF8-tail )
     *   UTF8-4      = %xF0 %x90-BF 2( UTF8-tail ) / %xF1-F3 3( UTF8-tail ) /
     *                 %xF4 %x80-8F 2( UTF8-tail )
     *   UTF8-tail   = %x80-BF
     */

    if (*untrusted_c <= 0x7F) {
        return 1;
    } else if (*untrusted_c >= 0xC2 && *untrusted_c <= 0xDF) {
        total_size = 2;
        tails_count = 1;
    } else switch (*untrusted_c) {
        case 0xE0:
            untrusted_c++;
            total_size = 3;
            if (*untrusted_c >= 0xA0 && *untrusted_c <= 0xBF)
                tails_count = 1;
            else
                return 0;
            break;
        case 0xE1: case 0xE2: case 0xE3: case 0xE4:
        case 0xE5: case 0xE6: case 0xE7: case 0xE8:
        case 0xE9: case 0xEA: case 0xEB: case 0xEC:
            /* 0xED */
        case 0xEE:
        case 0xEF:
            total_size = 3;
            tails_count = 2;
            break;
        case 0xED:
            untrusted_c++;
            total_size = 3;
            if (*untrusted_c >= 0x80 && *untrusted_c <= 0x9F)
                tails_count = 1;
            else
                return 0;
            break;
        case 0xF0:
            untrusted_c++;
            total_size = 4;
            if (*untrusted_c >= 0x90 && *untrusted_c <= 0xBF)
                tails_count = 2;
            else
                return 0;
            break;
        case 0xF1:
        case 0xF2:
        case 0xF3:
            total_size = 4;
            tails_count = 3;
            break;
        case 0xF4:
            untrusted_c++;
            if (*untrusted_c >= 0x80 && *untrusted_c <= 0x8F)
                tails_count = 2;
            else
                return 0;
            break;
        default:
            return 0;
    }

    while (tails_count-- > 0) {
        untrusted_c++;
        if (!(*untrusted_c >= 0x80 && *untrusted_c <= 0xBF))
            return 0;
    }
    return total_size;
}

/* replace non-printable characters with '_'
 * given string must be NULL terminated already */
static void sanitize_string_from_vm(unsigned char *untrusted_s, int allow_utf8)
{
    int utf8_ret;
    for (; *untrusted_s; untrusted_s++) {
        // allow only non-control ASCII chars
        if (*untrusted_s >= 0x20 && *untrusted_s <= 0x7E)
            continue;
        if (allow_utf8 && *untrusted_s >= 0x80) {
            utf8_ret = validate_utf8_char(untrusted_s);
            if (utf8_ret > 0) {
                /* loop will do one additional increment */
                untrusted_s += utf8_ret - 1;
                continue;
            }
        }
        *untrusted_s = '_';
    }
}

/* handle VM message: MSG_WMNAME
 * remove non-printable characters and pass to X server */
static void handle_wmname(Ghandles * g, struct windowdata *vm_window)
{
    XTextProperty text_prop;
    struct msg_wmname untrusted_msg;
    char buf[sizeof(untrusted_msg.data) + sizeof(g->vmname) + 3];
    size_t name_len;
    char *list[1] = { buf };

    read_struct(g->vchan, untrusted_msg);
    /* sanitize start */
    untrusted_msg.data[sizeof(untrusted_msg.data) - 1] = 0;
    // If the agent has changed the end of very long title to U+2026 but utf8 is disabled
    name_len = strlen(untrusted_msg.data);
    if (!g->allow_utf8_titles
        && name_len > 3
        && strcmp(untrusted_msg.data + name_len - 3, "\xE2\x80\xA6") == 0) {
        strcpy(untrusted_msg.data + name_len - 3, "...");
    }
    sanitize_string_from_vm((unsigned char *) (untrusted_msg.data),
                g->allow_utf8_titles);

    if (g->prefix_titles)
        snprintf(buf, sizeof(buf), "[%s] %s", g->vmname, untrusted_msg.data);
    else
        snprintf(buf, sizeof(buf), "%s", untrusted_msg.data);
    /* sanitize end */
    if (g->log_level > 1)
        fprintf(stderr, "set title for window 0x%x\n",
            (int) vm_window->local_winid);
    Xutf8TextListToTextProperty(g->display, list, 1, XUTF8StringStyle,
                    &text_prop);
    XSetWMName(g->display, vm_window->local_winid, &text_prop);
    XChangeProperty(g->display, vm_window->local_winid, g->net_wm_name,
        g->utf8_string, 8, PropModeReplace, (unsigned char *) buf, strlen(buf));
    XSetWMIconName(g->display, vm_window->local_winid, &text_prop);
    XChangeProperty(g->display, vm_window->local_winid, g->net_wm_icon_name,
        g->utf8_string, 8, PropModeReplace, (unsigned char *) buf, strlen(buf));
    XFree(text_prop.value);
}

/* handle VM message: MSG_WMCLASS
 * remove non-printable characters and pass to X server, prefixed with VM name */
static void handle_wmclass(Ghandles * g, struct windowdata *vm_window)
{
    struct msg_wmclass untrusted_msg;
    char res_class[sizeof(untrusted_msg.res_class) + sizeof(g->vmname) + 3];
    char res_name[sizeof(untrusted_msg.res_name) + sizeof(g->vmname) + 3];
    XClassHint class_hint;

    read_struct(g->vchan, untrusted_msg);
    if (vm_window->is_mapped) {
        /* ICCCM 4.1.2.5. allows changing WM_CLASS only in Withdrawn state */
        if (g->log_level > 0)
            fprintf(stderr, "cannot set class hint for window 0x%x, "
                    "because it is mapped (not in Withdrawn state)\n",
                    (int) vm_window->local_winid);
        return;
    }

    /* sanitize start */
    untrusted_msg.res_class[sizeof(untrusted_msg.res_class) - 1] = 0;
    sanitize_string_from_vm((unsigned char*)untrusted_msg.res_class, 0);
    untrusted_msg.res_name[sizeof(untrusted_msg.res_name) - 1] = 0;
    sanitize_string_from_vm((unsigned char*)untrusted_msg.res_name, 0);
    snprintf(res_class, sizeof(res_class), "%s:%s", g->vmname, untrusted_msg.res_class);
    snprintf(res_name, sizeof(res_name), "%s:%s", g->vmname, untrusted_msg.res_name);
    /* sanitize end */
    if (g->log_level > 1)
        fprintf(stderr, "set class hint for window 0x%x to (%s, %s)\n",
            (int) vm_window->local_winid, res_class, res_name);
    class_hint.res_name = res_name;
    class_hint.res_class = res_class;
    XSetClassHint(g->display, vm_window->local_winid, &class_hint);
}

/* handle VM message: MSG_WMHINTS
 * Pass hints for window manager to local X server */
static void handle_wmhints(Ghandles * g, struct windowdata *vm_window)
{
    struct msg_window_hints untrusted_msg;
    XSizeHints size_hints;

    memset(&size_hints, 0, sizeof(size_hints));

    read_struct(g->vchan, untrusted_msg);

    /* sanitize start */
    size_hints.flags = 0;
    /* check every value and pass it only when sane */
    if (untrusted_msg.flags & PMinSize) {
        if (untrusted_msg.min_width <= MAX_WINDOW_WIDTH
                && untrusted_msg.min_height <= MAX_WINDOW_HEIGHT) {
            size_hints.flags |= PMinSize;
            size_hints.min_width = untrusted_msg.min_width;
            size_hints.min_height = untrusted_msg.min_height;
        } else
            fprintf(stderr, "invalid PMinSize for 0x%x (%d/%d)\n",
                    (int) vm_window->local_winid,
                    untrusted_msg.min_width, untrusted_msg.min_height);
    }
    if (untrusted_msg.flags & PMaxSize) {
        if (untrusted_msg.max_width > 0
                && untrusted_msg.max_width <= MAX_WINDOW_WIDTH
                && untrusted_msg.max_height > 0
                && untrusted_msg.max_height <= MAX_WINDOW_HEIGHT) {
            size_hints.flags |= PMaxSize;
            size_hints.max_width = untrusted_msg.max_width;
            size_hints.max_height = untrusted_msg.max_height;
        } else
            fprintf(stderr, "invalid PMaxSize for 0x%x (%d/%d)\n",
                    (int) vm_window->local_winid,
                    untrusted_msg.max_width, untrusted_msg.max_height);
    }
    if (untrusted_msg.flags & PResizeInc) {
        if (untrusted_msg.width_inc < MAX_WINDOW_WIDTH
                && untrusted_msg.height_inc < MAX_WINDOW_HEIGHT) {
            size_hints.flags |= PResizeInc;
            size_hints.width_inc = untrusted_msg.width_inc;
            size_hints.height_inc = untrusted_msg.height_inc;
        } else
            fprintf(stderr, "invalid PResizeInc for 0x%x (%d/%d)\n",
                    (int) vm_window->local_winid,
                    untrusted_msg.width_inc, untrusted_msg.height_inc);
    }
    if (untrusted_msg.flags & PBaseSize) {
        if (untrusted_msg.base_width <= MAX_WINDOW_WIDTH
                && untrusted_msg.base_height <= MAX_WINDOW_HEIGHT) {
            size_hints.flags |= PBaseSize;
            size_hints.base_width = untrusted_msg.base_width;
            size_hints.base_height = untrusted_msg.base_height;
        } else
            fprintf(stderr, "invalid PBaseSize for 0x%x (%d/%d)\n",
                    (int) vm_window->local_winid,
                    untrusted_msg.base_width,
                    untrusted_msg.base_height);
    }
    if (untrusted_msg.flags & PPosition)
        size_hints.flags |= PPosition;
    if (untrusted_msg.flags & USPosition)
        size_hints.flags |= USPosition;
    /* sanitize end */

    if (g->log_level > 1)
        fprintf(stderr,
            "set WM_NORMAL_HINTS for window 0x%x to min=%d/%d, max=%d/%d, base=%d/%d, inc=%d/%d (flags 0x%x)\n",
            (int) vm_window->local_winid, size_hints.min_width,
            size_hints.min_height, size_hints.max_width,
            size_hints.max_height, size_hints.base_width,
            size_hints.base_height, size_hints.width_inc,
            size_hints.height_inc, (int) size_hints.flags);
    XSetWMNormalHints(g->display, vm_window->local_winid, &size_hints);
}

/* handle VM message: MSG_WINDOW_FLAGS
 * Pass window state flags for window manager to local X server */
static void handle_wmflags(Ghandles * g, struct windowdata *vm_window)
{
    struct msg_window_flags untrusted_msg;
    struct msg_window_flags msg;

    read_struct(g->vchan, untrusted_msg);

    /* sanitize start */
    VERIFY((untrusted_msg.flags_set & untrusted_msg.flags_unset) == 0);
    msg.flags_set = untrusted_msg.flags_set & (
            WINDOW_FLAG_FULLSCREEN |
            WINDOW_FLAG_DEMANDS_ATTENTION |
            WINDOW_FLAG_MINIMIZE);
    msg.flags_unset = untrusted_msg.flags_unset & (
            WINDOW_FLAG_FULLSCREEN |
            WINDOW_FLAG_DEMANDS_ATTENTION);
    /* sanitize end */

    if (!vm_window->is_mapped) {
        /* for unmapped windows, set property directly; only "set" list is
         * processed (will override property) */
        Atom state_list[10];
        int i = 0;

        vm_window->flags_set &= ~(WINDOW_FLAG_FULLSCREEN | WINDOW_FLAG_DEMANDS_ATTENTION);
        if (msg.flags_set & WINDOW_FLAG_FULLSCREEN) {
            if (g->allow_fullscreen) {
                vm_window->flags_set |= WINDOW_FLAG_FULLSCREEN;
                state_list[i++] = g->wm_state_fullscreen;
            } else {
                /* if fullscreen not allowed, substitute request with maximize */
                state_list[i++] = g->wm_state_maximized_vert;
                state_list[i++] = g->wm_state_maximized_horz;
                /* and note that maximize WM ack should be converted back to
                 * fullscreen ack for VM; otherwise some applications (Firefox)
                 * behaves badly when fullscreen request isn't acknowledged at
                 * all */
                vm_window->fullscreen_maximize_requested = 1;
            }
        } else
            vm_window->fullscreen_maximize_requested = 0;
        if (msg.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION) {
            vm_window->flags_set |= WINDOW_FLAG_DEMANDS_ATTENTION;
            state_list[i++] = g->wm_state_demands_attention;
        }
        if (i > 0) {
            /* FIXME: error checking? */
            XChangeProperty(g->display, vm_window->local_winid, g->wm_state,
                    XA_ATOM, 32, PropModeReplace, (unsigned char*)state_list,
                    i);
        } else
            /* just in case */
            XDeleteProperty(g->display, vm_window->local_winid, g->wm_state);

        /* Regarding WINDOW_FLAG_MINIMIZE:
         * Restoring window from minimize state is exactly the same as MSG_MAP,
         * so to not risk some regressions do not duplicate the code. */
    } else {
        /* for mapped windows, send message to window manager (via root window) */
        XClientMessageEvent ev;
        uint32_t flags_all = msg.flags_set | msg.flags_unset;

        if (!flags_all)
            /* no change requested */
            return;

        // WINDOW_FLAG_FULLSCREEN and WINDOW_FLAG_MINIMIZE are mutually exclusive
        if (msg.flags_set & WINDOW_FLAG_MINIMIZE)
            msg.flags_set &= ~WINDOW_FLAG_FULLSCREEN;

        memset(&ev, 0, sizeof(ev));
        ev.type = ClientMessage;
        ev.display = g->display;
        ev.window = vm_window->local_winid;
        ev.message_type = g->wm_state;
        ev.format = 32;
        ev.data.l[3] = 1; /* source indication: normal application */

        /* ev.data.l[0]: 1 - add/set property, 0 - remove/unset property */
        if (flags_all & WINDOW_FLAG_FULLSCREEN) {
            ev.data.l[0] = (msg.flags_set & WINDOW_FLAG_FULLSCREEN) ? 1 : 0;
            if (g->allow_fullscreen) {
                /* allow entering fullscreen only if g->allow_fullscreen */
                ev.data.l[1] = g->wm_state_fullscreen;
                ev.data.l[2] = 0;
            } else if (!ev.data.l[0] && !vm_window->fullscreen_maximize_requested) {
                /* allow exiting fullscreen even if entering wasn't allowed
                 * (this means it was triggered by the user with WM action);
                 * but only it if wasn't "fake fullscreen" (maximized window in
                 * practice) - otherwise still convert to unmaximize request */
                ev.data.l[1] = g->wm_state_fullscreen;
                ev.data.l[2] = 0;
            } else {
                /* convert request to maximize/unmaximize */
                ev.data.l[1] = g->wm_state_maximized_vert;
                ev.data.l[2] = g->wm_state_maximized_horz;
                /* and note that maximize WM ack should be converted back to
                 * fullscreen ack for VM */
                vm_window->fullscreen_maximize_requested = ev.data.l[0];
            }
            XSendEvent(g->display, g->root_win, False,
                    (SubstructureNotifyMask|SubstructureRedirectMask),
                    (XEvent*) &ev);
        }
        if (msg.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION) {
            ev.data.l[0] = (msg.flags_set & WINDOW_FLAG_DEMANDS_ATTENTION) ? 1 : 0;
            ev.data.l[1] = g->wm_state_demands_attention;
            ev.data.l[2] = 0;
            XSendEvent(g->display, g->root_win, False,
                    (SubstructureNotifyMask|SubstructureRedirectMask),
                    (XEvent*) &ev);
        }
        if (msg.flags_set & WINDOW_FLAG_MINIMIZE) {
            XIconifyWindow(g->display, vm_window->local_winid, g->screen);
        }
    }
}


/* Check if we should keep this window on top of others */
static bool should_keep_on_top(Ghandles *g, Window window) {
    int ret;
    XWindowAttributes attr;
    XClassHint hint;
    bool result;
    int i;

    /* Check if the window has override_redirect attribute, and is mapped. */
    ret = XGetWindowAttributes(g->display, window, &attr);
    if (!(ret && attr.override_redirect && attr.map_state == IsViewable))
        return false;

    /* Check if this is a dom0 screensaver window by looking at window class.
     * (VM windows have a prefix, so this is not spoofable by a VM). */
    ret = XGetClassHint(g->display, window, &hint);
    if (!ret)
        return false;

    result = false;
    for (i = 0; i < MAX_SCREENSAVER_NAMES && g->screensaver_names[i]; i++) {
        if (strcmp(hint.res_name, g->screensaver_names[i]) == 0) {
            result = true;
            break;
        }
    }

    XFree(hint.res_name);
    XFree(hint.res_class);
    return result;
}


/* Move a newly mapped override_redirect window below windows that need to be
 * kept on top, i.e. screen lockers. Returns new index (-1 == top of all) */
static int restack_windows(Ghandles *g, struct windowdata *vm_window)
{
    Window root;
    Window parent;
    Window *children_list;
    unsigned int children_count;
    int i, current_pos, goal_pos;

    /* Find all windows below parent */

    XQueryTree(g->display, g->root_win, &root, &parent, &children_list, &children_count);

    if (!children_list) {
        fprintf(stderr, "XQueryTree returned an empty list\n");
        return 0;
    }

    /* Traverse children_list, looking for bottom-most window that
     * need to be kept on top. The list is bottom-to-top, so we record only the
     * first such window, and break as soon as we encounter our own window. */

    current_pos = -1;
    goal_pos = -1;
    for (i = 0; i < (int) children_count; i++) {
        if (children_list[i] == vm_window->local_winid) {
            current_pos = i;
            break;
        } else if (goal_pos == -1 && should_keep_on_top(g, children_list[i]))
            goal_pos = i;
    }

    /* Reorder if needed */

    if (current_pos != -1 && goal_pos != -1) {
        Window to_restack[2];

        assert(current_pos > goal_pos);

        if (g->log_level > 0) {
            fprintf(stderr, "restack_windows: moving window 0x%x deeper\n",
                    (int) vm_window->local_winid);
        }

        to_restack[0] = children_list[goal_pos];
        to_restack[1] = vm_window->local_winid;

        XRestackWindows(g->display, to_restack, 2);
    }

    XFree(children_list);
    return goal_pos;
}


/* handle VM message: MSG_MAP
 * Map a window with given parameters */
static void handle_map(Ghandles * g, struct windowdata *vm_window)
{
    struct genlist *trans;
    struct msg_map_info untrusted_txt;
    XSetWindowAttributes attr;

    read_struct(g->vchan, untrusted_txt);
    if (g->invisible)
        return;
    /* if mapped, don't allow changing attributes */
    if (vm_window->is_mapped)
        return;
    /*
     * Mapping/unmapping of a docked window should be done via setting
     * _XEMBED_INFO property. Consider doing this in the future, but definitely
     * avoid direct action behind embedder backs.
     */
    if (vm_window->is_docked)
        return;
    if (untrusted_txt.transient_for
        && (trans =
        list_lookup(g->remote2local,
                untrusted_txt.transient_for))) {
        struct windowdata *transdata = trans->data;
        vm_window->transient_for = transdata;
        XSetTransientForHint(g->display, vm_window->local_winid,
                     transdata->local_winid);
    } else {
        if (vm_window->transient_for)
            XDeleteProperty(g->display, vm_window->local_winid,
                    XA_WM_TRANSIENT_FOR);
        vm_window->transient_for = NULL;
    }

    vm_window->override_redirect =
        validate_override_redirect(g, vm_window, !!(untrusted_txt.override_redirect));
    attr.override_redirect = vm_window->override_redirect;
    XChangeWindowAttributes(g->display, vm_window->local_winid,
                            CWOverrideRedirect, &attr);
    if (vm_window->override_redirect
        && force_on_screen(g, vm_window, override_redirect_padding,
                           "handle_map"))
        moveresize_vm_window(g, vm_window, false);
    if (vm_window->override_redirect) {
        /* force window update to draw colorful frame, even when VM have not
         * sent any content yet */
        do_shm_update(g, vm_window, 0, 0, vm_window->width, vm_window->height);
    }

    vm_window->is_mapped = 1;
    (void) XMapWindow(g->display, vm_window->local_winid);

    if (vm_window->override_redirect) {
        if (restack_windows(g, vm_window) == -1) {
            // Ensure override redirect window is raised after mapping
            // But only if no screensaver is active
            XRaiseWindow(g->display, vm_window->local_winid);
            XFlush(g->display);  // Ensure raise takes effect immediately
        }
    }
}

/* handle VM message: MSG_DOCK
 * Try to dock window in the tray
 * Rest of XEMBED protocol is catched in VM */
static void handle_dock(Ghandles * g, struct windowdata *vm_window)
{
    Window tray;
    if (g->log_level > 0)
        fprintf(stderr, "docking window 0x%x\n",
            (int) vm_window->local_winid);
    if (vm_window->is_mapped) {
        fprintf(stderr, "cannot dock mapped window 0x%x\n",
                (int) vm_window->local_winid);
        return;
    }
    if (vm_window->override_redirect) {
        XSetWindowAttributes attr;
        fprintf(stderr, "docking an override-redirect window 0x%x - "
                "clearing override-redirect\n",
                (int) vm_window->local_winid);
        /* changing directly is safe, because window is not mapped here yet */
        attr.override_redirect = vm_window->override_redirect = 0;
        XChangeWindowAttributes(g->display, vm_window->local_winid,
                CWOverrideRedirect, &attr);
    }
    tray = XGetSelectionOwner(g->display, g->tray_selection);
    if (tray != None) {
        long data[2];
        XClientMessageEvent msg;

        data[0] = 0; /* version */
        data[1] = 1; /* flags: XEMBED_MAPPED */
        XChangeProperty(g->display, vm_window->local_winid,
                g->xembed_info, g->xembed_info, 32,
                PropModeReplace, (unsigned char *) data,
                2);

        memset(&msg, 0, sizeof(msg));
        msg.type = ClientMessage;
        msg.window = tray;
        msg.message_type = g->tray_opcode;
        msg.format = 32;
        msg.data.l[0] = CurrentTime;
        msg.data.l[1] = SYSTEM_TRAY_REQUEST_DOCK;
        msg.data.l[2] = vm_window->local_winid;
        msg.display = g->display;
        XSendEvent(msg.display, msg.window, False, NoEventMask,
               (XEvent *) & msg);
    }
    vm_window->is_docked = 1;
}

/* Obtain/release inter-vm lock
 * Used for handling shared Xserver memory and clipboard file */
static void inter_appviewer_lock(Ghandles *g, int mode)
{
    int cmd;
    if (mode)
        cmd = LOCK_EX;
    else
        cmd = LOCK_UN;
    if (flock(g->inter_appviewer_lock_fd, cmd) < 0) {
        perror("lock");
        exit(1);
    }
}

/* release shared memory connected with given window */
static void release_mapped_mfns(Ghandles * g, struct windowdata *vm_window)
{
    if (g->invisible || vm_window->shmseg == QUBES_NO_SHM_SEGMENT)
        return;
    check_xcb_void(
        xcb_shm_detach(g->cb_connection, vm_window->shmseg),
        "xcb_shm_detach");
    vm_window->shmseg = QUBES_NO_SHM_SEGMENT;
}

static void
qubes_xcb_send_xen_fd(Ghandles *g,
                      struct windowdata *vm_window,
                      struct shm_args_hdr *shm_args,
                      size_t shm_args_len)
{
    xcb_generic_error_t *error = NULL;
    if (g->invisible)
        goto ack;
    shm_args->domid = g->domid;
    vm_window->shmseg = xcb_generate_id(g->cb_connection);
    if (vm_window->shmseg == QUBES_NO_SHM_SEGMENT) {
        fputs("xcb_generate_id returned QUBES_NO_SHM_SEGMENT!\n", stderr);
        abort();
    }
    if (shm_args_len > SHM_ARGS_SIZE)
        errx(1, "shm_args_len is %zu, exceeding maximum of %zu", shm_args_len,
             (size_t)SHM_ARGS_SIZE);
    inter_appviewer_lock(g, 1);
    int dup_fd;
    switch (shm_args->type) {
    case SHM_ARGS_TYPE_MFNS:
        if ((dup_fd = fcntl(g->xen_fd, F_DUPFD_CLOEXEC, 3)) < 3) {
            assert(dup_fd == -1);
            err(1, "fcntl(F_DUPFD_CLOEXEC)");
        }
        break;
    default:
        fputs("internal wrong command type (this is a bug)\n", stderr);
        abort();
    case SHM_ARGS_TYPE_GRANT_REFS:
        if ((dup_fd = openat(g->xen_dir_fd, "gntdev", O_RDWR|O_CLOEXEC|O_NOCTTY)) == -1)
            err(1, "open(\"/dev/xen/gntdev\")");
        struct shm_args_grant_refs *s =
            (struct shm_args_grant_refs *)((uint8_t *)shm_args + sizeof(struct shm_args_hdr));
        struct ioctl_gntdev_map_grant_ref *gref = malloc(
                s->count * sizeof(struct ioctl_gntdev_grant_ref) +
                offsetof(struct ioctl_gntdev_map_grant_ref, refs));
        if (!gref)
            err(1, "malloc failed");
        gref->count = s->count;
        gref->pad = 0;
        gref->index = UINT64_MAX;
        for (size_t i = 0; i < s->count; ++i) {
            gref->refs[i].domid = g->domid;
            gref->refs[i].ref = s->refs[i];
        }
        if (ioctl(dup_fd, IOCTL_GNTDEV_MAP_GRANT_REF, gref) != 0)
            err(1, "ioctl(IOCTL_GNTDEV_MAP_GRANT_REF)");
        if (gref->index != 0)
            fprintf(stderr,
                    "ioctl(IOCTL_GNTDEV_MAP_GRANT_REF) set index to nonzero value %" PRIu64 "\n",
                    (uint64_t)gref->index);
        s->off = gref->index;
        free(gref);
    }
    memcpy(g->shm_args, shm_args, shm_args_len);
    memset(((uint8_t *) g->shm_args) + shm_args_len, 0,
           SHM_ARGS_SIZE - shm_args_len);
    const xcb_void_cookie_t cookie =
        check_xcb_void(
            xcb_shm_attach_fd_checked(g->cb_connection, vm_window->shmseg,
                                      dup_fd, true),
            "xcb_shm_attach_fd_checked");
    xcb_aux_sync(g->cb_connection);
    error = xcb_request_check(g->cb_connection, cookie);
    inter_appviewer_lock(g, 0);
ack:
    if (g->protocol_version >= QUBES_GUID_MIN_MSG_WINDOW_DUMP_ACK) {
        struct msg_hdr hdr;
        hdr.type = MSG_WINDOW_DUMP_ACK;
        hdr.window = vm_window->remote_winid;
        hdr.untrusted_len = 0;
        write_struct(g->vchan, hdr);
    }
    if (error) {
        qubes_xcb_handler(g, "xcb_shm_attach_fd", vm_window, error);
        free(error);
        vm_window->shmseg = QUBES_NO_SHM_SEGMENT;
    }
}

__attribute__((cold)) _Noreturn static void
too_big_window_error(const char *msg, uint32_t untrusted_width, uint32_t untrusted_height)
{
    errx(1, "Width or height too large in MSG_%s: "
            "limit is %dx%d but got %" PRIu32 "x%" PRIu32,
         msg, MAX_WINDOW_WIDTH, MAX_WINDOW_HEIGHT,
         untrusted_width, untrusted_height);
}

__attribute__((cold)) static void
bad_length(uint32_t const protocol_version, const char *const msg,
           size_t const expected, uint32_t const untrusted_len)
{
    const unsigned int major_version = protocol_version >> 16;
    const unsigned int minor_version = protocol_version & 0xFFFF;
    warnx("incorrect untrusted_len for MSG_%s: expected %zu got %" PRIu32,
          msg, expected, untrusted_len);
    if (protocol_version >= PROTOCOL_VERSION(1, 6))
        errx(1, "Exiting since protocol version %u.%u >= 1.6",
             major_version, minor_version);
    else
        warnx("Continuing since protocol version %u.%u < 1.6",
              major_version, minor_version);
}

#define CHECK_LEN(expected, msg)                                             \
    do {                                                                     \
        size_t _expected = (expected);                                       \
        if (untrusted_len != _expected)                                      \
            bad_length(g->protocol_version, #msg, _expected, untrusted_len); \
        untrusted_len = _expected;                                           \
    } while (0)
#pragma GCC poison _expected

/* handle VM message: MSG_MFNDUMP
 * Retrieve memory addresses connected with composition buffer of remote window
 */
static void handle_mfndump(Ghandles * g, struct windowdata *vm_window, uint32_t untrusted_len)
{
    struct shm_cmd untrusted_shmcmd;
    uint32_t num_mfn, off;
    size_t shm_args_len;
    struct shm_args_hdr *shm_args;
    struct shm_args_mfns *shm_args_mfns;
    size_t mfns_len;

    release_mapped_mfns(g, vm_window);
    read_struct(g->vchan, untrusted_shmcmd);
    if (!g->in_dom0) {
        fprintf(stderr, "Qube %s (id %d) sent a MSG_MFNDUMP message, but this GUI daemon instance is not running in dom0.\n"
                        "Since we are not in dom0, we cannot process this request.\n"
                        "\n"
                        "Please upgrade the GUI agent in the VM if possible\n",
                        g->vmname,
                        g->domid);
        exit(1);
    }

    if (g->log_level > 1)
        fprintf(stderr, "MSG_MFNDUMP for 0x%x(0x%x): %dx%d, num_mfn 0x%x off 0x%x\n",
                (int) vm_window->local_winid, (int) vm_window->remote_winid,
                untrusted_shmcmd.width, untrusted_shmcmd.height,
                untrusted_shmcmd.num_mfn, untrusted_shmcmd.off);
    /* sanitize start */
    if (untrusted_shmcmd.num_mfn > MAX_MFN_COUNT)
        errx(1, "MSG_MFNDUMP: too many MFNs (limit %zu got %" PRIu32 ")",
             (size_t)MAX_MFN_COUNT, untrusted_shmcmd.num_mfn);
    num_mfn = untrusted_shmcmd.num_mfn;
    if (untrusted_shmcmd.width > MAX_WINDOW_WIDTH ||
        untrusted_shmcmd.height > MAX_WINDOW_HEIGHT)
    {
        too_big_window_error("MFNDUMP", untrusted_shmcmd.width, untrusted_shmcmd.height);
    }
    if (untrusted_shmcmd.off >= 4096)
        errx(1, "MSG_MFNDUMP has offset %" PRIu32 " which is not less than 4096", untrusted_shmcmd.off);
    off = untrusted_shmcmd.off;
    mfns_len = num_mfn * SIZEOF_SHARED_MFN;
    CHECK_LEN(mfns_len + sizeof untrusted_shmcmd, "MFNDUMP");
    /* unused for now: VERIFY(untrusted_shmcmd.bpp == 24); */
    /* sanitize end */

    vm_window->image_width = untrusted_shmcmd.width;
    vm_window->image_height = untrusted_shmcmd.height;    /* sanitized above */

    shm_args_len = sizeof(struct shm_args_hdr) + sizeof(struct shm_args_mfns) +
                   mfns_len;
    shm_args = calloc(1, shm_args_len);
    if (shm_args == NULL) {
        perror("malloc failed");
        exit(1);
    }
    shm_args_mfns = (struct shm_args_mfns *) (((uint8_t *) shm_args) +
                                              sizeof(struct shm_args_hdr));
    shm_args->type = SHM_ARGS_TYPE_MFNS;
    shm_args_mfns->count = num_mfn;
    shm_args_mfns->off = off;

    read_data(g->vchan, (char *) &shm_args_mfns->mfns[0], mfns_len);
    if (num_mfn * 4096 <
        vm_window->image_width * vm_window->image_height * 4 + off) {
        errx(1, "handle_mfndump for window 0x%x(remote 0x%x)"
            " got too small num_mfn= 0x%x\n",
            (int) vm_window->local_winid,
            (int) vm_window->remote_winid, num_mfn);
    }
    qubes_xcb_send_xen_fd(g, vm_window, shm_args, shm_args_len);
    free(shm_args);
}

static void handle_window_dump_body_grant_refs(Ghandles *g,
         size_t untrusted_wd_body_len, size_t *img_data_size, struct
         shm_args_hdr **shm_args, size_t *shm_args_len) {
    size_t refs_len;
    struct shm_args_grant_refs *shm_args_grant;

    // We don't have any custom arguments except the variable length refs list.
    static_assert(sizeof(struct msg_window_dump_grant_refs) == 0,
                   "struct def bug");

    // Check that we will not overflow during multiplication
    static_assert(MAX_GRANT_REFS_COUNT < INT32_MAX / 4096,
                   "MAX_GRANT_REFS_COUNT too large");

    if (untrusted_wd_body_len > MAX_GRANT_REFS_COUNT * SIZEOF_GRANT_REF ||
        untrusted_wd_body_len % SIZEOF_GRANT_REF != 0) {
        fprintf(stderr, "handle_msg_window_dump_grant_refs: "
                "invalid body size(%zu)\n",
                untrusted_wd_body_len);
        exit(1);
    }
    refs_len = untrusted_wd_body_len;

    *shm_args_len = sizeof(struct shm_args_hdr) +
                    sizeof(struct shm_args_grant_refs) +
                    refs_len;
    *shm_args = calloc(1, *shm_args_len);
    if (*shm_args == NULL) {
        perror("malloc failed");
        exit(1);
    }
    shm_args_grant = (struct shm_args_grant_refs *) (
            ((uint8_t *) *shm_args) + sizeof(struct shm_args_hdr));
    (*shm_args)->type = SHM_ARGS_TYPE_GRANT_REFS;
    shm_args_grant->count = refs_len / SIZEOF_GRANT_REF;
    *img_data_size = shm_args_grant->count * 4096;

    read_data(g->vchan, (char *) &shm_args_grant->refs[0], refs_len);
}

static void handle_window_dump_body(Ghandles *g, uint32_t wd_type, size_t
        untrusted_wd_body_len, size_t *img_data_size, struct shm_args_hdr
        **shm_args, size_t *shm_args_len) {
    switch (wd_type) {
        case WINDOW_DUMP_TYPE_GRANT_REFS:
            handle_window_dump_body_grant_refs(g, untrusted_wd_body_len,
                    img_data_size, shm_args, shm_args_len);
            break;
        default:
            // not reachable
            assert(false);
    }
}

static void handle_window_dump(Ghandles *g, struct windowdata *vm_window,
                               size_t const untrusted_wd_body_len) {
    struct msg_window_dump_hdr untrusted_wd_hdr;
    struct shm_args_hdr *shm_args = NULL;
    size_t shm_args_len = 0, img_data_size = 0;

    release_mapped_mfns(g, vm_window);

    read_struct(g->vchan, untrusted_wd_hdr);

    if (g->log_level > 1)
        fprintf(stderr, "MSG_WINDOW_DUMP for 0x%lx(0x%lx): %ux%u, type = %u\n",
                vm_window->local_winid, vm_window->remote_winid,
                untrusted_wd_hdr.width, untrusted_wd_hdr.height,
                untrusted_wd_hdr.type);

    switch (untrusted_wd_hdr.type) {
    case WINDOW_DUMP_TYPE_GRANT_REFS:
        break;
    default:
        fprintf(stderr, "unknown window dump type %u\n",
                untrusted_wd_hdr.type);
        exit(1);
    }
    const uint32_t wd_type = untrusted_wd_hdr.type;
    if (untrusted_wd_hdr.width > MAX_WINDOW_WIDTH ||
        untrusted_wd_hdr.height > MAX_WINDOW_HEIGHT)
    {
        too_big_window_error("WINDOW_DUMP", untrusted_wd_hdr.width, untrusted_wd_hdr.height);
    }
    vm_window->image_width = untrusted_wd_hdr.width;
    vm_window->image_height = untrusted_wd_hdr.height;
    //VERIFY(untrusted_wd_hdr.bpp == 24);

    handle_window_dump_body(g, wd_type, untrusted_wd_body_len, &img_data_size,
                            &shm_args, &shm_args_len);

    if (img_data_size < (size_t) (vm_window->image_width *
                                  vm_window->image_height *
                                  4)) {
        fprintf(stderr,
            "handle_window_dump: got too small image data size (%zu)"
            " for window 0x%lx (remote 0x%lx)\n",
            img_data_size, vm_window->local_winid, vm_window->remote_winid);
        exit(1);
    }

    qubes_xcb_send_xen_fd(g, vm_window, shm_args, shm_args_len);
    free(shm_args);
}

__attribute__((cold)) _Noreturn static void
msg_too_short_error(const char *msg, uint32_t untrusted_len, size_t expected_len)
{
    errx(1, "MSG_%s message is too short (got %" PRIu32 " expected >= %zu",
         msg, untrusted_len, expected_len);
}

/* VM message dispatcher */
static void handle_message(Ghandles * g)
{
    struct msg_hdr untrusted_hdr;
    XID window = 0;
    struct genlist *l;
    struct windowdata *vm_window = NULL;

    read_struct(g->vchan, untrusted_hdr);
    uint32_t untrusted_len = untrusted_hdr.untrusted_len;
    uint32_t const untrusted_type = untrusted_hdr.type;
    if (untrusted_type == MSG_CLIPBOARD_DATA) {
        handle_clipboard_data(g, untrusted_len);
        return;
    }
    l = list_lookup(g->remote2local, untrusted_hdr.window);
    if (untrusted_type == MSG_CREATE) {
        if (l) {
            fprintf(stderr,
                "CREATE for already existing window id 0x%x?\n",
                untrusted_hdr.window);
            exit(1);
        }
        window = untrusted_hdr.window;
    } else {
        if (!l) {
            fprintf(stderr,
                "msg 0x%x without CREATE for 0x%x\n",
                untrusted_type, untrusted_hdr.window);
            exit(1);
        }
        vm_window = l->data;
        /* not needed as it is in vm_window struct
           window = untrusted_hdr.window;
         */
    }

    switch (untrusted_type) {
    case MSG_CREATE:
        CHECK_LEN(sizeof(struct msg_create), CREATE);
        handle_create(g, window);
        break;
    case MSG_DESTROY:
        CHECK_LEN(0, DESTROY);
        handle_destroy(g, l);
        break;
    case MSG_MAP:
        CHECK_LEN(sizeof(struct msg_map_info), MAP);
        handle_map(g, vm_window);
        break;
    case MSG_UNMAP:
        CHECK_LEN(0, UNMAP);
        /*
         * Mapping/unmapping of a docked window should be done via setting
         * _XEMBED_INFO property. Consider doing this in the future, but definitely
         * avoid direct action behind embedder backs.
         */
        if (vm_window->is_docked)
            break;
        vm_window->is_mapped = 0;
        (void) XUnmapWindow(g->display, vm_window->local_winid);
        if (vm_window->remote_winid != FULLSCREEN_WINDOW_ID)
            release_mapped_mfns(g, vm_window);
        break;
    case MSG_CONFIGURE:
        CHECK_LEN(sizeof(struct msg_configure), CONFIGURE);
        handle_configure_from_vm(g, vm_window);
        break;
    case MSG_MFNDUMP:
        if (g->protocol_version >= PROTOCOL_VERSION(1, 6) &&
            untrusted_len < sizeof(struct shm_cmd))
            msg_too_short_error("MFNDUMP", untrusted_len, sizeof(struct shm_cmd));
        handle_mfndump(g, vm_window, untrusted_len);
        break;
    case MSG_SHMIMAGE:
        CHECK_LEN(sizeof(struct msg_shmimage), SHMIMAGE);
        handle_shmimage(g, vm_window);
        break;
    case MSG_WMNAME:
        CHECK_LEN(sizeof(struct msg_wmname), WMNAME);
        handle_wmname(g, vm_window);
        break;
    case MSG_WMCLASS:
        CHECK_LEN(sizeof(struct msg_wmclass), WMCLASS);
        handle_wmclass(g, vm_window);
        break;
    case MSG_DOCK:
        CHECK_LEN(0, DOCK);
        handle_dock(g, vm_window);
        break;
    case MSG_WINDOW_HINTS:
        CHECK_LEN(sizeof(struct msg_window_hints), WINDOW_HINTS);
        handle_wmhints(g, vm_window);
        break;
    case MSG_WINDOW_FLAGS:
        CHECK_LEN(sizeof(struct msg_window_flags), WINDOW_FLAGS);
        handle_wmflags(g, vm_window);
        break;
    case MSG_WINDOW_DUMP:
        if (untrusted_len < MSG_WINDOW_DUMP_HDR_LEN)
            msg_too_short_error("WINDOW_DUMP", untrusted_len, MSG_WINDOW_DUMP_HDR_LEN);
        handle_window_dump(g, vm_window, untrusted_len - MSG_WINDOW_DUMP_HDR_LEN);
        break;
    case MSG_CURSOR:
        CHECK_LEN(sizeof(struct msg_cursor), CURSOR);
        handle_cursor(g, vm_window);
        break;
    default:
        errx(1, "got unknown msg type %" PRIu32 "\n", untrusted_type);
    }
}

/* helper to get a file flag path */
static char *guid_fs_flag(const char *type, int domid)
{
    static char buf[256];
    snprintf(buf, sizeof(buf), "/run/qubes/guid-%s.%d",
         type, domid);
    return buf;
}

/* remove guid_running file at exit */
static void unset_alive_flag(void)
{
    unlink(guid_fs_flag("running", ghandles.domid));
}

/* signal handler - connected to SIGTERM */
static void dummy_signal_handler(int UNUSED(x))
{
    unset_alive_flag();
    _exit(0);
}

/* signal handler - connected to SIGHUP */
static void sighup_signal_handler(int UNUSED(x))
{
    ghandles.reload_requested = 1;
}

static void print_backtrace(void)
{
    void *array[100];
    size_t size;
    char **strings;
    size_t i;


    if (ghandles.log_level > 1) {
        size = backtrace(array, 100);
        strings = backtrace_symbols(array, size);
        fprintf(stderr, "Obtained %zd stack frames.\n", size);

        for (i = 0; i < size; i++)
            printf("%s\n", strings[i]);

        free(strings);
    }

}

static void send_xconf(Ghandles * g)
{
    struct msg_xconf xconf;
    XWindowAttributes attr;
    if (!XGetWindowAttributes(g->display, g->root_win, &attr))
        errx(1, "Cannot query root window attributes!");
    if (g->protocol_version >= QUBES_GUID_MIN_BIDIRECTIONAL_NEGOTIATION_VERSION) {
        /* Bidirectional protocol negotiation is supported */
        _Static_assert(sizeof g->protocol_version == 4,
                       "g->protocol_version must be a uint32_t");
        write_struct(g->vchan, g->protocol_version);
    }
    xconf.w = _VIRTUALX(attr.width);
    xconf.h = attr.height;
    xconf.depth = attr.depth;
    xconf.mem = xconf.w * xconf.h * 4 / 1024 + 1;
    write_struct(g->vchan, xconf);
}

/* receive from VM and compare protocol version
 * abort if mismatch */
static void get_protocol_version(Ghandles * g)
{
    uint32_t untrusted_version;
    char message[1024];
    unsigned int version_major, version_minor;
    read_struct(g->vchan, untrusted_version);
    version_major = untrusted_version >> 16;
    version_minor = untrusted_version & 0xffff;

    if (version_major == PROTOCOL_VERSION_MAJOR) {
        /* agent is compatible */
        g->protocol_version = PROTOCOL_VERSION(version_major, min(version_minor, PROTOCOL_VERSION_MINOR));
        return;
    }
    if (version_major < PROTOCOL_VERSION_MAJOR) {
        /* agent is too old */
        if ((unsigned)snprintf(message, sizeof message, "%s %s \""
                "The GUI agent that runs in the VM '%s' implements outdated protocol (%u:%u), and must be updated.\n\n"
                "To start and access the VM or template without GUI virtualization, use the following commands:\n"
                "qvm-start --no-guid vmname\n"
                "qvm-console-dispvm vmname\"",
                g->use_kdialog ? KDIALOG_PATH : ZENITY_PATH,
                g->use_kdialog ? "--sorry" : "--error --text ",
                g->vmname, version_major, version_minor) >= sizeof message)
            abort();
    } else {
        /* agent is too new */
        if ((unsigned)snprintf(message, sizeof message, "%s %s \""
                "The Dom0 GUI daemon does not support protocol version %u:%u, requested by the VM '%s'.\n\n"
                "To update Dom0, use 'qubes-dom0-update' command or do it via qubes-manager\"",
                g->use_kdialog ? KDIALOG_PATH : ZENITY_PATH,
                g->use_kdialog ? "--sorry" : "--error --text ",
                version_major, version_minor, g->vmname) >= sizeof message)
            abort();
    }
    ignore_result(system(message));
    exit(1);
}

/* wait until child process connects to VM */
static void wait_for_connection_in_parent(int *pipe_notify)
{
    // inside the parent process
    // wait for daemon to get connection with AppVM
    struct pollfd pipe_pollfd;
    int tries, ret;

    if (ghandles.log_level > 0)
        fprintf(stderr, "Connecting to VM's GUI agent: ");
    close(pipe_notify[1]);    // close the writing end
    pipe_pollfd.fd = pipe_notify[0];
    pipe_pollfd.events = POLLIN;

    for (tries = 0;; tries++) {
        if (ghandles.log_level > 0)
            fprintf(stderr, ".");
        ret = poll(&pipe_pollfd, 1, 1000);
        if (ret < 0) {
            perror("poll");
            exit(1);
        }
        if (ret > 0) {
            if (pipe_pollfd.revents & POLLIN)
                break;
            if (ghandles.log_level > 0)
                fprintf(stderr, "exiting\n");
            exit(1);
        }
        if (tries >= ghandles.startup_timeout) {
            if (ghandles.startup_timeout > 0) {
                if (ghandles.log_level > 0)
                    fprintf(stderr, "timeout\n");
                exit(1);
            } else {
                if (ghandles.log_level > 0)
                    fprintf(stderr, "in the background\n");
            }
            exit(0);
        }

    }
    if (ghandles.log_level > 0)
        fprintf(stderr, "connected\n");
    exit(0);
}

enum {
    opt_trayicon_mode = 257,
    opt_screensaver_name = 258,
};

struct option longopts[] = {
    { "override-redirect", required_argument, NULL, 'r' },
    { "config", required_argument, NULL, 'C' },
    { "domid", required_argument, NULL, 'd' },
    { "name", required_argument, NULL, 'N' },
    { "target-domid", required_argument, NULL, 't' },
    { "color", required_argument, NULL, 'c' },
    { "label", required_argument, NULL, 'l' },
    { "icon", required_argument, NULL, 'i' },
    { "verbose", no_argument, NULL, 'v' },
    { "quiet", no_argument, NULL, 'q' },
    { "background", no_argument, NULL, 'n' },
    { "invisible", no_argument, NULL, 'I' },
    { "qrexec-for-clipboard", no_argument, NULL, 'Q' },
    { "foreground", no_argument, NULL, 'f' },
    { "kill-on-connect", required_argument, NULL, 'K' },
    { "prop", required_argument, NULL, 'p' },
    { "title-name", no_argument, NULL, 'T' },
    { "trayicon-mode", required_argument, NULL, opt_trayicon_mode },
    { "screensaver-name", required_argument, NULL, opt_screensaver_name },
    { "max-clipboard-size", required_argument, NULL, 'X' },
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 'V' },   // V is virtual and not a short option
    { 0, 0, 0, 0 },
};
static const char optstring[] = "+C:d:t:N:c:l:i:K:vqQnafIp:Th";

static void usage(FILE *stream)
{
    fprintf(stream,
        "Usage: qubes-guid -d domain_id -N domain_name [options]\n");
    fprintf(stream, "\n");
    fprintf(stream, "Options:\n");
    fprintf(stream, " --config=PATH, -C PATH\tpath to configuration file\n");
    fprintf(stream, " --verbose, -v\tincrease log verbosity\n");
    fprintf(stream, " --quiet, -q\tdecrease log verbosity\n");
    fprintf(stream, " --domid=ID, -d ID\tdomain ID running GUI agent\n");
    fprintf(stream, " --target-domid=ID, -t ID\tdomain ID of actual VM (may be different from --domid in case of stubdomain)\n");
    fprintf(stream, " --name=NAME, -N NAME\tVM name\n");
    fprintf(stream, " --color=COLOR, -c COLOR\tVM color (format 0xRRGGBB or color name)\n");
    fprintf(stream, " --label=LABEL_INDEX, -l LABEL_INDEX\tVM label index\n");
    fprintf(stream, " --icon=ICON, -i ICON\tIcon name (without suffix), or full icon path\n");
    fprintf(stream, " --qrexec-for-clipboard, -Q\tforce usage of Qrexec for clipboard operations\n");
    fprintf(stream, " --background, -n\tdo not wait for agent connection\n");
    fprintf(stream, " --foreground, -f\tdo not fork into background\n");
    fprintf(stream, " --invisible, -I\trun in \"invisible\" mode - do not show any VM window\n");
    fprintf(stream, " --kill-on-connect=PID, -K PID\twhen established connection to VM agent, send SIGUSR1 to given pid (ignored when -f set)\n");
    fprintf(stream, " --prop=name=type:value, -p\tadd additional X11 property on all the windows of this VM (up to 10 properties)\n");
    fprintf(stream, "          specify value as \"s:text\" for string, \"a:atom\" for atom, \"c:cardinal1,cardinal2,...\" for unsigned number(s)\n");
    fprintf(stream, " --title-name, -T\tprefix window titles with VM name\n");
    fprintf(stream, " --trayicon-mode\ttrayicon coloring mode (see below); default: tint\n");
    fprintf(stream, " --screensaver-name\tscreensaver window name, can be repeated, default: xscreensaver\n");
    fprintf(stream, " --max-clipboard-size=SIZE\tmaximum clipboard size VM is allowed to send to global clipboard\n");
    fprintf(stream, " --help, -h\tshow command help\n");
    fprintf(stream, " --version\tshow protocol version\n");
    fprintf(stream, " --override-redirect=disabled\tdisable the “override redirect” flag (will likely break applications)\n");
    fprintf(stream, "\n");
    fprintf(stream, "Log levels:\n");
    fprintf(stream, " 0 - only errors\n");
    fprintf(stream, " 1 - some basic messages (default)\n");
    fprintf(stream, " 2 - debug\n");
    fprintf(stream, "\n");
    fprintf(stream, "Available trayicon modes:\n");
    fprintf(stream, "  bg:\tcolor full icon background to the VM color\n");
    fprintf(stream, "  border1:\tadd 1px border at the icon edges\n");
    fprintf(stream, "  border2:\tadd 1px border 1px from the icon edges\n");
    fprintf(stream, "  tint :\ttint icon to the VM color,\n");
    fprintf(stream, "    can be used with additional modifiers (you can enable multiple of them)\n");
    fprintf(stream, "  tint+border1,tint+border2:\tsame as tint, but also add a border\n");
    fprintf(stream, "  tint+saturation50:\tsame as tint, but reduce icon saturation by 50%%\n");
    fprintf(stream, "  tint+whitehack:\tsame as tint, but change white pixels (0xffffff) to almost-white (0xfefefe)\n");
    fprintf(stream, "\n");
}

static void parse_cmdline_config_path(Ghandles * g, int argc, char **argv)
{
    int opt;
    optind = 1;

    while ((opt = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {
        if (opt == 'C') {
            strncpy(g->config_path, optarg, sizeof(g->config_path));
            g->config_path[sizeof(g->config_path) - 1] = '\0';
            if (strcmp(g->config_path, optarg)) {
                fprintf(stderr, "config path too long");
                exit(1);
            }
        } else if (opt == 'h') {
            usage(stdout);
            exit(0);
        } else if (opt == 'V') {
            fprintf(stdout, "Qubes GUI Daemon protocol version: %d.%d\n",
                    PROTOCOL_VERSION_MAJOR,
                    PROTOCOL_VERSION_MINOR);
            exit(0);
        }
    }
}

static void parse_cmdline_prop(Ghandles *g) {
    int prop_num;
    char *name, *type, *value;

    for (prop_num = 0; prop_num < MAX_EXTRA_PROPS; prop_num++) {
        if (!g->extra_props[prop_num].raw_option)
            continue;

        name = strtok(g->extra_props[prop_num].raw_option, "=");
        type = strtok(NULL, ":");
        value = strtok(NULL, "");

        if (!name || !type || !value) {
            fprintf(stderr, "Invalid -p argument format: should be PROP_NAME=TYPE:VALUE\n");
            exit(1);
        }

        g->extra_props[prop_num].prop = XInternAtom(g->display, name, False);
        if (g->extra_props[prop_num].prop == None) {
            fprintf(stderr, "Failed to get %s atom\n", name);
            exit(1);
        }
        if (strcmp(type, "s") == 0) {
            g->extra_props[prop_num].type = XA_STRING;
            g->extra_props[prop_num].format = 8;
            g->extra_props[prop_num].data = strdup(value);
            g->extra_props[prop_num].nelements = strlen(value);
        } else if (strcmp(type, "a") == 0) {
            Atom value_a;
            g->extra_props[prop_num].type = XA_ATOM;
            g->extra_props[prop_num].format = 32;
            g->extra_props[prop_num].data = malloc(sizeof(Atom));
            value_a = XInternAtom(g->display, value, False);
            if (value_a == None) {
                fprintf(stderr, "Failed to get %s atom\n", value);
                exit(1);
            }
            *(Atom*)g->extra_props[prop_num].data = value_a;
            g->extra_props[prop_num].nelements = 1;
        } else if (strcmp(type, "c") == 0) {
            char *value_tok;
            long *value_c;
            int nelements, i;

            nelements = 1;
            for (i = 0; value[i] != '\0'; i++)
                if (value[i] == ',')
                    nelements++;

            value_c = malloc(nelements * 4);
            if (!value_c) {
                fprintf(stderr, "Out of memory\n");
                exit(1);
            }
            i = 0;
            while ((value_tok = strtok(value, ","))) {
                errno = 0;
                value_c[i] = strtoul(value_tok, NULL, 0);
                if (errno) {
                    perror("strtoul");
                    exit(1);
                }
                i++;
                value = NULL;
            }
            assert(i == nelements);
            g->extra_props[prop_num].type = XA_CARDINAL;
            g->extra_props[prop_num].format = 32;
            g->extra_props[prop_num].data = (char*)value_c;
            g->extra_props[prop_num].nelements = nelements;
        } else {
            fprintf(stderr, "Unsupported -p format: %s\n", type);
            exit(1);
        }
        free(g->extra_props[prop_num].raw_option);
        g->extra_props[prop_num].raw_option = NULL;
    }
}

static void parse_trayicon_mode(Ghandles *g, const char *mode_str) {
    if (strcmp(mode_str, "bg") == 0) {
        g->trayicon_mode = TRAY_BACKGROUND;
        g->trayicon_border = 0;
    } else if (strcmp(mode_str, "border1") == 0) {
        g->trayicon_mode = TRAY_BORDER;
        g->trayicon_border = 1;
    } else if (strcmp(mode_str, "border2") == 0) {
        g->trayicon_mode = TRAY_BORDER;
        g->trayicon_border = 2;
    } else if (strncmp(mode_str, "tint", 4) == 0) {
        g->trayicon_mode = TRAY_TINT;
        g->trayicon_border = 0;
        if (strstr(mode_str, "+border1"))
            g->trayicon_border = 1;
        if (strstr(mode_str, "+border2"))
            g->trayicon_border = 2;
        if (strstr(mode_str, "+saturation50"))
            g->trayicon_tint_reduce_saturation = 1;
        if (strstr(mode_str, "+whitehack"))
            g->trayicon_tint_whitehack = 1;
    } else {
        fprintf(stderr, "Invalid trayicon mode: %s\n", mode_str);
        exit(1);
    }
}

/* FIXME: should be in a utility library */
static uint16_t parse_domid(const char *num)
{
    char *endp;
    long const s = strtol(num, &endp, 10);

    if (*endp)
        errx(1, "Trailing junk \"%s\" after domain ID", endp);

    if (s < 0)
        errx(1, "Domain ID cannot be negative (got %ld)", s);

    if (s == 0)
        errx(1, "Domain 0 never runs a GUI agent");

    if (s >= 0x7FF0)
        errx(1, "Domain %ld too large for a Xen domid", s);

    return (uint16_t)s;
}

static void parse_cmdline(Ghandles * g, int argc, char **argv)
{
    const char *lastarg;
    int opt;
    int prop_num = 0;
    int screensaver_name_num = 0;
    /* defaults */
    g->log_level = 1;
    g->qrexec_clipboard = 0;
    g->nofork = 0;
    g->kill_on_connect = 0;
    g->prefix_titles = 0;
    memset(g->extra_props, 0, MAX_EXTRA_PROPS * sizeof(struct extra_prop));
    memset(g->screensaver_names, 0, MAX_SCREENSAVER_NAMES * sizeof(char*));

    optind = 1;

    while ((lastarg = (optind < argc ? argv[optind] : NULL)),
           (opt = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {
        switch (opt) {
        case 'C':
            /* already handled in parse_cmdline_config_path */
            break;
        case 'd':
            if (g->domid)
                errx(1, "Cannot specify domid more than once (previous value %u)", g->domid);
            g->domid = parse_domid(optarg);
            break;
        case 't':
            if (g->target_domid)
                errx(1, "Cannot specify target domid more than once (previous value %u)", g->target_domid);
            g->target_domid = parse_domid(optarg);
            break;
        case 'N': {
            struct QubesSlice s = qubes_pure_buffer_init_from_nul_terminated_string(optarg);
            if (qubes_pure_is_valid_qube_name(s) != QUBE_NAME_OK)
                errx(1, "domain name '%s' not valid", optarg);
            if (s.length >= sizeof(g->vmname))
                errx(1, "domain name '%s' is too long (this is a bug)", optarg);
            memcpy(g->vmname, s.pointer, s.length + 1);
            break;
        }
        case 'c':
            g->cmdline_color = optarg;
            break;
        case 'l':
            g->label_index = strtoul(optarg, NULL, 0);
            break;
        case 'i':
            g->cmdline_icon = optarg;
            break;
        case 'q':
            if (g->log_level > 0)
                g->log_level--;
            break;
        case 'v':
            g->log_level++;
            break;
        case 'Q':
            g->qrexec_clipboard = 1;
            break;
        case 'n':
            g->startup_timeout = 0;
            break;
        case 'f':
            g->nofork = 1;
            break;
        case 'I':
            g->invisible = 1;
            break;
        case 'K':
            g->kill_on_connect = strtoul(optarg, NULL, 0);
            break;
        case 'r':
            if (!strcmp(optarg, "allow"))
                g->disable_override_redirect = 0;
            else if (!strcmp(optarg, "disabled"))
                g->disable_override_redirect = 1;
            else {
                fputs("Unsupported argument to --override-redirect=\n", stderr);
                exit(1);
            }
            break;
        case 'p':
            if (prop_num >= MAX_EXTRA_PROPS) {
                fprintf(stderr, "Too many extra properties (-p)\n");
                exit(1);
            }
            /* delay parsing until connected to X server */
            g->extra_props[prop_num++].raw_option = strdup(optarg);
            break;
        case 'T':
            g->prefix_titles = 1;
            break;
        case opt_trayicon_mode:
            parse_trayicon_mode(g, optarg);
            break;
        case opt_screensaver_name:
            if (screensaver_name_num >= MAX_SCREENSAVER_NAMES) {
                fprintf(stderr, "Too many --screensaver-name args\n");
                exit(1);
            }
            g->screensaver_names[screensaver_name_num++] = strdup(optarg);
            break;
        case 'X':
            unsigned int value;
            if (sscanf(optarg, "%u", &value) != 1)
                errx(1, "maximum clipboardsize '%s' is not an integer", optarg);
            if (value > MAX_CLIPBOARD_BUFFER_SIZE || value < MIN_CLIPBOARD_BUFFER_SIZE)
                errx(1, "unsupported value ‘%d’ for --max-clipboard-size "
                        "(must be between %d to %d characters)",
                        value, MAX_CLIPBOARD_BUFFER_SIZE, MIN_CLIPBOARD_BUFFER_SIZE);
            g->clipboard_buffer_size = value;
            break;
        default:
            usage(stderr);
            exit(1);
        }
    }
    if (g->domid<=0)
        errx(1, "no domain ID provided");

    if (g->nofork) {
        /* -K (kill on connect) doesn't make much sense in case of foreground
         * process, clear that flag. This will prevent killing innocent process
         * in case of guid restart (-f is appended there).
         */
        g->kill_on_connect = 0;
    }

    /* default target_domid to domid */
    if (!g->target_domid)
        g->target_domid = g->domid;
    if (g->vmname[0]=='\0') {
        fprintf(stderr, "domain name?");
        exit(1);
    }

    if (screensaver_name_num == 0)
        g->screensaver_names[0] = "xscreensaver";

    if (argc != optind)
        errx(1, "GUI daemon takes no non-option arguments");

    if (lastarg && !strcmp(lastarg, "--"))
        errx(1, "GUI daemon does not use -- to mark end of options");
}

static void load_default_config_values(Ghandles * g)
{
    strcpy(g->config_path, GUID_CONFIG_FILE);
    g->allow_utf8_titles = 0;
    g->copy_seq_mask = ControlMask | ShiftMask;
    g->copy_seq_key = XK_c;
    g->paste_seq_mask = ControlMask | ShiftMask;
    g->paste_seq_key = XK_v;
    g->keyboard_grab_seq_mask = ControlMask;
    g->keyboard_grab_seq_key = XK_Control_R;
    g->clipboard_buffer_size = DEFAULT_CLIPBOARD_BUFFER_SIZE;
    g->allow_fullscreen = 0;
    g->override_redirect_protection = 1;
    g->startup_timeout = 45;
    g->trayicon_mode = TRAY_TINT;
    g->trayicon_border = 0;
    g->trayicon_tint_reduce_saturation = 0;
    g->trayicon_tint_whitehack = 0;
    g->window_background_color_pre_parse = "white";
}

// parse string describing key sequence like Ctrl-Alt-c
static void parse_key_sequence(const char *seq, int *mask, KeySym * key, char *vmname)
{
    const char *seqp = seq;
    char *name;
    int found_modifier;
    size_t i;

    const struct {
        char *name;
        int mask;
    } masks[] = {
        {"Shift-", ShiftMask},
        {"Ctrl-", ControlMask},
        {"Mod1-", Mod1Mask},
        {"Alt-", Mod1Mask}, /* alternate convenience name */
        {"Mod2-", Mod2Mask},
        {"Mod3-", Mod3Mask},
        {"Mod4-", Mod4Mask},
    };

    // ignore null string
    if (seq == NULL)
        return;

    // Option to disable hotkeys. Good for dom0 with dedicated GUIVMs or untrusted VMs
    if ((strcasecmp(seq, "disable") == 0) || (strcasecmp(seq, "none") == 0)) {
        *key = NoSymbol;
        fprintf(stderr,
            "Warning: Disabling copy or paste or keyboard grab hotkeys for %s\n",
            vmname);
        return;
    }

    *mask = 0;
    do {
        found_modifier = 0;
        for (i = 0; i < sizeof(masks)/sizeof(masks[0]); i++) {
            name = masks[i].name;
            if (strncasecmp(seqp, name, strlen(name)) == 0) {
                *mask |= masks[i].mask;
                seqp += strlen(name);
                found_modifier = 1;
            }
        }
    } while (found_modifier);

    *key = XStringToKeysym(seqp);
    if (*key == NoSymbol) {
        fprintf(stderr,
            "Error: key sequence (%s) set for %s is invalid\n",
            seq, vmname);
        exit(1);
    }
}

static void parse_vm_config(Ghandles * g, config_setting_t * group)
{
    config_setting_t *setting;

    if ((setting =
         config_setting_get_member(group, "secure_copy_sequence"))) {
        parse_key_sequence(config_setting_get_string(setting),
                   &g->copy_seq_mask, &g->copy_seq_key, (char *)&g->vmname);
    }
    if ((setting =
         config_setting_get_member(group, "secure_paste_sequence"))) {
        parse_key_sequence(config_setting_get_string(setting),
                   &g->paste_seq_mask, &g->paste_seq_key, (char *)&g->vmname);
    }
    if ((setting =
         config_setting_get_member(group, "keyboard_grab_sequence"))) {
        parse_key_sequence(config_setting_get_string(setting),
                   &g->keyboard_grab_seq_mask, &g->keyboard_grab_seq_key, (char *)&g->vmname);
    }

    if ((setting =
         config_setting_get_member(group, "max_clipboard_size"))) {
        int value = config_setting_get_int(setting);
        if (value > MAX_CLIPBOARD_BUFFER_SIZE || value < MIN_CLIPBOARD_BUFFER_SIZE) {
            fprintf(stderr,
                    "unsupported value ‘%d’ for max_clipboard_size "
                    "(must be between %d to %d characters).\n",
                    value, MAX_CLIPBOARD_BUFFER_SIZE, MIN_CLIPBOARD_BUFFER_SIZE);
        } else {
            g->clipboard_buffer_size = value;
        }
    }

    if ((setting =
         config_setting_get_member(group, "allow_utf8_titles"))) {
        g->allow_utf8_titles = config_setting_get_bool(setting);
    }

    if ((setting =
         config_setting_get_member(group, "log_level"))) {
        g->log_level = config_setting_get_int(setting);
    }

    if ((setting =
         config_setting_get_member(group, "allow_fullscreen"))) {
        g->allow_fullscreen = config_setting_get_bool(setting);
    }

    if ((setting =
         config_setting_get_member(group, "override_redirect_protection"))) {
        g->override_redirect_protection = config_setting_get_bool(setting);
    }

    if ((setting =
         config_setting_get_member(group, "trayicon_mode"))) {
        parse_trayicon_mode(g, config_setting_get_string(setting));
    }

    if ((setting =
         config_setting_get_member(group, "window_background_color"))) {
        g->window_background_color_pre_parse = config_setting_get_string(setting);
    }

    if ((setting =
         config_setting_get_member(group, "startup_timeout"))) {
        g->startup_timeout = config_setting_get_int(setting);
    }

    if ((setting =
         config_setting_get_member(group, "override_redirect"))) {
        const char *value;
        if ((value = config_setting_get_string(setting)) == NULL) {
            fputs("override_redirect must be a string\n", stderr);
            exit(1);
        } else if (!strcmp(value, "disabled"))
            g->disable_override_redirect = 1;
        else if (!strcmp(value, "allow"))
            g->disable_override_redirect = 0;
        else {
            fprintf(stderr,
                    "unsupported value '%s' for override_redirect (must be 'disabled' or 'allow')\n",
                    value);
            exit(1);
        }
    }

    if ((setting =
         config_setting_get_member(group, "events_max_delay"))) {
        int delay_val = config_setting_get_int(setting);
        if (delay_val < 0 || delay_val > 5000) {
            fprintf(stderr,
                    "unsupported value '%d' for events_max_delay (must be >= 0 and <= 5000)",
                    delay_val);
            exit(1);
        }
        g->ebuf_max_delay = delay_val;
    }
}

static void parse_config(Ghandles * g)
{
    config_t config;
    config_setting_t *setting;

    config_init(&config);
#if (((LIBCONFIG_VER_MAJOR == 1) && (LIBCONFIG_VER_MINOR > 5)) \
        || (LIBCONFIG_VER_MAJOR > 1))
    config_set_include_dir(&config, GUID_CONFIG_DIR);
#endif
    if (config_read_file(&config, g->config_path) == CONFIG_FALSE) {
#if (((LIBCONFIG_VER_MAJOR == 1) && (LIBCONFIG_VER_MINOR >= 4)) \
        || (LIBCONFIG_VER_MAJOR > 1))
        if (config_error_type(&config) == CONFIG_ERR_FILE_IO) {
#else
        if (strcmp(config_error_text(&config), "file I/O error") ==
            0) {
#endif
            fprintf(stderr,
                "Warning: cannot read config file (%s): %s\n",
                g->config_path,
                config_error_text(&config));
        } else {
            fprintf(stderr,
                "Critical: error reading config (%s:%d): %s\n",
#if (((LIBCONFIG_VER_MAJOR == 1) && (LIBCONFIG_VER_MINOR >= 4)) \
        || (LIBCONFIG_VER_MAJOR > 1))
                config_error_file(&config),
#else
                g->config_path,
#endif
                config_error_line(&config),
                config_error_text(&config));
            exit(1);
        }
    }
    if ((setting = config_lookup(&config, "global"))) {
        parse_vm_config(g, setting);
    }
}

static int guid_boot_lock = -1;

/* create guid_running file when connected to VM */
static void set_alive_flag(int domid)
{
    char pid_buf[10];
    int fd = open(guid_fs_flag("running", domid),
              O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    snprintf(pid_buf, sizeof(pid_buf), "%d\n", getpid());
    if (write(fd, pid_buf, strlen(pid_buf)) < (int)strlen(pid_buf)) {
        warn("failed to write pid file %s", guid_fs_flag("running", domid));
    }
    close(fd);
    unlink(guid_fs_flag("booting", domid));
    close(guid_boot_lock);
}

static void vchan_close()
{
    libvchan_close(ghandles.vchan);
}

static void get_boot_lock(int domid)
{
    struct stat st;
    int fd = open(guid_fs_flag("booting", domid),
              O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("cannot get boot lock ???\n");
        exit(1);
    }
    if (flock(fd, LOCK_EX) < 0) {
        unlink(guid_fs_flag("booting", domid));
        perror("lock");
        exit(1);
    }
    if (!stat(guid_fs_flag("running", domid), &st)) {
        /* guid running, nothing to do */
        unlink(guid_fs_flag("booting", domid));
        exit(0);
    }
    guid_boot_lock = fd;
}

static void cleanup() {
    XFree(ghandles.hostname.value);
    XCloseDisplay(ghandles.display);
    unset_alive_flag();
    close(ghandles.inter_appviewer_lock_fd);
}

static char** restart_argv;
static void restart_guid() {
    cleanup();
    execv("/usr/bin/qubes-guid", restart_argv);
    perror("execv");
}

int main(int argc, char **argv)
{
    int xfd;
    int childpid;
    int pipe_notify[2];
    char dbg_log[256];
    char dbg_log_old[256];
    char shmid_filename[SHMID_FILENAME_LEN];
    int logfd;
    char cmd_tmp[256];
    struct stat stat_buf;
    char *display_str;
    int display_num;

    load_default_config_values(&ghandles);
    /* get the config file path first */
    parse_cmdline_config_path(&ghandles, argc, argv);
    /* load config file */
    parse_config(&ghandles);
    /* parse cmdline, possibly overriding values from config */
    parse_cmdline(&ghandles, argc, argv);
    get_boot_lock(ghandles.domid);
    /* init event queue */
    TAILQ_INIT(&(ghandles.ebuf_head));

    if (!ghandles.nofork) {
        // daemonize...
        if (pipe(pipe_notify) < 0) {
            perror("cannot create pipe:");
            exit(1);
        }

        childpid = fork();
        if (childpid < 0) {
            fprintf(stderr, "Cannot fork :(\n");
            exit(1);
        } else if (childpid > 0) {
            wait_for_connection_in_parent(pipe_notify);
            exit(0);
        }
        close(pipe_notify[0]);
    }

    // inside the daemonized process...
    if (!ghandles.invisible) {
        display_str = getenv("DISPLAY");
        if (display_str == NULL) {
            fprintf(stderr, "No DISPLAY in environment\n");
            exit(1);
        }
        if (sscanf(display_str, ":%d.%*d", &display_num) != 1 &&
                sscanf(display_str, ":%d", &display_num) != 1) {
            fprintf(stderr, "DISPLAY parse error, expected format like :0 or :0.0\n");
            exit(1);
        }
        if ((unsigned)snprintf(shmid_filename, SHMID_FILENAME_LEN,
             SHMID_FILENAME_PREFIX "%d", display_num) >= SHMID_FILENAME_LEN)
            abort();
        int f = open(shmid_filename, O_RDWR|O_NOCTTY|O_NOFOLLOW /* should not be symlink */|O_CLOEXEC);
        if (f < 0) {
            if (errno != ENOENT)
                err(1, "Cannot open %s", shmid_filename);
            fprintf(stderr,
                    "Missing %s; run X with preloaded shmoverride\n",
                    shmid_filename);
            exit(1);
        }
        ghandles.shm_args = mmap(NULL, SHM_ARGS_SIZE, PROT_READ|PROT_WRITE,
                                 MAP_SHARED_VALIDATE, f, 0);
        if (ghandles.shm_args == MAP_FAILED)
            err(1, "Could not map shared memory file %s", shmid_filename);
    }

    /* prepare argv for possible restarts */
    if (ghandles.nofork) {
        /* "-f" option already given, use the same argv */
        restart_argv = argv;
    } else {
        /* append "-f" option */
        static char f[3] = "-f";

        if (!(restart_argv = malloc((argc+2) * sizeof(char*))))
            err(1, "malloc");
        restart_argv[0] = argv[0];
        restart_argv[1] = f;
        memcpy(restart_argv + 2, argv + 1, argc * sizeof(char *));
    }

    if (!ghandles.nofork) {
        int fd;
        /* output redirection only when started as daemon, if "nofork" option
         * is set as part of guid restart, output is already redirected */
        while ((fd = open("/dev/null", O_RDONLY | O_CLOEXEC)) < 0) {
            if (errno != EINTR)
                err(1, "open /dev/null");
        }
        while (dup2(fd, 0) < 0) {
            if (errno != EINTR)
                err(1, "dup2");
        }
        close(fd);
        fd = -1;
        snprintf(dbg_log, sizeof(dbg_log),
                "/var/log/qubes/guid.%s.log", ghandles.vmname);
        snprintf(dbg_log_old, sizeof(dbg_log_old),
                "/var/log/qubes/guid.%s.log.old", ghandles.vmname);
        if (stat(dbg_log, &stat_buf) == 0) {
            if (rename(dbg_log, dbg_log_old) < 0) {
                perror("Rename old logfile");
            }
        }
        umask(0007);
        logfd = open(dbg_log, O_WRONLY | O_CREAT | O_TRUNC, 0640);
        umask(0077);
        if (logfd < 0) {
            fprintf(stderr,
                    "Failed to open log file: %s\n", strerror (errno));
            exit(1);
        }
        while (dup2(logfd, 1) < 0 || dup2(logfd, 2) < 0) {
            if (errno != EINTR)
                err(1, "dup2");
        }
        if (logfd > 2)
            close(logfd);
    }

    if (chdir("/run/qubes")) {
        perror("chdir /run/qubes");
        exit(1);
    }
    errno = 0;
    if (!ghandles.nofork && setsid() < 0) {
        perror("setsid()");
        exit(1);
    }
    mkghandles(&ghandles);
    XSetErrorHandler(x11_error_handler);
    default_x11_io_error_handler = XSetIOErrorHandler(x11_io_error_handler);
    double_buffer_init();
    ghandles.vchan = libvchan_client_init(ghandles.domid, 6000);
    if (!ghandles.vchan) {
        fprintf(stderr, "Failed to connect to gui-agent\n");
        exit(1);
    }
    atexit(vchan_close);
    /* drop root privileges */
    if (setgid(getgid()) < 0) {
        perror("setgid()");
        exit(1);
    }
    if (setuid(getuid()) < 0) {
        perror("setuid()");
        exit(1);
    }
    set_alive_flag(ghandles.domid);
    atexit(unset_alive_flag);

    // let write return -EPIPE, instead of delivering a signal
    signal(SIGPIPE, SIG_IGN);

    if (!ghandles.nofork) {
        // let the parent know we connected sucessfully
        ignore_result(write(pipe_notify[1], "Q", 1));
        close (pipe_notify[1]);
    }

    signal(SIGTERM, dummy_signal_handler);
    signal(SIGUSR1, dummy_signal_handler);
    signal(SIGHUP, sighup_signal_handler);
    atexit(print_backtrace);

    if (ghandles.kill_on_connect) {
        kill(ghandles.kill_on_connect, SIGUSR1);
    }


    xfd = ConnectionNumber(ghandles.display);

    /* provide keyboard map before VM Xserver starts */

    if (access(QUBES_RELEASE, F_OK) != -1) {
        /* cast return value to unsigned, so (unsigned)-1 > sizeof(cmd_tmp) */
        if ((unsigned)snprintf(cmd_tmp, sizeof(cmd_tmp), "/usr/bin/qubesdb-write -d %s "
                 "/qubes-keyboard \"`/usr/bin/setxkbmap -print`\"",
                 ghandles.vmname) < sizeof(cmd_tmp)) {
            /* intentionally ignore return value - don't fail gui-daemon if only
             * keyboard layout fails */
            ignore_result(system(cmd_tmp));
        }
        ghandles.in_dom0 = true;
    } else if (errno != ENOENT) {
        perror("cannot determine if " QUBES_RELEASE " exists");
        exit(1);
    } else {
        ghandles.in_dom0 = false;
    }
    vchan_register_at_eof(restart_guid);

    get_protocol_version(&ghandles);
    send_xconf(&ghandles);

    for (;;) {
        int busy;
        if (ghandles.reload_requested) {
            fprintf(stderr, "reloading X server parameters...\n");
            reload(&ghandles);
            ghandles.reload_requested = 0;
        }
        do {
            busy = 0;
            if (XPending(ghandles.display)) {
                process_xevent(&ghandles);
                busy = 1;
            }
            if (libvchan_data_ready(ghandles.vchan) >= (int)sizeof(struct msg_hdr)) {
                handle_message(&ghandles);
                busy = 1;
            }
            if (ghandles.ebuf_max_delay > 0) {
                ebuf_release_xevents(&ghandles);
            }
        } while (busy);
        if (ghandles.ebuf_max_delay > 0) {
            wait_for_vchan_or_argfd_once(ghandles.vchan, xfd, ghandles.ebuf_next_timeout);
        } else {
            wait_for_vchan_or_argfd_once(ghandles.vchan, xfd, VCHAN_DEFAULT_POLL_DURATION);
        }
    }
    return 0;
}
