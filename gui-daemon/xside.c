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

/* high level documentation is here:
 * http://wiki.qubes-os.org/trac/wiki/GUIdocs
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <execinfo.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/Xatom.h>
#include <libconfig.h>
#include "messages.h"
#include "txrx.h"
#include "list.h"
#include "error.h"
#include "shm_cmd.h"
#include "qlimits.h"
#include "tray.h"

/* some configuration */

/* default width of forced colorful border */
#define BORDER_WIDTH 2
#define QUBES_CLIPBOARD_FILENAME "/var/run/qubes/qubes_clipboard.bin"
#define GUID_CONFIG_FILE "/etc/qubes/guid.conf"
#define GUID_CONFIG_DIR "/etc/qubes"

#define SPECAL_KEYS_MASK (Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask | ShiftMask | ControlMask )

/* per-window data */
struct windowdata {
	int width;
	int height;
	int x;
	int y;
	int is_mapped;
	int is_docked;		/* is it docked tray icon */
	XID remote_winid;	/* window id on VM side */
	Window local_winid;	/* window id on X side */
	struct windowdata *parent;	/* parent window */
	struct windowdata *transient_for;	/* transient_for hint for WM, see http://tronche.com/gui/x/icccm/sec-4.html#WM_TRANSIENT_FOR */
	int override_redirect;	/* see http://tronche.com/gui/x/xlib/window/attributes/override-redirect.html */
	XShmSegmentInfo shminfo;	/* temporary shmid; see shmoverride/README */
	XImage *image;		/* image with window content */
	int image_height;	/* size of window content, not always the same as window in dom0! */
	int image_width;
	int have_queued_configure;	/* have configure request been sent to VM - waiting for confirmation */
};

/* global variables
 * keep them in this struct for readability
 */
struct _global_handles {
	/* local X server handles and attributes */
	Display *display;
	int screen;		/* shortcut to the default screen */
	Window root_win;	/* root attributes */
	int root_width;		/* size of root window */
	int root_height;
	GC context;		/* context for pixmap operations */
	GC frame_gc;		/* graphic context to paint window frame */
	GC tray_gc;		/* graphic context to paint tray background */
	/* atoms for comunitating with xserver */
	Atom wmDeleteMessage;	/* Atom: WM_DELETE_WINDOW */
	Atom tray_selection;	/* Atom: _NET_SYSTEM_TRAY_SELECTION_S<creen number> */
	Atom tray_opcode;	/* Atom: _NET_SYSTEM_TRAY_MESSAGE_OPCODE */
	Atom xembed_message;	/* Atom: _XEMBED */
	Atom xembed_info;	/* Atom: _XEMBED_INFO */
	/* shared memory handling */
	struct shm_cmd *shmcmd;	/* shared memory with Xorg */
	int cmd_shmid;		/* shared memory id - received from shmoverride.so through shm.id file */
	/* Client VM parameters */
	char vmname[32];	/* name of VM */
	int domid;		/* Xen domain id */
	char *cmdline_color;	/* color of frame */
	char *cmdline_icon;	/* icon hint for WM */
	int label_index;	/* label (frame color) hint for WM */
	/* lists of windows: */
	/*   indexed by remote window id */
	struct genlist *remote2local;
	/*   indexed by local window id */
	struct genlist *wid2windowdata;
	/* counters and other state */
	int clipboard_requested;	/* if clippoard content was requested by dom0 */
	int windows_count;	/* created window count */
	int windows_count_limit;	/* current window limit; ask user what to do when exceeded */
	int windows_count_limit_param; /* initial limit of created windows - after exceed, warning the user */
	struct windowdata *last_input_window;
	/* signal was caught */
	int volatile signal_caught;
	pid_t pulseaudio_pid;
	/* configuration */
	int log_level;		/* log level */
	int allow_utf8_titles;	/* allow UTF-8 chars in window title */
	int copy_seq_mask;	/* modifiers mask for secure-copy key sequence */
	KeySym copy_seq_key;	/* key for secure-copy key sequence */
	int paste_seq_mask;	/* modifiers mask for secure-paste key sequence */
	KeySym paste_seq_key;	/* key for secure-paste key sequence */
};

typedef struct _global_handles Ghandles;
Ghandles ghandles;

/* macro used to verify data from VM */
#define VERIFY(x) if (!(x)) { \
		if (ask_whether_verify_failed(g, __STRING(x))) \
			return; \
	}

/* calculate virtual width */
#define XORG_DEFAULT_XINC 8
#define _VIRTUALX(x) ( (((x)+XORG_DEFAULT_XINC-1)/XORG_DEFAULT_XINC)*XORG_DEFAULT_XINC )

/* short macro for beginning of each xevent handling function
 * checks if this window is managed by guid and declares windowdata struct
 * pointer */
#define CHECK_NONMANAGED_WINDOW(g, id) struct windowdata *vm_window; \
	if (!(vm_window=check_nonmanged_window(g, id))) return

#ifndef min
#define min(x,y) ((x)>(y)?(y):(x))
#endif
#ifndef max
#define max(x,y) ((x)<(y)?(y):(x))
#endif

void inter_appviewer_lock(int mode);
void release_mapped_mfns(Ghandles * g, struct windowdata *vm_window);

/* ask user when VM sent invalid message */
int ask_whether_verify_failed(Ghandles * g, const char *cond)
{
	char text[1024];
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
			"information). <br/><br/>"
			"Click \"Terminate\" to terminate this domain immediately, or "
			"\"Ignore\" to ignore this condition check and allow the GUI request "
			"to proceed.",
		 g->vmname);
#else
	snprintf(text, sizeof(text),
			"The domain %s attempted to perform an invalid or suspicious GUI "
			"request. This might be a sign that the domain has been compromised "
			"and is attempting to compromise the GUI daemon (Dom0 domain). In "
			"rare cases, however, it might be possible that a legitimate "
			"application trigger such condition (check the guid logs for more "
			"information). <br/><br/>"
			"Do you allow this VM to continue running?",
		 g->vmname);
#endif

	pid = fork();
	switch (pid) {
		case 0:
#ifdef NEW_KDIALOG
			execlp("kdialog", "kdialog", "--no-label", "Terminate", "--yes-label", "Ignore", "--warningyesno", text, (char*)NULL);
#else
			execlp("kdialog", "kdialog", "--warningyesno", text, (char*)NULL);
#endif
		case -1:
			perror("fork");
			exit(1);
		default:
			waitpid(pid, &ret, 0);
			ret = WEXITSTATUS(ret);
	}
	switch (ret) {
//	case 2:	/*cancel */
//		break;
	case 0:	/* YES */
		return 0;
	case 1:	/* NO */
		execl("/usr/sbin/xl", "xl", "destroy", g->vmname, (char*)NULL);
		perror("Problems executing xl");
		exit(1);
	default:
		fprintf(stderr, "Problems executing kdialog ?\n");
		exit(1);
	}
	/* should never happend */
	return 1;
}

/* prepare graphic context for painting colorful frame */
void get_frame_gc(Ghandles * g, char *name)
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
	values.foreground = fcolor.pixel;
	g->frame_gc =
	    XCreateGC(g->display, g->root_win, GCForeground, &values);
}

/* prepare graphic context for tray background */
void get_tray_gc(Ghandles * g)
{
	XGCValues values;
	values.foreground = WhitePixel(g->display, g->screen);
	g->tray_gc =
	    XCreateGC(g->display, g->root_win, GCForeground, &values);
}


