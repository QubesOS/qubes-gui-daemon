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
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <X11/Xlib.h>
#include <X11/Intrinsic.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/Xlibint.h>
#include <X11/Xatom.h>
#include "messages.h"
#include "txrx.h"
#include "list.h"
#include "error.h"
#include "shm_cmd.h"
#include "qlimits.h"
#include "libvchan.h"
#include "u2mfnlib.h"

int damage_event, damage_error;
struct _global_handles {
	Display *display;
	int screen;		/* shortcut to the default screen */
	Window root_win;	/* root attributes */
	GC context;
	Atom wmDeleteMessage;
	Atom wmProtocols;
	int xserver_fd;
	Window clipboard_win;
	unsigned char *clipboard_data;
	unsigned int clipboard_data_len;
};

struct genlist *windows_list;
typedef struct _global_handles Ghandles;

#define SKIP_NONMANAGED_WINDOW if (!list_lookup(windows_list, window)) return

void process_xevent_damage(Ghandles * g, XID window,
			   int x, int y, int width, int height)
{
	struct msg_shmimage mx;
	struct msghdr hdr;
	static int cnt = 0;
	SKIP_NONMANAGED_WINDOW;

	cnt++;
	if (0 || cnt == 50) {
		fprintf(stderr, "update_pixmap (one in 50) for 0x%x "
			"x=%d y=%d w=%d h=%d\n",
			(int) window, x, y, width, height);
		cnt = 0;
	}
	hdr.type = MSG_SHMIMAGE;
	hdr.window = window;
	mx.x = x;
	mx.y = y;
	mx.width = width;
	mx.height = height;
	write_message(hdr, mx);
}

void process_xevent_createnotify(Ghandles * g, XCreateWindowEvent * ev)
{
	struct msghdr hdr;
	struct msg_create crt;
	XWindowAttributes attr;
	int ret;
	ret = XGetWindowAttributes(g->display, ev->window, &attr);
	if (ret != 1) {
		fprintf(stderr, "XGetWindowAttributes for 0x%x failed in "
			"handle_create, ret=0x%x\n", (int) ev->window,
			ret);
		return;
	};

//      fprintf(stderr, "XDamageCreate for 0x%x class 0x%x\n",
//              (int) window, attr.class);
	if (list_lookup(windows_list, ev->window)) {
		fprintf(stderr, "CREATE for already existing 0x%x\n",
			(int) ev->window);
		return;
	}
	list_insert(windows_list, ev->window, 0);
	if (attr.class != InputOnly)
		XDamageCreate(g->display, ev->window,
			      XDamageReportRawRectangles);
	// the following hopefully avoids missed damage events
	XSync(g->display, False);
	XSelectInput(g->display, ev->window, PropertyChangeMask);
	hdr.type = MSG_CREATE;
	hdr.window = ev->window;
	crt.width = ev->width;
	crt.height = ev->height;
	crt.parent = ev->parent;
	crt.x = ev->x;
	crt.y = ev->y;
	crt.override_redirect = ev->override_redirect;
	write_struct(hdr);
	write_struct(crt);
}

void feed_xdriver(Ghandles * g, int type, int arg1, int arg2)
{
	char buf[256];
	int ret;
	snprintf(buf, sizeof(buf), "%c 0x%x 0x%x\n", (char) type, arg1,
		 arg2);
	if (write(g->xserver_fd, buf, strlen(buf)) != strlen(buf)) {
		perror("unix write");
		exit(1);
	}
	buf[0] = '1';
	ret = read(g->xserver_fd, buf, 1);
	if (ret != 1 || buf[0] != '0') {
		perror("unix read");
		fprintf(stderr, "read returned %d, char read=0x%x\n", ret,
			(int) buf[0]);
		exit(1);
	}
}

void read_discarding(int fd, int size)
{
	char buf[1024];
	int n, count, total = 0;
	while (total < size) {
		if (size > sizeof(buf))
			count = sizeof(buf);
		else
			count = size;
		n = read(fd, buf, count);
		if (n < 0) {
			perror("read_discarding");
			exit(1);
		}
		if (n == 0) {
			fprintf(stderr, "EOF in read_discarding\n");
			exit(1);
		}
		total += n;
	}
}

