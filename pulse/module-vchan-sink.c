/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/select.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/socket-util.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "module-vchan-sink-symdef.h"
#include "qubes-vchan-sink.h"
#include <libvchan.h>

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("VCHAN sink/source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE("sink_name=<name for the sink> "
		"sink_properties=<properties for the sink> "
		"format=<sample format> "
		"rate=<sample rate>"
		"channels=<number of channels> "
		"channel_map=<channel map>");

#define DEFAULT_SINK_NAME "vchan_output"

#define VCHAN_BUF 8192

struct userdata {
	pa_core *core;
	pa_module *module;
	pa_sink *sink;

	struct libvchan *play_ctrl;

	pa_thread *thread;
	pa_thread_mq thread_mq;
	pa_rtpoll *rtpoll;

	pa_memchunk memchunk_sink;

	pa_rtpoll_item *play_rtpoll_item;
};

static const char *const valid_modargs[] = {
	"sink_name",
	"sink_properties",
	"format",
	"rate",
	"channels",
	"channel_map",
	NULL
};

static int sink_process_msg(pa_msgobject * o, int code, void *data,
			    int64_t offset, pa_memchunk * chunk)
{
	int r;
	struct userdata *u = PA_SINK(o)->userdata;
	int state;
	switch (code) {
	case PA_SINK_MESSAGE_SET_STATE:
		state = PA_PTR_TO_UINT(data);
		r = pa_sink_process_msg(o, code, data, offset, chunk);
		if (r >= 0) {
			pa_log("sink cork req state =%d, now state=%d\n", state,
			       (int) (u->sink->state));
		}
		return r;

	case PA_SINK_MESSAGE_GET_LATENCY:{
			size_t n = 0;
			n += u->memchunk_sink.length;

			*((pa_usec_t *) data) =
			    pa_bytes_to_usec(n, &u->sink->sample_spec);
			return 0;
		}
	}

	return pa_sink_process_msg(o, code, data, offset, chunk);
}

static int write_to_vchan(struct libvchan *ctrl, char *buf, int size)
{
	static int all = 0, waited = 0, nonwaited = 0, full = 0;
	ssize_t l;
	fd_set rfds;
	struct timeval tv = { 0, 0 };
	int ret, fd = libvchan_fd_for_select(ctrl);
	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);
	all++;
	ret = select(fd + 1, &rfds, NULL, NULL, &tv);
	if (ret == -1) {
		pa_log("Failed to select() in vchan: %s",
		       pa_cstrerror(errno));
		return -1;
	}
	if (ret) {
		if (libvchan_wait(ctrl) < 0) {
			pa_log("Failed libvchan_wait");
			return -1;
		}
		waited++;
	} else
		nonwaited++;
	if (libvchan_buffer_space(ctrl)) {
		l = libvchan_write(ctrl, buf, size);
	} else {
		l = -1;
		errno = EAGAIN;
		full++;
	}
	if ((all % 8000) == 0) {
		pa_log
		    ("write_to_vchan: all=%d waited=%d nonwaited=%d full=%d\n",
		     all, waited, nonwaited, full);
	}
	return l;
}

static int process_sink_render(struct userdata *u)
{
	pa_assert(u);

	if (u->memchunk_sink.length <= 0)
		pa_sink_render(u->sink, libvchan_buffer_space(u->play_ctrl), &u->memchunk_sink);

	pa_assert(u->memchunk_sink.length > 0);

	for (;;) {
		ssize_t l;
		void *p;

		p = pa_memblock_acquire(u->memchunk_sink.memblock);
		l = write_to_vchan(u->play_ctrl, (char *) p +
				   u->memchunk_sink.index, u->memchunk_sink.length);
		pa_memblock_release(u->memchunk_sink.memblock);

		pa_assert(l != 0);

		if (l < 0) {

			if (errno == EINTR)
				continue;
			else if (errno == EAGAIN)
				return 0;
			else {
				pa_log
				    ("Failed to write data to VCHAN: %s",
				     pa_cstrerror(errno));
				return -1;
			}

		} else {

			u->memchunk_sink.index += (size_t) l;
			u->memchunk_sink.length -= (size_t) l;

			if (u->memchunk_sink.length <= 0) {
				pa_memblock_unref(u->memchunk_sink.memblock);
				pa_memchunk_reset(&u->memchunk_sink);
			}
		}

		return 0;
	}
}

static void thread_func(void *userdata)
{
	struct userdata *u = userdata;

	pa_assert(u);

	pa_log_debug("Thread starting up");

	pa_thread_mq_install(&u->thread_mq);
	for (;;) {
		struct pollfd *play_pollfd;
		int ret;

		play_pollfd = pa_rtpoll_item_get_pollfd(u->play_rtpoll_item, NULL);

		/* Render some data and write it to the fifo */
		if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {

			if (u->sink->thread_info.rewind_requested)
				pa_sink_process_rewind(u->sink, 0);

			if (play_pollfd->revents || libvchan_buffer_space(u->play_ctrl)) {
				if (process_sink_render(u) < 0)
					goto fail;

				play_pollfd->revents = 0;
			}
		}

		/* Hmm, nothing to do. Let's sleep */
		play_pollfd->events =
		    (short) (u->sink->thread_info.state ==
			     PA_SINK_RUNNING ? POLLIN : 0);

		if ((ret = pa_rtpoll_run(u->rtpoll, TRUE)) < 0)
			goto fail;

		if (ret == 0)
			goto finish;
	}

      fail:
	/* If this was no regular exit from the loop we have to continue
	 * processing messages until we received PA_MESSAGE_SHUTDOWN */
	pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core),
			  PA_CORE_MESSAGE_UNLOAD_MODULE, u->module,
			  0, NULL, NULL);
	pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

      finish:
	pa_log_debug("Thread shutting down");
}

