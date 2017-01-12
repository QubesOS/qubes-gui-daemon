/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2016  Ilpo Järvinen  <ilpo.jarvinen@helsinki.fi>
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

#define SHMID_DISPLAY_MAXLEN    20
#define SHMID_FILENAME_PREFIX    "/var/run/qubes/shm.id."
#define SHMID_FILENAME_LEN    (sizeof(SHMID_FILENAME_PREFIX) + SHMID_DISPLAY_MAXLEN)
