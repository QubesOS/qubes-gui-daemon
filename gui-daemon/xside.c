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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <sys/shm.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <execinfo.h>
#include <getopt.h>
#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/shmproto.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <libconfig.h>
#include <libnotify/notify.h>
#include <assert.h>
#include <qubes-gui-protocol.h>
#include <qubes-xorg-tray-defs.h>
#include <libvchan.h>
#include "xside.h"
#include "txrx.h"
#include "double-buffer.h"
#include "list.h"
#include "error.h"
#include "png.h"
#include "trayicon.h"
#include "shm-args.h"
#include "util.h"

/* Supported protocol version */

#define PROTOCOL_VERSION_MAJOR 1
#define PROTOCOL_VERSION_MINOR 3
#define PROTOCOL_VERSION (PROTOCOL_VERSION_MAJOR << 16 | PROTOCOL_VERSION_MINOR)

#if !(PROTOCOL_VERSION_MAJOR == QUBES_GUID_PROTOCOL_VERSION_MAJOR && \
      PROTOCOL_VERSION_MINOR <= QUBES_GUID_PROTOCOL_VERSION_MINOR)
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
    if (!(vm_window=check_nonmanaged_window(g, id))) return

#ifndef min
#define min(x,y) ((x)>(y)?(y):(x))
#endif
#ifndef max
#define max(x,y) ((x)<(y)?(y):(x))
#endif

#define ignore_result(x) { __typeof__(x) __attribute__((unused)) _ignore=(x);}

/* XShmAttach return value inform only about successful queueing the operation,
 * not its execution. Errors during XShmAttach are reported asynchronously with
 * registered X11 error handler.
 */
static bool shm_attach_failed = false;

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
    notify = notify_notification_new(message, NULL, g->cmdline_icon);
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

#ifdef MAKE_X11_ERRORS_FATAL
/* nothing called from X11 error handler can send X11 requests, so release
 * shared memory and simply forget about the image - the gui daemon will exit
 * shortly anyway.
 */
static void release_all_shm_no_x11_calls() {
    struct genlist *curr;
    if (ghandles.log_level > 1)
        fprintf(stderr, "release_all_shm_no_x11_calls running\n");
    print_backtrace();
    for (curr = ghandles.wid2windowdata->next;
         curr != ghandles.wid2windowdata; curr = curr->next) {
        struct windowdata *vm_window = curr->data;
        if (vm_window->image) {
            vm_window->image = NULL;
            shmctl(vm_window->shminfo.shmid, IPC_RMID, 0);
        }
    }

}
#endif

int x11_error_handler(Display * dpy, XErrorEvent * ev)
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

    if (ev->request_code == ghandles.shm_major_opcode
            && ev->minor_code == X_ShmAttach
            && ev->error_code == BadAccess) {
        shm_attach_failed = true;
        /* shmoverride failed to attach memory region,
         * handled in handle_mfndump/handle_window_dump */
        return 0;
    }
#ifdef MAKE_X11_ERRORS_FATAL
    /* The exit(1) below will call release_all_mapped_mfns (registerd with
     * atexit(3)), which would try to release window images with XShmDetach. We
     * can't send X11 requests in X11 error handler, so clean window images
     * without calling to X11. And hope that X server will call XShmDetach
     * internally when cleaning windows of disconnected client */
    release_all_shm_no_x11_calls();
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
    /* The default error handler below will call exit(1), which will then call
     * release_all_mapped_mfns (registerd with atexit(3)), which would try to
     * release window images with XShmDetach.
     * When the IO error occurs in X11 it is no longer safe/possible to
     * communicate with the X server. Clean window images without calling to
     * X11. And hope that X server will call XShmDetach internally when
     * cleaning windows of disconnected client */
    release_all_shm_no_x11_calls();
    if (default_x11_io_error_handler)
        default_x11_io_error_handler(dpy);
    exit(1);
}


/* prepare graphic context for painting colorful frame and set RGB value of the
 * color */
static void get_frame_gc(Ghandles * g, const char *name)
{
    XGCValues values;
    XColor fcolor, dummy;
    if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
        unsigned int rgb = strtoul(name, 0, 16);
        fcolor.blue = (rgb & 0xff) * 257;
        rgb >>= 8;
        fcolor.green = (rgb & 0xff) * 257;
        rgb >>= 8;
        fcolor.red = (rgb & 0xff) * 257;
        XAllocColor(g->display,
                XDefaultColormap(g->display, g->screen),
                &fcolor);
    } else
        XAllocNamedColor(g->display,
                 XDefaultColormap(g->display, g->screen),
                 name, &fcolor, &dummy);
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
    Window parent;
    XSizeHints my_size_hints;    /* hints for the window manager */
    int i;
    XSetWindowAttributes attr;

    my_size_hints.flags = PSize;
    my_size_hints.width = vm_window->width;
    my_size_hints.height = vm_window->height;

    if (vm_window->parent)
        parent = vm_window->parent->local_winid;
    else
        parent = g->root_win;
    attr.override_redirect = vm_window->override_redirect;
    child_win = XCreateWindow(g->display, parent,
                    vm_window->x, vm_window->y,
                    vm_window->width,
                    vm_window->height, 0,
		    CopyFromParent,
                    CopyFromParent,
                    CopyFromParent,
                    CWOverrideRedirect, &attr);
    /* pass my size hints to the window manager, along with window
       and icon names */
    (void) XSetStandardProperties(g->display, child_win,
                      "VMapp command", "Pixmap", None,
                      gargv, 0, &my_size_hints);
    (void) XSelectInput(g->display, child_win,
                ExposureMask | KeyPressMask | KeyReleaseMask |
                ButtonPressMask | ButtonReleaseMask |
                PointerMotionMask | EnterWindowMask | LeaveWindowMask |
                FocusChangeMask | StructureNotifyMask | PropertyChangeMask);
    XSetWMProtocols(g->display, child_win, &g->wmDeleteMessage, 1);
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

static const int desktop_coordinates_size = 4;
static const long max_display_width = 1UL << 20;