static int do_conn(struct userdata *u)
{
	int fd;
	u->play_ctrl = libvchan_server_init(QUBES_PA_SINK_VCHAN_PORT);
	if (!u->play_ctrl) {
		pa_log("libvchan_server_init play failed\n");
		return -1;
	}
	fd = libvchan_fd_for_select(u->play_ctrl);
	pa_log("play libvchan_fd_for_select=%d, ctrl=%p\n", fd, u->play_ctrl);
	return fd;
}


int pa__init(pa_module * m)
{
	struct userdata *u;
	pa_sample_spec ss;
	pa_channel_map map;
	pa_modargs *ma;
	struct pollfd *pollfd;
	pa_sink_new_data data_sink;

	pa_assert(m);

	if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
		pa_log("Failed to parse module arguments.");
		goto fail;
	}

	ss = m->core->default_sample_spec;
	map = m->core->default_channel_map;
	if (pa_modargs_get_sample_spec_and_channel_map
	    (ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
		pa_log
		    ("Invalid sample format specification or channel map");
		goto fail;
	}

	u = pa_xnew0(struct userdata, 1);
	u->core = m->core;
	u->module = m;
	m->userdata = u;
	pa_memchunk_reset(&u->memchunk_sink);
	u->rtpoll = pa_rtpoll_new();
	pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

	if ((do_conn(u)) < 0) {

		pa_log("get_early_allocated_vchan: %s",
		       pa_cstrerror(errno));
		goto fail;
	}
	/* SINK preparation */
	pa_sink_new_data_init(&data_sink);
	data_sink.driver = __FILE__;
	data_sink.module = m;
	pa_sink_new_data_set_name(&data_sink,
				  pa_modargs_get_value(ma,
						       "sink_name",
						       DEFAULT_SINK_NAME));
	pa_proplist_sets(data_sink.proplist,
			 PA_PROP_DEVICE_STRING, DEFAULT_SINK_NAME);
	pa_proplist_setf(data_sink.proplist,
			PA_PROP_DEVICE_DESCRIPTION,
			"Unix VCHAN sink");
	pa_sink_new_data_set_sample_spec(&data_sink, &ss);
	pa_sink_new_data_set_channel_map(&data_sink, &map);

	if (pa_modargs_get_proplist
		(ma, "sink_properties", data_sink.proplist, PA_UPDATE_REPLACE) < 0) {
		pa_log("Invalid properties");
		pa_sink_new_data_done(&data_sink);
		goto fail;
	}

	u->sink = pa_sink_new(m->core, &data_sink, PA_SINK_LATENCY);
	pa_sink_new_data_done(&data_sink);

	if (!u->sink) {
		pa_log("Failed to create sink.");
		goto fail;
	}

	u->sink->parent.process_msg = sink_process_msg;
	u->sink->userdata = u;

	pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
	pa_sink_set_rtpoll(u->sink, u->rtpoll);
	pa_sink_set_max_request(u->sink, VCHAN_BUF);
	pa_sink_set_fixed_latency(u->sink,
				  pa_bytes_to_usec
				  (VCHAN_BUF,
				   &u->sink->sample_spec));

	u->play_rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
	pollfd = pa_rtpoll_item_get_pollfd(u->play_rtpoll_item, NULL);
	pollfd->fd = libvchan_fd_for_select(u->play_ctrl);
	pollfd->events = POLLIN;
	pollfd->revents = 0;

#if PA_CHECK_VERSION(0,9,22)
	if (!(u->thread = pa_thread_new("vchan-sink", thread_func, u))) {
#else
	if (!(u->thread = pa_thread_new(thread_func, u))) {
#endif
		pa_log("Failed to create thread.");
		goto fail;
	}

	pa_sink_put(u->sink);

	pa_modargs_free(ma);

	return 0;

      fail:
	if (ma)
		pa_modargs_free(ma);

	pa__done(m);

	return -1;
}

int pa__get_n_used(pa_module * m)
{
	struct userdata *u;

	pa_assert(m);
	pa_assert_se(u = m->userdata);

	return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module * m)
{
	struct userdata *u;

	pa_assert(m);

	if (!(u = m->userdata))
		return;

	if (u->sink)
		pa_sink_unlink(u->sink);

	if (u->thread) {
		pa_asyncmsgq_send(u->thread_mq.inq, NULL,
				  PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
		pa_thread_free(u->thread);
	}

	pa_thread_mq_done(&u->thread_mq);

	if (u->sink)
		pa_sink_unref(u->sink);

	if (u->memchunk_sink.memblock)
		pa_memblock_unref(u->memchunk_sink.memblock);

	if (u->play_rtpoll_item)
		pa_rtpoll_item_free(u->play_rtpoll_item);


	if (u->rtpoll)
		pa_rtpoll_free(u->rtpoll);

	if (u->play_ctrl)
		libvchan_close(u->play_ctrl);


	pa_xfree(u);
}
