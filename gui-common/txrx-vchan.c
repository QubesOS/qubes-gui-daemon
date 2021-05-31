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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <libvchan.h>
#include <sys/select.h>
#include <errno.h>
#include "double-buffer.h"

void (*vchan_at_eof)(void) = NULL;
int vchan_is_closed = 0;

/* double buffered in gui-daemon to deal with deadlock
 * during send large clipboard content
 */
int double_buffered = 1;

int wait_for_vchan_or_argfd_once(libvchan_t *vchan, int nfd, int *fd, fd_set * retset);

void vchan_register_at_eof(void (*new_vchan_at_eof)(void)) {
    vchan_at_eof = new_vchan_at_eof;
}

void handle_vchan_error(libvchan_t *vchan, const char *op)
{
    if (!libvchan_is_open(vchan)) {
        fprintf(stderr, "EOF\n");
        exit(0);
    } else {
        fprintf(stderr, "Error while vchan %s\n, terminating", op);
        exit(1);
    }
}

int write_data_exact(libvchan_t *vchan, char *buf, int size)
{
    int written = 0;
    int ret;

    while (written < size) {
        ret = libvchan_write(vchan, buf + written, size - written);
        if (ret <= 0)
            handle_vchan_error(vchan, "write data");
        written += ret;
    }
//      fprintf(stderr, "sent %d bytes\n", size);
    return size;
}

int write_data(libvchan_t *vchan, char *buf, int size)
{
    int count;
    if (!double_buffered)
        return write_data_exact(vchan, buf, size); // this may block
    double_buffer_append(buf, size);
    count = libvchan_buffer_space(vchan);
    if (count > double_buffer_datacount())
        count = double_buffer_datacount();
        // below, we write only as much data as possible without
        // blocking; remainder of data stays in the double buffer
    write_data_exact(vchan, double_buffer_data(), count);
    double_buffer_substract(count);
    return size;
}

int real_write_message(libvchan_t *vchan, char *hdr, int size, char *data, int datasize)
{
    write_data(vchan, hdr, size);
    write_data(vchan, data, datasize);
    return 0;
}

int read_data(libvchan_t *vchan, char *buf, int size)
{
    int written = 0;
    int ret;
    while (written < size) {
        while (!libvchan_data_ready(vchan))
            wait_for_vchan_or_argfd_once(vchan, 0, NULL, NULL);
        ret = libvchan_read(vchan, buf + written, size - written);
        if (ret <= 0)
            handle_vchan_error(vchan, "read data");
        written += ret;
    }
//      fprintf(stderr, "read %d bytes\n", size);
    return size;
}

int wait_for_vchan_or_argfd_once(libvchan_t *vchan, int nfd, int *fd, fd_set * retset)
{
    fd_set rfds;
    int vfd, max = 0, ret, i;
    struct timeval tv = { 0, 1000000 };
    write_data(vchan, NULL, 0);    // trigger write of queued data, if any present
    vfd = libvchan_fd_for_select(vchan);
    FD_ZERO(&rfds);
    for (i = 0; i < nfd; i++) {
        int cfd = fd[i];
        FD_SET(cfd, &rfds);
        if (cfd > max)
            max = cfd;
    }
    FD_SET(vfd, &rfds);
    if (vfd > max)
        max = vfd;
    max++;
    ret = select(max, &rfds, NULL, NULL, &tv);
    if (ret < 0 && errno == EINTR)
        return -1;
    if (ret < 0) {
        perror("select");
        exit(1);
    }
    if (!libvchan_is_open(vchan)) {
        fprintf(stderr, "libvchan_is_eof\n");
        libvchan_close(vchan);
        if (vchan_at_eof != NULL) {
            vchan_at_eof();
            return -1;
        } else
            exit(0);
    }
    if (FD_ISSET(vfd, &rfds))
        // the following will never block; we need to do this to
        // clear libvchan_fd pending state 
        libvchan_wait(vchan);
    if (retset)
        *retset = rfds;
    return ret;
}

int wait_for_vchan_or_argfd(libvchan_t *vchan, int nfd, int *fd, fd_set * retset)
{
    int ret;
    while ((ret=wait_for_vchan_or_argfd_once(vchan, nfd, fd, retset)) == 0);
    return ret;
}
