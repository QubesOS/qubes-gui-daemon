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
#include "messages.h"
#include "txrx.h"
#include "list.h"
#include "error.h"
#include "shm_cmd.h"
#include "cmd_socket.h"
#include "qlimits.h"

struct _global_handles {
	Display *display;
	int screen;		/* shortcut to the default screen */
	Window root_win;	/* root attributes */
	GC context;
	Atom wmDeleteMessage;
	char vmname[16];
	struct shm_cmd *shmcmd;
	int cmd_shmid;
	int domid;
	int execute_cmd_in_vm;
	char cmd_for_vm[256];
	int clipboard_requested;
	GC frame_gc;
	char *cmdline_color;
	char *cmdline_icon;
	int label_index;
};

typedef struct _global_handles Ghandles;
Ghandles ghandles;
struct conndata {
	int width;
	int height;
	int remote_x;
	int remote_y;
	int is_mapped;
	XID remote_winid;
	Window local_winid;
	struct conndata *parent;
	struct conndata *transient_for;
	int override_redirect;
	XShmSegmentInfo shminfo;
	XImage *image;
	int image_height;
	int image_width;
	int have_queued_resize;
};
struct conndata *last_input_window;
struct genlist *remote2local;
struct genlist *wid2conndata;


Window mkwindow(Ghandles * g, struct conndata *item)
{
	char *gargv[1] = { 0 };
	Window child_win;
	Window parent;
	XSizeHints my_size_hints;	/* hints for the window manager */
	Atom atom_label, atom_vmname;

	my_size_hints.flags = PSize;
	my_size_hints.height = item->width;
	my_size_hints.width = item->height;

	if (item->parent)
		parent = item->parent->local_winid;
	else
		parent = g->root_win;
#if 0
	if (item->override_redirect && !item->parent && last_input_window) {
		XWindowAttributes attr;
		ret =
		    XGetWindowAttributes(g->display,
					 last_input_window->local_winid,
					 &attr);
		if (ret != 0) {
			parent = last_input_window->local_winid;
			x = item->remote_x - last_input_window->remote_x +
			    attr.x;
			y = item->remote_y - last_input_window->remote_y +
			    attr.y;
		}
	}
#endif
#if 1
	// we will set override_redirect later, if needed
	child_win = XCreateSimpleWindow(g->display, parent,
					0, 0, item->width, item->height,
					0, BlackPixel(g->display,
						      g->screen),
					WhitePixel(g->display, g->screen));
#endif
#if 0
	attr.override_redirect = item->override_redirect;
	child_win = XCreateWindow(g->display, parent,
				  x, y, item->width, item->height,
				  0,
				  CopyFromParent,
				  CopyFromParent,
				  CopyFromParent,
				  CWOverrideRedirect, &attr);
#endif
/* pass my size hints to the window manager, along with window
	and icon names */
	(void) XSetStandardProperties(g->display, child_win,
				      "VMapp command", "Pixmap", None,
				      gargv, 0, &my_size_hints);
	(void) XSelectInput(g->display, child_win,
			    ExposureMask | KeyPressMask | KeyReleaseMask |
			    ButtonPressMask | ButtonReleaseMask |
			    PointerMotionMask | EnterWindowMask |
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
			g->vmname, strlen (g->vmname));


	return child_win;
}


void mkghandles(Ghandles * g)
{
	g->display = XOpenDisplay(NULL);
	if (!g->display) {
		perror("XOpenDisplay");
		exit(1);
	}
	g->screen = DefaultScreen(g->display);
	g->root_win = RootWindow(g->display, g->screen);
	g->context = XCreateGC(g->display, g->root_win, 0, NULL);
	g->wmDeleteMessage =
	    XInternAtom(g->display, "WM_DELETE_WINDOW", True);
	g->clipboard_requested = 0;
}

struct conndata *check_nonmanged_window(XID id)
{
	struct genlist *item = list_lookup(wid2conndata, id);
	if (!item) {
		fprintf(stderr, "cannot lookup 0x%x in wid2conndata\n",
			(int) id);
		return 0;
	}
	return item->data;
}

void inter_appviewer_lock(int mode);

#define CHECK_NONMANAGED_WINDOW(id) struct conndata *conn; \
	if (!(conn=check_nonmanged_window(id))) return

#define QUBES_CLIPBOARD_FILENAME "/var/run/qubes/qubes_clipboard.bin"
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

