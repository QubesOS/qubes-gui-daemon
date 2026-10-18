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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <libvchan.h>
#include <errno.h>
#include <poll.h>
#include <assert.h>
#include <sys/socket.h>

#include <double-buffer.h>
#include "../include/txrx.h"

void (*vchan_at_eof)(void) = NULL;
int vchan_is_closed = 0;
static int monitored_fd = -1;
static void (*argfd_at_eof)(void) = NULL;

/* double buffered in gui-daemon to deal with deadlock
 * during send large clipboard content
 */
int double_buffered = 1;

void vchan_register_at_eof(void (*new_vchan_at_eof)(void)) {
    vchan_at_eof = new_vchan_at_eof;
}

void vchan_register_argfd(int fd, void (*new_argfd_at_eof)(void)) {
    monitored_fd = fd;
    argfd_at_eof = new_argfd_at_eof;
}

static void handle_vchan_error(libvchan_t *vchan, const char *op)
{
    if (!libvchan_is_open(vchan)) {
        fprintf(stderr, "EOF\n");
        exit(0);
    } else {
        fprintf(stderr, "Error while vchan %s\n, terminating", op);
        exit(1);
    }
}

static int write_data_exact(libvchan_t *vchan, char *buf, int size)
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

static int wait_for_vchan_or_argfd_events(libvchan_t *vchan, int fd,
        short events, int timeout);

int read_data(libvchan_t *vchan, char *buf, int size)
{
    int written = 0;
    int ret;
    while (written < size) {
        while (!libvchan_data_ready(vchan))
            wait_for_vchan_or_argfd_events(vchan, monitored_fd, POLLRDHUP, 1000);
        ret = libvchan_read(vchan, buf + written, size - written);
        if (ret <= 0)
            handle_vchan_error(vchan, "read data");
        written += ret;
    }
//      fprintf(stderr, "read %d bytes\n", size);
    return size;
}

/*
 * Wait for vchan activity and for the events requested on fd. Hangup of fd is
 * always detected, even with events == 0 or POLLRDHUP only. Callers that do not
 * consume data from fd must not ask for POLLIN, or poll() would return
 * immediately for as long as there is unread data on fd.
 */
static int wait_for_vchan_or_argfd_events(libvchan_t *vchan, int fd,
        short events, int timeout)
{
    int ret;
    write_data(vchan, NULL, 0);    // trigger write of queued data, if any present
    struct pollfd fds[] = {
        { .fd = libvchan_fd_for_select(vchan), .events = POLLIN, .revents = 0 },
        { .fd = fd, .events = events, .revents = 0 },
    };
    ret = poll(fds, fd == -1 ? 1 : 2, timeout);
    if (ret < 0) {
        if (errno == EINTR)
            return -1;
        perror("poll");
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
    if (fd != -1 && fds[1].revents) {
        int eof = 0;
        if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL | POLLRDHUP)) {
            eof = 1;
        } else if (fds[1].revents & POLLIN) {
            char peek_buf;
            ssize_t peek_ret = recv(fd, &peek_buf, 1, MSG_PEEK | MSG_DONTWAIT);
            if (peek_ret == 0 || (peek_ret < 0 && (errno == ECONNRESET || errno == EPIPE))) {
                eof = 1;
            }
        }
        if (eof) {
            fprintf(stderr, "Connection on monitored fd %d closed or hung up\n", fd);
            if (argfd_at_eof != NULL) {
                argfd_at_eof();
                return -1;
            } else {
                exit(1);
            }
        }
    }
    if (fds[0].revents) {
        // the following will never block; we need to do this to
        // clear libvchan_fd pending state 
        libvchan_wait(vchan);
    }
    return ret;
}

int wait_for_vchan_or_argfd_once(libvchan_t *vchan, int fd, int timeout)
{
    return wait_for_vchan_or_argfd_events(vchan, fd, POLLIN, timeout);
}