void send_pixmap_mfns(Ghandles * g, XID window)
{
	struct shm_cmd shmcmd;
	struct msghdr hdr;
	uint32_t *mfnbuf;
	int ret, rcvd = 0, size;

	feed_xdriver(g, 'W', (int) window, 0);
	if (read(g->xserver_fd, &shmcmd, sizeof(shmcmd)) != sizeof(shmcmd)) {
		perror("unix read shmcmd");
		exit(1);
	}
	if (shmcmd.num_mfn == 0 || shmcmd.num_mfn > MAX_MFN_COUNT) {
		fprintf(stderr, "got num_mfn=0x%x for window 0x%x\n",
			shmcmd.num_mfn, (int) window);
		read_discarding(g->xserver_fd,
				shmcmd.num_mfn * sizeof(*mfnbuf));
		return;
	}
	size = shmcmd.num_mfn * sizeof(*mfnbuf);
	mfnbuf = alloca(size);
	while (rcvd < size) {
		ret =
		    read(g->xserver_fd, ((char *) mfnbuf) + rcvd,
			 size - rcvd);
		if (ret == 0) {
			fprintf(stderr, "unix read EOF\n");
			exit(1);
		}
		if (ret < 0) {
			perror("unix read error");
			exit(1);
		}
		rcvd += ret;
	}
	hdr.type = MSG_MFNDUMP;
	hdr.window = window;
	write_message(hdr, shmcmd);
	write_data((char *) mfnbuf, size);
}

void getwmname_tochar(Ghandles * g, XID window, char *outbuf, int bufsize)
{
	XTextProperty text_prop_return;
	char **list;
	int count;

	outbuf[0] = 0;
	if (!XGetWMName(g->display, window, &text_prop_return) ||
	    !text_prop_return.value || !text_prop_return.nitems)
		return;
	if (XmbTextPropertyToTextList(g->display,
				      &text_prop_return, &list,
				      &count) < 0 || count <= 0
	    || !*list) {
		XFree(text_prop_return.value);
		return;
	}
	strncat(outbuf, list[0], bufsize);
	XFree(text_prop_return.value);
	XFreeStringList(list);
	fprintf(stderr, "got wmname=%s\n", outbuf);
}

void send_wmname(Ghandles * g, XID window)
{
	struct msghdr hdr;
	struct msg_wmname msg;
	getwmname_tochar(g, window, msg.data, sizeof(msg.data));
	hdr.window = window;
	hdr.type = MSG_WMNAME;
	write_message(hdr, msg);
}

void process_xevent_map(Ghandles * g, XID window)
{
	XWindowAttributes attr;
	struct msghdr hdr;
	struct msg_map_info map_info;
	Window transient;
	SKIP_NONMANAGED_WINDOW;

	send_pixmap_mfns(g, window);
	XGetWindowAttributes(g->display, window, &attr);
	if (XGetTransientForHint(g->display, window, &transient))
		map_info.transient_for = transient;
	else
		map_info.transient_for = 0;
	map_info.override_redirect = attr.override_redirect;
	hdr.type = MSG_MAP;
	hdr.window = window;
	write_struct(hdr);
	write_struct(map_info);
	send_wmname(g, window);
//      process_xevent_damage(g, window, 0, 0, attr.width, attr.height);
}

void process_xevent_unmap(Ghandles * g, XID window)
{
	struct msghdr hdr;
	hdr.type = MSG_UNMAP;
	SKIP_NONMANAGED_WINDOW;
	hdr.window = window;
	write_struct(hdr);
}

void process_xevent_destroy(Ghandles * g, XID window)
{
	struct msghdr hdr;
	struct genlist *l;
	SKIP_NONMANAGED_WINDOW;
	fprintf(stderr, "handle destroy 0x%x\n", (int) window);
	hdr.type = MSG_DESTROY;
	hdr.window = window;
	write_struct(hdr);
	l = list_lookup(windows_list, window);
	list_remove(l);
}