void handle_clipboard_data(unsigned int len)
{
	FILE *file;
	char *data;
	fprintf(stderr, "handle_clipboard_data, len=0x%x\n", len);
	if (len > MAX_CLIPBOARD_SIZE) {
		fprintf(stderr, "clipboard data len 0x%x?\n", len);
		exit(1);
	}
	data = malloc(len);
	if (!data) {
		perror("malloc");
		exit(1);
	}
	read_data(data, len);
	if (!ghandles.clipboard_requested) {
		free(data);	//warn maybe ?
		return;
	}
	inter_appviewer_lock(1);
	file = fopen(QUBES_CLIPBOARD_FILENAME, "w");
	if (!file) {
		perror("open " QUBES_CLIPBOARD_FILENAME);
		exit(1);
	}
	fwrite(data, len, 1, file);
	fclose(file);
	file = fopen(QUBES_CLIPBOARD_FILENAME ".source", "w");
	if (!file) {
		perror("open " QUBES_CLIPBOARD_FILENAME ".source");
		exit(1);
	}
	fwrite(ghandles.vmname, strlen(ghandles.vmname), 1, file);
	fclose(file);
	inter_appviewer_lock(0);
	ghandles.clipboard_requested = 0;
	free(data);
}

int is_special_keypress(XKeyEvent * ev, XID remote_winid)
{
	struct msghdr hdr;
	char *data;
	int len;
	if ((ev->state & (ShiftMask | ControlMask)) !=
	    (ShiftMask | ControlMask))
		return 0;
	if (ev->keycode == XKeysymToKeycode(ghandles.display, XK_c)) {
		if (ev->type != KeyPress)
			return 1;
		ghandles.clipboard_requested = 1;
		hdr.type = MSG_CLIPBOARD_REQ;
		hdr.window = remote_winid;
		fprintf(stderr, "Ctrl-Shift-c\n");
		write_struct(hdr);
		return 1;
	}
	if (ev->keycode == XKeysymToKeycode(ghandles.display, XK_v)) {
		if (ev->type != KeyPress)
			return 1;
		hdr.type = MSG_CLIPBOARD_DATA;
		fprintf(stderr, "Ctrl-Shift-v\n");
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

void process_xevent_keypress(XKeyEvent * ev)
{
	struct msghdr hdr;
	struct msg_keypress k;
	CHECK_NONMANAGED_WINDOW(ev->window);
	last_input_window = conn;
	if (is_special_keypress(ev, conn->remote_winid))
		return;
	k.type = ev->type;
	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.keycode = ev->keycode;
	hdr.type = MSG_KEYPRESS;
	hdr.window = conn->remote_winid;
	write_message(hdr, k);
//      fprintf(stderr, "win 0x%x(0x%x) type=%d keycode=%d\n",
//              (int) ev->window, hdr.window, k.type, k.keycode);
}

// debug routine
void dump_mapped()
{
	struct genlist *item = wid2conndata->next;
	for (; item != wid2conndata; item = item->next) {
		struct conndata *c = item->data;
		if (c->is_mapped) {
			fprintf(stderr,
				"id 0x%x(0x%x) w=0x%x h=0x%x rx=%d ry=%d ovr=%d\n",
				(int) c->local_winid,
				(int) c->remote_winid, c->width, c->height,
				c->remote_x, c->remote_y,
				c->override_redirect);
		}
	}
}

void process_xevent_button(XButtonEvent * ev)
{
	struct msghdr hdr;
	struct msg_button k;
	CHECK_NONMANAGED_WINDOW(ev->window);

// for debugging only, inactive
	if (0 && ev->button == 4) {
		dump_mapped();
		return;
	}

	last_input_window = conn;
	k.type = ev->type;

	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.button = ev->button;
	hdr.type = MSG_BUTTON;
	hdr.window = conn->remote_winid;
	write_message(hdr, k);
	fprintf(stderr, "xside: win 0x%x(0x%x) type=%d button=%d\n",
		(int) ev->window, hdr.window, k.type, k.button);
}

void process_xevent_close(XID window)
{
	struct msghdr hdr;
	CHECK_NONMANAGED_WINDOW(window);
	hdr.type = MSG_CLOSE;
	hdr.window = conn->remote_winid;
	write_struct(hdr);
}

void send_resize(struct conndata *conn, int w, int h)
{
	struct msghdr hdr;
	struct msg_resize k;
	hdr.type = MSG_RESIZE;
	hdr.window = conn->remote_winid;
	k.height = h;
	k.width = w;
	write_message(hdr, k);
}

void process_xevent_configure(XConfigureEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(ev->window);
//      fprintf(stderr, "process_xevent_configure, %d/%d, was"
//              "%d/%d\n", ev->width, ev->height,
//              conn->width, conn->height);
	if (conn->width == ev->width && conn->height == ev->height)
		return;
	conn->width = ev->width;
	conn->height = ev->height;
// if AppVM has not unacknowledged previous resize msg, do not send another one
	if (conn->have_queued_resize)
		return;
	conn->have_queued_resize = 1;
	send_resize(conn, ev->width, ev->height);
}

void handle_configure_from_vm(Ghandles * g, struct conndata *item)
{
	struct msg_configure conf;
	XWindowAttributes attr;
	XWindowChanges xchange;
	int size_changed, ret;

	read_struct(conf);
	fprintf(stderr, "handle_configure_from_vm, %d/%d, was"
		" %d/%d, ovr=%d\n", conf.width, conf.height,
		item->width, item->height, conf.override_redirect);
	if (conf.width > MAX_WINDOW_WIDTH)
		conf.width = MAX_WINDOW_WIDTH;
	if (conf.height > MAX_WINDOW_HEIGHT)
		conf.height = MAX_WINDOW_HEIGHT;

	if (item->width != conf.width || item->height != conf.height)
		size_changed = 1;
	else
		size_changed = 0;
	item->override_redirect = conf.override_redirect;
	if (item->have_queued_resize) {
		if (size_changed) {
			send_resize(item, item->width, item->height);
			return;
		} else
			// same dimensions; this is an ack for our previously sent resize req
			item->have_queued_resize = 0;
	}
	if (!item->override_redirect) {
		item->remote_x = conf.x;
		item->remote_y = conf.y;
		if (size_changed) {
			item->width = conf.width;
			item->height = conf.height;
			XResizeWindow(g->display, item->local_winid,
				      conf.width, conf.height);
		}
		return;
	}
// from now on, handle configuring of override_redirect (menu) window
// we calculate the delta from the previous position and move dom0 window by delta
	ret = XGetWindowAttributes(g->display, item->local_winid, &attr);
	if (ret == 0) {
		fprintf(stderr,
			"XGetWindowAttributes fail in handle_configure for 0x%x\n",
			(int) item->local_winid);
		return;
	}
	xchange.width = conf.width;
	xchange.height = conf.height;
	xchange.x = attr.x + conf.x - item->remote_x;
	xchange.y = attr.y + conf.y - item->remote_y;
// do not let menu window hide its color frame by moving outside of the screen  
	if (xchange.x < 0)
		xchange.x = 0;
	if (xchange.y < 0)
		xchange.y = 0;

	item->width = conf.width;
	item->height = conf.height;
	item->remote_x = conf.x;
	item->remote_y = conf.y;
	XConfigureWindow(g->display,
			 item->local_winid,
			 CWX | CWY | CWWidth | CWHeight, &xchange);
}

void process_xevent_motion(XMotionEvent * ev)
{
	struct msghdr hdr;
	struct msg_motion k;
	CHECK_NONMANAGED_WINDOW(ev->window);

	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.is_hint = ev->is_hint;
	hdr.type = MSG_MOTION;
	hdr.window = conn->remote_winid;
	write_message(hdr, k);
//      fprintf(stderr, "motion in 0x%x", ev->window);
}

void process_xevent_crossing(XCrossingEvent * ev)
{
	struct msghdr hdr;
	struct msg_crossing k;
	CHECK_NONMANAGED_WINDOW(ev->window);

	hdr.type = MSG_CROSSING;
	hdr.window = conn->remote_winid;
	k.type = ev->type;
	k.x = ev->x;
	k.y = ev->y;
	k.state = ev->state;
	k.mode = ev->mode;
	k.detail = ev->detail;
	k.focus = ev->focus;
	write_message(hdr, k);
}

void process_xevent_focus(XFocusChangeEvent * ev)
{
	struct msghdr hdr;
	struct msg_focus k;
	CHECK_NONMANAGED_WINDOW(ev->window);
	hdr.type = MSG_FOCUS;
	hdr.window = conn->remote_winid;
	k.type = ev->type;
	k.mode = ev->mode;
	k.detail = ev->detail;
	write_message(hdr, k);
	if (ev->type == FocusIn) {
		char keys[32];
		XQueryKeymap(ghandles.display, keys);
		hdr.type = MSG_KEYMAP_NOTIFY;
		hdr.window = 0;
		write_message(hdr, keys);
	}
}

void do_shm_update(struct conndata *conn, int x, int y, int w, int h)
{
	int border_width = 2;

	if (!conn->override_redirect) {
		// Window Manager will take care of the frame...
		border_width = 0;
	}


	int do_border = 0;
	int hoff = 0, woff = 0, delta, i;
	if (!conn->image)
		return;
	if (conn->image_height > conn->height)
		hoff = (conn->image_height - conn->height) / 2;
	if (conn->image_width > conn->width)
		hoff = (conn->image_width - conn->width) / 2;
	if (x < 0 || y < 0) {
		fprintf(stderr,
			"do_shm_update for 0x%x(remote 0x%x), x=%d, y=%d, w=%d, h=%d ?\n",
			(int) conn->local_winid, (int) conn->remote_winid,
			x, y, w, h);
		return;
	}
	if (conn->width < border_width * 2
	    || conn->height < border_width * 2)
		return;
	delta = border_width - x;
	if (delta > 0) {
		w -= delta;
		x = border_width;
		do_border = 1;
	}
	delta = x + w - (conn->width - border_width);
	if (delta > 0) {
		w -= delta;
		do_border = 1;
	}
	delta = border_width - y;
	if (delta > 0) {
		h -= delta;
		y = border_width;
		do_border = 1;
	}
	delta = y + h - (conn->height - border_width);
	if (delta > 0) {
		h -= delta;
		do_border = 1;
	}

	if (w <= 0 || h <= 0)
		return;
	XShmPutImage(ghandles.display, conn->local_winid, ghandles.context,
		     conn->image, x + woff, y + hoff, x, y, w, h, 0);
	if (!do_border)
		return;
	for (i = 0; i < border_width; i++)
		XDrawRectangle(ghandles.display, conn->local_winid,
			       ghandles.frame_gc, i, i,
			       conn->width - 1 - 2 * i,
			       conn->height - 1 - 2 * i);

}

void process_xevent_expose(XExposeEvent * ev)
{
	CHECK_NONMANAGED_WINDOW(ev->window);
	do_shm_update(conn, ev->x, ev->y, ev->width, ev->height);
}

void process_xevent_mapnotify(XMapEvent * ev)
{
	XWindowAttributes attr;
	CHECK_NONMANAGED_WINDOW(ev->window);
	if (conn->is_mapped)
		return;
	XGetWindowAttributes(ghandles.display, conn->local_winid, &attr);
	if (attr.map_state == IsViewable) {
		(void) XUnmapWindow(ghandles.display, conn->local_winid);
		fprintf(stderr, "WM tried to map 0x%x, revert\n",
			(int) conn->local_winid);
	}
}

void process_xevent()
{
	XEvent event_buffer;
	XNextEvent(ghandles.display, &event_buffer);
	switch (event_buffer.type) {
	case KeyPress:
	case KeyRelease:
		process_xevent_keypress((XKeyEvent *) & event_buffer);
		break;
	case ConfigureNotify:
		process_xevent_configure((XConfigureEvent *) &
					 event_buffer);
		break;
	case ButtonPress:
	case ButtonRelease:
		process_xevent_button((XButtonEvent *) & event_buffer);
		break;
	case MotionNotify:
		process_xevent_motion((XMotionEvent *) & event_buffer);
		break;
	case EnterNotify:
	case LeaveNotify:
		process_xevent_crossing((XCrossingEvent *) & event_buffer);
		break;
	case FocusIn:
	case FocusOut:
		process_xevent_focus((XFocusChangeEvent *) & event_buffer);
		break;
	case Expose:
		process_xevent_expose((XExposeEvent *) & event_buffer);
		break;
	case MapNotify:
		process_xevent_mapnotify((XMapEvent *) & event_buffer);
		break;
	case ClientMessage:
//              fprintf(stderr, "xclient, atom=%s, wm=0x%x\n",
//                      XGetAtomName(ghandles.display,
//                                   event_buffer.xclient.message_type),
//                      ghandles.wmDeleteMessage);
		if (event_buffer.xclient.data.l[0] ==
		    ghandles.wmDeleteMessage) {
			fprintf(stderr, "close for 0x%x\n",
				(int) event_buffer.xclient.window);
			process_xevent_close(event_buffer.xclient.window);
		}
		break;
	default:;
	}
}



void handle_shmimage(Ghandles * g, struct conndata *item)
{
	struct msg_shmimage mx;
	read_struct(mx);
	if (!item->is_mapped)
		return;
	do_shm_update(item, mx.x, mx.y, mx.width, mx.height);
}

int windows_count;
int windows_count_limit = 100;
void ask_whether_flooding()
{
	char text[1024];
	int ret;
	snprintf(text, sizeof(text),
		 "kdialog --yesnocancel "
		 "'VMapp \"%s\" has created %d windows; it looks numerous, "
		 "so it may be "
		 "a beginning of a DoS attack. Do you want to continue:'",
		 ghandles.vmname, windows_count);
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
			windows_count_limit += 100;
			break;
		default:
			fprintf(stderr, "Problems executing kdialog ?\n");
			exit(1);
		}
	} while (ret == 2);
}