/* create local window - on VM request.
 * parameters are sanitized already
 */
Window mkwindow(Ghandles * g, struct windowdata *vm_window)
{
	char *gargv[1] = { NULL };
	Window child_win;
	Window parent;
	XSizeHints my_size_hints;	/* hints for the window manager */
	Atom atom_label;

	my_size_hints.flags = PSize;
	my_size_hints.height = vm_window->width;
	my_size_hints.width = vm_window->height;

	if (vm_window->parent)
		parent = vm_window->parent->local_winid;
	else
		parent = g->root_win;
	// we will set override_redirect later, if needed
	child_win = XCreateSimpleWindow(g->display, parent,
					vm_window->x, vm_window->y,
					vm_window->width,
					vm_window->height, 0,
					BlackPixel(g->display, g->screen),
					WhitePixel(g->display, g->screen));
	/* pass my size hints to the window manager, along with window
	   and icon names */
	(void) XSetStandardProperties(g->display, child_win,
				      "VMapp command", "Pixmap", None,
				      gargv, 0, &my_size_hints);
	(void) XSelectInput(g->display, child_win,
			    ExposureMask | KeyPressMask | KeyReleaseMask |
			    ButtonPressMask | ButtonReleaseMask |
			    PointerMotionMask | EnterWindowMask | LeaveWindowMask |
			    FocusChangeMask | StructureNotifyMask);
	XSetWMProtocols(g->display, child_win, &g->wmDeleteMessage, 1);
	if (g->cmdline_icon) {
		XClassHint class_hint =
		    { g->cmdline_icon, g->cmdline_icon };
		XSetClassHint(g->display, child_win, &class_hint);
	}
	// Set '_QUBES_LABEL' property so that Window Manager can read it and draw proper decoration
	atom_label = XInternAtom(g->display, "_QUBES_LABEL", 0);
	XChangeProperty(g->display, child_win, atom_label, XA_CARDINAL,
			8 /* 8 bit is enough */ , PropModeReplace,
			(unsigned char *) &g->label_index, 1);

	// Set '_QUBES_VMNAME' property so that Window Manager can read it and nicely display it
	atom_label = XInternAtom(g->display, "_QUBES_VMNAME", 0);
	XChangeProperty(g->display, child_win, atom_label, XA_STRING,
			8 /* 8 bit is enough */ , PropModeReplace,
			(const unsigned char *) g->vmname,
			strlen(g->vmname));


	return child_win;
}

/* prepare global variables content:
 * most of them are handles to local Xserver structures */
void mkghandles(Ghandles * g)
{
	char tray_sel_atom_name[64];
	XWindowAttributes attr;
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
	g->wmDeleteMessage =
	    XInternAtom(g->display, "WM_DELETE_WINDOW", True);
	g->clipboard_requested = 0;
	snprintf(tray_sel_atom_name, sizeof(tray_sel_atom_name),
		 "_NET_SYSTEM_TRAY_S%u", DefaultScreen(g->display));
	g->tray_selection =
	    XInternAtom(g->display, tray_sel_atom_name, False);
	g->tray_opcode =
	    XInternAtom(g->display, "_NET_SYSTEM_TRAY_OPCODE", False);
	g->xembed_message = XInternAtom(g->display, "_XEMBED", False);
	g->xembed_info = XInternAtom(g->display, "_XEMBED_INFO", False);
	/* create graphical contexts */
	get_frame_gc(g, g->cmdline_color ? : "red");
	get_tray_gc(g);
	/* initialize windows limit */
	g->windows_count_limit = g->windows_count_limit_param;
	/* init window lists */
	g->remote2local = list_new();
	g->wid2windowdata = list_new();
}

/* find if window (given by id) is managed by this guid */
struct windowdata *check_nonmanged_window(Ghandles * g, XID id)
{
	struct genlist *item = list_lookup(g->wid2windowdata, id);
	if (!item) {
		fprintf(stderr, "cannot lookup 0x%x in wid2windowdata\n",
			(int) id);
		return NULL;
	}
	return item->data;
}

/* fetch clippboard content from file */
void get_qubes_clipboard(char **data, int *len)
{
	FILE *file;
	*len = 0;
	inter_appviewer_lock(1);
	file = fopen(QUBES_CLIPBOARD_FILENAME, "r");
	if (!file)
		goto out;
	fseek(file, 0, SEEK_END);
	*len = ftell(file);
	*data = malloc(*len);
	if (!*data) {
		perror("malloc");
		exit(1);
	}
	fseek(file, 0, SEEK_SET);
	fread(*data, *len, 1, file);
	fclose(file);
	truncate(QUBES_CLIPBOARD_FILENAME, 0);
	file = fopen(QUBES_CLIPBOARD_FILENAME ".source", "w+");
	fclose(file);
      out:
	inter_appviewer_lock(0);
}

/* handle VM message: MSG_CLIPBOARD_DATA
 *  - checks if clipboard data was requested
 *  - store it in file
 */
void handle_clipboard_data(Ghandles * g, unsigned int untrusted_len)
{
	FILE *file;
	char *untrusted_data;
	size_t untrusted_data_sz;
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
	read_data(untrusted_data, untrusted_data_sz);
	if (!g->clipboard_requested) {
		free(untrusted_data);
		fprintf(stderr,
			"received clipboard data when not requested\n");
		return;
	}
	inter_appviewer_lock(1);
	file = fopen(QUBES_CLIPBOARD_FILENAME, "w");
	if (!file) {
		perror("open " QUBES_CLIPBOARD_FILENAME);
		exit(1);
	}
	fwrite(untrusted_data, untrusted_data_sz, 1, file);
	fclose(file);
	file = fopen(QUBES_CLIPBOARD_FILENAME ".source", "w");
	if (!file) {
		perror("open " QUBES_CLIPBOARD_FILENAME ".source");
		exit(1);
	}
	fwrite(g->vmname, strlen(g->vmname), 1, file);
	fclose(file);
	inter_appviewer_lock(0);
	g->clipboard_requested = 0;
	free(untrusted_data);
}

/* check and handle guid-special keys
 * currently only for inter-vm clipboard copy
 */
int is_special_keypress(Ghandles * g, XKeyEvent * ev, XID remote_winid)
{
	struct msghdr hdr;
	char *data;
	int len;
	if ((ev->state & SPECAL_KEYS_MASK) ==
	    g->copy_seq_mask
	    && ev->keycode == XKeysymToKeycode(g->display,
					       g->copy_seq_key)) {
		if (ev->type != KeyPress)
			return 1;
		g->clipboard_requested = 1;
		hdr.type = MSG_CLIPBOARD_REQ;
		hdr.window = remote_winid;
		if (g->log_level > 0)
			fprintf(stderr, "secure copy\n");
		write_struct(hdr);
		return 1;
	}
	if ((ev->state & SPECAL_KEYS_MASK) ==
	    g->paste_seq_mask
	    && ev->keycode == XKeysymToKeycode(g->display,
					       g->paste_seq_key)) {
		if (ev->type != KeyPress)
			return 1;
		hdr.type = MSG_CLIPBOARD_DATA;
		if (g->log_level > 0)
			fprintf(stderr, "secure paste\n");
		get_qubes_clipboard(&data, &len);
		if (len > 0) {
			hdr.window = len;
			real_write_message((char *) &hdr, sizeof(hdr),
					   data, len);
			free(data);
		}

		return 1;
	}
	return 0;
}

