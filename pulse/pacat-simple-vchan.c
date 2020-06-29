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

#define _GNU_SOURCE
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <pulse/pulseaudio.h>
#include <pulse/error.h>
#include <pulse/gccmacro.h>
#include <pulse/glib-mainloop.h>
#include <libvchan.h>

#include "pacat-simple-vchan.h"
#include "qubes-vchan-sink.h"

#define CLEAR_LINE "\x1B[K"
#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

/* The Sample format to use */
static pa_sample_spec sample_spec = {
    .format = PA_SAMPLE_S16LE,
    .rate = 44100,
    .channels = 2
};
static const pa_buffer_attr custom_bufattr ={
    .maxlength = (uint32_t)-1,
    .minreq = (uint32_t)-1,
    .prebuf = (uint32_t)-1,
    .fragsize = 4096,
    .tlength = 4096
};
const pa_buffer_attr * bufattr = NULL;

static int verbose = 1;

static void context_state_callback(pa_context *c, void *userdata);

void pacat_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}


/* A shortcut for terminating the application */
static void quit(struct userdata *u, int ret) {
    assert(u->loop);
    u->ret = ret;
    g_main_loop_quit(u->loop);
}

void pulseaudio_reconnect(struct userdata *u) {
    char *server = NULL;

    pacat_log("Reconnecting to PulseAudio server...");
    if (u->play_stream) {
        pa_stream_unref(u->play_stream);
        u->play_stream = NULL;
    }

    if (u->rec_stream) {
        pa_stream_unref(u->rec_stream);
        u->rec_stream = NULL;
    }

    if (u->context) {
        pa_context_disconnect(u->context);
        pa_context_unref(u->context);
        u->context = NULL;
    }

    if (!(u->context = pa_context_new_with_proplist(u->mainloop_api, NULL, u->proplist))) {
        pacat_log("pa_context_new() failed.");
        quit(u, 1);
    }

    pa_context_set_state_callback(u->context, context_state_callback, u);
    /* Connect the context */
    if (pa_context_connect(u->context, server, PA_CONTEXT_NOFAIL, NULL) < 0) {
        pacat_log("pa_context_connect() failed: %s", pa_strerror(pa_context_errno(u->context)));
        quit(u, 1);
    }
}

/* Connection draining complete */
static void context_drain_complete(pa_context*c, void *UNUSED(userdata)) {
    pa_context_disconnect(c);
}

/* Stream draining complete */
static void stream_drain_complete(pa_stream*s, int success, void *userdata) {
    struct userdata *u = userdata;

    assert(s == u->play_stream);

    if (!success) {
        pacat_log("Failed to drain stream: %s", pa_strerror(pa_context_errno(u->context)));
        quit(u, 1);
    }

    if (verbose)
        pacat_log("Playback stream drained.");

    pa_stream_disconnect(s);
    pa_stream_unref(s);
    u->play_stream = NULL;

    if (!pa_context_drain(u->context, context_drain_complete, NULL))
        pa_context_disconnect(u->context);
    else {
        if (verbose)
            pacat_log("Draining connection to server.");
    }
}