void handle_create(XID window)
{
	struct conndata *item;
	struct genlist *l;
	struct msg_create crt;
	if (windows_count++ > windows_count_limit)
		ask_whether_flooding();
	item = (struct conndata *) calloc(1, sizeof(struct conndata));
#if 0
	because of calloc item->image = 0;
	item->is_mapped = 0;
	item->local_winid = 0;
	item->dest = item->src = item->pix = 0;
#endif
	item->remote_winid = window;
	list_insert(remote2local, window, item);
	read_struct(crt);
	item->width = crt.width;
	item->height = crt.height;
	if (item->width < 0 || item->height < 0) {
		fprintf(stderr,
			"handle_create for remote 0x%x got w/h %d %d\n",
			(int) window, item->width, item->height);
		exit(1);
	}
	if (item->width > MAX_WINDOW_WIDTH)	//XXX should we abort ?
		item->width = MAX_WINDOW_WIDTH;
	if (item->height > MAX_WINDOW_HEIGHT)
		item->height = MAX_WINDOW_HEIGHT;
	item->remote_x = crt.x;
	item->remote_y = crt.y;
	item->override_redirect = crt.override_redirect;
	l = list_lookup(remote2local, crt.parent);
	if (l)
		item->parent = l->data;
	else
		item->parent = NULL;
	item->transient_for = NULL;
	item->local_winid = mkwindow(&ghandles, item);
	fprintf(stderr,
		"Created 0x%x(0x%x) parent 0x%x(0x%x) ovr=%d\n",
		(int) item->local_winid, (int) window,
		(int) (item->parent ? item->parent->local_winid : 0),
		crt.parent, crt.override_redirect);
	list_insert(wid2conndata, item->local_winid, item);
}

