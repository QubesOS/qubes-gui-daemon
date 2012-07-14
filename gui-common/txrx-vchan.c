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
#include <xs.h>
#include <xenctrl.h>
#include "double_buffer.h"

struct libvchan *ctrl;
int double_buffered = 0;
int is_server;
void (*vchan_at_eof)(void) = NULL;
int vchan_is_closed = 0;


void vchan_register_at_eof(void (*new_vchan_at_eof)(void)) {
	vchan_at_eof = new_vchan_at_eof;
}

int write_data_exact(char *buf, int size)
{
	int written = 0;
	int ret;

	while (written < size) {
		ret = libvchan_write(ctrl, buf + written, size - written);
		if (ret <= 0) {
			perror("write");
			exit(1);
		}
		written += ret;
	}
//      fprintf(stderr, "sent %d bytes\n", size);
	return size;
}

int write_data(char *buf, int size)
{
	int count;
	if (!double_buffered)
		return write_data_exact(buf, size); // this may block
	double_buffer_append(buf, size);
	count = libvchan_buffer_space(ctrl);
	if (count > double_buffer_datacount())
		count = double_buffer_datacount();
        // below, we write only as much data as possible without
        // blocking; remainder of data stays in the double buffer
	write_data_exact(double_buffer_data(), count);
	double_buffer_substract(count);
	return size;
}

int real_write_message(char *hdr, int size, char *data, int datasize)
{
	write_data(hdr, size);
	write_data(data, datasize);
	return 0;
}

int read_data(char *buf, int size)
{
	int written = 0;
	int ret;
	while (written < size) {
		ret = libvchan_read(ctrl, buf + written, size - written);
		if (ret == 0) {
			fprintf(stderr, "EOF\n");
			exit(1);
		}
		if (ret < 0) {
			perror("read");
			exit(1);
		}
		written += ret;
	}
//      fprintf(stderr, "read %d bytes\n", size);
	return size;
}

int read_ready()
{
	return libvchan_data_ready(ctrl);
}

// if the remote domain is destroyed, we get no notification
// thus, we check for the status periodically

#ifdef XENCTRL_HAS_XC_INTERFACE
static xc_interface *xc_handle = NULL;
#else
static int xc_handle = -1;
#endif
void slow_check_for_libvchan_is_eof(struct libvchan *ctrl)
{
	struct evtchn_status evst;
	evst.port = ctrl->evport;
	evst.dom = DOMID_SELF;
	if (xc_evtchn_status(xc_handle, &evst)) {
		perror("xc_evtchn_status");
		vchan_is_closed = 1;
		exit(1);
	}
	if (evst.status != EVTCHNSTAT_interdomain) {
		fprintf(stderr, "event channel disconnected\n");
		vchan_is_closed = 1;
		if (vchan_at_eof != NULL)
			vchan_at_eof();
		exit(0);
	}
}


int wait_for_vchan_or_argfd_once(int nfd, int *fd, fd_set * retset)
{
	fd_set rfds;
	int vfd, max = 0, ret, i;
	struct timeval tv = { 0, 100000 };
	write_data(NULL, 0);	// trigger write of queued data, if any present
	vfd = libvchan_fd_for_select(ctrl);
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
		return 0;
	if (ret < 0) {
		perror("select");
		exit(1);
	}
	if (libvchan_is_eof(ctrl)) {
		fprintf(stderr, "libvchan_is_eof\n");
		vchan_is_closed = 1;
		if (vchan_at_eof != NULL)
			vchan_at_eof();
		exit(0);
	}
	if (!is_server && ret == 0)
		slow_check_for_libvchan_is_eof(ctrl);
	if (FD_ISSET(vfd, &rfds))
		// the following will never block; we need to do this to
		// clear libvchan_fd pending state 
		libvchan_wait(ctrl);
	if (retset)
		*retset = rfds;
	return ret;
}

void wait_for_vchan_or_argfd(int nfd, int *fd, fd_set * retset)
{
	while (wait_for_vchan_or_argfd_once(nfd, fd, retset) == 0);
}

int peer_server_init(int port)
{
	double_buffered = 0; // writes to vchan may block
	is_server = 1;
	ctrl = libvchan_server_init(port);
	if (!ctrl) {
		perror("libvchan_server_init");
		exit(1);
	}
	return 0;
}

char *get_vm_name(int dom)
{
	struct xs_handle *xs;
	char buf[64];
	char *name;
	unsigned int len = 0;

	xs = xs_daemon_open();
	if (!xs) {
		perror("xs_daemon_open");
		exit(1);
	}
	snprintf(buf, sizeof(buf), "/local/domain/%d/name", dom);
	name = xs_read(xs, 0, buf, &len);
	if (!name) {
		perror("xs_read domainname");
		exit(1);
	}
	xs_daemon_close(xs);
	return name;
}

void peer_client_init(int dom, int port)
{
	struct xs_handle *xs;
	char *dummy;
	unsigned int len = 0;
	char devbuf[128];
	unsigned int count;
	char **vec;

	double_buffered = 1; // writes to vchan are buffered, nonblocking
	double_buffer_init();
	xs = xs_daemon_open();
	if (!xs) {
		perror("xs_daemon_open");
		exit(1);
	}
	snprintf(devbuf, sizeof(devbuf),
		 "/local/domain/%d/device/vchan/%d/event-channel", dom,
		 port);
	xs_watch(xs, devbuf, devbuf);
	do {
		vec = xs_read_watch(xs, &count);
		if (vec)
			free(vec);
		len = 0;
		dummy = xs_read(xs, 0, devbuf, &len);
	}
	while (!dummy || !len); // wait for the server to create xenstore entries
	free(dummy);
	xs_daemon_close(xs);

	if (!(ctrl = libvchan_client_init(dom, port))) {
          perror("libvchan_client_init");
          exit(1);
        }

#ifdef XENCTRL_HAS_XC_INTERFACE
	xc_handle = xc_interface_open(NULL, 0, 0);
	if (!xc_handle) {
#else
	xc_handle = xc_interface_open();
	if (xc_handle < 0) {
#endif
		perror("xc_interface_open");
		exit(1);
	}
}

void vchan_close()
{
	if (!vchan_is_closed)
		libvchan_close(ctrl);
	vchan_is_closed = 1;
}
