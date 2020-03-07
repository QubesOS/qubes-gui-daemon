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
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <math.h>
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


/* based on /usr/share/awesome/lib/gears/colors.lua */

static inline double max3(double a, double b, double c) {
    double r = a;
    if (b > r)
        r = b;
    if (c > r)
        r = c;
    return r;
}

static inline double min3(double a, double b, double c) {
    double r = a;
    if (b < r)
        r = b;
    if (c < r)
        r = c;
    return r;
}

static void rgb_to_hls(uint32_t rgb, double *out_h, double *out_l, double *out_s) {
    double r, g, b;
    double maxc, minc, l, h, s, rc, gc, bc;

    r = ((rgb >> 16) & 0xff) * 1.0 / 255.0;
    g = ((rgb >>  8) & 0xff) * 1.0 / 255.0;
    b = ((rgb >>  0) & 0xff) * 1.0 / 255.0;


    maxc = max3(r, g, b);
    minc = min3(r, g, b);
    // XXX Can optimize (maxc+minc) and (maxc-minc)
    l = (minc+maxc)/2.0;
    if (minc == maxc) {
        *out_h = 0.0;
        *out_l = l;
        *out_s = 0.0;
        return;
    }
    if (l <= 0.5) {
        s = (maxc-minc) / (maxc+minc);
    } else {
        s = (maxc-minc) / (2.0-maxc-minc);
    }
    rc = (maxc-r) / (maxc-minc);
    gc = (maxc-g) / (maxc-minc);
    bc = (maxc-b) / (maxc-minc);
    if (r == maxc) {
        h = bc-gc;
    } else if (g == maxc) {
        h = 2.0+rc-bc;
    } else {
        h = 4.0+gc-rc;
    }
    h = (h/6.0) - floor(h/6.0);

    *out_h = h;
    *out_l = l;
    *out_s = s;
}

/* based on /usr/share/awesome/lib/gears/colors.lua */
static uint8_t v(double m1, double m2, double hue) {
    hue = hue - floor(hue);
    if (hue < 1.0/6.0)
        return (m1 + (m2-m1)*hue*6.0) * 0xff;
    if (hue < 0.5)
        return (m2) * 0xff;
    if (hue < 2.0/3.0)
        return (m1 + (m2-m1)*(2.0/3.0-hue)*6.0) * 0xff;
    return m1 * 0xff;
}

static uint32_t hls_to_rgb(double h, double l, double s) {
    double m1, m2;

    if (s == 0.0)
        return
            (int)(l * 0xff) << 16 |
            (int)(l * 0xff) <<  8 |
            (int)(l * 0xff) <<  0;
    if (l <= 0.5)
        m2 = l * (1.0+s);
    else
        m2 = l+s-(l*s);
    m1 = 2.0*l - m2;
    return
        (uint32_t)v(m1, m2, h+1.0/3.0) << 16 |
        (uint32_t)v(m1, m2, h)         <<  8 |
        (uint32_t)v(m1, m2, h-1.0/3.0) <<  0;
}

void init_tray_tint(Ghandles *g) {
    double l_ignore;
    rgb_to_hls(g->label_color_rgb, &g->tint_h, &l_ignore, &g->tint_s);

    if (g->trayicon_tint_reduce_saturation)
        g->tint_s *= 0.5;
}

void tint_tray_and_update(Ghandles *g, struct windowdata *vm_window,
        int x, int y, int w, int h) {
    int yp, xp;
    uint32_t pixel;
    double h_ignore, l, s_ignore;

    if (!vm_window->image) {
        /* TODO: implement screen_window handling */
        return;
    }
    /* Create local pixmap, put vmside image to it
     * then get local image of the copy.
     * This is needed because XGetPixel does not seem to work
     * with XShmImage data.
     */
    Pixmap pixmap =
        XCreatePixmap(g->display, vm_window->local_winid,
                vm_window->image_width,
                vm_window->image_height,
                24);
    XShmPutImage(g->display, pixmap, g->context,
            vm_window->image, 0, 0, 0, 0,
            vm_window->image_width,
            vm_window->image_height, 0);
    XImage *image = XGetImage(g->display, pixmap, x, y, w, h,
            0xFFFFFFFF, ZPixmap);
    /* tint image */
    for (yp = 0; yp < h; yp++) {
        for (xp = 0; xp < w; xp++) {
            pixel = XGetPixel(image, xp, yp);
            if (g->trayicon_tint_whitehack && pixel == 0xffffff)
                pixel = 0xfefefe;
            else {
                rgb_to_hls(pixel, &h_ignore, &l, &s_ignore);
                pixel = hls_to_rgb(g->tint_h, l, g->tint_s);
            }
            if (!XPutPixel(image, xp, yp, pixel)) {
                fprintf(stderr, "Failed to update pixel %d,%d of tray window 0x%lx(remote 0x%lx)\n",
                        xp, yp, vm_window->local_winid, vm_window->remote_winid);
                exit(1);
            }
        }
    }
    XPutImage(g->display, vm_window->local_winid,
            g->context, image, 0, 0, x, y, w, h);
    XDestroyImage(image);
    XFreePixmap(g->display, pixmap);
}