void release_mapped_mfns(Ghandles * g, struct conndata *item);

void handle_destroy(Ghandles * g, struct genlist *l)
{
	struct genlist *l2;
	struct conndata *item = l->data;
	windows_count--;
	if (item == last_input_window)
		last_input_window = NULL;
	XDestroyWindow(g->display, item->local_winid);
	fprintf(stderr, " XDestroyWindow 0x%x\n", (int) item->local_winid);
	if (item->image)
		release_mapped_mfns(g, item);
	l2 = list_lookup(wid2conndata, item->local_winid);
	list_remove(l);
	list_remove(l2);
}

void sanitize_string_from_vm(unsigned char *s)
{
	static unsigned char allowed_chars[] = { '-', '_', ' ', '.' };
	int i, ok;
	for (; *s; s++) {
		if (*s >= '0' || *s <= '9')
			continue;
		if (*s >= 'a' || *s <= 'z')
			continue;
		if (*s >= 'A' || *s <= 'Z')
			continue;
		ok = 0;
		for (i = 0; i < sizeof(allowed_chars); i++)
			if (*s == allowed_chars[i])
				ok = 1;
		if (!ok)
			*s = '_';
	}
}

void fix_menu(struct conndata *item, struct conndata *originator)
{
#ifndef REPARENT_MENUS
	int ret, absx, absy;
	Window dummy;
	XWindowAttributes oattr;
#endif
	int newx, newy;
	XSetWindowAttributes attr;
	attr.override_redirect = 1;
	XChangeWindowAttributes(ghandles.display, item->local_winid,
				CWOverrideRedirect, &attr);
	item->override_redirect = 1;
#ifdef REPARENT_MENUS
	newx = item->remote_x - originator->remote_x;
	newy = item->remote_y - originator->remote_y;
// do not allow menu windows to hide the color frame
	if (newx < 0)
		newx = 0;
	if (newy < 0)
		newy = 0;
	XReparentWindow(ghandles.display, item->local_winid,
			originator->local_winid, newx, newy);
#else
	ret = XGetWindowAttributes(ghandles.display,
				   originator->local_winid, &oattr);
	XTranslateCoordinates(ghandles.display,
			      originator->local_winid, oattr.root, 0, 0,
			      &absx, &absy, &dummy);
	fprintf(stderr,
		"move menu window ret=%d x=%d y=%d rx=%d ry=%d ox=%d oy=%d origid=0x%x\n",
		ret, absx, absy, item->remote_x, item->remote_y,
		originator->remote_x, originator->remote_y,
		(int) originator->local_winid);
	newx = item->remote_x - originator->remote_x + absx;
	newy = item->remote_y - originator->remote_y + absy;
// do not allow menu windows to hide the color frame
	if (newx < 0)
		newx = 0;
	if (newy < 0)
		newy = 0;
	XMoveWindow(ghandles.display, item->local_winid, newx, newy);
#endif
}