/* handle local Xserver event: XKeyEvent
 * send it to relevant window in VM
 */
void process_xevent_keypress(Ghandles * g, XKeyEvent * ev)
{
	struct msghdr hdr;
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
	write_message(hdr, k);
//      fprintf(stderr, "win 0x%x(0x%x) type=%d keycode=%d\n",
//              (int) ev->window, hdr.window, k.type, k.keycode);
}

// debug routine
void dump_mapped(Ghandles * g)
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

/* handle local Xserver event: XButtonEvent
 * same as XKeyEvent - send to relevant window in VM */
void process_xevent_button(Ghandles * g, XButtonEvent * ev)
{
	struct msghdr hdr;
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
	write_message(hdr, k);
	if (g->log_level > 1)
		fprintf(stderr,
			"xside: win 0x%x(0x%x) type=%d button=%d x=%d, y=%d\n",
			(int) ev->window, hdr.window, k.type, k.button,
			k.x, k.y);
}

/* handle local Xserver event: XCloseEvent
 * send to relevant window in VM */
void process_xevent_close(Ghandles * g, XID window)
{
	struct msghdr hdr;
	CHECK_NONMANAGED_WINDOW(g, window);
	hdr.type = MSG_CLOSE;
	hdr.window = vm_window->remote_winid;
	write_struct(hdr);
}

/* send configure request for specified VM window */
void send_configure(struct windowdata *vm_window, int x, int y, int w,
		    int h)
{
	struct msghdr hdr;
	struct msg_configure msg;
	hdr.type = MSG_CONFIGURE;
	hdr.window = vm_window->remote_winid;
	msg.height = h;
	msg.width = w;
	msg.x = x;
	msg.y = y;
	write_message(hdr, msg);
}

/* fix position of docked tray icon;
 * icon position is relative to embedder 0,0 so we must translate it to
 * absolute position */
int fix_docked_xy(Ghandles * g, struct windowdata *vm_window, char *caller)
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
void moveresize_vm_window(Ghandles * g, struct windowdata *vm_window)
{
	int x = 0, y = 0;
	Window win;
	if (!vm_window->is_docked) {
		x = vm_window->x;
		y = vm_window->y;
	} else
		XTranslateCoordinates(g->display, g->root_win,
				      vm_window->local_winid, vm_window->x,
				      vm_window->y, &x, &y, &win);
	if (g->log_level > 1)
		fprintf(stderr,
			"XMoveResizeWindow local 0x%x remote 0x%x, xy %d %d (vm_window is %d %d) wh %d %d\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid, x, y, vm_window->x,
			vm_window->y, vm_window->width, vm_window->height);
	XMoveResizeWindow(g->display, vm_window->local_winid, x, y,
			  vm_window->width, vm_window->height);
}


/* force window to not hide it's frame
 * checks if at least border_width is from every screen edge (and fix if no)
 * Exception: allow window to be entriely off the screen */
int force_on_screen(Ghandles * g, struct windowdata *vm_window,
		    int border_width, char *caller)
{
	int do_move = 0, reason = -1;
	int x = vm_window->x, y = vm_window->y, w = vm_window->width, h =
	    vm_window->height;

	if (vm_window->x < border_width
	    && vm_window->x + vm_window->width > 0) {
		vm_window->x = border_width;
		do_move = 1;
		reason = 1;
	}
	if (vm_window->y < border_width
	    && vm_window->y + vm_window->height > 0) {
		vm_window->y = border_width;
		do_move = 1;
		reason = 2;
	}
	if (vm_window->x < g->root_width &&
	    vm_window->x + vm_window->width >
	    g->root_width - border_width) {
		vm_window->width =
		    g->root_width - vm_window->x - border_width;
		do_move = 1;
		reason = 3;
	}
	if (vm_window->y < g->root_height &&
	    vm_window->y + vm_window->height >
	    g->root_height - border_width) {
		vm_window->height =
		    g->root_height - vm_window->y - border_width;
		do_move = 1;
		reason = 4;
	}
	if (do_move)
		if (g->log_level > 0)
			fprintf(stderr,
				"force_on_screen(from %s) returns 1 (reason %d): window 0x%x, xy %d %d, wh %d %d, root %d %d borderwidth %d\n",
				caller, reason,
				(int) vm_window->local_winid, x, y, w, h,
				g->root_width, g->root_height,
				border_width);
	return do_move;
}

/* handle local Xserver event: XConfigureEvent
 * after some checks/fixes send to relevant window in VM */
void process_xevent_configure(Ghandles * g, XConfigureEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (g->log_level > 1)
		fprintf(stderr,
			"process_xevent_configure local 0x%x remote 0x%x, %d/%d, was "
			"%d/%d, xy %d/%d was %d/%d\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid, ev->width,
			ev->height, vm_window->width, vm_window->height,
			ev->x, ev->y, vm_window->x, vm_window->y);
	if (vm_window->width == ev->width
	    && vm_window->height == ev->height && vm_window->x == ev->x
	    && vm_window->y == ev->y)
		return;
	vm_window->width = ev->width;
	vm_window->height = ev->height;
	if (!vm_window->is_docked) {
		vm_window->x = ev->x;
		vm_window->y = ev->y;
	} else
		fix_docked_xy(g, vm_window, "process_xevent_configure");

// if AppVM has not unacknowledged previous resize msg, do not send another one
	if (vm_window->have_queued_configure)
		return;
	vm_window->have_queued_configure = 1;
	send_configure(vm_window, vm_window->x, vm_window->y,
		       vm_window->width, vm_window->height);
}

/* handle VM message: MSG_CONFIGURE
 * check if we like new dimensions/position and move relevant window */