/* Start draining */
static void start_drain(struct userdata *u, pa_stream *s) {

    if (s) {
        pa_operation *o;

        pa_stream_set_write_callback(s, NULL, NULL);

        if (!(o = pa_stream_drain(s, stream_drain_complete, u))) {
            pacat_log("pa_stream_drain(): %s", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            quit(u, 1);
            return;
        }

        pa_operation_unref(o);
    } else
        quit(u, 0);
}

static void process_playback_data(struct userdata *u, pa_stream *s, size_t max_length)
{
    int l = 0;
    size_t index, buffer_length;
    void *buffer = NULL;
    assert(s);

    if (!libvchan_data_ready(u->play_ctrl) || !max_length)
        return;

    buffer_length = max_length;

    if (pa_stream_begin_write(s, &buffer, &buffer_length) < 0) {
        pacat_log("pa_stream_begin_write() failed: %s", pa_strerror(pa_context_errno(u->context)));
        quit(u, 1);
        return;
    }
    index = 0;

    while (libvchan_data_ready(u->play_ctrl) && buffer_length > 0) {
        l = libvchan_read(u->play_ctrl, buffer + index, buffer_length);
        if (l < 0) {
            pacat_log("vchan read failed");
            quit(u, 1);
            return;
        }
        if (l == 0) {
            pacat_log("disconnected");
            quit(u, 0);
            return;
        }
        buffer_length -= l;
        index += l;
    }

    if (index) {
        if (pa_stream_write(s, buffer, index, NULL, 0, PA_SEEK_RELATIVE) < 0) {
            pacat_log("pa_stream_write() failed: %s", pa_strerror(pa_context_errno(u->context)));
            quit(u, 1);
            return;
        }
    } else
        pa_stream_cancel_write(s);

    return;
}

/* This is called whenever new data may be written to the stream */
static void stream_write_callback(pa_stream *s, size_t length, void *userdata) {
    struct userdata *u = userdata;

    assert(s);
    assert(length > 0);

    process_playback_data(u, s, length);
}

static void send_rec_data(pa_stream *s, struct userdata *u) {
    int l;
    const void *rec_buffer;
    size_t rec_buffer_length;
    int rec_buffer_index;

    assert(s);
    assert(u);

    if (pa_stream_readable_size(s) <= 0)
        return;

    if (pa_stream_peek(s, &rec_buffer, &rec_buffer_length) < 0) {
        pacat_log("pa_stream_peek failed");
        quit(u, 1);
        return;
    }
    rec_buffer_index = 0;

    while (rec_buffer_length > 0 && u->rec_allowed) {
        /* can block */
        if ((l=libvchan_write(u->rec_ctrl, rec_buffer + rec_buffer_index, rec_buffer_length)) < 0) {
            pacat_log("libvchan_write failed");
            quit(u, 1);
            return;
        }
        rec_buffer_length -= l;
        rec_buffer_index += l;
    }
    pa_stream_drop(s);
}

/* This is called whenever new data may is available */
static void stream_read_callback(pa_stream *s, size_t length, void *userdata) {
    struct userdata *u = userdata;

    assert(s);
    assert(length > 0);

    send_rec_data(s, u);
}

/* vchan event */
static void vchan_play_callback(pa_mainloop_api *UNUSED(a),
        pa_io_event *UNUSED(e),    int UNUSED(fd), pa_io_event_flags_t UNUSED(f),
        void *userdata) {
    struct userdata *u = userdata;

    /* receive event */
    libvchan_wait(u->play_ctrl);

    if (!libvchan_is_open(u->play_ctrl)) {
        pacat_log("vchan_is_eof");
        start_drain(u, u->play_stream);
        return;
    }

    /* process playback data */
    process_playback_data(u, u->play_stream, pa_stream_writable_size(u->play_stream));
}

static void vchan_rec_callback(pa_mainloop_api *UNUSED(a),
        pa_io_event *UNUSED(e), int UNUSED(fd), pa_io_event_flags_t UNUSED(f),
        void *userdata) {
    struct userdata *u = userdata;

    /* receive event */
    libvchan_wait(u->rec_ctrl);

    if (!libvchan_is_open(u->rec_ctrl)) {
        pacat_log("vchan_is_eof");
        quit(u, 0);
        return;
    }

    /* process VM control command */
    uint32_t cmd;
    while (libvchan_data_ready(u->rec_ctrl) >= (int)sizeof(cmd)) {
        if (libvchan_read(u->rec_ctrl, (char*)&cmd, sizeof(cmd)) != sizeof(cmd)) {
            if (!pa_stream_is_corked(u->rec_stream))
                pa_stream_cork(u->rec_stream, 1, NULL, u);
            fprintf(stderr, "Failed to read from vchan\n");
            quit(u, 1);
            return;
        }
        switch (cmd) {
            case QUBES_PA_SOURCE_START_CMD:
                g_mutex_lock(&u->prop_mutex);
                u->rec_requested = 1;
                if (u->rec_allowed) {
                    pacat_log("Recording start");
                    pa_stream_cork(u->rec_stream, 0, NULL, u);
                } else
                    pacat_log("Recording requested but not allowed");
                g_mutex_unlock(&u->prop_mutex);
                break;
            case QUBES_PA_SOURCE_STOP_CMD:
                g_mutex_lock(&u->prop_mutex);
                u->rec_requested = 0;
                if (!pa_stream_is_corked(u->rec_stream)) {
                    pacat_log("Recording stop");
                    pa_stream_cork(u->rec_stream, 1, NULL, u);
                }
                g_mutex_unlock(&u->prop_mutex);
                break;
            case QUBES_PA_SINK_CORK_CMD:
                pacat_log("Stream cork");
                pa_stream_cork(u->play_stream, 1, NULL, u);
                break;
            case QUBES_PA_SINK_UNCORK_CMD:
                pacat_log("Stream uncork");
                pa_stream_cork(u->play_stream, 0, NULL, u);
                break;
        }
    }
    /* send the data if space is available */
    if (libvchan_buffer_space(u->rec_ctrl)) {
        send_rec_data(u->rec_stream, u);
    }
}

/* This routine is called whenever the stream state changes */
static void stream_state_callback(pa_stream *s, void *userdata) {
    struct userdata *u = userdata;
    assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_CREATING:
	    break;
        case PA_STREAM_TERMINATED:
	    if (u->play_stdio_event && u->play_stream == s) {
               pacat_log("play stream terminated");
		assert(u->mainloop_api);
		u->mainloop_api->io_free(u->play_stdio_event);
	    }
	    if (u->rec_stdio_event && u->rec_stream == s) {
               pacat_log("rec stream terminated");
		assert(u->mainloop_api);
		u->mainloop_api->io_free(u->rec_stdio_event);
	    }
            break;

        case PA_STREAM_READY:

            if (verbose) {
                const pa_buffer_attr *a;
                char cmt[PA_CHANNEL_MAP_SNPRINT_MAX], sst[PA_SAMPLE_SPEC_SNPRINT_MAX];

                pacat_log("Stream successfully created.");

                if (!(a = pa_stream_get_buffer_attr(s)))
                    pacat_log("pa_stream_get_buffer_attr() failed: %s", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
                else {
                    pacat_log("Buffer metrics: maxlength=%u, tlength=%u, prebuf=%u, minreq=%u", a->maxlength, a->tlength, a->prebuf, a->minreq);
                }

                pacat_log("Using sample spec '%s', channel map '%s'.",
                        pa_sample_spec_snprint(sst, sizeof(sst), pa_stream_get_sample_spec(s)),
                        pa_channel_map_snprint(cmt, sizeof(cmt), pa_stream_get_channel_map(s)));

                pacat_log("Connected to device %s (%u, %ssuspended).",
                        pa_stream_get_device_name(s),
                        pa_stream_get_device_index(s),
                        pa_stream_is_suspended(s) ? "" : "not ");
            }
	    if (u->play_stream == s) {
		u->play_stdio_event = u->mainloop_api->io_new(u->mainloop_api,
                    libvchan_fd_for_select(u->play_ctrl), PA_IO_EVENT_INPUT, vchan_play_callback,  u);
		if (!u->play_stdio_event) {
		    pacat_log("io_new play failed");
		    quit(u, 1);
		}
	    }
	    if (u->rec_stream == s) {
		u->rec_stdio_event = u->mainloop_api->io_new(u->mainloop_api,
			libvchan_fd_for_select(u->rec_ctrl), PA_IO_EVENT_INPUT, vchan_rec_callback, u);
		if (!u->rec_stdio_event) {
		    pacat_log("io_new rec failed");
		    quit(u, 1);
		}
	    }
            break;

        case PA_STREAM_FAILED:
            if (pa_context_errno(pa_stream_get_context(s)) == PA_ERR_CONNECTIONTERMINATED) {
                /* handled at context level */
                break;
            }
            /* fallthrough */
        default:
            pacat_log("Stream error: %s", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            quit(u, 1);
    }
}

static void stream_suspended_callback(pa_stream *s, void *UNUSED(userdata)) {
    assert(s);

    if (verbose) {
        if (pa_stream_is_suspended(s))
            pacat_log("Stream device %s suspended.%s", pa_stream_get_device_name(s), CLEAR_LINE);
        else
            pacat_log("Stream device %s resumed.%s", pa_stream_get_device_name(s), CLEAR_LINE);
    }
}

static void stream_underflow_callback(pa_stream *s, void *UNUSED(userdata)) {
    assert(s);

    if (verbose)
        pacat_log("Stream underrun.%s", CLEAR_LINE);
}

static void stream_overflow_callback(pa_stream *s, void *UNUSED(userdata)) {
    assert(s);

    if (verbose)
        pacat_log("Stream %s overrun.%s", pa_stream_get_device_name(s), CLEAR_LINE);
}

static void stream_started_callback(pa_stream *s, void *UNUSED(userdata)) {
    assert(s);

    if (verbose)
        pacat_log("Stream started.%s", CLEAR_LINE);
}

static void stream_moved_callback(pa_stream *s, void *UNUSED(userdata)) {
    assert(s);

    if (verbose)
        pacat_log("Stream moved to device %s (%u, %ssuspended).%s", pa_stream_get_device_name(s), pa_stream_get_device_index(s), pa_stream_is_suspended(s) ? "" : "not ",  CLEAR_LINE);
}

static void stream_buffer_attr_callback(pa_stream *s, void *UNUSED(userdata)) {
    assert(s);

    if (verbose)
        pacat_log("Stream buffer attributes changed.%s", CLEAR_LINE);
}

static void stream_event_callback(pa_stream *s, const char *name, pa_proplist *pl, void *UNUSED(userdata)) {
    char *t;

    assert(s);
    assert(name);
    assert(pl);

    t = pa_proplist_to_string_sep(pl, ", ");
    pacat_log("Got event '%s', properties '%s'", name, t);
    pa_xfree(t);
}



/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata) {
    struct userdata *u = userdata;
    pa_stream_flags_t flags = 0;
    pa_channel_map channel_map;

    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:

            pa_channel_map_init_extend(&channel_map, sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);

            if (!pa_channel_map_compatible(&channel_map, &sample_spec)) {
                pacat_log("Channel map doesn't match sample specification");
                goto fail;
            }


            assert(!u->play_stream);

            if (verbose)
                pacat_log("Connection established.%s", CLEAR_LINE);

            if (!(u->play_stream = pa_stream_new_with_proplist(c, "playback", &sample_spec, &channel_map, u->proplist))) {
                pacat_log("play pa_stream_new() failed: %s", pa_strerror(pa_context_errno(c)));
                goto fail;
            }

            pa_stream_set_state_callback(u->play_stream, stream_state_callback, u);
            pa_stream_set_write_callback(u->play_stream, stream_write_callback, u);
            /* pa_stream_set_read_callback */
            pa_stream_set_suspended_callback(u->play_stream, stream_suspended_callback, u);
            pa_stream_set_moved_callback(u->play_stream, stream_moved_callback, u);
            pa_stream_set_underflow_callback(u->play_stream, stream_underflow_callback, u);
            pa_stream_set_overflow_callback(u->play_stream, stream_overflow_callback, u);
            pa_stream_set_started_callback(u->play_stream, stream_started_callback, u);
            pa_stream_set_event_callback(u->play_stream, stream_event_callback, u);
            pa_stream_set_buffer_attr_callback(u->play_stream, stream_buffer_attr_callback, u);
            flags = PA_STREAM_ADJUST_LATENCY;

            if (pa_stream_connect_playback(u->play_stream, u->play_device, bufattr, flags, NULL /* volume */, NULL) < 0) {
                pacat_log("pa_stream_connect_playback() failed: %s", pa_strerror(pa_context_errno(c)));
                goto fail;
            }

            /* setup recording stream */
            assert(!u->rec_stream);

            if (!(u->rec_stream = pa_stream_new_with_proplist(c, "record", &sample_spec, &channel_map, u->proplist))) {
                pacat_log("rec pa_stream_new() failed: %s", pa_strerror(pa_context_errno(c)));
                goto fail;
            }

            pa_stream_set_state_callback(u->rec_stream, stream_state_callback, u);
            /* pa_stream_set_write_callback */
            pa_stream_set_read_callback(u->rec_stream, stream_read_callback, u);
            pa_stream_set_suspended_callback(u->rec_stream, stream_suspended_callback, u);
            pa_stream_set_moved_callback(u->rec_stream, stream_moved_callback, u);
            pa_stream_set_underflow_callback(u->rec_stream, stream_underflow_callback, u);
            pa_stream_set_overflow_callback(u->rec_stream, stream_overflow_callback, u);
            pa_stream_set_started_callback(u->rec_stream, stream_started_callback, u);
            pa_stream_set_event_callback(u->rec_stream, stream_event_callback, u);
            pa_stream_set_buffer_attr_callback(u->rec_stream, stream_buffer_attr_callback, u);

            flags = PA_STREAM_START_CORKED | PA_STREAM_ADJUST_LATENCY;
            u->rec_allowed = u->rec_requested = 0;

            if (pa_stream_connect_record(u->rec_stream, u->rec_device, bufattr, flags) < 0) {
                pacat_log("pa_stream_connect_record() failed: %s", pa_strerror(pa_context_errno(c)));
                goto fail;
            }

            break;

        case PA_CONTEXT_TERMINATED:
            pacat_log("pulseaudio connection terminated");
            quit(u, 0);
            break;

        case PA_CONTEXT_FAILED:
            if (pa_context_errno(c) == PA_ERR_CONNECTIONTERMINATED) {
                pulseaudio_reconnect(u);
                break;
            }
            /* fallthrough */
        default:
            pacat_log("Connection failure: %s", pa_strerror(pa_context_errno(c)));
            goto fail;
    }

    return;

fail:
    quit(u, 1);

}