void handle_wmname(Ghandles * g, struct conndata *item)
{
	XTextProperty text_prop;
	struct msg_wmname msg;
	char buf[sizeof(msg.data) + 1];
	char *list[1] = { buf };

	read_struct(msg);
	msg.data[sizeof(msg.data) - 1] = 0;
	sanitize_string_from_vm((unsigned char *) (msg.data));
	snprintf(buf, sizeof(buf), "%s", msg.data);
	fprintf(stderr, "set title for window 0x%x to %s\n",
		(int) item->local_winid, buf);
	XmbTextListToTextProperty(g->display, list, 1, XStringStyle,
				  &text_prop);
	XSetWMName(g->display, item->local_winid, &text_prop);
	XSetWMIconName(g->display, item->local_winid, &text_prop);
	XFree(text_prop.value);
}

void handle_map(struct conndata *item)
{
	struct genlist *trans;
	struct msg_map_info txt;
	item->is_mapped = 1;
	read_struct(txt);
	if (txt.transient_for
	    && (trans = list_lookup(remote2local, txt.transient_for))) {
		struct conndata *transdata = trans->data;
		item->transient_for = transdata;
		XSetTransientForHint(ghandles.display, item->local_winid,
				     transdata->local_winid);
	} else
		item->transient_for = 0;
	item->override_redirect = 0;
	if (txt.override_redirect) {
		if (item->transient_for) {
			struct conndata *recursive_orig =
			    item->transient_for;
#ifdef REPARENT_MENUS
			while (recursive_orig->transient_for)
				recursive_orig =
				    recursive_orig->transient_for;
#endif
			fix_menu(item, recursive_orig);
		} else if (last_input_window) {
			fix_menu(item, last_input_window);
			fprintf(stderr,
				"Desperately setting the originator of override_redirect window 0x%x(remote 0x%x) to last_input_window 0x%x(remote 0x%x)\n",
				(int) item->local_winid,
				(int) item->remote_winid,
				(int) last_input_window->local_winid,
				(int) last_input_window->remote_winid);
		}
	}
	(void) XMapWindow(ghandles.display, item->local_winid);
}

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