/* update g when the current desktop changes */
static void update_work_area(Ghandles *g) {
    unsigned long *scratch;
    unsigned long nitems, bytesleft;
    Atom act_type;
    int ret, act_fmt;

    ret = XGetWindowProperty(g->display, g->root_win, g->wm_current_desktop,
        0, 1, False, XA_CARDINAL, &act_type, &act_fmt, &nitems, &bytesleft,
        (unsigned char**)&scratch);
    if (ret != Success || nitems != 1 || act_fmt != 32 || act_type != XA_CARDINAL) {
        if (None == act_fmt && !act_fmt && !bytesleft) {
            g->work_x = 0;
            g->work_y = 0;
            g->work_width = g->root_width;
            g->work_height = g->root_height;
            return;
        }
        /* Panic!  Serious window manager problem. */
        fputs("PANIC: cannot obtain current desktop\n"
                "Instead of creating a security hole we will just exit.\n",
            stderr);
        exit(1);
    }
    if (*scratch > max_display_width) {
        fputs("Absurd current desktop, crashing\n", stderr);
        abort();
    }
    uint32_t current_desktop = (uint32_t)*scratch;
    XFree(scratch);
    ret = XGetWindowProperty(g->display, g->root_win, g->wm_workarea,
            current_desktop * desktop_coordinates_size,
            desktop_coordinates_size, False, XA_CARDINAL, &act_type, &act_fmt,
            &nitems, &bytesleft, (unsigned char**)&scratch);
    if (ret != Success || nitems != desktop_coordinates_size || act_fmt != 32 ||
        act_type != XA_CARDINAL) {
        if (None == act_fmt && !act_fmt && !bytesleft) {
            g->work_x = 0;
            g->work_y = 0;
            g->work_width = g->root_width;
            g->work_height = g->root_height;
            return;
        }
        /* Panic!  We have no idea where the window should be.  The only safe
         * thing to do is exit. */
        fputs("PANIC: cannot obtain work area\n"
                "Instead of creating a security hole we will just exit.\n",
                stderr);
        exit(1);
    }
    for (int s = 0; s < desktop_coordinates_size; ++s) {
        if (scratch[s] > max_display_width) {
            fputs("Absurd work area, crashing\n", stderr);
            abort();
        }
    }
    g->work_x = scratch[0];
    g->work_y = scratch[1];
    g->work_width = scratch[2];
    g->work_height = scratch[3];
    XFree(scratch);
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
        { &g->wm_current_desktop, "_NET_WM_CURRENT_DESKTOP" },
        { &g->wmDeleteMessage, "WM_DELETE_WINDOW" },
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

/* prepare global variables content:
 * most of them are handles to local Xserver structures */
static void mkghandles(Ghandles * g)
{
    int ev_base, err_base; /* ignore */
    XWindowAttributes attr;
    int i;

    g->display = XOpenDisplay(NULL);
    if (!g->display) {
        perror("XOpenDisplay");
        exit(1);
    }
    g->screen = DefaultScreen(g->display);
    g->root_win = RootWindow(g->display, g->screen);
    XGetWindowAttributes(g->display, g->root_win, &attr);
    g->root_width = _VIRTUALX(attr.width);
    g->root_height = attr.height;
    g->context = XCreateGC(g->display, g->root_win, 0, NULL);
    g->clipboard_requested = 0;
    g->clipboard_xevent_time = 0;
    intern_global_atoms(g);
    if (!XQueryExtension(g->display, "MIT-SHM",
                &g->shm_major_opcode, &ev_base, &err_base))
        fprintf(stderr, "MIT-SHM X extension missing!\n");
    /* get the work area */
    update_work_area(g);
    /* create graphical contexts */
    get_frame_gc(g, g->cmdline_color ? g->cmdline_color : "red");
    if (g->trayicon_mode == TRAY_BACKGROUND)
        init_tray_bg(g);
    else if (g->trayicon_mode == TRAY_TINT)
        init_tray_tint(g);
    /* nothing extra needed for TRAY_BORDER */
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
}

/* reload X server parameters, especially after monitor/screen layout change */
void reload(Ghandles * g) {
    XWindowAttributes attr;

    g->screen = DefaultScreen(g->display);
    g->root_win = RootWindow(g->display, g->screen);
    XGetWindowAttributes(g->display, g->root_win, &attr);
    g->root_width = _VIRTUALX(attr.width);
    g->root_height = attr.height;
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

/* caller must take inter_appviewer_lock first */
static Time get_clipboard_file_xevent_timestamp() {
    FILE *file;
    Time timestamp;

    file = fopen(QUBES_CLIPBOARD_FILENAME ".xevent", "r");
    if (!file) {
        perror("open " QUBES_CLIPBOARD_FILENAME ".xevent");
        return 0;
    }
    if (fscanf(file, "%lu", &timestamp) < 1) {
        fprintf(stderr, "Failed to load " QUBES_CLIPBOARD_FILENAME " file (empty?)\n");
        timestamp = 0;
    }
    fclose(file);
    return timestamp;
}

/* caller must take inter_appviewer_lock first */
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

/* fetch clippboard content from file */
/* lock already taken in is_special_keypress() */
static void get_qubes_clipboard(Ghandles *g, char **data, int *len)
{
    FILE *file;
    *len = 0;
    file = fopen(QUBES_CLIPBOARD_FILENAME, "r");
    if (!file)
        return;
    if (fseek(file, 0, SEEK_END) < 0) {
        show_error_message(g, "secure paste: failed to seek in " QUBES_CLIPBOARD_FILENAME);
        goto close_done;
    }
    *len = ftell(file);
    if (*len < 0) {
        *len = 0;
        show_error_message(g, "secure paste: failed to determine size of "
            QUBES_CLIPBOARD_FILENAME);
        goto close_done;
    }
    if (*len == 0)
        goto close_done;
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
        goto close_done;
    }
    *len=fread(*data, 1, *len, file);
    if (*len < 0) {
        *len = 0;
        free(*data);
        *data=NULL;
        show_error_message(g, "secure paste: failed to read from "
            QUBES_CLIPBOARD_FILENAME);
        goto close_done;
    }
close_done:
    fclose(file);
    if (truncate(QUBES_CLIPBOARD_FILENAME, 0)) {
        perror("failed to truncate " QUBES_CLIPBOARD_FILENAME ", trying unlink instead\n");
        unlink(QUBES_CLIPBOARD_FILENAME);
    }
    save_clipboard_source_vmname("");
}

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
                rl.rlim_cur = MAX_CLIPBOARD_SIZE;
                rl.rlim_max = MAX_CLIPBOARD_SIZE;
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

static int fetch_qubes_clipboard_using_qrexec(Ghandles * g) {
    int ret;

    inter_appviewer_lock(g, 1);
    ret = run_clipboard_rpc(g, CLIPBOARD_COPY);
    if (ret) {
        save_clipboard_source_vmname(g->vmname);
        save_clipboard_file_xevent_timestamp(g->clipboard_xevent_time);
    } else {
        if (truncate(QUBES_CLIPBOARD_FILENAME, 0)) {
            perror("failed to truncate " QUBES_CLIPBOARD_FILENAME ", trying unlink instead\n");
            unlink(QUBES_CLIPBOARD_FILENAME);
        }
        save_clipboard_source_vmname("");
    }

    inter_appviewer_lock(g, 0);
    return ret;
}

/* lock already taken in is_special_keypress() */
static int paste_qubes_clipboard_using_qrexec(Ghandles * g) {
    int ret;

    ret = run_clipboard_rpc(g, CLIPBOARD_PASTE);
    if (ret) {
        if (truncate(QUBES_CLIPBOARD_FILENAME, 0)) {
            perror("failed to truncate " QUBES_CLIPBOARD_FILENAME ", trying unlink instead\n");
            unlink(QUBES_CLIPBOARD_FILENAME);
        }
        save_clipboard_source_vmname("");
    }

    return ret;
}


/* handle VM message: MSG_CLIPBOARD_DATA
 *  - checks if clipboard data was requested
 *  - store it in file
 */
static void handle_clipboard_data(Ghandles * g, unsigned int untrusted_len)
{
    FILE *file;
    char *untrusted_data;
    size_t untrusted_data_sz;
    Time clipboard_file_xevent_time;
    mode_t old_umask;

    if (g->log_level > 0)
        fprintf(stderr, "handle_clipboard_data, len=0x%x\n",
            untrusted_len);
    if (untrusted_len > MAX_CLIPBOARD_SIZE) {
        fprintf(stderr, "clipboard data len 0x%x?\n",
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
    inter_appviewer_lock(g, 1);
    clipboard_file_xevent_time = get_clipboard_file_xevent_timestamp();
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

static int evaluate_clipboard_policy_dom0(Ghandles *g,
        const char *const source_vm) {
    const int sockfd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    int status = 0;
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
        return evaluate_clipboard_policy_dom0(g, source_vm);
    else
        return evaluate_clipboard_policy_domU(g, source_vm);
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
 * currently only for inter-vm clipboard copy
 */
static int is_special_keypress(Ghandles * g, const XKeyEvent * ev, XID remote_winid)
{
    struct msg_hdr hdr;
    char *data;
    int len;
    Time clipboard_file_xevent_time;

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
                fprintf(stderr, "secure copy\n");
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
        clipboard_file_xevent_time = get_clipboard_file_xevent_timestamp();
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
                if (g->agent_version < 0x00010002)
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
    return 0;
}

/* handle local Xserver event: XKeyEvent
 * send it to relevant window in VM
 */
static void process_xevent_keypress(Ghandles * g, const XKeyEvent * ev)
{
    struct msg_hdr hdr;
    struct msg_keypress k;
    CHECK_NONMANAGED_WINDOW(g, ev->window);
    g->last_input_window = vm_window;
    if (is_special_keypress(g, ev, vm_window->remote_winid))
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

    g->last_input_window = vm_window;
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
    if ((vm_window->parent && ev->parent == vm_window->parent->local_winid) ||
        (!vm_window->parent && ev->parent == g->root_win))
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
    struct msg_configure msg;
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

/* undo the calculations that fix_docked_xy did, then perform move&resize */
static void moveresize_vm_window(Ghandles * g, struct windowdata *vm_window)
{
    int x = 0, y = 0;
    Window win;
    Atom act_type;
    long *frame_extents; // left, right, top, bottom
    unsigned long nitems, bytesleft;
    int ret, act_fmt;

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
    if (g->log_level > 1)
        fprintf(stderr,
            "XMoveResizeWindow local 0x%x remote 0x%x, xy %d %d (vm_window is %d %d) wh %d %d\n",
            (int) vm_window->local_winid,
            (int) vm_window->remote_winid, x, y, vm_window->x,
            vm_window->y, vm_window->width, vm_window->height);
    XMoveResizeWindow(g->display, vm_window->local_winid, x, y,
              vm_window->width, vm_window->height);
}


/* force window to not hide its frame
 * checks if at least border_width is from every screen edge (and fix if not)
 * Exception: allow window to be entirely off the screen */
static int force_on_screen(Ghandles * g, struct windowdata *vm_window,
            int border_width, const char *caller)
{
    int do_move = 0, reason = -1;
    int x = vm_window->x, y = vm_window->y, w = vm_window->width, h =
        vm_window->height;

    const int border_x = border_width + g->work_x,
        border_y = border_width + g->work_y,
        max_width = g->work_width - border_width,
        max_height = g->work_height - border_width;
    if (vm_window->x < border_x
        && vm_window->x + border_width + (int)vm_window->width > 0) {
        vm_window->x = border_x;
        do_move = 1;
        reason = 1;
    }
    if (vm_window->y < border_y
        && vm_window->y + border_width + (int)vm_window->width > 0) {
        vm_window->y = border_y;
        do_move = 1;
        reason = 2;
    }
    /* Note that vm_window->x and vm_window->y have already be sanitized */
    if (vm_window->x < g->root_width + border_width &&
        vm_window->x + (int)vm_window->width > max_width) {
        vm_window->width = max_width - vm_window->x;
        do_move = 1;
        reason = 3;
    }
    if (vm_window->y < g->root_height &&
        vm_window->y + (int)vm_window->height > max_height) {
        vm_window->height = max_height - vm_window->y;
        do_move = 1;
        reason = 4;
    }
    if (do_move)
        if (g->log_level > 0)
            fprintf(stderr,
                "force_on_screen(from %s) returns 1 (reason %d): window 0x%x, xy %d %d, wh %d %d, work area %d %d borderwidth %d\n",
                caller, reason,
                (int) vm_window->local_winid, x, y, w, h,
                g->work_width, g->work_height,
                border_width);
    return do_move;
}

static void set_override_redirect(Ghandles * g, struct windowdata *vm_window,
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
		vm_window->override_redirect = 0;
		return;
	}

	avail = (uint64_t) g->root_width * (uint64_t) g->root_height;
	desired = (uint64_t) vm_window->width * (uint64_t) vm_window->height;

	if (g->override_redirect_protection && req_override_redirect &&
	    desired > ((avail * MAX_OVERRIDE_REDIRECT_PERCENTAGE) / 100)) {
		req_override_redirect = 0;

		if (g->log_level > 0)
			fprintf(stderr,
				"%s unset override_redirect for "
				"local 0x%x remote 0x%x, "
				"window w=%u h=%u, root w=%d h=%d\n",
				__func__,
				(unsigned) vm_window->local_winid,
				(unsigned) vm_window->remote_winid,
				vm_window->width, vm_window->height,
				g->root_width, g->root_height);

		/* Show a message to the user, but do this only once. */
		if (!warning_shown) {
			show_message(g, "WARNING", warning_msg,
				     NOTIFY_EXPIRES_NEVER);
			warning_shown = 1;
		}
	}

	vm_window->override_redirect = req_override_redirect;
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
        Window parent, child;
        if (vm_window->parent)
            parent = vm_window->parent->local_winid;
        else
            parent = g->root_win;
        XTranslateCoordinates(g->display, ev->window, parent,
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
    int conf_changed;

    read_struct(g->vchan, untrusted_conf);
    if (g->log_level > 1)
        fprintf(stderr,
            "handle_configure_from_vm, local 0x%x remote 0x%x, %d/%d, was"
            " %d/%d, ovr=%d, xy %d/%d, was %d/%d\n",
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
    /* sanitize end */
    if (vm_window->width != width || vm_window->height != height ||
        vm_window->x != x || vm_window->y != y)
        conf_changed = 1;
    else
        conf_changed = 0;
    set_override_redirect(g, vm_window, !!(untrusted_conf.override_redirect));

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
    if (vm_window->override_redirect)
        // do not let menu window hide its color frame by moving outside of the screen
        // if it is located offscreen, then allow negative x/y
        force_on_screen(g, vm_window, 0,
                "handle_configure_from_vm");
    moveresize_vm_window(g, vm_window);
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

    /* sanitize start */
    if (untrusted_x < 0 || untrusted_y < 0) {
        if (g->log_level > 1)
            fprintf(stderr,
                "do_shm_update for 0x%x(remote 0x%x), x=%d, y=%d, w=%d, h=%d ?\n",
                (int) vm_window->local_winid,
                (int) vm_window->remote_winid, untrusted_x,
                untrusted_y, untrusted_w, untrusted_h);
        return;
    }
    if (vm_window->image) {
        x = min(untrusted_x, vm_window->image_width);
        y = min(untrusted_y, vm_window->image_height);
        w = min(max(untrusted_w, 0), vm_window->image_width - x);
        h = min(max(untrusted_h, 0), vm_window->image_height - y);
    } else if (g->screen_window) {
        /* update only onscreen window part */
        if (vm_window->x >= g->screen_window->image_width ||
                vm_window->y >= g->screen_window->image_height)
            return;
        if (vm_window->x < 0 && vm_window->x+untrusted_x < 0)
            untrusted_x = -vm_window->x;
        if (vm_window->y < 0 && vm_window->y+untrusted_y < 0)
            untrusted_y = -vm_window->y;
        x = min(untrusted_x, g->screen_window->image_width - vm_window->x);
        y = min(untrusted_y, g->screen_window->image_height - vm_window->y);
        w = min(max(untrusted_w, 0), g->screen_window->image_width - vm_window->x - x);
        h = min(max(untrusted_h, 0), g->screen_window->image_height - vm_window->y - y);
    } else {
        /* no image to update, will return after possibly drawing a frame */
        x = min(untrusted_x, (int)vm_window->width);
        y = min(untrusted_y, (int)vm_window->height);
        w = min(max(untrusted_w, 0), (int)vm_window->width - x);
        h = min(max(untrusted_h, 0), (int)vm_window->height - y);
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

    int do_border = 0;
    int delta, i;
    /* window contains only (forced) frame, so no content to update */
    if ((int)vm_window->width <= border_width * 2
        || (int)vm_window->height <= border_width * 2) {
        XFillRectangle(g->display, vm_window->local_winid,
                   g->frame_gc, 0, 0,
                   vm_window->width,
                   vm_window->height);
        return;
    }
    /* force frame to be visible: */
    /*   * left */
    delta = border_width - x;
    if (delta > 0) {
        w -= delta;
        x = border_width;
        do_border = 1;
    }
    /*   * right */
    delta = x + w - (vm_window->width - border_width);
    if (delta > 0) {
        w -= delta;
        do_border = 1;
    }
    /*   * top */
    delta = border_width - y;
    if (delta > 0) {
        h -= delta;
        y = border_width;
        do_border = 1;
    }
    /*   * bottom */
    delta = y + h - (vm_window->height - border_width);
    if (delta > 0) {
        h -= delta;
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
    } else {
        if (vm_window->image) {
            XShmPutImage(g->display, vm_window->local_winid,
                    g->context, vm_window->image, x,
                    y, x, y, w, h, 0);
        } else if (g->screen_window && g->screen_window->image) {
            XShmPutImage(g->display, vm_window->local_winid,
                    g->context, g->screen_window->image, vm_window->x+x,
                    vm_window->y+y, x, y, w, h, 0);
        }
        /* else no window content to update, but still draw a frame (if needed) */
    }
    if (!do_border)
        return;
    for (i = border_padding; i < border_padding + border_width; i++)
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
    Atom act_type;
    Atom *state_list;
    unsigned long nitems, bytesleft, i;
    int ret, act_fmt;
    int maximize_flags_seen;
    uint32_t flags;
    struct msg_hdr hdr;
    struct msg_window_flags msg;

    if (ev->window == g->root_win) {
        if (ev->state != PropertyNewValue)
            return;
        update_work_area(g);
    }
    CHECK_NONMANAGED_WINDOW(g, ev->window);
    if (ev->atom == g->wm_state) {
        if (!vm_window->is_mapped)
            return;
        if (ev->state == PropertyNewValue) {
            ret = XGetWindowProperty(g->display, vm_window->local_winid, g->wm_state, 0, 10,
                    False, XA_ATOM, &act_type, &act_fmt, &nitems, &bytesleft, (unsigned char**)&state_list);
            if (ret == Success && bytesleft > 0) {
              /* Ensure we read all of the atoms */
              XFree(state_list);
              ret = XGetWindowProperty(g->display, vm_window->local_winid, g->wm_state,
                    0, (10 * 4 + bytesleft + 3) / 4, False, XA_ATOM, &act_type, &act_fmt,
                    &nitems, &bytesleft, (unsigned char**)&state_list);
            }
            if (ret != Success) {
                if (g->log_level > 0) {
                    fprintf(stderr, "Failed to get 0x%x window state details\n", (int)ev->window);
                    return;
                }
            }
            flags = 0;
            /* check if both VERT and HORZ states are set */
            maximize_flags_seen = 0;
            for (i = 0; i < nitems; i++) {
                flags |= flags_from_atom(g, state_list[i]);
                if (state_list[i] == g->wm_state_maximized_vert)
                    maximize_flags_seen |= 1;
                if (state_list[i] == g->wm_state_maximized_horz)
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

/* dispatch local Xserver event */
static void process_xevent(Ghandles * g)
{
    XEvent event_buffer;
    XNextEvent(g->display, &event_buffer);
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
        }
        break;
    default:;
    }
}


/* handle VM message: MSG_SHMIMAGE
 * pass message data to do_shm_update - there input validation will be done */
static void handle_shmimage(Ghandles * g, struct windowdata *vm_window)
{
    struct msg_shmimage untrusted_mx;

    read_struct(g->vchan, untrusted_mx);
    if (!vm_window->is_mapped)
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
    struct genlist *l;
    struct msg_create untrusted_crt;
    XID parent;

    vm_window =
        (struct windowdata *) calloc(1, sizeof(struct windowdata));
    if (!vm_window) {
        perror("malloc(vm_window in handle_create)");
        exit(1);
    }
    /*
       because of calloc vm_window->image = 0;
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
    set_override_redirect(g, vm_window, !!(untrusted_crt.override_redirect));
    parent = untrusted_crt.parent;
    /* sanitize end */
    vm_window->remote_winid = window;
    if (!list_insert(g->remote2local, window, vm_window)) {
        fprintf(stderr, "list_insert(g->remote2local failed\n");
        exit(1);
    }
    l = list_lookup(g->remote2local, parent);
    if (l)
        vm_window->parent = l->data;
    else
        vm_window->parent = NULL;
    vm_window->transient_for = NULL;
    vm_window->local_winid = mkwindow(&ghandles, vm_window);
    if (g->log_level > 0)
        fprintf(stderr,
            "Created 0x%x(0x%x) parent 0x%x(0x%x) ovr=%d x/y %d/%d w/h %d/%d\n",
            (int) vm_window->local_winid, (int) window,
            (int) (vm_window->parent ? vm_window->parent->
                   local_winid : 0), (unsigned) parent,
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
        && force_on_screen(g, vm_window, 0, "handle_create"))
        moveresize_vm_window(g, vm_window);
}

/* handle VM message: MSG_DESTROY
 * destroy window locally, as requested */
static void handle_destroy(Ghandles * g, struct genlist *l)
{
    struct genlist *l2;
    struct windowdata *vm_window = l->data;
    g->windows_count--;
    if (vm_window == g->last_input_window)
        g->last_input_window = NULL;
    XDestroyWindow(g->display, vm_window->local_winid);
    if (g->log_level > 0)
        fprintf(stderr, " XDestroyWindow 0x%x\n",
            (int) vm_window->local_winid);
    if (vm_window->image)
        release_mapped_mfns(g, vm_window);
    l2 = list_lookup(g->wid2windowdata, vm_window->local_winid);
    list_remove(l);
    list_remove(l2);
    if (vm_window == g->screen_window)
        g->screen_window = NULL;
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
    char *list[1] = { buf };

    read_struct(g->vchan, untrusted_msg);
    /* sanitize start */
    untrusted_msg.data[sizeof(untrusted_msg.data) - 1] = 0;
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
    XSetWMIconName(g->display, vm_window->local_winid, &text_prop);
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
 * kept on top, i.e. screen lockers. */
static void restack_windows(Ghandles *g, struct windowdata *vm_window)
{
    Window root;
    Window parent;
    Window *children_list;
    unsigned int children_count;
    int i, current_pos, goal_pos;

    /* Find all windows below parent */

    XQueryTree(
        g->display,
        vm_window->parent ? vm_window->parent->local_winid : g->root_win,
        &root, &parent, &children_list, &children_count);

    if (!children_list) {
        fprintf(stderr, "XQueryTree returned an empty list\n");
        return;
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
    vm_window->is_mapped = 1;
    if (untrusted_txt.transient_for
        && (trans =
        list_lookup(g->remote2local,
                untrusted_txt.transient_for))) {
        struct windowdata *transdata = trans->data;
        vm_window->transient_for = transdata;
        XSetTransientForHint(g->display, vm_window->local_winid,
                     transdata->local_winid);
    } else
        vm_window->transient_for = NULL;

    set_override_redirect(g, vm_window, !!(untrusted_txt.override_redirect));
    attr.override_redirect = vm_window->override_redirect;
    XChangeWindowAttributes(g->display, vm_window->local_winid,
                            CWOverrideRedirect, &attr);
    if (vm_window->override_redirect
        && force_on_screen(g, vm_window, 0, "handle_map"))
        moveresize_vm_window(g, vm_window);
    if (vm_window->override_redirect) {
        /* force window update to draw colorful frame, even when VM have not
         * sent any content yet */
        do_shm_update(g, vm_window, 0, 0, vm_window->width, vm_window->height);
    }

    (void) XMapWindow(g->display, vm_window->local_winid);

    if (vm_window->override_redirect) {
        restack_windows(g, vm_window);
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
    tray = XGetSelectionOwner(g->display, g->tray_selection);
    if (tray != None) {
        long data[2];
        XClientMessageEvent msg;

        data[0] = 0;
        data[1] = 1;
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
    if (g->invisible)
        return;
    inter_appviewer_lock(g, 1);
    g->shm_args->shmid = vm_window->shminfo.shmid;
    XShmDetach(g->display, &vm_window->shminfo);
    XDestroyImage(vm_window->image);
    XSync(g->display, False);
    inter_appviewer_lock(g, 0);
    vm_window->image = NULL;
    shmctl(vm_window->shminfo.shmid, IPC_RMID, 0);
}

/* handle VM message: MSG_MFNDUMP
 * Retrieve memory addresses connected with composition buffer of remote window
 */
static void handle_mfndump(Ghandles * g, struct windowdata *vm_window)
{
    struct shm_cmd untrusted_shmcmd;
    unsigned num_mfn, off;
    static char dummybuf[100];
    size_t shm_args_len;
    struct shm_args_hdr *shm_args;
    struct shm_args_mfns *shm_args_mfns;
    size_t mfns_len;

    if (vm_window->image)
        release_mapped_mfns(g, vm_window);
    read_struct(g->vchan, untrusted_shmcmd);

    if (g->log_level > 1)
        fprintf(stderr, "MSG_MFNDUMP for 0x%x(0x%x): %dx%d, num_mfn 0x%x off 0x%x\n",
                (int) vm_window->local_winid, (int) vm_window->remote_winid,
                untrusted_shmcmd.width, untrusted_shmcmd.height,
                untrusted_shmcmd.num_mfn, untrusted_shmcmd.off);
    /* sanitize start */
    VERIFY(untrusted_shmcmd.num_mfn <= (unsigned)MAX_MFN_COUNT);
    num_mfn = untrusted_shmcmd.num_mfn;
    VERIFY((int) untrusted_shmcmd.width >= 0
           && (int) untrusted_shmcmd.height >= 0);
    VERIFY((int) untrusted_shmcmd.width <= MAX_WINDOW_WIDTH
           && (int) untrusted_shmcmd.height <= MAX_WINDOW_HEIGHT);
    VERIFY(untrusted_shmcmd.off < 4096);
    off = untrusted_shmcmd.off;
    /* unused for now: VERIFY(untrusted_shmcmd.bpp == 24); */
    /* sanitize end */
    vm_window->image_width = untrusted_shmcmd.width;
    vm_window->image_height = untrusted_shmcmd.height;    /* sanitized above */

    mfns_len = num_mfn * SIZEOF_SHARED_MFN;
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
    if (g->invisible)
        goto out_free_shm_args;
    vm_window->image =
        XShmCreateImage(g->display,
                DefaultVisual(g->display, g->screen), 24,
                ZPixmap, NULL, &vm_window->shminfo,
                vm_window->image_width,
                vm_window->image_height);
    if (!vm_window->image) {
        perror("XShmCreateImage");
        exit(1);
    }
    /* the below sanity check must be AFTER XShmCreateImage, it uses vm_window->image */
    if (num_mfn * 4096 <
        vm_window->image->bytes_per_line * vm_window->image->height +
        off) {
        fprintf(stderr,
            "handle_mfndump for window 0x%x(remote 0x%x)"
            " got too small num_mfn= 0x%x\n",
            (int) vm_window->local_winid,
            (int) vm_window->remote_winid, num_mfn);
        exit(1);
    }
    // temporary shmid; see shmoverride/README
    vm_window->shminfo.shmid =
        shmget(IPC_PRIVATE, 1, IPC_CREAT | 0700);
    if (vm_window->shminfo.shmid < 0) {
        perror("shmget");
        exit(1);
    }
    shm_args->shmid = vm_window->shminfo.shmid;
    shm_args->domid = g->domid;
    inter_appviewer_lock(g, 1);
    memcpy(g->shm_args, shm_args, shm_args_len);
    if (shm_args_len < SHM_ARGS_SIZE) {
        memset(((uint8_t *) g->shm_args) + shm_args_len, 0,
               SHM_ARGS_SIZE - shm_args_len);
    }
    vm_window->shminfo.shmaddr = vm_window->image->data = dummybuf;
    vm_window->shminfo.readOnly = True;
    shm_attach_failed = false;
    if (!XShmAttach(g->display, &vm_window->shminfo))
        shm_attach_failed = true;
    /* shm_attach_failed can be also set by the X11 error handler */
    XSync(g->display, False);
    g->shm_args->shmid = g->cmd_shmid;
    inter_appviewer_lock(g, 0);
    if (shm_attach_failed) {
        fprintf(stderr,
            "XShmAttach failed for window 0x%lx(remote 0x%lx)\n",
            vm_window->local_winid,
            vm_window->remote_winid);
        XDestroyImage(vm_window->image);
        vm_window->image = NULL;
        shmctl(vm_window->shminfo.shmid, IPC_RMID, 0);
    }

out_free_shm_args:
    free(shm_args);
}

static void handle_window_dump_body_grant_refs(Ghandles *g,
         size_t untrusted_wd_body_len, size_t *img_data_size, struct
         shm_args_hdr **shm_args, size_t *shm_args_len) {
    size_t refs_len;
    struct shm_args_grant_refs *shm_args_grant;

    // We don't have any custom arguments except the variable length refs list.
    assert(sizeof(struct msg_window_dump_grant_refs) == 0);

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
                               uint32_t untrusted_len) {
    struct msg_window_dump_hdr untrusted_wd_hdr;
    size_t untrusted_wd_body_len;
    uint32_t wd_type;
    static char dummybuf[100];
    size_t img_data_size = 0;
    struct shm_args_hdr *shm_args = NULL;
    size_t shm_args_len = 0;

    if (vm_window->image)
        release_mapped_mfns(g, vm_window);

    VERIFY(untrusted_len >= MSG_WINDOW_DUMP_HDR_LEN);
    read_struct(g->vchan, untrusted_wd_hdr);
    untrusted_wd_body_len = untrusted_len - MSG_WINDOW_DUMP_HDR_LEN;

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
    wd_type = untrusted_wd_hdr.type;
    VERIFY((int) untrusted_wd_hdr.width >= 0
           && (int) untrusted_wd_hdr.height >= 0);
    VERIFY((int) untrusted_wd_hdr.width <= MAX_WINDOW_WIDTH
           && (int) untrusted_wd_hdr.height <= MAX_WINDOW_HEIGHT);
    vm_window->image_width = untrusted_wd_hdr.width;
    vm_window->image_height = untrusted_wd_hdr.height;
    //VERIFY(untrusted_wd_hdr.bpp == 24);

    handle_window_dump_body(g, wd_type, untrusted_wd_body_len, &img_data_size,
                            &shm_args, &shm_args_len);

    if (g->invisible)
        return;

    // temporary shmid; see shmoverride/README
    vm_window->shminfo.shmid =
        shmget(IPC_PRIVATE, 1, IPC_CREAT | 0700);
    if (vm_window->shminfo.shmid < 0) {
        perror("shmget failed");
        exit(1);
    }

    vm_window->image =
        XShmCreateImage(g->display,
                DefaultVisual(g->display, g->screen), 24,
                ZPixmap, NULL, &vm_window->shminfo,
                vm_window->image_width,
                vm_window->image_height);
    if (!vm_window->image) {
        perror("XShmCreateImage");
        exit(1);
    }
    /* the below sanity check must be AFTER XShmCreateImage, it uses vm_window->image */
    if (img_data_size < (size_t) (vm_window->image->bytes_per_line *
                                  vm_window->image->height)) {
        fprintf(stderr,
            "handle_window_dump: got too small image data size (%zu)"
            " for window 0x%lx (remote 0x%lx)\n",
            img_data_size, vm_window->local_winid, vm_window->remote_winid);
        exit(1);
    }

    shm_args->domid = g->domid;
    shm_args->shmid = vm_window->shminfo.shmid;
    inter_appviewer_lock(g, 1);
    memcpy(g->shm_args, shm_args, shm_args_len);
    if (shm_args_len < SHM_ARGS_SIZE) {
        memset(((uint8_t *) g->shm_args) + shm_args_len, 0,
               SHM_ARGS_SIZE - shm_args_len);
    }
    vm_window->shminfo.shmaddr = vm_window->image->data = dummybuf;
    vm_window->shminfo.readOnly = True;
    shm_attach_failed = false;
    if (!XShmAttach(g->display, &vm_window->shminfo))
        shm_attach_failed = true;
    /* shm_attach_failed can be also set by the X11 error handler */
    XSync(g->display, False);
    g->shm_args->shmid = g->cmd_shmid;
    inter_appviewer_lock(g, 0);
    if (shm_attach_failed) {
        fprintf(stderr,
            "XShmAttach failed for window 0x%lx(remote 0x%lx)\n",
            vm_window->local_winid,
            vm_window->remote_winid);
        XDestroyImage(vm_window->image);
        vm_window->image = NULL;
        shmctl(vm_window->shminfo.shmid, IPC_RMID, 0);
    }
    free(shm_args);
}

/* VM message dispatcher */
static void handle_message(Ghandles * g)
{
    struct msg_hdr untrusted_hdr;
    uint32_t type;
    XID window = 0;
    struct genlist *l;
    struct windowdata *vm_window = NULL;

    read_struct(g->vchan, untrusted_hdr);
    VERIFY(untrusted_hdr.type > MSG_MIN
           && untrusted_hdr.type < MSG_MAX);
    /* sanitized msg type */
    type = untrusted_hdr.type;
    if (type == MSG_CLIPBOARD_DATA) {
        handle_clipboard_data(g, untrusted_hdr.untrusted_len);
        return;
    }
    l = list_lookup(g->remote2local, untrusted_hdr.window);
    if (type == MSG_CREATE) {
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
                type, untrusted_hdr.window);
            exit(1);
        }
        vm_window = l->data;
        /* not needed as it is in vm_window struct
           window = untrusted_hdr.window;
         */
    }

    switch (type) {
    case MSG_CREATE:
        handle_create(g, window);
        break;
    case MSG_DESTROY:
        handle_destroy(g, l);
        break;
    case MSG_MAP:
        handle_map(g, vm_window);
        break;
    case MSG_UNMAP:
        vm_window->is_mapped = 0;
        (void) XUnmapWindow(g->display, vm_window->local_winid);
        break;
    case MSG_CONFIGURE:
        handle_configure_from_vm(g, vm_window);
        break;
    case MSG_MFNDUMP:
        handle_mfndump(g, vm_window);
        break;
    case MSG_SHMIMAGE:
        handle_shmimage(g, vm_window);
        break;
    case MSG_WMNAME:
        handle_wmname(g, vm_window);
        break;
    case MSG_WMCLASS:
        handle_wmclass(g, vm_window);
        break;
    case MSG_DOCK:
        handle_dock(g, vm_window);
        break;
    case MSG_WINDOW_HINTS:
        handle_wmhints(g, vm_window);
        break;
    case MSG_WINDOW_FLAGS:
        handle_wmflags(g, vm_window);
        break;
    case MSG_WINDOW_DUMP:
        handle_window_dump(g, vm_window, untrusted_hdr.untrusted_len);
        break;
    case MSG_CURSOR:
        handle_cursor(g, vm_window);
        break;
    default:
        fprintf(stderr, "got unknown msg type %d\n", type);
        exit(1);
    }
}

/* signal handler - connected to SIGTERM */
static void dummy_signal_handler(int UNUSED(x))
{
    /* The exit(0) below will call release_all_mapped_mfns (registerd with
     * atexit(3)), which would try to release window images with XShmDetach. We
     * can't send X11 requests if one is currently being handled. Since signals
     * are asynchronous, we don't know that. Clean window images
     * without calling to X11. And hope that X server will call XShmDetach
     * internally when cleaning windows of disconnected client */
    release_all_shm_no_x11_calls();
    exit(0);
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

/* release all windows mapped memory */
static void release_all_mapped_mfns(void)
{
    struct genlist *curr;
    if (ghandles.log_level > 1)
        fprintf(stderr, "release_all_mapped_mfns running\n");
    print_backtrace();
    for (curr = ghandles.wid2windowdata->next;
         curr != ghandles.wid2windowdata; curr = curr->next) {
        struct windowdata *vm_window = curr->data;
        if (vm_window->image)
            /* use ghandles directly, as no other way get it (atexit cannot
             * pass argument) */
            release_mapped_mfns(&ghandles, vm_window);
    }
}

static void send_xconf(Ghandles * g)
{
    struct msg_xconf xconf;
    XWindowAttributes attr;
    XGetWindowAttributes(g->display, g->root_win, &attr);
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
    uint32_t version_major, version_minor;
    read_struct(g->vchan, untrusted_version);
    version_major = untrusted_version >> 16;
    version_minor = untrusted_version & 0xffff;

    if (version_major == PROTOCOL_VERSION_MAJOR &&
            version_minor <= PROTOCOL_VERSION_MINOR) {
        /* agent is compatible */
        g->agent_version = version_major << 16 | version_minor;
        return;
    }
    if (version_major < PROTOCOL_VERSION_MAJOR)
        /* agent is too old */
        snprintf(message, sizeof message, "%s %s \""
                "The GUI agent that runs in the VM '%s' implements outdated protocol (%d:%d), and must be updated.\n\n"
                "To start and access the VM or template without GUI virtualization, use the following commands:\n"
                "qvm-start --no-guid vmname\n"
                "qvm-console-dispvm vmname\"",
                g->use_kdialog ? KDIALOG_PATH : ZENITY_PATH,
                g->use_kdialog ? "--sorry" : "--error --text ",
                g->vmname, version_major, version_minor);
    else
        /* agent is too new */
        snprintf(message, sizeof message, "%s %s \""
                "The Dom0 GUI daemon do not support protocol version %d:%d, requested by the VM '%s'.\n"
                "To update Dom0, use 'qubes-dom0-update' command or do it via qubes-manager\"",
                g->use_kdialog ? KDIALOG_PATH : ZENITY_PATH,
                g->use_kdialog ? "--sorry" : "--error --text ",
                version_major, version_minor, g->vmname);
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
    { "help", no_argument, NULL, 'h' },
    { 0, 0, 0, 0 },
};
static const char optstring[] = "C:d:t:N:c:l:i:K:vqQnafIp:Th";

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
    fprintf(stream, " --color=COLOR, -c COLOR\tVM color (in format 0xRRGGBB)\n");
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

static _Bool parse_vm_name(const char *arg, Ghandles *g) {
    if (('a' > *arg || *arg > 'z') &&
        ('A' > *arg || *arg > 'Z'))
        return false;
    for (size_t i = 1; i < sizeof(g->vmname); ++i) {
        switch (arg[i]) {
        case 'A'...'Z':
        case 'a'...'z':
        case '0'...'9':
        case '-':
        case '_':
            continue;
        case '\0':
            memcpy(g->vmname, arg, i + 1);
            return true;
        default:
            return false;
        }
    }
    return false;
}

static void parse_cmdline(Ghandles * g, int argc, char **argv)
{
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

    while ((opt = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {
        switch (opt) {
        case 'C':
            /* already handled in parse_cmdline_config_path */
            break;
        case 'd':
            g->domid = atoi(optarg);
            break;
        case 't':
            g->target_domid = atoi(optarg);
            break;
        case 'N':
            if (parse_vm_name(optarg, g))
                break;
            fprintf(stderr, "domain name not valid");
            exit(1);
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
        default:
            usage(stderr);
            exit(1);
        }
    }
    if (g->domid<=0) {
        fprintf(stderr, "domid<=0?");
        exit(1);
    }

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

    if (screensaver_name_num == 0) {
        g->screensaver_names[0] = "xscreensaver";
    }
}

static void load_default_config_values(Ghandles * g)
{
    strcpy(g->config_path, GUID_CONFIG_FILE);
    g->allow_utf8_titles = 0;
    g->copy_seq_mask = ControlMask | ShiftMask;
    g->copy_seq_key = XK_c;
    g->paste_seq_mask = ControlMask | ShiftMask;
    g->paste_seq_key = XK_v;
    g->allow_fullscreen = 0;
    g->override_redirect_protection = 1;
    g->startup_timeout = 45;
    g->trayicon_mode = TRAY_TINT;
    g->trayicon_border = 0;
    g->trayicon_tint_reduce_saturation = 0;
    g->trayicon_tint_whitehack = 0;
}

// parse string describing key sequence like Ctrl-Alt-c
static void parse_key_sequence(const char *seq, int *mask, KeySym * key)
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
            "Warning: key sequence (%s) is invalid (will be disabled)\n",
            seq);
    }
}

static void parse_vm_config(Ghandles * g, config_setting_t * group)
{
    config_setting_t *setting;

    if ((setting =
         config_setting_get_member(group, "secure_copy_sequence"))) {
        parse_key_sequence(config_setting_get_string(setting),
                   &g->copy_seq_mask, &g->copy_seq_key);
    }
    if ((setting =
         config_setting_get_member(group, "secure_paste_sequence"))) {
        parse_key_sequence(config_setting_get_string(setting),
                   &g->paste_seq_mask, &g->paste_seq_key);
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
            fprintf(stderr, "unsupported value ‘%s’ for override_redirect\n", value);
            exit(1);
        }
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

/* helper to get a file flag path */
static char *guid_fs_flag(const char *type, int domid)
{
    static char buf[256];
    snprintf(buf, sizeof(buf), "/run/qubes/guid-%s.%d",
         type, domid);
    return buf;
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

/* remove guid_running file at exit */
static void unset_alive_flag(void)
{
    unlink(guid_fs_flag("running", ghandles.domid));
}

void vchan_close()
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
    release_all_mapped_mfns();
    XCloseDisplay(ghandles.display);
    unset_alive_flag();
    close(ghandles.inter_appviewer_lock_fd);
}

static char** restart_argv;
void restart_guid() {
    cleanup();
    execv("/usr/bin/qubes-guid", restart_argv);
    perror("execv");
}

int main(int argc, char **argv)
{
    int xfd;
    FILE *f;
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

    if (!ghandles.nofork) {
        // daemonize...
        if (pipe(pipe_notify) < 0) {
            perror("canot create pipe:");
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
        snprintf(shmid_filename, SHMID_FILENAME_LEN,
             SHMID_FILENAME_PREFIX "%d", display_num);
        f = fopen(shmid_filename, "r");
        if (!f) {
            fprintf(stderr,
                    "Missing %s; run X with preloaded shmoverride\n",
                    shmid_filename);
            exit(1);
        }
        if (fscanf(f, "%d", &ghandles.cmd_shmid) < 1) {
            fprintf(stderr,
                    "Failed to load %s; run X with preloaded shmoverride\n",
                    shmid_filename);
            exit(1);
        }
        fclose(f);
        ghandles.shm_args = shmat(ghandles.cmd_shmid, NULL, 0);
        if (ghandles.shm_args == (void *) (-1UL)) {
            fprintf(stderr,
                    "Invalid or stale shm id 0x%x in %s\n",
                    ghandles.cmd_shmid,
                    shmid_filename);
            exit(1);
        }
    }

    /* prepare argv for possible restarts */
    if (ghandles.nofork) {
        /* "-f" option already given, use the same argv */
        restart_argv = argv;
    } else {
        /* append "-f" option */
        int i;

        restart_argv = malloc((argc+2) * sizeof(char*));
        for (i=0;i<argc;i++)
            restart_argv[i] = argv[i];
        restart_argv[argc] = strdup("-f");
        restart_argv[argc+1] = (char*)NULL;
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
    atexit(release_all_mapped_mfns);

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
        int select_fds[2] = { xfd };
        fd_set retset;
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
            if (libvchan_data_ready(ghandles.vchan)) {
                handle_message(&ghandles);
                busy = 1;
            }
        } while (busy);
        wait_for_vchan_or_argfd(ghandles.vchan, 1, select_fds, &retset);
    }
    return 0;
}