void process_xevent_configure(Ghandles * g, XID window,
			      XConfigureEvent * ev)
{
	struct msghdr hdr;
	struct msg_configure conf;
	SKIP_NONMANAGED_WINDOW;
	fprintf(stderr, "handle configure event 0x%x w=%d h=%d ovr=%d\n",
		(int) window, ev->width, ev->height,
		(int) ev->override_redirect);
	hdr.type = MSG_CONFIGURE;
	hdr.window = window;
	conf.x = ev->x;
	conf.y = ev->y;
	conf.width = ev->width;
	conf.height = ev->height;
	conf.override_redirect = ev->override_redirect;
	write_struct(hdr);
	write_struct(conf);
	send_pixmap_mfns(g, window);
}

void send_clipboard_data(char *data, int len)
{
	struct msghdr hdr;
	fprintf(stderr, "sending clipboard data\n");
	hdr.type = MSG_CLIPBOARD_DATA;
	if (len > MAX_CLIPBOARD_SIZE)
		hdr.window = MAX_CLIPBOARD_SIZE;
	else
		hdr.window = len;
	write_struct(hdr);
	write_data((char *) data, len);
}

void process_xevent_selection(Ghandles * g, XSelectionEvent * ev)
{
	int format, result;
	Atom type;
	unsigned long len, bytes_left, dummy;
	unsigned char *data;
	Atom Qprop = XInternAtom(g->display, "QUBES_SELECTION", False);
	fprintf(stderr, "selection event\n");
	if (ev->requestor != g->clipboard_win || ev->property != Qprop)
		return;
	XGetWindowProperty(g->display, ev->requestor, Qprop, 0, 0, 0,
			   AnyPropertyType, &type, &format, &len,
			   &bytes_left, &data);
	if (bytes_left <= 0)
		return;
	result =
	    XGetWindowProperty(g->display, ev->requestor, Qprop, 0,
			       bytes_left, 0,
			       AnyPropertyType, &type,
			       &format, &len, &dummy, &data);
	if (result != Success)
		return;
	send_clipboard_data((char *) data, len);
	XFree(data);

}

void process_xevent_selection_req(Ghandles * g,
				  XSelectionRequestEvent * req)
{
	XSelectionEvent resp;
	Atom Targets = XInternAtom(g->display, "TARGETS", False);
	fprintf(stderr, "selection req event\n");
	if (req->target == XA_STRING) {
		XChangeProperty(g->display,
				req->requestor,
				req->property,
				XA_STRING,
				8,
				PropModeReplace,
				g->clipboard_data, g->clipboard_data_len);
		resp.property = req->property;
	} else if (req->target == Targets) {
		static Atom tmp = XA_STRING;
		XChangeProperty(g->display,
				req->requestor,
				req->property,
				Targets,
				32, PropModeReplace, (unsigned char *)
				&tmp, sizeof(tmp));
		resp.property = req->property;
	} else {
		fprintf(stderr,
			"Not supported selection_req target 0x%x %s\n",
			(int) req->target, XGetAtomName(g->display,
							req->target));
		resp.property = None;
	}
	resp.type = SelectionNotify;
	resp.display = req->display;
	resp.requestor = req->requestor;
	resp.selection = req->selection;
	resp.target = req->target;
	resp.time = req->time;
	XSendEvent(g->display, req->requestor, 0, 0, (XEvent *) & resp);
}

void process_xevent_property(Ghandles * g, XID window, XPropertyEvent * ev)
{
	SKIP_NONMANAGED_WINDOW;
	fprintf(stderr, "handle property %s for window 0x%x\n",
		XGetAtomName(g->display, ev->atom), (int) ev->window);
	if (ev->atom != XInternAtom(g->display, "WM_NAME", False))
		return;
	send_wmname(g, window);
}