void release_mapped_mfns(Ghandles * g, struct conndata *item)
{
	inter_appviewer_lock(1);
	g->shmcmd->shmid = item->shminfo.shmid;
	XShmDetach(g->display, &item->shminfo);
	XDestroyImage(item->image);
	XSync(g->display, False);
	inter_appviewer_lock(0);
	item->image = NULL;
}

char dummybuf[100];
void handle_mfndump(Ghandles * g, struct conndata *item)
{
	char shmcmd_data_from_remote[4096 * SHM_CMD_NUM_PAGES];
	struct shm_cmd *tmp_shmcmd =
	    (struct shm_cmd *) shmcmd_data_from_remote;
	if (item->image)
		release_mapped_mfns(g, item);
	read_data(shmcmd_data_from_remote, sizeof(struct shm_cmd));
	if (tmp_shmcmd->num_mfn > MAX_MFN_COUNT) {
		fprintf(stderr, "got num_mfn=0x%x\n", tmp_shmcmd->num_mfn);
		pause();
		exit(1);
	}
	item->image_width = tmp_shmcmd->width;
	item->image_height = tmp_shmcmd->height;
	if (item->image_width < 0 || item->image_height < 0) {
		fprintf(stderr,
			"handle_mfndump for window 0x%x(remote 0x%x) got w/h %d %d\n",
			(int) item->local_winid, (int) item->remote_winid,
			item->image_width, item->image_height);
		exit(1);
	}
	if (item->image_width > MAX_WINDOW_WIDTH)	//XXX should we abort ?
		item->image_width = MAX_WINDOW_WIDTH;
	if (item->image_height > MAX_WINDOW_HEIGHT)
		item->image_height = MAX_WINDOW_HEIGHT;
	if (tmp_shmcmd->off >= 4096) {
		fprintf(stderr,
			"handle_mfndump for window 0x%x(remote 0x%x) got shmcmd->off 0x%x\n",
			(int) item->local_winid, (int) item->remote_winid,
			tmp_shmcmd->off);
		exit(1);
	}
	read_data((char *) tmp_shmcmd->mfns,
		  SIZEOF_SHARED_MFN * tmp_shmcmd->num_mfn);
	item->image =
	    XShmCreateImage(g->display,
			    DefaultVisual(g->display, g->screen), 24,
			    ZPixmap, NULL, &item->shminfo,
			    item->image_width, item->image_height);
	if (!item->image) {
		perror("XShmCreateImage");
		exit(1);
	}
	if (tmp_shmcmd->num_mfn * 4096 <
	    item->image->bytes_per_line * item->image->height +
	    tmp_shmcmd->off) {
		fprintf(stderr,
			"handle_mfndump for window 0x%x(remote 0x%x)"
			" got too small num_mfn= 0x%x\n",
			(int) item->local_winid, (int) item->remote_winid,
			tmp_shmcmd->num_mfn);
		exit(1);
	}
	// temporary shmid; see shmoverride/README
	item->shminfo.shmid = shmget(IPC_PRIVATE, 1, IPC_CREAT | 0700);
	if (item->shminfo.shmid < 0) {
		perror("shmget");
		exit(1);
	}
	tmp_shmcmd->shmid = item->shminfo.shmid;
	tmp_shmcmd->domid = g->domid;
	inter_appviewer_lock(1);
	memcpy(g->shmcmd, shmcmd_data_from_remote,
	       4096 * SHM_CMD_NUM_PAGES);
	item->shminfo.shmaddr = item->image->data = dummybuf;
	item->shminfo.readOnly = True;
	XSync(g->display, False);
	if (!XShmAttach(g->display, &item->shminfo)) {
		fprintf(stderr,
			"XShmAttach failed for window 0x%x(remote 0x%x)\n",
			(int) item->local_winid, (int) item->remote_winid);
	}
	XSync(g->display, False);
	g->shmcmd->shmid = g->cmd_shmid;
	inter_appviewer_lock(0);
	shmctl(item->shminfo.shmid, IPC_RMID, 0);
}