/*
 * domid: remote end domid (IN)
 * pidfile_path: path to the created file (OUT)
 * pidfile_fd: open FD of the pidfile, used also as a lockfile (OUT)
 *
 * Returns: 0 on success -1 otherwise
 */
static int create_pidfile(int domid, char **pidfile_path, int *pidfile_fd)
{
    int fd, ret;
    char pid_s[16];

    if (asprintf(pidfile_path, PACAT_PIDFILE_PATH_TPL, domid) < 0) {
        pacat_log("Failed to construct pidfile path, out of memory?");
        return -1;
    }

    fd = open(*pidfile_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        pacat_log("Failed to create pidfile %s: %s", *pidfile_path, strerror(errno));
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno == EWOULDBLOCK) {
            ret = read(fd, pid_s, sizeof(pid_s)-1);
            if (ret <= 0) {
                pacat_log("Another instance of pacat-simple-vchan is already running for this VM, but failed to get its PID");
            } else {
                pid_s[ret] = '\0';
                if (pid_s[ret-1] == '\n')
                    pid_s[ret-1] = '\0';
                pacat_log("Another instance of pacat-simple-vchan is already running for this VM (PID: %s)", pid_s);
            }
        } else {
            pacat_log("Failed to obtain a lock on the pid file: %s", pa_strerror(errno));
        }
        close(fd);
        return -1;
    }

    ret = snprintf(pid_s, sizeof(pid_s), "%d\n", getpid());
    if (write(fd, pid_s, ret) != ret) {
        pacat_log("Failed to write a pid file: %s", pa_strerror(errno));
        close(fd);
        return -1;
    }

    *pidfile_fd = fd;
    return 0;
}

