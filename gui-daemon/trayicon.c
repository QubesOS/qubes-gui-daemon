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

#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include "xside.h"

/* initialization required for TRAY_BACKGROUND mode */
void init_tray_bg(Ghandles *g) {
    /* prepare graphic context for tray background */
	XGCValues values;
	values.foreground = WhitePixel(g->display, g->screen);
	g->tray_gc =
	    XCreateGC(g->display, g->root_win, GCForeground, &values);
}

/* Color tray icon background (use top-left corner as a "transparent" base),
 * and update image on screen. Do the operation on given area only.
 */
void fill_tray_bg_and_update(Ghandles *g, struct windowdata *vm_window,
        int x, int y, int w, int h) {
    char *data, *datap;
    size_t data_sz;
    int xp, yp;

    if (!vm_window->image) {
        /* TODO: implement screen_window handling */
        return;
    }
    /* allocate image_width _bits_ for each image line */
    data_sz =
        (vm_window->image_width / 8 +
         1) * vm_window->image_height;
    data = datap = calloc(1, data_sz);
    if (!data) {
        fprintf(stderr, "malloc(%dx%x -> %zu\n",
                vm_window->image_width, vm_window->image_height, data_sz);
        exit(1);
    }

    /* Create local pixmap, put vmside image to it
     * then get local image of the copy.
     * This is needed because XGetPixel does not seem to work
     * with XShmImage data.
     *
     * Always use 0,0 w+x,h+y coordinates to generate proper mask. */
    w = w + x;
    h = h + y;
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
            if (datap >= data + data_sz) {
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

    XFreePixmap(g->display, mask);
    XDestroyImage(image);
    XFreePixmap(g->display, pixmap);
    free(data);
}