void handle_message()
{
	struct msghdr hdr;
	struct genlist *l;
	struct conndata *item = 0;
	read_struct(hdr);
	if (hdr.type <= MSG_MIN || hdr.type >= MSG_MAX) {
		fprintf(stderr, "msg type 0x%x window 0x%x\n",
			hdr.type, hdr.window);
		exit(1);
	}
	if (hdr.type == MSG_CLIPBOARD_DATA) {
		handle_clipboard_data(hdr.window);
		return;
	}
	l = list_lookup(remote2local, hdr.window);
	if (hdr.type == MSG_CREATE) {
		if (l) {
			fprintf(stderr,
				"CREATE for already existing window id 0x%x?\n",
				hdr.window);
			exit(1);
		}
	} else {
		if (!l) {
			fprintf(stderr,
				"msg 0x%x without CREATE for 0x%x\n",
				hdr.type, hdr.window);
			exit(1);
		}
		item = l->data;
	}

	switch (hdr.type) {
	case MSG_CREATE:
		handle_create(hdr.window);
		break;
	case MSG_DESTROY:
		handle_destroy(&ghandles, l);
		break;
	case MSG_MAP:
		handle_map(item);
		break;
	case MSG_UNMAP:
		item->is_mapped = 0;
		(void) XUnmapWindow(ghandles.display, item->local_winid);
		break;
	case MSG_CONFIGURE:
		handle_configure_from_vm(&ghandles, item);
		break;
	case MSG_MFNDUMP:
		handle_mfndump(&ghandles, item);
		break;
	case MSG_SHMIMAGE:
		handle_shmimage(&ghandles, item);
		break;
	case MSG_WMNAME:
		handle_wmname(&ghandles, item);
		break;
	default:
		fprintf(stderr, "got msg type %d\n", hdr.type);
		exit(1);
	}
}

void send_cmd_to_vm(char *cmd)
{
	struct msghdr hdr;
	struct msg_execute exec_data;

	hdr.type = MSG_EXECUTE;
	hdr.window = 0;
	strncpy(exec_data.cmd, cmd, sizeof(exec_data.cmd));
	exec_data.cmd[sizeof(exec_data.cmd) - 1] = 0;
	write_message(hdr, exec_data);
}

static int volatile signal_caught;
void dummy_signal_handler(int x)
{
	signal_caught = 1;
}

void print_backtrace(void)
{
	void *array[100];
	size_t size;
	char **strings;
	size_t i;

	size = backtrace(array, 100);
	strings = backtrace_symbols(array, size);

	fprintf(stderr, "Obtained %zd stack frames.\n", size);

	for (i = 0; i < size; i++)
		printf("%s\n", strings[i]);

	free(strings);
}

void release_all_mapped_mfns()
{
	struct genlist *curr;
	fprintf(stderr, "release_all_mapped_mfns running\n");
	print_backtrace();
	for (curr = wid2conndata->next; curr != wid2conndata;
	     curr = curr->next) {
		struct conndata *item = curr->data;
		if (item->image)
			release_mapped_mfns(&ghandles, item);
	}
}

void exec_pacat(int domid)
{
	int i, fd;
	char domid_txt[20];
	char logname[80];
	snprintf(domid_txt, sizeof domid_txt, "%d", domid);
	snprintf(logname, sizeof logname, "/var/log/qubes/pacat.%d.log",
		 domid);
	switch (fork()) {
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
	default:;
	}
}

void send_xconf(Ghandles * g)
{
	struct msg_xconf xconf;
	XWindowAttributes attr;
	XGetWindowAttributes(g->display, g->root_win, &attr);
	xconf.w = attr.width;
	xconf.h = attr.height;
	xconf.depth = attr.depth;
	xconf.mem = xconf.w * xconf.h * 4 / 1024 + 1;
	write_struct(xconf);
}

void get_protocol_version()
{
	uint32_t version;
	char message[1024];
	read_struct(version);
	if (version == QUBES_GUID_PROTOCOL_VERSION)
		return;
	snprintf(message, sizeof message, "kdialog --sorry \"The remote "
		 "protocol version is %d, the local protocol version is %d. Upgrade "
		 "qubes-gui-dom0 (in dom0) and qubes-gui-vm (in template VM) packages "
		 "so that they provide compatible/latest software. You can run 'xm console "
		 "vmname' (as root) to access shell prompt in the VM.\"",
		 version, QUBES_GUID_PROTOCOL_VERSION);
	system(message);
	exit(1);
}

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

void wait_for_connection_in_parent(int *pipe_notify)
{
	// inside the parent process
	// wait for daemon to get connection with AppVM
	struct pollfd pipe_pollfd;
	int tries, ret;

	fprintf(stderr, "Connecting to VM's GUI agent: ");
	close(pipe_notify[1]);	// close the writing end
	pipe_pollfd.fd = pipe_notify[0];
	pipe_pollfd.events = POLLIN;

	for (tries = 0;; tries++) {
		fprintf(stderr, ".");
		ret = poll(&pipe_pollfd, 1, 1000);
		if (ret < 0) {
			perror("poll");
			exit(1);
		}
		if (ret > 0) {
			if (pipe_pollfd.revents == POLLIN)
				break;
			fprintf(stderr, "exiting\n");
			exit(1);
		}
		if (tries >= 45) {
			fprintf(stderr,
				"\nHmm... this takes more time than usual --"
				" is the VM running?\n");
			fprintf(stderr, "Connecting to VM's GUI agent: ");
			tries = 0;
		}

	}
	fprintf(stderr, "connected\n");
	exit(0);
}

