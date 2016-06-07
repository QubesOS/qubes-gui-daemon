/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (C) 2016  Marek Marczykowski-Górecki <marmarek@invisiblethingslab.com>
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

void init_tray_bg(Ghandles *g);
void fill_tray_bg_and_update(Ghandles *g, struct windowdata *vm_window,
        int x, int y, int w, int h);

void init_tray_tint(Ghandles *g);
void tint_tray_and_update(Ghandles *g, struct windowdata *vm_window,
        int x, int y, int w, int h);
