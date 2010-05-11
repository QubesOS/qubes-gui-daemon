/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/gccmacro.h>
#include <libvchan.h>
#include "vchanio.h"
#include "qubes-vchan-sink.h"
static ssize_t loop_read(struct libvchan *ctrl, char *data, size_t size)
{
	ssize_t ret = 0;

	while (size > 0) {
		ssize_t r;
		if (!libvchan_data_ready(ctrl))
			wait_for_vchan(ctrl);
		if ((r = libvchan_read(ctrl, data, size)) < 0)
			return r;

		if (r == 0)
			break;

		ret += r;
		data = data + r;
		size -= (size_t) r;
	}

	return ret;
}

int main(int argc, char *argv[])
{

	/* The Sample format to use */
	static const pa_sample_spec ss = {
		.format = PA_SAMPLE_S16LE,
		.rate = 44100,
		.channels = 2
	};
#ifdef CUSTOM_BUFFERING
	static const pa_buffer_attr custom_bufattr ={
	        .maxlength = 8192,
	        .minreq = (uint32_t)-1,
	        .prebuf = (uint32_t)-1,
	        .tlength = 4096
        };
        pa_buffer_attr * bufattr = &custom_bufattr;
#else
        pa_buffer_attr * bufattr = NULL;
#endif         
	struct libvchan *ctrl;
	pa_simple *s = NULL;
	int ret = 1;
	int error;
	int bufsize = 4096;

	/* replace STDIN with the specified file if needed */
	if (argc <= 1) {
		fprintf(stderr, "usage: %s domid\n", argv[0]);
		exit(1);
	}
	ctrl = peer_client_init(atoi(argv[1]), QUBES_PA_SINK_VCHAN_PORT);
	if (!ctrl) {
		perror("libvchan_client_init");
		exit(1);
	}
	setuid(getuid());
	/* Create a new playback stream */
	if (!
	    (s =
	     pa_simple_new(NULL, argv[0], PA_STREAM_PLAYBACK, NULL,
			   "playback", &ss, NULL, bufattr, &error))) {
		fprintf(stderr, __FILE__ ": pa_simple_new() failed: %s\n",
			pa_strerror(error));
		goto finish;
	}

	for (;;) {
		char buf[bufsize];
		ssize_t r;

		/* Read some data ... */
		if ((r = loop_read(ctrl, buf, bufsize)) <= 0) {
			if (r == 0)	/* EOF */
				break;

			fprintf(stderr, __FILE__ ": read() failed: %s\n",
				strerror(errno));
			goto finish;
		}

		/* ... and play it */
		if (pa_simple_write(s, buf, (size_t) r, &error) < 0) {
			fprintf(stderr,
				__FILE__
				": pa_simple_write() failed: %s\n",
				pa_strerror(error));
			goto finish;
		}
	}

	/* Make sure that every single sample was played */
	if (pa_simple_drain(s, &error) < 0) {
		fprintf(stderr,
			__FILE__ ": pa_simple_drain() failed: %s\n",
			pa_strerror(error));
		goto finish;
	}

	ret = 0;

      finish:

	if (s)
		pa_simple_free(s);

	return ret;
}
