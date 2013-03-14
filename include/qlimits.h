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

//arbitrary
#define MAX_CLIPBOARD_SIZE 65000
#define MAX_WINDOW_WIDTH 8192
#define MAX_WINDOW_HEIGHT 3072

//not arbitrary
#include "shm-cmd.h"
#define DUMMY_DRV_FB_BPP 32
#define SIZEOF_SHARED_MFN 4
#define MAX_WINDOW_MEM (MAX_WINDOW_HEIGHT*MAX_WINDOW_WIDTH*DUMMY_DRV_FB_BPP/8)
#define NUM_PAGES(x) ((x+4095)>>12)
//finally, used stuff
#define MAX_MFN_COUNT NUM_PAGES(MAX_WINDOW_MEM)
#define SHM_CMD_NUM_PAGES NUM_PAGES(MAX_MFN_COUNT*SIZEOF_SHARED_MFN+sizeof(struct shm_cmd))
