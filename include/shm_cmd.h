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

#ifndef SHM_CMD_H
#define SHM_CMD_H
#include <stdint.h>

/* Messages are described here:
 * http://wiki.qubes-os.org/trac/wiki/GUIdocs
 */

/* VM -> Dom0 */
struct shm_cmd {
	uint32_t shmid;
	uint32_t width;
	uint32_t height;
	uint32_t bpp;
	uint32_t off;
	uint32_t num_mfn;
	uint32_t domid;
	uint32_t mfns[0];
};
#endif