void process_xevent(Ghandles * g)
{
	XDamageNotifyEvent *dev;
	XEvent event_buffer;
	XNextEvent(g->display, &event_buffer);
	switch (event_buffer.type) {
	case CreateNotify:
		process_xevent_createnotify(g, (XCreateWindowEvent *)
					    & event_buffer);
		break;
	case DestroyNotify:
		process_xevent_destroy(g,
				       event_buffer.xdestroywindow.window);
		break;
	case MapNotify:
		process_xevent_map(g, event_buffer.xmap.window);
		break;
	case UnmapNotify:
		process_xevent_unmap(g, event_buffer.xmap.window);
		break;
	case ConfigureNotify:
		process_xevent_configure(g,
					 event_buffer.xconfigure.window,
					 (XConfigureEvent *) &
					 event_buffer);
		break;
	case SelectionNotify:
		process_xevent_selection(g,
					 (XSelectionEvent *) &
					 event_buffer);
		break;
	case SelectionRequest:
		process_xevent_selection_req(g,
					     (XSelectionRequestEvent *) &
					     event_buffer);
		break;
	case PropertyNotify:
		process_xevent_property(g, event_buffer.xproperty.window,
					(XPropertyEvent *) & event_buffer);
		break;
	default:
		if (event_buffer.type == (damage_event + XDamageNotify)) {
			dev = (XDamageNotifyEvent *) & event_buffer;
//      fprintf(stderr, "x=%hd y=%hd gx=%hd gy=%hd w=%hd h=%hd\n",
//        dev->area.x, dev->area.y, dev->geometry.x, dev->geometry.y, dev->area.width, dev->area.height); 
			process_xevent_damage(g, dev->drawable,
					      dev->area.x,
					      dev->area.y,
					      dev->area.width,
					      dev->area.height);
//                      fprintf(stderr, "@");
		} else
			fprintf(stderr, "#");
	}

}

extern void wait_for_unix_socket(int *fd);

void mkghandles(Ghandles * g)
{
	wait_for_unix_socket(&g->xserver_fd); // wait for Xorg qubes_drv to connect to us
	g->display = XOpenDisplay(NULL);
	if (!g->display) {
		perror("XOpenDisplay");
		exit(1);
	}
	fprintf(stderr, "Connection to local X server established.\n");
	g->screen = DefaultScreen(g->display);	/* get CRT id number */
	g->root_win = RootWindow(g->display, g->screen);	/* get default attributes */
	g->context = XCreateGC(g->display, g->root_win, 0, NULL);
	g->wmDeleteMessage =
	    XInternAtom(g->display, "WM_DELETE_WINDOW", False);
	g->wmProtocols = XInternAtom(g->display, "WM_PROTOCOLS", False);
	g->clipboard_win = XCreateSimpleWindow(g->display, g->root_win,
					       0, 0, 1, 1,
					       0, BlackPixel(g->display,
							     g->screen),
					       WhitePixel(g->display,
							  g->screen));
	g->clipboard_data = NULL;
	g->clipboard_data_len = 0;
}

void handle_keypress(Ghandles * g, XID winid)
{
	struct msg_keypress key;
//      XKeyEvent event;
//        char buf[256];
	read_data((char *) &key, sizeof(key));
#if 0
//XGetInputFocus(g->display, &focus_return, &revert_to_return);
//      fprintf(stderr, "vmside: type=%d keycode=%d currfoc=0x%x\n", key.type,
//              key.keycode, (int)focus_return);

//      XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = 1;
	event.y_root = 1;
	event.same_screen = TRUE;
	event.type = key.type;
	event.keycode = key.keycode;
	event.state = key.state;
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   KeyPressMask, (XEvent *) & event);
#else
	feed_xdriver(g, 'K', key.keycode, key.type == KeyPress ? 1 : 0);
#endif
//      fprintf(stderr, "win 0x%x type %d keycode %d\n",
//              (int) winid, key.type, key.keycode);
//      XSync(g->display, 0);
}

