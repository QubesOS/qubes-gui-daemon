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

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#ifndef SHMOVERRIDE_LIB_PATH
#define SHMOVERRIDE_LIB_PATH "shmoverride.so"
#endif
#define XORG_PATH "/usr/bin/Xorg"
#define XORG_PATH_NEW "/usr/libexec/Xorg.bin"
#define XORG_PATH_NEWER "/usr/libexec/Xorg" /* Fedora 23 */

int main(int argc __attribute__((__unused__)), char **argv) {
    putenv("LD_PRELOAD=" SHMOVERRIDE_LIB_PATH);

    if (access(XORG_PATH_NEWER, X_OK) == 0)
        execv(XORG_PATH_NEWER, argv);
    else if (access(XORG_PATH_NEW, X_OK) == 0)
        execv(XORG_PATH_NEW, argv);
    else
        execv (XORG_PATH, argv);

    perror("X-wrapper-qubes: execv");
    return 1;
}