#if 0
void setup_icon(char *xpmfile)
{
	Pixmap dummy;
	XpmAttributes attributes;
	int ret;
	Display *dis = ghandles.display;
	attributes.valuemask = XpmColormap;
	attributes.x_hotspot = 0;
	attributes.y_hotspot = 0;
	attributes.depth = DefaultDepth(dis, DefaultScreen(dis));
	attributes.colormap = DefaultColormap(dis, DefaultScreen(dis));
	attributes.valuemask = XpmSize;

	ret = XpmReadFileToPixmap
	    (dis, ghandles.root_win, xpmfile, &ghandles.icon,
	     &dummy, &attributes);
	if (ret != XpmSuccess) {
		fprintf(stderr, "XpmReadFileToPixmap returned %d\n", ret);
		ghandles.icon = None;
	}
}
#endif
void usage()
{
	fprintf(stderr,
		"usage: qubes_quid -d domain_id [-e command] [-c color] [-l label_index] [-i icon name, no suffix]\n");
}

void parse_cmdline(int argc, char **argv)
{
	int opt;
	while ((opt = getopt(argc, argv, "d:e:c:l:i:")) != -1) {
		switch (opt) {
		case 'd':
			ghandles.domid = atoi(optarg);
			break;
		case 'e':
			ghandles.execute_cmd_in_vm = 1;
			strncpy(ghandles.cmd_for_vm,
				optarg, sizeof(ghandles.cmd_for_vm));
			break;
		case 'c':
			ghandles.cmdline_color = optarg;
			break;
		case 'l':
			ghandles.label_index = strtoul(optarg, 0, 0);
			break;
		case 'i':
			ghandles.cmdline_icon = optarg;
			break;
		default:
			usage();
			exit(1);
		}
	}
	if (!ghandles.domid) {
		fprintf(stderr, "domid=0?");
		exit(1);
	}
}

int main(int argc, char **argv)
{
	int xfd, cmd_socket;
	char *vmname;
	FILE *f;
	int childpid;
	int pipe_notify[2];
	char dbg_log[256];
	int logfd;
	parse_cmdline(argc, argv);


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
	ghandles.shmcmd = shmat(ghandles.cmd_shmid, 0, 0);
	if (ghandles.shmcmd == (void *) (-1UL)) {
		fprintf(stderr,
			"Invalid or stale shm id 0x%x in /var/run/shm.id\n",
			ghandles.cmd_shmid);
		exit(1);
	}

	close(0);
	snprintf(dbg_log, sizeof(dbg_log),
		 "/var/log/qubes/qubes.%d.log", ghandles.domid);
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
	get_frame_gc(&ghandles, ghandles.cmdline_color ? : "red");
#if 0
	if (ghandles.cmdline_icon)
		setup_icon(ghandles.cmdline_icon);
	else
		ghandles.icon = None;
#endif
	remote2local = list_new();
	wid2conndata = list_new();
	XSetErrorHandler(dummy_handler);
	vmname = peer_client_init(ghandles.domid, 6000);
	exec_pacat(ghandles.domid);
	setuid(getuid());
	cmd_socket = get_cmd_socket(ghandles.domid);
	write(pipe_notify[1], "Q", 1);	// let the parent know we connected sucessfully

	signal(SIGTERM, dummy_signal_handler);
	atexit(release_all_mapped_mfns);

	strncpy(ghandles.vmname, vmname, sizeof(ghandles.vmname));
	xfd = ConnectionNumber(ghandles.display);

	get_protocol_version();
	send_xconf(&ghandles);

	if (ghandles.execute_cmd_in_vm) {
		fprintf(stderr,
			"Sending cmd to execute: %s\n",
			ghandles.cmd_for_vm);
		send_cmd_to_vm(ghandles.cmd_for_vm);
	}

	for (;;) {
		int select_fds[2] = { xfd, cmd_socket };
		fd_set retset;
		int busy;
		if (signal_caught) {
			fprintf(stderr, "exiting on signal...\n");
			exit(0);
		}
		do {
			busy = 0;
			if (XPending(ghandles.display)) {
				process_xevent();
				busy = 1;
			}
			if (read_ready()) {
				handle_message();
				busy = 1;
			}
		} while (busy);
		wait_for_vchan_or_argfd(2, select_fds, &retset);
		if (FD_ISSET(cmd_socket, &retset)) {
			char *cmd = get_line_from_cmd_socket(cmd_socket);
			if (cmd)
				send_cmd_to_vm(cmd);
		}
	}
	return 0;
}