void handle_button(Ghandles * g, XID winid)
{
	struct msg_button key;
//      XButtonEvent event;
	XWindowAttributes attr;
	int ret;

	read_data((char *) &key, sizeof(key));
	ret = XGetWindowAttributes(g->display, winid, &attr);
	if (ret != 1) {
		fprintf(stderr,
			"XGetWindowAttributes for 0x%x failed in "
			"do_button, ret=0x%x\n", (int) winid, ret);
		return;
	};

#if 0
	XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
//      XRaiseWindow(g->display, winid);
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = attr.x + key.x;
	event.y_root = attr.y + key.y;
	event.same_screen = TRUE;
	event.type = key.type;
	event.button = key.button;
	event.state = key.state;
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   ButtonPressMask, (XEvent *) & event);
//      XSync(g->display, 0);
#endif
	fprintf(stderr,
		"send buttonevent, win 0x%x type=%d button=%d\n",
		(int) winid, key.type, key.button);
	feed_xdriver(g, 'B', key.button, key.type == ButtonPress ? 1 : 0);
}

void handle_motion(Ghandles * g, XID winid)
{
	struct msg_motion key;
//      XMotionEvent event;
	XWindowAttributes attr;
	int ret;

	read_data((char *) &key, sizeof(key));
	ret = XGetWindowAttributes(g->display, winid, &attr);
	if (ret != 1) {
		fprintf(stderr,
			"XGetWindowAttributes for 0x%x failed in "
			"do_button, ret=0x%x\n", (int) winid, ret);
		return;
	};

#if 0
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = attr.x + key.x;
	event.y_root = attr.y + key.y;
	event.same_screen = TRUE;
	event.is_hint = key.is_hint;
	event.state = key.state;
	event.type = MotionNotify;
//      fprintf(stderr, "motion notify for 0x%x\n", (int)winid);
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   0, (XEvent *) & event);
//      XSync(g->display, 0);
#endif
	feed_xdriver(g, 'M', attr.x + key.x, attr.y + key.y);
}

// msg_crossing is not processed currently; just eat the message and return
void handle_crossing(Ghandles * g, XID winid)
{
	struct msg_crossing key;
	XCrossingEvent event;
	XWindowAttributes attr;
	int ret;

	read_data((char *) &key, sizeof(key));
	return;
	
	ret = XGetWindowAttributes(g->display, winid, &attr);
	if (ret != 1) {
		fprintf(stderr,
			"XGetWindowAttributes for 0x%x failed in "
			"do_button, ret=0x%x\n", (int) winid, ret);
		return;
	};

//XGetInputFocus(g->display, &focus_return, &revert_to_return);
//      fprintf(stderr, "vmside: type=%d keycode=%d currfoc=0x%x\n", key.type,
//              key.keycode, (int)focus_return);

//      XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
	event.display = g->display;
	event.window = winid;
	event.root = g->root_win;
	event.subwindow = None;
	event.time = CurrentTime;
	event.x = key.x;
	event.y = key.y;
	event.x_root = attr.x + key.x;
	event.y_root = attr.y + key.y;
	event.type = key.type;
	event.same_screen = TRUE;
	event.mode = key.mode;
	event.detail = key.detail;
	event.focus = key.focus;
	event.state = key.state;

//      fprintf(stderr, "motion notify for 0x%x\n", (int)winid);
	XSendEvent(event.display, event.window, TRUE,
//                 event.type==KeyPress?KeyPressMask:KeyReleaseMask, 
		   0, (XEvent *) & event);
//      XSync(g->display, 0);
}

void handle_focus(Ghandles * g, XID winid)
{
	struct msg_focus key;
//      XFocusChangeEvent event;

	read_data((char *) &key, sizeof(key));
#if 0
	event.display = g->display;
	event.window = winid;
	event.type = key.type;
	event.mode = key.mode;
	event.detail = key.detail;

	fprintf(stderr, "send focuschange for 0x%x type %d\n",
		(int) winid, key.type);
	XSendEvent(event.display, event.window, TRUE,
		   0, (XEvent *) & event);
#endif
	if (key.type == FocusIn && (key.mode == NotifyNormal || key.mode == NotifyUngrab)) {
		XRaiseWindow(g->display, winid);
		XSetInputFocus(g->display, winid, RevertToParent,
			       CurrentTime);
		fprintf(stderr, "0x%x raised\n", (int) winid);
	}
}

