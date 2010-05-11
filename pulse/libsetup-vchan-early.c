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

#include <pwd.h>
#include <libvchan.h>
#include <stdio.h>
#include <stdlib.h>
#include <grp.h>
#include <unistd.h>
#include "qubes-vchan-sink.h"
static struct libvchan *ctrl;
int __attribute__ ((constructor)) initfunc()
{
	char *username = getenv("DROPTOUSER");
	struct passwd *pwd;
	char fdname[20];
	unsetenv("LD_PRELOAD");
	ctrl = libvchan_server_init(QUBES_PA_SINK_VCHAN_PORT);
	if (!username)
		return 0;
	pwd = getpwnam(username);
	if (!pwd)
		return 0;
	setgid(pwd->pw_gid);
	initgroups(username, pwd->pw_gid);
	setuid(pwd->pw_uid);
	setenv("HOME", pwd->pw_dir, 1);
	snprintf(fdname, sizeof fdname, "%d",
		 libvchan_fd_for_select(ctrl));
	setenv("PULSE_PASSED_FD", fdname, 1);
	return 0;
}

struct libvchan *get_early_allocated_vchan(int id)
{
	return ctrl;
}