static void check_vchan_eof_timer(pa_mainloop_api*a, pa_time_event* e,
               const struct timeval *UNUSED(tv), void *userdata)
{
    struct userdata *u = userdata;
    struct timeval restart_tv;
    assert(u);

    if (!libvchan_is_open(u->play_ctrl)) {
        pacat_log("vchan_is_eof (timer)");
        quit(u, 0);
        return;
    }

    pa_gettimeofday(&restart_tv);
    pa_timeval_add(&restart_tv, (pa_usec_t) 5 * 1000 * PA_USEC_PER_MSEC);
    a->time_restart(e, &restart_tv);
}

static void control_socket_callback(pa_mainloop_api *UNUSED(a),
        pa_io_event *UNUSED(e), int fd, pa_io_event_flags_t f,
        void *userdata) {
    struct userdata *u = userdata;
    int client_fd;
    char command_buffer[32];
    size_t command_len = 0;
    int ret;
    int new_rec_allowed = -1;

    if (!(f & PA_IO_EVENT_INPUT))
        return;

    client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0) {
        pacat_log("Accept control connection failed: %s", strerror(errno));
        return;
    }

    /* read until either:
     *  - end of command (\n) is found
     *  - EOF
     */
    do {
        ret = read(client_fd, command_buffer+command_len, sizeof(command_buffer)-command_len);
        if (ret < 0) {
            pacat_log("Control client read failed: %s", strerror(errno));
        }
        command_len += ret;
        if (ret == 0)
            break;
    } while (!memchr(command_buffer, '\n', command_len));

    if (strncmp(command_buffer, "audio-input 0\n", command_len) == 0) {
        new_rec_allowed = 0;
    } else if (strncmp(command_buffer, "audio-input 1\n", command_len) == 0) {
        new_rec_allowed = 1;
    }
    if (new_rec_allowed != -1) {
        g_mutex_lock(&u->prop_mutex);
        u->rec_allowed = new_rec_allowed;
        pacat_log("Setting audio-input to %s", u->rec_allowed ? "enabled" : "disabled");
        if (u->rec_allowed && u->rec_requested) {
            pacat_log("Recording start");
            pa_stream_cork(u->rec_stream, 0, NULL, NULL);
        } else if (!u->rec_allowed &&
                (u->rec_requested || !pa_stream_is_corked(u->rec_stream))) {
            pacat_log("Recording stop");
            pa_stream_cork(u->rec_stream, 1, NULL, NULL);
        }
        g_mutex_unlock(&u->prop_mutex);
        if (!qdb_write(u->qdb, u->qdb_path, new_rec_allowed ? "1" : "0", 1)) {
            pacat_log("Failed to write QubesDB %s: %s", u->qdb_path, strerror(errno));
        }
    }
    /* accept only one command per connection */
    close(client_fd);
}