int bitset(unsigned char *keys, int num)
{
	return (keys[num / 8] >> (num % 8)) & 1;
}

void handle_keymap_notify(Ghandles * g)
{
	int i;
	unsigned char remote_keys[32], local_keys[32];
	read_struct(remote_keys);
	XQueryKeymap(g->display, (char *) local_keys);
	for (i = 0; i < 256; i++) {
		if (!bitset(remote_keys, i) && bitset(local_keys, i)) {
			feed_xdriver(g, 'K', i, 0);
			fprintf(stderr,
				"handle_keymap_notify: unsetting key %d\n",
				i);
		}
	}
}


void handle_configure(Ghandles * g, XID winid)
{
	struct msg_configure r;
	XWindowAttributes attr;
	XGetWindowAttributes(g->display, winid, &attr);
	read_data((char *) &r, sizeof(r));
	XMoveResizeWindow(g->display, winid, r.x, r.y, r.width, r.height);
	fprintf(stderr, "configure msg, x/y %d %d (was %d %d), w/h %d %d (was %d %d)\n", 
	r.x, r.y, attr.x, attr.y, r.width,
		r.height, attr.width, attr.height);

}

void handle_close(Ghandles * g, XID winid)
{
	XClientMessageEvent ev;

	ev.type = ClientMessage;
	ev.display = g->display;
	ev.window = winid;
	ev.format = 32;
	ev.message_type = g->wmProtocols;
	ev.data.l[0] = g->wmDeleteMessage;
//        XSetInputFocus(g->display, winid, RevertToParent, CurrentTime);
	XSendEvent(ev.display, ev.window, TRUE, 0, (XEvent *) & ev);
	fprintf(stderr, "wmDeleteMessage sent for 0x%x\n", (int) winid);
}

void do_execute(char *user, char *cmd)
{
	int i, fd;
	switch (fork()) {
	case -1:
		perror("fork cmd");
		break;
	case 0:
		for (i = 0; i < 256; i++)
			close(i);
		fd = open("/dev/null", O_RDWR);
		for (i = 0; i <= 2; i++)
			dup2(fd, i);
		signal(SIGCHLD, SIG_DFL);
		if (user)
			execl("/bin/su", "su", "-", user, "-c", cmd, NULL);
		else
			execl("/bin/bash", "bash", "-c", cmd, NULL);
		perror("execl cmd");
		exit(1);
	default:;
	}
}

void handle_execute()
{
	char *ptr;
	struct msg_execute exec_data;
	read_data((char *) &exec_data, sizeof(exec_data));
	exec_data.cmd[sizeof(exec_data.cmd) - 1] = 0;
	ptr = index(exec_data.cmd, ':');
	if (!ptr)
		return;
	*ptr = 0;
	fprintf(stderr, "handle_execute(): cmd = %s:%s\n", exec_data.cmd,
		ptr + 1);
	do_execute(exec_data.cmd, ptr + 1);
}

#define CLIPBOARD_4WAY
void handle_clipboard_req(Ghandles * g, XID winid)
{
	Atom Clp;
	Atom QProp = XInternAtom(g->display, "QUBES_SELECTION", False);
	Window owner;
#ifdef CLIPBOARD_4WAY
	Clp = XInternAtom(g->display, "CLIPBOARD", False);
#else
	Clp = XA_PRIMARY;
#endif
	owner = XGetSelectionOwner(g->display, Clp);
	fprintf(stderr, "clipboard req, owner=0x%x\n", (int) owner);
	if (owner == None) {
		send_clipboard_data(NULL, 0);
		return;
	}
	XConvertSelection(g->display, Clp, XA_STRING, QProp,
			  g->clipboard_win, CurrentTime);
}