void handle_configure_from_vm(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_configure untrusted_conf;
	unsigned int x, y, width, height, override_redirect;
	int conf_changed;

	read_struct(untrusted_conf);
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
	VERIFY(width >= 0 && height >= 0);
	if (untrusted_conf.override_redirect > 0)
		override_redirect = 1;
	else
		override_redirect = 0;
	/* there is no really good limits for x/y, so pass them to Xorg and hope
	 * that everything will be ok... */
	x = untrusted_conf.x;
	y = untrusted_conf.y;
	/* sanitize end */
	if (vm_window->width != width || vm_window->height != height ||
	    vm_window->x != x || vm_window->y != y)
		conf_changed = 1;
	else
		conf_changed = 0;
	vm_window->override_redirect = override_redirect;

	/* We do not allow a docked window to change its size, period. */
	if (vm_window->is_docked) {
		if (conf_changed)
			send_configure(vm_window, vm_window->x,
				       vm_window->y, vm_window->width,
				       vm_window->height);
		vm_window->have_queued_configure = 0;
		return;
	}


	if (vm_window->have_queued_configure) {
		if (conf_changed) {
			send_configure(vm_window, vm_window->x,
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
void process_xevent_crossing(Ghandles * g, XCrossingEvent * ev)
{
	struct msghdr hdr;
	struct msg_crossing k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);

	if (ev->type == EnterNotify) {
		char keys[32];
		XQueryKeymap(g->display, keys);
		hdr.type = MSG_KEYMAP_NOTIFY;
		hdr.window = 0;
		write_message(hdr, keys);
	}
	/* move tray to correct position in VM */
	if (fix_docked_xy(g, vm_window, "process_xevent_crossing")) {
		send_configure(vm_window, vm_window->x, vm_window->y,
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
	write_message(hdr, k);
}

/* handle local Xserver event: XMotionEvent
 * send to relevant window in VM */
void process_xevent_motion(Ghandles * g, XMotionEvent * ev)
{
	struct msghdr hdr;
	struct msg_motion k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);

	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.is_hint = ev->is_hint;
	hdr.type = MSG_MOTION;
	hdr.window = vm_window->remote_winid;
	write_message(hdr, k);
//      fprintf(stderr, "motion in 0x%x", ev->window);
}

/* handle local Xserver event: FocusIn, FocusOut
 * send to relevant window in VM */
void process_xevent_focus(Ghandles * g, XFocusChangeEvent * ev)
{
	struct msghdr hdr;
	struct msg_focus k;
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (ev->type == FocusIn) {
		char keys[32];
		XQueryKeymap(g->display, keys);
		hdr.type = MSG_KEYMAP_NOTIFY;
		hdr.window = 0;
		write_message(hdr, keys);
	}
	hdr.type = MSG_FOCUS;
	hdr.window = vm_window->remote_winid;
	k.type = ev->type;
	k.mode = ev->mode;
	k.detail = ev->detail;
	write_message(hdr, k);
}

/* update given fragment of window image
 * can be requested by VM (MSG_SHMIMAGE) and Xserver (XExposeEvent)
 * parameters are not sanitized earlier - we must check it carefully
 * also do not let to cover forced colorful frame (for undecoraded windows)
 */
void do_shm_update(Ghandles * g, struct windowdata *vm_window,
		   int untrusted_x, int untrusted_y, int untrusted_w,
		   int untrusted_h)
{
	int border_width = BORDER_WIDTH;
	int x, y, w, h;

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
	x = min(untrusted_x, vm_window->image_width);
	y = min(untrusted_y, vm_window->image_height);
	w = min(max(untrusted_w, 0), vm_window->image_width - x);
	h = min(max(untrusted_h, 0), vm_window->image_height - y);
	/* sanitize end */

	if (!vm_window->override_redirect) {
		// Window Manager will take care of the frame...
		border_width = 0;
	}

	int do_border = 0;
	int hoff = 0, woff = 0, delta, i;
	if (!vm_window->image)
		return;
	/* when image larger than local window - place middle part of image in the
	 * window */
	if (vm_window->image_height > vm_window->height)
		hoff = (vm_window->image_height - vm_window->height) / 2;
	if (vm_window->image_width > vm_window->width)
		woff = (vm_window->image_width - vm_window->width) / 2;
	/* window contains only (forced) frame, so no content to update */
	if (vm_window->width < border_width * 2
	    || vm_window->height < border_width * 2)
		return;
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

	if (vm_window->is_docked) {
		char *data, *datap;
		size_t data_sz;
		int xp, yp;

		/* allocate image_width _bits_ for each image line */
		data_sz =
		    (vm_window->image_width / 8 +
		     1) * vm_window->image_height;
		data = datap = calloc(1, data_sz);
		if (!data) {
			perror("malloc");
			exit(1);
		}

		/* Create local pixmap, put vmside image to it
		 * then get local image of the copy.
		 * This is needed because XGetPixel does not seem to work
		 * with XShmImage data.
		 *
		 * Always use 0,0 w+x,h+y coordinates to generate proper mask. */
		w = w + x + woff;
		h = h + y + hoff;
		if (w > vm_window->image_width)
			w = vm_window->image_width;
		if (h > vm_window->image_height)
			h = vm_window->image_height;
		Pixmap pixmap =
		    XCreatePixmap(g->display, vm_window->local_winid,
				  vm_window->image_width,
				  vm_window->image_height,
				  24);
		XShmPutImage(g->display, pixmap, g->context,
			     vm_window->image, 0, 0, 0, 0,
			     vm_window->image_width,
			     vm_window->image_height, 0);
		XImage *image = XGetImage(g->display, pixmap, 0, 0, w, h,
					  0xFFFFFFFF, ZPixmap);
		/* Use top-left corner pixel color as transparency color */
		unsigned long back = XGetPixel(image, 0, 0);
		/* Generate data for transparency mask Bitmap */
		for (yp = 0; yp < h; yp++) {
			int step = 0;
			for (xp = 0; xp < w; xp++) {
				if (datap - data >= data_sz) {
					fprintf(stderr,
						"Impossible internal error\n");
					exit(1);
				}
				if (XGetPixel(image, xp, yp) != back)
					*datap |= 1 << (step % 8);
				if (step % 8 == 7)
					datap++;
				step++;
			}
			/* ensure that new line will start at new byte */
			if ((step - 1) % 8 != 7)
				datap++;
		}
		Pixmap mask = XCreateBitmapFromData(g->display,
						    vm_window->local_winid,
						    data, w, h);
		/* set trayicon background to white color */
		XFillRectangle(g->display, vm_window->local_winid,
			       g->tray_gc, 0, 0, vm_window->width,
			       vm_window->height);
		/* Paint clipped Image */
		XSetClipMask(g->display, g->context, mask);
		XPutImage(g->display, vm_window->local_winid,
			  g->context, image, 0, 0, 0, 0, w, h);
		/* Remove clipping */
		XSetClipMask(g->display, g->context, None);
		/* Draw VM color frame in case VM tries to cheat
		 * and puts its own background color */
		XDrawRectangle(g->display, vm_window->local_winid,
			       g->frame_gc, 0, 0,
			       vm_window->width - 1,
			       vm_window->height - 1);

		XFreePixmap(g->display, mask);
		XDestroyImage(image);
		XFreePixmap(g->display, pixmap);
		free(data);
		return;
	} else
		XShmPutImage(g->display, vm_window->local_winid,
			     g->context, vm_window->image, x + woff,
			     y + hoff, x, y, w, h, 0);
	if (!do_border)
		return;
	for (i = 0; i < border_width; i++)
		XDrawRectangle(g->display, vm_window->local_winid,
			       g->frame_gc, i, i,
			       vm_window->width - 1 - 2 * i,
			       vm_window->height - 1 - 2 * i);

}

/* handle local Xserver event: XExposeEvent
 * update relevant part of window using stored image
 */
void process_xevent_expose(Ghandles * g, XExposeEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	do_shm_update(g, vm_window, ev->x, ev->y, ev->width, ev->height);
}

/* handle local Xserver event: XMapEvent
 * after some checks, send to relevant window in VM */
void process_xevent_mapnotify(Ghandles * g, XMapEvent * ev)
{
	XWindowAttributes attr;
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (vm_window->is_mapped)
		return;
	XGetWindowAttributes(g->display, vm_window->local_winid, &attr);
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
		struct msghdr hdr;
		struct msg_map_info map_info;
		map_info.override_redirect = attr.override_redirect;
		hdr.type = MSG_MAP;
		hdr.window = vm_window->remote_winid;
		write_struct(hdr);
		write_struct(map_info);
		if (vm_window->is_docked
		    && fix_docked_xy(g, vm_window,
				     "process_xevent_mapnotify"))
			send_configure(vm_window, vm_window->x,
				       vm_window->y, vm_window->width,
				       vm_window->height);
	}
}

/* handle local Xserver event: _XEMBED
 * if window isn't mapped already - map it now */
void process_xevent_xembed(Ghandles * g, XClientMessageEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(g, ev->window);
	if (g->log_level > 1)
		fprintf(stderr, "_XEMBED message %ld\n", ev->data.l[1]);
	if (ev->data.l[1] == XEMBED_EMBEDDED_NOTIFY) {
		if (vm_window->is_docked < 2) {
			vm_window->is_docked = 2;
			if (!vm_window->is_mapped)
				XMapWindow(g->display, ev->window);
			/* move tray to correct position in VM */
			if (fix_docked_xy
			    (g, vm_window, "process_xevent_xembed")) {
				send_configure(vm_window, vm_window->x,
					       vm_window->y,
					       vm_window->width,
					       vm_window->height);
			}
		}
	}
}

/* dispath local Xserver event */
void process_xevent(Ghandles * g)
{
	XEvent event_buffer;
	XNextEvent(g->display, &event_buffer);
	switch (event_buffer.type) {
	case KeyPress:
	case KeyRelease:
		process_xevent_keypress(g, (XKeyEvent *) & event_buffer);
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
	case ClientMessage:
//              fprintf(stderr, "xclient, atom=%s\n",
//                      XGetAtomName(g->display,
//                                   event_buffer.xclient.message_type));
		if (event_buffer.xclient.message_type == g->xembed_message) {
			process_xevent_xembed(g, (XClientMessageEvent *) &
					      event_buffer);
		} else if (event_buffer.xclient.data.l[0] ==
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
void handle_shmimage(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_shmimage untrusted_mx;

	read_struct(untrusted_mx);
	if (!vm_window->is_mapped)
		return;
	/* WARNING: passing raw values, input validation is done inside of
	 * do_shm_update */
	do_shm_update(g, vm_window, untrusted_mx.x, untrusted_mx.y,
		      untrusted_mx.width, untrusted_mx.height);
}

/* ask user when VM creates to many windows */
void ask_whether_flooding(Ghandles * g)
{
	char text[1024];
	int ret;
	snprintf(text, sizeof(text),
		 "kdialog --yesnocancel "
		 "'VMapp \"%s\" has created %d windows; it looks numerous, "
		 "so it may be "
		 "a beginning of a DoS attack. Do you want to continue:'",
		 g->vmname, g->windows_count);
	do {
		ret = system(text);
		ret = WEXITSTATUS(ret);
//              fprintf(stderr, "ret=%d\n", ret);
		switch (ret) {
		case 2:	/*cancel */
			break;
		case 1:	/* NO */
			exit(1);
		case 0:	/*YES */
			g->windows_count_limit += g->windows_count_limit_param;
			break;
		default:
			fprintf(stderr, "Problems executing kdialog ?\n");
			exit(1);
		}
	} while (ret == 2);
}

/* handle VM message: MSG_CREATE
 * checks given attributes and create appropriate window in local Xserver
 * (using mkwindow) */
void handle_create(Ghandles * g, XID window)
{
	struct windowdata *vm_window;
	struct genlist *l;
	struct msg_create untrusted_crt;
	XID parent;

	if (g->windows_count++ > g->windows_count_limit)
		ask_whether_flooding(g);
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
	read_struct(untrusted_crt);
	/* sanitize start */
	VERIFY((int) untrusted_crt.width >= 0
	       && (int) untrusted_crt.height >= 0);
	vm_window->width =
	    min((int) untrusted_crt.width, MAX_WINDOW_WIDTH);
	vm_window->height =
	    min((int) untrusted_crt.height, MAX_WINDOW_HEIGHT);
	/* there is no really good limits for x/y, so pass them to Xorg and hope
	 * that everything will be ok... */
	vm_window->x = untrusted_crt.x;
	vm_window->y = untrusted_crt.y;
	if (untrusted_crt.override_redirect)
		vm_window->override_redirect = 1;
	else
		vm_window->override_redirect = 0;
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
			"Created 0x%x(0x%x) parent 0x%x(0x%x) ovr=%d\n",
			(int) vm_window->local_winid, (int) window,
			(int) (vm_window->parent ? vm_window->parent->
			       local_winid : 0), (unsigned) parent,
			vm_window->override_redirect);
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
void handle_destroy(Ghandles * g, struct genlist *l)
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
	free(vm_window);
}

/* replace non-printable charactes with '_'
 * given string must be NULL terminated already */
void sanitize_string_from_vm(unsigned char *untrusted_s, int allow_utf8)
{
	for (; *untrusted_s; untrusted_s++) {
		// allow only non-controll ASCII chars
		if (*untrusted_s >= 0x20 && *untrusted_s <= 0x7E)
			continue;
		if (allow_utf8 && *untrusted_s >= 0x80)
			continue;
		*untrusted_s = '_';
	}
}

/* fix menu window parameters: override_redirect and force to not hide its
 * frame */
void fix_menu(Ghandles * g, struct windowdata *vm_window)
{
	XSetWindowAttributes attr;

	attr.override_redirect = 1;
	XChangeWindowAttributes(g->display, vm_window->local_winid,
				CWOverrideRedirect, &attr);
	vm_window->override_redirect = 1;

	// do not let menu window hide its color frame by moving outside of the screen
	// if it is located offscreen, then allow negative x/y
	// correction - remove this check
	// MapEvent does not change window dimensions; so if VM sent evil dimensions,
	// in create or configure_from_vm message, they would have been 
	// corrected once the mesage has arrived.
	// Apparently, KDE tray temporarily moves the window without resizing so 
	// that it is partially offscreen (which is fine:
	// we allow window manager do to anything, it is trusted), which
	// triggered the below check and caused tray malfunction.
	//      if (force_on_screen(g, vm_window, 0, "fix_menu"))
	//              moveresize_vm_window(g, vm_window);
}

/* handle VM message: MSG_VMNAME
 * remove non-printable characters and pass to X server */
void handle_wmname(Ghandles * g, struct windowdata *vm_window)
{
	XTextProperty text_prop;
	struct msg_wmname untrusted_msg;
	char buf[sizeof(untrusted_msg.data) + 1];
	char *list[1] = { buf };

	read_struct(untrusted_msg);
	/* sanitize start */
	untrusted_msg.data[sizeof(untrusted_msg.data) - 1] = 0;
	sanitize_string_from_vm((unsigned char *) (untrusted_msg.data),
				g->allow_utf8_titles);
	snprintf(buf, sizeof(buf), "%s", untrusted_msg.data);
	/* sanitize end */
	if (g->log_level > 0)
		fprintf(stderr, "set title for window 0x%x to %s\n",
			(int) vm_window->local_winid, buf);
	Xutf8TextListToTextProperty(g->display, list, 1, XUTF8StringStyle,
				    &text_prop);
	XSetWMName(g->display, vm_window->local_winid, &text_prop);
	XSetWMIconName(g->display, vm_window->local_winid, &text_prop);
	XFree(text_prop.value);
}

/* handle VM message: MSG_WMHINTS
 * Pass hints for window manager to local X server */
void handle_wmhints(Ghandles * g, struct windowdata *vm_window)
{
	struct msg_window_hints untrusted_msg;
	XSizeHints size_hints;

	memset(&size_hints, 0, sizeof(size_hints));

	read_struct(untrusted_msg);

	/* sanitize start */
	size_hints.flags = 0;
	/* check every value and pass it only when sane */
	if ((untrusted_msg.flags & PMinSize) &&
	    untrusted_msg.min_width >= 0
	    && untrusted_msg.min_width <= MAX_WINDOW_WIDTH
	    && untrusted_msg.min_height >= 0
	    && untrusted_msg.min_height <= MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PMinSize;
		size_hints.min_width = untrusted_msg.min_width;
		size_hints.min_height = untrusted_msg.min_height;
	} else
		fprintf(stderr, "invalid PMinSize for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.min_width, untrusted_msg.min_height);
	if ((untrusted_msg.flags & PMaxSize) && untrusted_msg.max_width > 0
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
	if ((untrusted_msg.flags & PResizeInc) && size_hints.width_inc >= 0
	    && size_hints.width_inc < MAX_WINDOW_WIDTH
	    && size_hints.height_inc >= 0
	    && size_hints.height_inc < MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PResizeInc;
		size_hints.width_inc = untrusted_msg.width_inc;
		size_hints.height_inc = untrusted_msg.height_inc;
	} else
		fprintf(stderr, "invalid PResizeInc for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.width_inc, untrusted_msg.height_inc);
	if ((untrusted_msg.flags & PBaseSize) && size_hints.base_width >= 0
	    && size_hints.base_width <= MAX_WINDOW_WIDTH
	    && size_hints.base_height >= 0
	    && size_hints.base_height <= MAX_WINDOW_HEIGHT) {
		size_hints.flags |= PBaseSize;
		size_hints.base_width = untrusted_msg.base_width;
		size_hints.base_height = untrusted_msg.base_height;
	} else
		fprintf(stderr, "invalid PBaseSize for 0x%x (%d/%d)\n",
			(int) vm_window->local_winid,
			untrusted_msg.base_width,
			untrusted_msg.base_height);
	/* sanitize end */

	/* if no valid values self - do not send it to X server */
	if (size_hints.flags == 0)
		return;

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

/* handle VM message: MSG_MAP
 * Map a window with given parameters */
void handle_map(Ghandles * g, struct windowdata *vm_window)
{
	struct genlist *trans;
	struct msg_map_info untrusted_txt;

	read_struct(untrusted_txt);
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
	// ignore override_redirect attribute from the message
	// so that we can skip force_on_screen check
	// vm_window->override_redirect = 0;
	if (vm_window->override_redirect || vm_window->is_docked)
		fix_menu(g, vm_window);
	(void) XMapWindow(g->display, vm_window->local_winid);
}

/* handle VM message: MSG_DOCK
 * Try to dock window in the tray
 * Rest of XEMBED protocol is catched in VM */
void handle_dock(Ghandles * g, struct windowdata *vm_window)
{
	Window tray;
	if (g->log_level > 0)
		fprintf(stderr, "docking window 0x%x\n",
			(int) vm_window->local_winid);
	tray = XGetSelectionOwner(g->display, g->tray_selection);
	if (tray != None) {
		long data[2];
		XClientMessageEvent msg;

		data[0] = 1;
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
void inter_appviewer_lock(int mode)
{
	int cmd;
	static int fd = 0;
	if (!fd) {
		fd = open("/var/run/qubes/appviewer.lock",
			  O_RDWR | O_CREAT, 0600);
		if (fd < 0) {
			perror("create lock");
			exit(1);
		}
	}
	if (mode)
		cmd = LOCK_EX;
	else
		cmd = LOCK_UN;
	if (flock(fd, cmd) < 0) {
		perror("lock");
		exit(1);
	}
}

/* release shared memory connected with given window */
void release_mapped_mfns(Ghandles * g, struct windowdata *vm_window)
{
	inter_appviewer_lock(1);
	g->shmcmd->shmid = vm_window->shminfo.shmid;
	XShmDetach(g->display, &vm_window->shminfo);
	XDestroyImage(vm_window->image);
	XSync(g->display, False);
	inter_appviewer_lock(0);
	vm_window->image = NULL;
}

/* handle VM message: MSG_MFNDUMP
 * Retrieve memory addresses connected with composition buffer of remote window
 */
void handle_mfndump(Ghandles * g, struct windowdata *vm_window)
{
	char untrusted_shmcmd_data_from_remote[4096 * SHM_CMD_NUM_PAGES];
	struct shm_cmd *untrusted_shmcmd =
	    (struct shm_cmd *) untrusted_shmcmd_data_from_remote;
	unsigned num_mfn, off;
	static char dummybuf[100];


	if (vm_window->image)
		release_mapped_mfns(g, vm_window);
	read_data(untrusted_shmcmd_data_from_remote,
		  sizeof(struct shm_cmd));
	/* sanitize start */
	VERIFY(untrusted_shmcmd->num_mfn <= MAX_MFN_COUNT);
	num_mfn = untrusted_shmcmd->num_mfn;
	VERIFY((int) untrusted_shmcmd->width >= 0
	       && (int) untrusted_shmcmd->height >= 0);
	VERIFY((int) untrusted_shmcmd->width < MAX_WINDOW_WIDTH
	       && (int) untrusted_shmcmd->height < MAX_WINDOW_HEIGHT);
	VERIFY(untrusted_shmcmd->off < 4096);
	off = untrusted_shmcmd->off;
	/* unused for now: VERIFY(untrusted_shmcmd->bpp == 24); */
	/* sanitize end */
	vm_window->image_width = untrusted_shmcmd->width;
	vm_window->image_height = untrusted_shmcmd->height;	/* sanitized above */
	read_data((char *) untrusted_shmcmd->mfns,
		  SIZEOF_SHARED_MFN * num_mfn);
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
	/* ensure that _every_ not sanitized field is overrided by some trusted
	 * value */
	untrusted_shmcmd->shmid = vm_window->shminfo.shmid;
	untrusted_shmcmd->domid = g->domid;
	inter_appviewer_lock(1);
	memcpy(g->shmcmd, untrusted_shmcmd_data_from_remote,
	       4096 * SHM_CMD_NUM_PAGES);
	vm_window->shminfo.shmaddr = vm_window->image->data = dummybuf;
	vm_window->shminfo.readOnly = True;
	XSync(g->display, False);
	if (!XShmAttach(g->display, &vm_window->shminfo)) {
		fprintf(stderr,
			"XShmAttach failed for window 0x%x(remote 0x%x)\n",
			(int) vm_window->local_winid,
			(int) vm_window->remote_winid);
	}
	XSync(g->display, False);
	g->shmcmd->shmid = g->cmd_shmid;
	inter_appviewer_lock(0);
	shmctl(vm_window->shminfo.shmid, IPC_RMID, 0);
}

/* VM message dispatcher */
void handle_message(Ghandles * g)
{
	struct msghdr untrusted_hdr;
	uint32_t type;
	XID window;
	struct genlist *l;
	struct windowdata *vm_window = NULL;

	read_struct(untrusted_hdr);
	VERIFY(untrusted_hdr.type > MSG_MIN
	       && untrusted_hdr.type < MSG_MAX);
	/* sanitized msg type */
	type = untrusted_hdr.type;
	if (type == MSG_CLIPBOARD_DATA) {
		/* window field has special meaning here */
		handle_clipboard_data(g, untrusted_hdr.window);
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
	case MSG_DOCK:
		handle_dock(g, vm_window);
		break;
	case MSG_WINDOW_HINTS:
		handle_wmhints(g, vm_window);
		break;
	default:
		fprintf(stderr, "got unknown msg type %d\n", type);
		exit(1);
	}
}

/* signal handler - connected to SIGTERM */
void dummy_signal_handler(int x)
{
	ghandles.signal_caught = 1;
}

void print_backtrace(void)
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
void release_all_mapped_mfns()
{
	struct genlist *curr;
	if (ghandles.log_level > 1)
		fprintf(stderr, "release_all_mapped_mfns running\n");
	print_backtrace();
	for (curr = ghandles.wid2windowdata->next;
	     curr != ghandles.wid2windowdata; curr = curr->next) {
		struct windowdata *vm_window = curr->data;
		if (vm_window->image)
			/* use og ghandles directly, as no other way get it (atexec cannot
			 * pass argument) */
			release_mapped_mfns(&ghandles, vm_window);
	}
}

/* start pulseaudio Dom0 proxy */
void exec_pacat(Ghandles * g)
{
	int i, fd;
	pid_t pid;
	char domid_txt[20];
	char logname[80];
	snprintf(domid_txt, sizeof domid_txt, "%d", g->domid);
	snprintf(logname, sizeof logname, "/var/log/qubes/pacat.%d.log",
		 g->domid);
	switch (pid=fork()) {
	case -1:
		perror("fork pacat");
		exit(1);
	case 0:
		for (i = 0; i < 256; i++)
			close(i);
		fd = open("/dev/null", O_RDWR);
		for (i = 0; i <= 1; i++)
			dup2(fd, i);
		umask(0007);
		fd = open(logname, O_WRONLY | O_CREAT | O_TRUNC, 0640);
		umask(0077);
		execl("/usr/bin/pacat-simple-vchan", "pacat-simple-vchan",
		      domid_txt, NULL);
		perror("execl");
		exit(1);
	default:
		g->pulseaudio_pid = pid;
	}
}

/* send configuration parameters of X server to VM */
void send_xconf(Ghandles * g)
{
	struct msg_xconf xconf;
	XWindowAttributes attr;
	XGetWindowAttributes(g->display, g->root_win, &attr);
	xconf.w = _VIRTUALX(attr.width);
	xconf.h = attr.height;
	xconf.depth = attr.depth;
	xconf.mem = xconf.w * xconf.h * 4 / 1024 + 1;
	write_struct(xconf);
}

/* receive from VM and compare protocol version
 * abort if mismatch */
void get_protocol_version()
{
	uint32_t untrusted_version;
	char message[1024];
	read_struct(untrusted_version);
	if (untrusted_version == QUBES_GUID_PROTOCOL_VERSION)
		return;
	snprintf(message, sizeof message, "kdialog --sorry \"The remote "
		 "protocol version is %d, the local protocol version is %d. Upgrade "
		 "qubes-gui-dom0 (in dom0) and qubes-gui-vm (in template VM) packages "
		 "so that they provide compatible/latest software. You can run 'xm console "
		 "vmname' (as root) to access shell prompt in the VM.\"",
		 untrusted_version, QUBES_GUID_PROTOCOL_VERSION);
	system(message);
	exit(1);
}

/* wait until child process connects to VM */
void wait_for_connection_in_parent(int *pipe_notify)
{
	// inside the parent process
	// wait for daemon to get connection with AppVM
	struct pollfd pipe_pollfd;
	int tries, ret;

	if (ghandles.log_level > 0)
		fprintf(stderr, "Connecting to VM's GUI agent: ");
	close(pipe_notify[1]);	// close the writing end
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
			if (pipe_pollfd.revents == POLLIN)
				break;
			if (ghandles.log_level > 0)
				fprintf(stderr, "exiting\n");
			exit(1);
		}
		if (tries >= 45) {
			if (ghandles.log_level > 0) {
				fprintf(stderr,
					"\nHmm... this takes more time than usual --"
					" is the VM running?\n");
				fprintf(stderr,
					"Connecting to VM's GUI agent: ");
			}
			tries = 0;
		}

	}
	if (ghandles.log_level > 0)
		fprintf(stderr, "connected\n");
	exit(0);
}

void usage()
{
	fprintf(stderr,
		"usage: qubes_quid -d domain_id [-c color] [-l label_index] [-i icon name, no suffix] [-v] [-q]\n");
	fprintf(stderr, "       -v  increase log verbosity\n");
	fprintf(stderr, "       -q  decrease log verbosity\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Log levels:\n");
	fprintf(stderr, " 0 - only errors\n");
	fprintf(stderr, " 1 - some basic messages (default)\n");
	fprintf(stderr, " 2 - debug\n");
}

void parse_cmdline(Ghandles * g, int argc, char **argv)
{
	int opt;
	/* defaults */
	g->log_level = 1;

	while ((opt = getopt(argc, argv, "d:e:c:l:i:vq")) != -1) {
		switch (opt) {
		case 'd':
			g->domid = atoi(optarg);
			break;
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
			g->log_level--;
			break;
		case 'v':
			g->log_level++;
			break;
		default:
			usage();
			exit(1);
		}
	}
	if (!g->domid) {
		fprintf(stderr, "domid=0?");
		exit(1);
	}
}

void load_default_config_values(Ghandles * g)
{

	g->allow_utf8_titles = 0;
	g->copy_seq_mask = ControlMask | ShiftMask;
	g->copy_seq_key = XK_c;
	g->paste_seq_mask = ControlMask | ShiftMask;
	g->paste_seq_key = XK_v;
	g->windows_count_limit_param = 500;
}

// parse string describing key sequence like Ctrl-Alt-c
void parse_key_sequence(const char *seq, int *mask, KeySym * key)
{
	const char *seqp = seq;
	int found_modifier;

	// ignore null string
	if (seq == NULL)
		return;
	*mask = 0;
	do {
		found_modifier = 1;
		if (strncasecmp(seqp, "Ctrl-", 5) == 0) {
			*mask |= ControlMask;
			seqp += 5;
		} else if (strncasecmp(seqp, "Mod1-", 5) == 0) {
			*mask |= Mod1Mask;
			seqp += 4;
		} else if (strncasecmp(seqp, "Mod2-", 5) == 0) {
			*mask |= Mod2Mask;
			seqp += 4;
		} else if (strncasecmp(seqp, "Mod3-", 5) == 0) {
			*mask |= Mod3Mask;
			seqp += 4;
		} else if (strncasecmp(seqp, "Mod4-", 5) == 0) {
			*mask |= Mod4Mask;
			seqp += 4;
			/* second name just for convenience */
		} else if (strncasecmp(seqp, "Alt-", 4) == 0) {
			*mask |= Mod1Mask;
			seqp += 4;
		} else if (strncasecmp(seqp, "Shift-", 6) == 0) {
			*mask |= ShiftMask;
			seqp += 6;
		} else
			found_modifier = 0;
	} while (found_modifier);

	*key = XStringToKeysym(seqp);
	if (*key == NoSymbol) {
		fprintf(stderr,
			"Warning: key sequence (%s) is invalid (will be disabled)\n",
			seq);
	}
}

void parse_vm_config(Ghandles * g, config_setting_t * group)
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
	     config_setting_get_member(group, "windows_count_limit"))) {
		g->windows_count_limit_param = config_setting_get_int(setting);
	}

	if ((setting =
	     config_setting_get_member(group, "log_level"))) {
		g->log_level = config_setting_get_int(setting);
	}
}

void parse_config(Ghandles * g)
{
	config_t config;
	config_setting_t *setting;
	char buf[128];

	config_init(&config);
#if (((LIBCONFIG_VER_MAJOR == 1) && (LIBCONFIG_VER_MINOR > 5)) \
		|| (LIBCONFIG_VER_MAJOR > 1))
	config_set_include_dir(&config, GUID_CONFIG_DIR);
#endif
	if (config_read_file(&config, GUID_CONFIG_FILE) == CONFIG_FALSE) {
#if (((LIBCONFIG_VER_MAJOR == 1) && (LIBCONFIG_VER_MINOR >= 4)) \
		|| (LIBCONFIG_VER_MAJOR > 1))
		if (config_error_type(&config) == CONFIG_ERR_FILE_IO) {
#else
		if (strcmp(config_error_text(&config), "file I/O error") ==
		    0) {
#endif
			fprintf(stderr,
				"Warning: cannot read config file (%s): %s\n",
				GUID_CONFIG_FILE,
				config_error_text(&config));
		} else {
			fprintf(stderr,
				"Critical: error reading config (%s:%d): %s\n",
#if (((LIBCONFIG_VER_MAJOR == 1) && (LIBCONFIG_VER_MINOR >= 4)) \
		|| (LIBCONFIG_VER_MAJOR > 1))
				config_error_file(&config),
#else
				GUID_CONFIG_FILE,
#endif
				config_error_line(&config),
				config_error_text(&config));
			exit(1);
		}
	}
	// first load global settings
	if ((setting = config_lookup(&config, "global"))) {
		parse_vm_config(g, setting);
	}
	// then try to load per-VM settings
	snprintf(buf, sizeof(buf), "VM/%s", g->vmname);
	if ((setting = config_lookup(&config, buf))) {
		parse_vm_config(g, setting);
	}
}

/* helper to get a file flag path */
char *guid_fs_flag(char *type, int domid)
{
	static char buf[256];
	snprintf(buf, sizeof(buf), "/var/run/qubes/guid_%s.%d",
		 type, domid);
	return buf;
}

int guid_boot_lock = -1;

/* create guid_running file when connected to VM */
void set_alive_flag(int domid)
{
	int fd = open(guid_fs_flag("running", domid),
		      O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
	close(fd);
	unlink(guid_fs_flag("booting", domid));
	close(guid_boot_lock);

}

/* remove guid_running file at exit */
void unset_alive_flag()
{
	unlink(guid_fs_flag("running", ghandles.domid));
}

void kill_pacat() {
	pid_t pid = ghandles.pulseaudio_pid;
	if (pid > 0) {
		kill(pid, SIGTERM);
	}
}

void wait_for_pacat(int signum) {
	int status;

	if (ghandles.pulseaudio_pid > 0) {
		if (waitpid(ghandles.pulseaudio_pid, &status, WNOHANG) > 0) {
			ghandles.pulseaudio_pid = -1;
			if (status != 0 && ghandles.log_level > 0) {
				fprintf(stderr, "pacat exited with %d status\n", status);
			}
		}
	}
}


void get_boot_lock(int domid)
{
	struct stat st;
	int fd = open(guid_fs_flag("booting", domid),
		      O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		perror("cannot get boot lock ???\n");
		exit(1);
	}
	if (flock(fd, LOCK_EX) < 0) {
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

int main(int argc, char **argv)
{
	int xfd;
	char *vmname;
	FILE *f;
	int childpid;
	int pipe_notify[2];
	char dbg_log[256];
	int logfd;
	char cmd_tmp[256];

	parse_cmdline(&ghandles, argc, argv);
	get_boot_lock(ghandles.domid);
	/* load config file */
	load_default_config_values(&ghandles);
	parse_config(&ghandles);

	// daemonize...
	if (pipe(pipe_notify) < 0) {
		perror("canot create pipe:");
		exit(1);
	}

	childpid = fork();
	if (childpid < 0) {
		fprintf(stderr, "Cannot fork :(\n");
		exit(1);
	} else if (childpid > 0)
		wait_for_connection_in_parent(pipe_notify);

	// inside the daemonized process...
	f = fopen("/var/run/shm.id", "r");
	if (!f) {
		fprintf(stderr,
			"Missing /var/run/shm.id; run X with preloaded shmoverride\n");
		exit(1);
	}
	fscanf(f, "%d", &ghandles.cmd_shmid);
	fclose(f);
	ghandles.shmcmd = shmat(ghandles.cmd_shmid, NULL, 0);
	if (ghandles.shmcmd == (void *) (-1UL)) {
		fprintf(stderr,
			"Invalid or stale shm id 0x%x in /var/run/shm.id\n",
			ghandles.cmd_shmid);
		exit(1);
	}

	close(0);
	snprintf(dbg_log, sizeof(dbg_log),
		 "/var/log/qubes/guid.%d.log", ghandles.domid);
	umask(0007);
	logfd = open(dbg_log, O_WRONLY | O_CREAT | O_TRUNC, 0640);
	umask(0077);
	dup2(logfd, 1);
	dup2(logfd, 2);

	chdir("/var/run/qubes");
	errno = 0;
	if (setsid() < 0) {
		perror("setsid()");
		exit(1);
	}
	mkghandles(&ghandles);
	XSetErrorHandler(dummy_handler);
	vmname = peer_client_init(ghandles.domid, 6000);
	atexit(vchan_close);
	signal(SIGCHLD, wait_for_pacat);
	exec_pacat(&ghandles);
	atexit(kill_pacat);
	/* drop root privileges */
	setuid(getuid());
	set_alive_flag(ghandles.domid);
	atexit(unset_alive_flag);

	write(pipe_notify[1], "Q", 1);	// let the parent know we connected sucessfully

	signal(SIGTERM, dummy_signal_handler);
	atexit(release_all_mapped_mfns);

	strncpy(ghandles.vmname, vmname, sizeof(ghandles.vmname));
	ghandles.vmname[sizeof(ghandles.vmname) - 1] = 0;
	xfd = ConnectionNumber(ghandles.display);

	/* provide keyboard map before VM Xserver starts */

	if (snprintf(cmd_tmp, sizeof(cmd_tmp), "/usr/bin/xenstore-write	"
		     "/local/domain/%d/qubes_keyboard \"`/usr/bin/setxkbmap -print`\"",
		     ghandles.domid) < sizeof(cmd_tmp)) {
		/* intentionally ignore return value - don't fail gui-daemon if only
		 * keyboard layout fails */
		system(cmd_tmp);
	}

	get_protocol_version();
	send_xconf(&ghandles);

	for (;;) {
		int select_fds[2] = { xfd };
		fd_set retset;
		int busy;
		if (ghandles.signal_caught) {
			fprintf(stderr, "exiting on signal...\n");
			exit(0);
		}
		do {
			busy = 0;
			if (XPending(ghandles.display)) {
				process_xevent(&ghandles);
				busy = 1;
			}
			if (read_ready()) {
				handle_message(&ghandles);
				busy = 1;
			}
		} while (busy);
		wait_for_vchan_or_argfd(1, select_fds, &retset);
	}
	return 0;
}

// vim:ts=4:sw=4:noet:
