/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
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

/* Messagess are described here:
 * http://wiki.qubes-os.org/trac/wiki/GUIdocs
 */

#include <stdint.h>
#define QUBES_GUID_PROTOCOL_VERSION 5
struct msghdr {
	uint32_t type;
	uint32_t window;
};
enum {
	MSG_MIN = 123,
	MSG_KEYPRESS,
	MSG_BUTTON,
	MSG_MOTION,
	MSG_CROSSING,
	MSG_FOCUS,
	MSG_RESIZE,
	MSG_CREATE,
	MSG_DESTROY,
	MSG_MAP,
	MSG_UNMAP,
	MSG_CONFIGURE,
	MSG_MFNDUMP,
	MSG_SHMIMAGE,
	MSG_CLOSE,
	MSG_EXECUTE,
	MSG_CLIPBOARD_REQ,
	MSG_CLIPBOARD_DATA,
	MSG_WMNAME,
	MSG_KEYMAP_NOTIFY,
	MSG_DOCK,
	MSG_WINDOW_HINTS,
	MSG_WINDOW_FLAGS,
	MSG_MAX
};
/* VM -> Dom0, Dom0 -> VM */
struct msg_map_info {
	uint32_t transient_for;
	uint32_t override_redirect;
};

/* VM -> Dom0 */
struct msg_create {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;	/* size of image */
	uint32_t parent;
	uint32_t override_redirect;
};
/* Dom0 -> VM, obsolete */
struct msg_resize {
	uint32_t width;
	uint32_t height;	/* size of image */
};
/* Dom0 -> VM */
struct msg_keypress {
	uint32_t type;
	uint32_t x;
	uint32_t y;
	uint32_t state;
	uint32_t keycode;
};
/* Dom0 -> VM */
struct msg_button {
	uint32_t type;
	uint32_t x;
	uint32_t y;
	uint32_t state;
	uint32_t button;
};
/* Dom0 -> VM */
struct msg_motion {
	uint32_t x;
	uint32_t y;
	uint32_t state;
	uint32_t is_hint;
};
/* Dom0 -> VM */
struct msg_crossing {
	uint32_t type;
	uint32_t x;
	uint32_t y;
	uint32_t state;
	uint32_t mode;
	uint32_t detail;
	uint32_t focus;
};
/* VM -> Dom0, Dom0 -> VM */
struct msg_configure {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t override_redirect;
};
/* VM -> Dom0 */
struct msg_shmimage {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};
/* Dom0 -> VM */
struct msg_focus {
	uint32_t type;
	uint32_t mode;
	uint32_t detail;
};
/* Dom0 -> VM */
struct msg_execute {
	char cmd[255];
};
/* Dom0 -> VM */
struct msg_xconf {
	uint32_t w;
	uint32_t h;
	uint32_t depth;
	uint32_t mem;
};
/* VM -> Dom0 */
struct msg_wmname {
	char data[128];
};
/* Dom0 -> VM */
struct msg_keymap_notify {
	char keys[32];
};
/* VM -> Dom0 */
struct msg_window_hints {
	uint32_t flags;
	uint32_t min_width;
	uint32_t min_height;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t width_inc;
	uint32_t height_inc;
	uint32_t base_width;
	uint32_t base_height;
};
/* VM -> Dom0, Dom0 -> VM */
struct msg_window_flags {
	uint32_t flags_set;
	uint32_t flags_unset;
};
#define WINDOW_FLAG_FULLSCREEN			(1<<0)
#define WINDOW_FLAG_DEMANDS_ATTENTION	(1<<1)