static int setup_control(struct userdata *u) {
    int socket_fd = -1;
    struct sockaddr_un addr;

    socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        pacat_log("socket failed: %s", strerror(errno));
        goto fail;
    }

    if (snprintf(addr.sun_path, sizeof(addr.sun_path),
                "/var/run/qubes/audio-control.%s", u->name)
            >= (int)sizeof(addr.sun_path)) {
        pacat_log("VM name too long");
        goto fail;
    }

    /* ignore result */
    unlink(addr.sun_path);

    if (bind(socket_fd, &addr, sizeof(addr)) == -1) {
        pacat_log("bind to %s failed: %s", addr.sun_path, strerror(errno));
        goto fail;
    }

    if (listen(socket_fd, 5) == -1) {
        pacat_log("listen on %s failed: %s", addr.sun_path, strerror(errno));
        goto fail;
    }

    u->control_socket_event = u->mainloop_api->io_new(u->mainloop_api,
            socket_fd, PA_IO_EVENT_INPUT, control_socket_callback, u);
    if (!u->control_socket_event) {
        pacat_log("io_new control failed");
        goto fail;
    }

    u->qdb = qdb_open(NULL);
    if (!u->qdb) {
        pacat_log("qdb_open failed: %s", strerror(errno));
        goto fail;
    }

    if (asprintf(&u->qdb_path, "/audio-input/%s", u->name) < 0) {
        pacat_log("QubesDB path setup failed: %s", strerror(errno));
        u->qdb_path = NULL;
        goto fail;
    }

    if (!qdb_write(u->qdb, u->qdb_path, "0", 1)) {
        pacat_log("qdb_write failed: %s", strerror(errno));
        goto fail;
    }

    u->control_socket_fd = socket_fd;

    return 0;

