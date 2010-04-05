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
int write_data(char *buf, int size)
{
	int written = 0;
	int ret;
	while (written < size) {
		ret = write(1, buf + written, size - written);
		if (ret <= 0) {
			perror("write");
			exit(1);
		}
		written += ret;
	}
//      fprintf(stderr, "sent %d bytes\n", size);
	return size;
}

int read_data(char *buf, int size)
{
	int written = 0;
	int ret;
	while (written < size) {
		ret = read(0, buf + written, size - written);
		if (ret == 0) {
			fprintf(stderr, "EOF\n");
			exit(1);
		}
		if (ret < 0) {
			perror("write");
			exit(1);
		}
		written += ret;
	}
//      fprintf(stderr, "read %d bytes\n", size);
	return size;
}

int read_ready()
{
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(0, &rfds);
	struct timeval tv = { 0, 0 };
	int ret;
	ret = select(1, &rfds, NULL, NULL, &tv);
	if (ret == -1) {
		perror("read_ready");
		exit(1);
	}
	return ret;
}

void wait_for_vchan_or_argfd(int fd)
{
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(0, &rfds);
	FD_SET(fd, &rfds);
	if (select(fd + 1, &rfds, NULL, NULL, NULL) < 0) {
		perror("select");
		exit(1);
	}
}

int peer_server_init(int port)
{
	return 0;
}

char *peer_client_init(int dom, int port)
{
	return "dummy VMname";
}

int real_write_message(char *hdr, int size, char *data, int datasize)
{
	write_data(hdr, size);
	write_data(data, datasize);
	return 0;
}