void handle_clipboard_data(Ghandles * g, int len)
{
	Atom Clp = XInternAtom(g->display, "CLIPBOARD", False);

	if (g->clipboard_data)
		free(g->clipboard_data);
	g->clipboard_data = malloc(len);
	if (!g->clipboard_data) {
		perror("malloc");
		exit(1);
	}
	g->clipboard_data_len = len;
	read_data((char *) g->clipboard_data, len);
	XSetSelectionOwner(g->display, XA_PRIMARY, g->clipboard_win,
			   CurrentTime);
	XSetSelectionOwner(g->display, Clp, g->clipboard_win, CurrentTime);
#ifndef CLIPBOARD_4WAY
	XSync(g->display, False);
	feed_xdriver(g, 'B', 2, 1);
	feed_xdriver(g, 'B', 2, 0);
#endif
}


void handle_message(Ghandles * g)
{
	struct msghdr hdr;
	read_data((char *) &hdr, sizeof(hdr));
	switch (hdr.type) {
	case MSG_KEYPRESS:
		handle_keypress(g, hdr.window);
		break;
	case MSG_CONFIGURE:
		handle_configure(g, hdr.window);
		break;
	case MSG_BUTTON:
		handle_button(g, hdr.window);
		break;
	case MSG_MOTION:
		handle_motion(g, hdr.window);
		break;
	case MSG_CLOSE:
		handle_close(g, hdr.window);
		break;
	case MSG_CROSSING:
		handle_crossing(g, hdr.window);
		break;
	case MSG_FOCUS:
		handle_focus(g, hdr.window);
		break;
	case MSG_CLIPBOARD_REQ:
		handle_clipboard_req(g, hdr.window);
		break;
	case MSG_CLIPBOARD_DATA:
		handle_clipboard_data(g, hdr.window);
		break;
	case MSG_EXECUTE:
		handle_execute();
		break;
	case MSG_KEYMAP_NOTIFY:
		handle_keymap_notify(g);
		break;
	default:
		fprintf(stderr, "got msg type %d\n", hdr.type);
		exit(1);
	}
}

void get_xconf_and_run_x()
{
	struct msg_xconf xconf;
	char val[64];
	read_struct(xconf);
	snprintf(val, sizeof(val), "%d", xconf.w);
	setenv("W", val, 1);
	snprintf(val, sizeof(val), "%d", xconf.h);
	setenv("H", val, 1);
	snprintf(val, sizeof(val), "%d", xconf.mem);
	setenv("MEM", val, 1);
	snprintf(val, sizeof(val), "%d", xconf.depth);
	setenv("DEPTH", val, 1);
	do_execute(NULL, "/usr/bin/qubes_run_xorg.sh");
}

void send_protocol_version()
{
	uint32_t version = QUBES_GUID_PROTOCOL_VERSION;
	write_struct(version);
}

int main(int argc, char **argv)
{
	int i;
	int xfd;
	Ghandles g;

	peer_server_init(6000);
	send_protocol_version();
	get_xconf_and_run_x();
	mkghandles(&g);
	for (i = 0; i < ScreenCount(g.display); i++)
		XCompositeRedirectSubwindows(g.display,
					     RootWindow(g.display, i),
//                                           CompositeRedirectAutomatic);
					     CompositeRedirectManual);
	for (i = 0; i < ScreenCount(g.display); i++)
		XSelectInput(g.display, RootWindow(g.display, i),
			     SubstructureNotifyMask);


	if (!XDamageQueryExtension(g.display, &damage_event,
				   &damage_error)) {
		perror("XDamageQueryExtension");
		exit(1);
	}
	XAutoRepeatOff(g.display);
	signal(SIGCHLD, SIG_IGN);
	do_execute(NULL, "/usr/bin/start-pulseaudio-with-vchan");
	windows_list = list_new();
	XSetErrorHandler(dummy_handler);
	xfd = ConnectionNumber(g.display);
	for (;;) {
		int busy;
		do {
			busy = 0;
			if (XPending(g.display)) {
				process_xevent(&g);
				busy = 1;
			}
			while (read_ready()) {
				handle_message(&g);
				busy = 1;
			}
		} while (busy);
		wait_for_vchan_or_argfd(1, &xfd, NULL);
	}
	return 0;
}