fail:
    if (u->qdb_path)
        free(u->qdb_path);
    u->qdb_path = NULL;
    if (u->qdb)
        qdb_close(u->qdb);
    u->qdb = NULL;
    if (u->control_socket_event)
        u->mainloop_api->io_free(u->control_socket_event);
    u->control_socket_event = NULL;
    if (socket_fd >= 0)
        close(socket_fd);

    return 1;
}

static void control_cleanup(struct userdata *u) {

    if (u->control_socket_event)
        u->mainloop_api->io_free(u->control_socket_event);
    if (u->control_socket_fd > 0)
        close(u->control_socket_fd);
    if (u->qdb && u->qdb_path)
        qdb_rm(u->qdb, u->qdb_path);
    if (u->qdb_path)
        free(u->qdb_path);
    if (u->qdb)
        qdb_close(u->qdb);
}

int main(int argc, char *argv[])
{
    struct timeval tv;
    struct userdata u;
    pa_glib_mainloop* m = NULL;
    pa_time_event *time_event = NULL;
    char *server = NULL;
    char *domname = NULL;
    int domid = -1;
    char *pidfile_path;
    int pidfile_fd;
    int i;

    if (argc <= 2) {
        fprintf(stderr, "usage: %s [-l] domid domname\n", argv[0]);
        fprintf(stderr, "  -l - low-latency mode (higher CPU usage)\n");
        exit(1);
    }
    for (i=1; i<argc; i++) {
        if (argv[i][0] == '-') {
            switch (argv[i][1]) {
                case 'l':
                    bufattr = &custom_bufattr;
                    break;
                default:
                    fprintf(stderr, "Invalid option: %c\n", argv[i][1]);
                    exit(1);
            }
        } else {
            if (domid < 0)
                domid = atoi(argv[i]);
            else 
                domname = argv[i];
        }
    }
    if (domid < 0) { /* not-a-number returns 0 */
        fprintf(stderr, "invalid domid\n");
        exit(1);
    }
    if (!domname) {
        fprintf(stderr, "missing domname\n");
        exit(1);
    }

    if (create_pidfile(domid, &pidfile_path, &pidfile_fd) < 0)
        /* error already printed by create_pidfile() */
        exit(1);

    memset(&u, 0, sizeof(u));
    u.ret = 1;

    g_mutex_init(&u.prop_mutex);

    u.name = domname;

    u.play_ctrl = libvchan_client_init(domid, QUBES_PA_SINK_VCHAN_PORT);
    if (!u.play_ctrl) {
        perror("libvchan_client_init");
        exit(1);
    }
    u.rec_ctrl = libvchan_client_init(domid, QUBES_PA_SOURCE_VCHAN_PORT);
    if (!u.rec_ctrl) {
        perror("libvchan_client_init");
        exit(1);
    }
    if (setgid(getgid()) < 0) {
        perror("setgid");
        exit(1);
    }
    if (setuid(getuid()) < 0) {
        perror("setuid");
        exit(1);
    }

    u.proplist = pa_proplist_new();
    pa_proplist_sets(u.proplist, PA_PROP_APPLICATION_NAME, u.name);
    pa_proplist_sets(u.proplist, PA_PROP_MEDIA_NAME, u.name);

    /* Set up a new main loop */
    if (!(u.loop = g_main_loop_new (NULL, FALSE))) {
        pacat_log("g_main_loop_new() failed.");
        goto quit;
    }
    if (!(m = pa_glib_mainloop_new(g_main_loop_get_context(u.loop)))) {
        pacat_log("pa_glib_mainloop_new() failed.");
        goto quit;
    }

    u.mainloop_api = pa_glib_mainloop_get_api(m);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, (pa_usec_t) 5 * 1000 * PA_USEC_PER_MSEC);
    time_event = u.mainloop_api->time_new(u.mainloop_api, &tv, check_vchan_eof_timer, &u);
    if (!time_event) {
        pacat_log("time_event create failed");
        goto quit;
    }

    if (!(u.context = pa_context_new_with_proplist(u.mainloop_api, NULL, u.proplist))) {
        pacat_log("pa_context_new() failed.");
        goto quit;
    }

    pa_context_set_state_callback(u.context, context_state_callback, &u);
    /* Connect the context */
    if (pa_context_connect(u.context, server, PA_CONTEXT_NOFAIL, NULL) < 0) {
        pacat_log("pa_context_connect() failed: %s", pa_strerror(pa_context_errno(u.context)));
        goto quit;
    }

    if (setup_control(&u) < 0) {
        pacat_log("control socket initialization failed");
        goto quit;
    }

    u.ret = 0;

    /* Run the main loop */
    g_main_loop_run (u.loop);

quit:
    if (u.control_socket_event) {
        control_cleanup(&u);
    }

    if (u.play_stream)
        pa_stream_unref(u.play_stream);

    if (u.rec_stream)
        pa_stream_unref(u.rec_stream);

    if (u.context)
        pa_context_unref(u.context);

    if (time_event) {
        assert(u.mainloop_api);
        u.mainloop_api->time_free(time_event);
    }

    /* discard remaining data */
    if (libvchan_data_ready(u.play_ctrl)) {
        char buf[2048];
        libvchan_read(u.play_ctrl, buf, sizeof(buf));
    }
    if (libvchan_data_ready(u.rec_ctrl)) {
        char buf[2048];
        libvchan_read(u.rec_ctrl, buf, sizeof(buf));
    }

    /* close vchan */
    if (u.play_ctrl) {
        libvchan_close(u.play_ctrl);
    }

    if (u.rec_ctrl)
        libvchan_close(u.rec_ctrl);

    if (m) {
        pa_signal_done();
        pa_glib_mainloop_free(m);
    }

    if (u.proplist)
        pa_proplist_free(u.proplist);

    g_mutex_clear(&u.prop_mutex);

    unlink(pidfile_path);
    close(pidfile_fd);
    return u.ret;
}
