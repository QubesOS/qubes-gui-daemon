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

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <err.h>

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
static pa_buffer_attr custom_bufattr ={
    .maxlength = (uint32_t)-1,
    .minreq = (uint32_t)-1,
    .prebuf = (uint32_t)-1,
    .fragsize = 2048,
    .tlength = 4096
};
const pa_buffer_attr * bufattr = NULL;

static int verbose = 1;

static void context_state_callback(pa_context *c, void *userdata);

static void playback_stream_drain(struct userdata *u);

static void playback_stream_drain_cb(pa_stream *s, int success, void *userdata);

void pacat_log(const char *fmt, ...) {
    va_list args;
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    fprintf(stderr, "%10jd.%06ld ", ts.tv_sec, ts.tv_nsec / 1000);
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
    int l = 0, space_in_vchan;
    size_t buffer_length;
    void *buffer = NULL;
    assert(s);

    if ((space_in_vchan = libvchan_data_ready(u->play_ctrl)) < 0) {
        pacat_log("libvchan_data_ready() failed: return value %d", space_in_vchan);
        quit(u, 1);
        return;
    }

    if (verbose > 1) {
        pa_usec_t latency = 0;
        int negative;

        if (pa_stream_get_latency(s, &latency, &negative))
            pacat_log("pa_stream_get_latency() failed");

        pacat_log("process_playback_data(): vchan data %d max_length %d latency %llu", space_in_vchan, max_length, latency);
    }

    buffer_length = (size_t)space_in_vchan > max_length ? max_length : (size_t)space_in_vchan;
    if (!buffer_length)
        goto maybe_cork;

    if (pa_stream_begin_write(s, &buffer, &buffer_length) || !buffer) {
        pacat_log("pa_stream_begin_write() failed: %s", pa_strerror(pa_context_errno(u->context)));
        quit(u, 1);
        return;
    }

    if (buffer_length) {
        l = libvchan_read(u->play_ctrl, buffer, buffer_length);
        if (l < 0) {
            pacat_log("libvchan_read() failed: return value %d", l);
            quit(u, 1);
            return;
        }
        if (l == 0) {
            pacat_log("disconnected");
            quit(u, 0);
            return;
        }
        if ((size_t)l != buffer_length) {
            pacat_log("unexpected short vchan read: expected %zu, got %d", buffer_length, l);
            quit(u, 1);
            return;
        }
        if (pa_stream_write(s, buffer, buffer_length, NULL, 0, PA_SEEK_RELATIVE) < 0) {
            pacat_log("pa_stream_write() failed: %s", pa_strerror(pa_context_errno(u->context)));
            quit(u, 1);
            return;
        }
        space_in_vchan -= (int)buffer_length;
    } else
        pa_stream_cancel_write(s);

maybe_cork:
    if (u->pending_play_cork && space_in_vchan == 0) {
        pacat_log("Cork requested and playback vchan empty. Draining playback stream.");
        playback_stream_drain(u);
    }

    return;
}

static void playback_stream_drain(struct userdata *u) {
    pa_operation *o;
    pa_stream *s = u->play_stream;

    if (u->draining)
        return; /* Cannot have more than one drain operation ongoing */
    u->draining = true;

    if (!(o = pa_stream_drain(s, playback_stream_drain_cb, u))) {
        pacat_log("pa_stream_drain(): %s", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
        quit(u, 1);
        return;
    }

    pa_operation_unref(o);
}

static void playback_stream_drain_cb(pa_stream *s, int success, void *userdata) {
    struct userdata *u = userdata;
    assert(s == u->play_stream);
    u->draining = false;
    /* Check that the draining was successful. */
    if (!success)
        return;

    /*
     * Check that the agent has not sent QUBES_PA_SINK_UNCORK_CMD in the
     * meantime. This ensures correct behavior in the following situation:
     *
     * 1. Agent sends QUBES_PA_SINK_CORK_CMD.
     * 2. Vchan is drained.
     * 3. pa_stream_drain() is called.
     * 4. Agent sends QUBES_PA_SINK_UNCORK_CMD.
     * 5. PulseAudio calls playback_stream_drain_cb().
     *
     * The stream must not get corked in this case.
     */
    if (!u->pending_play_cork)
        return;

    /*
     * Check that the vchan is still empty before actually corking the stream.
     * This ensures correct behavior in the following situation:
     *
     * 1. Agent sends QUBES_PA_SINK_CORK_CMD.
     * 2. Vchan is drained.
     * 3. pa_stream_drain() is called.
     * 4. Agent sends QUBES_PA_SINK_UNCORK_CMD.
     * 5. Agent sends more data on the vchan.
     * 6. Agent sends QUBES_PA_SINK_CORK_CMD.
     * 7. PulseAudio calls playback_stream_drain_cb().
     *
     * Without this check, the data sent in step 5 would be (wrongly) left in
     * the vchan.  If the agent does not send any more data, pa_stream_drain()
     * will be called again when the vchan does become empty, so the stream will
     * eventually be corked.
     */
    int space_in_vchan = libvchan_data_ready(u->play_ctrl);
    if (space_in_vchan == 0) {
        pacat_log("Playback vchan empty and playback stream drained. Corking playback stream.");
        u->pending_play_cork = false;
        pa_stream_cork(u->play_stream, 1, NULL, u);
    } else if (space_in_vchan < 0) {
        pacat_log("libvchan_data_ready() failed: return value %d", space_in_vchan);
        quit(u, 1);
        return;
    }
}

/* This is called whenever new data may be written to the stream */
static void stream_write_callback(pa_stream *s, size_t length, void *userdata) {
    struct userdata *u = userdata;

    assert(s);
    assert(length > 0);

    process_playback_data(u, s, length);
}

static void send_rec_data(pa_stream *s, struct userdata *u, bool discard_overrun) {
    const void *rec_buffer;
    size_t rec_buffer_length, rec_buffer_index = 0;
    int l, vchan_buffer_space;

    assert(s);
    assert(u);

    if (!u->rec_allowed)
        return;

    if (pa_stream_readable_size(s) <= 0)
        return;

    if (pa_stream_peek(s, &rec_buffer, &rec_buffer_length) < 0) {
        pacat_log("pa_stream_peek failed");
        quit(u, 1);
        return;
    }
    if (!rec_buffer)
        return;

    if (u->never_block) {
        if ((vchan_buffer_space = libvchan_buffer_space(u->rec_ctrl)) < 0) {
            pacat_log("libvchan_buffer_space failed");
            quit(u, 1);
            return;
        }

        if (rec_buffer_length > (size_t)vchan_buffer_space) {
            if (!discard_overrun)
                return;
            size_t bytes_to_discard = rec_buffer_length - (size_t)vchan_buffer_space;
            pacat_log("Overrun: discarding %zu bytes", bytes_to_discard);
            rec_buffer += bytes_to_discard;
            rec_buffer_length = (size_t)vchan_buffer_space;
        }
    }

    while (rec_buffer_length > 0) {
        /* can block if u->never_block is not set */
        if ((l=libvchan_write(u->rec_ctrl, rec_buffer + rec_buffer_index, rec_buffer_length)) < 0) {
            pacat_log("libvchan_write failed: return value %d", l);
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

    send_rec_data(s, u, true);
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
                u->pending_play_cork = true;
                if (libvchan_data_ready(u->play_ctrl) > 0) {
                    pacat_log("Deferred stream drain");
                } else {
                    pacat_log("Stream drain");
                    playback_stream_drain(u);
                }
                break;
            case QUBES_PA_SINK_UNCORK_CMD:
                pacat_log("Stream uncork");
                u->pending_play_cork = false;
                /*
                 * do not clear u->draining, as draining while a drain
                 * operation is in progress is not safe
                 */
                pa_stream_cork(u->play_stream, 0, NULL, u);
                break;
            default:
                break;
        }
    }
    /* send the data if space is available */
    send_rec_data(u->rec_stream, u, false);
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
            if (verbose > 1)
                flags |= PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING;

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
            u->rec_requested = 0;

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
    char pid_s[16] = { 0 };

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
            return;
        }
        command_len += ret;
        if (ret == 0)
            break;
    } while (!memchr(command_buffer + (command_len-ret), '\n', ret));

    if (strncmp(command_buffer, "audio-input 0\n", command_len) == 0) {
        new_rec_allowed = 0;
    } else if (strncmp(command_buffer, "audio-input 1\n", command_len) == 0) {
        new_rec_allowed = 1;
    } else {
        pacat_log("Invalid command buffer");
        return;
    }
    if (new_rec_allowed != -1) {
        g_mutex_lock(&u->prop_mutex);
        u->rec_allowed = new_rec_allowed;
        pacat_log("Setting audio-input to %s", u->rec_allowed ? "enabled" : "disabled");
        if (u->rec_allowed && u->rec_requested) {
            pacat_log("Recording start");
            pa_stream_cork(u->rec_stream, 0, NULL, NULL);
        } else if (!u->rec_allowed && u->rec_stream &&
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
    /* better safe than sorry - zero initialize the buffer */
    struct sockaddr_un addr = { 0 };

    socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        pacat_log("socket failed: %s", strerror(errno));
        goto fail;
    }

    if ((size_t)snprintf(addr.sun_path, sizeof(addr.sun_path),
                "/var/run/qubes/audio-control.%s", u->name)
            >= sizeof(addr.sun_path)) {
        pacat_log("VM name too long");
        goto fail;
    }
    /* without this line, the bind() fails in many linux versions
       with Invalid Argument, and mic cannot attach */
    addr.sun_family = AF_UNIX;

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

static void connect_pa_daemon(struct userdata *u) {
    char *server = NULL;

    pacat_log("Connection to qube established, connecting to PulseAudio daemon\n");

    /* Connect the context */
    if (pa_context_connect(u->context, server, PA_CONTEXT_NOFAIL, NULL) < 0) {
        pacat_log("pa_context_connect() failed: %s", pa_strerror(pa_context_errno(u->context)));
        quit(u, 1);
    }
}

static void vchan_play_async_connect(pa_mainloop_api *UNUSED(a),
        pa_io_event *e, int UNUSED(fd), pa_io_event_flags_t UNUSED(f),
        void *userdata) {
    struct userdata *u = userdata;
    int ret;

    assert(e == u->play_ctrl_event);

    ret = libvchan_client_init_async_finish(u->play_ctrl, true);
    if (ret > 0)
        /* wait more */
        return;
    if (ret < 0) {
        perror("libvchan_client_init_async_finish");
        quit(u, 1);
        return;
    }

    /* connection established, no need to watch this FD anymore */
    u->mainloop_api->io_free(u->play_ctrl_event);
    u->play_ctrl_event = NULL;

    /* when both vchans are connected, connect to the daemon */
    if (libvchan_is_open(u->rec_ctrl) == VCHAN_CONNECTED)
        connect_pa_daemon(u);
}

static void vchan_rec_async_connect(pa_mainloop_api *UNUSED(a),
        pa_io_event *e, int UNUSED(fd), pa_io_event_flags_t UNUSED(f),
        void *userdata) {
    struct userdata *u = userdata;
    int ret;

    assert(e == u->rec_ctrl_event);

    ret = libvchan_client_init_async_finish(u->rec_ctrl, true);
    if (ret > 0)
        /* wait more */
        return;
    if (ret < 0) {
        perror("libvchan_client_init_async_finish");
        quit(u, 1);
        return;
    }

    /* connection established, no need to watch this FD anymore */
    u->mainloop_api->io_free(u->rec_ctrl_event);
    u->rec_ctrl_event = NULL;

    /* when both vchans are connected, connect to the daemon */
    if (libvchan_is_open(u->play_ctrl) == VCHAN_CONNECTED)
        connect_pa_daemon(u);
}

void cleanup_pidfile(char *pidfile_path, int pidfile_fd) {
    unlink(pidfile_path);
    close(pidfile_fd);
}

static _Noreturn void usage(char *arg0, int arg) {
    FILE *stream = arg ? stderr : stdout;
    fprintf(stream, "usage: %s [options] [--] domid domname\n",
            arg0 ? arg0 : "pacat-simple-vchan");
    fprintf(stream, "  -l - low-latency mode (higher CPU usage)\n");
    fprintf(stream, "  -n - never block on vchan I/O (overrides previous -b option)\n");
    fprintf(stream, "  -b - always block on vchan I/O (default, overrides previous -n option)\n");
    fprintf(stream, "  -v - verbose logging (a lot of output, may affect performance)\n");
    fprintf(stream, "  -t size - target playback buffer fill, implies -l, default %d\n",
            custom_bufattr.tlength);
    fprintf(stream, "  -h - print this message\n");
    if (fflush(NULL) || ferror(stdout) || ferror(stderr))
        exit(1);
    exit(arg);
}

int main(int argc, char *argv[])
{
    struct timeval tv;
    struct userdata u;
    pa_glib_mainloop* m = NULL;
    pa_time_event *time_event = NULL;
    char *pidfile_path;
    int pidfile_fd;
    int i;
    unsigned long tlength;
    char *endptr;

    memset(&u, 0, sizeof(u));
    if (argc <= 2)
        usage(argv[0], 1);
    while ((i = getopt(argc, argv, "+lnbvt:h")) != -1) {
        switch (i) {
            case 'l':
                bufattr = &custom_bufattr;
                break;
            case 'n':
                u.never_block = true;
                break;
            case 'b':
                u.never_block = false;
                break;
            case 'v':
                verbose += 1;
                break;
            case 't':
                errno = 0;
                tlength = strtoul(optarg, &endptr, 0);
                if (*endptr)
                    errx(1, "Invalid -t argument: %s", optarg);
                if (tlength > UINT32_MAX)
                    errno = ERANGE;
                if (errno)
                    err(1, "Invalid -t argument: %s", optarg);
                bufattr = &custom_bufattr;
                custom_bufattr.tlength = tlength;
                break;
            case 'h':
                usage(argv[0], 0);
            default:
                usage(argv[0], 1);
        }
    }
    if (argc - optind != 2)
        usage(argv[0], 1);
    const char *domid_str = argv[optind], *domname = argv[optind + 1];
    errno = 0;
    long l_domid = strtol(domid_str, &endptr, 10);
    /* 0x7FF0 is DOMID_FIRST_RESERVED */
    if (l_domid < 0 || l_domid >= 0x7FF0 || errno == ERANGE)
        errx(1, "domid %ld out of range 0 through %d inclusive", l_domid,
                0x7FF0 - 1);
    if (errno)
        err(1, "invalid domid %s", domid_str);
    if (*endptr)
        errx(1, "trailing junk after domid %s", domid_str);

    u.domid = (int)l_domid;
    u.name = domname;

    if (create_pidfile(u.domid, &pidfile_path, &pidfile_fd) < 0)
        /* error already printed by create_pidfile() */
        exit(1);
    goto main;

main:
    u.ret = 1;

    g_mutex_init(&u.prop_mutex);

    u.play_ctrl = libvchan_client_init_async(u.domid, QUBES_PA_SINK_VCHAN_PORT, &u.play_watch_fd);
    if (!u.play_ctrl) {
        perror("libvchan_client_init_async");
        cleanup_pidfile(pidfile_path, pidfile_fd);
        exit(u.ret);
    }
    u.rec_ctrl = libvchan_client_init_async(u.domid, QUBES_PA_SOURCE_VCHAN_PORT, &u.rec_watch_fd);
    if (!u.rec_ctrl) {
        perror("libvchan_client_init_async");
        cleanup_pidfile(pidfile_path, pidfile_fd);
        exit(u.ret);
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

    u.play_ctrl_event = u.mainloop_api->io_new(
        u.mainloop_api, u.play_watch_fd, PA_IO_EVENT_INPUT, vchan_play_async_connect, &u);
    if (!u.play_ctrl_event) {
        pacat_log("io_new play_ctrl failed");
        goto quit;
    }

    u.rec_ctrl_event = u.mainloop_api->io_new(
        u.mainloop_api, u.rec_watch_fd, PA_IO_EVENT_INPUT, vchan_rec_async_connect, &u);
    if (!u.rec_ctrl_event) {
        pacat_log("io_new rec_ctrl failed");
        goto quit;
    }

    u.rec_allowed = 0;

    if (!(u.context = pa_context_new_with_proplist(u.mainloop_api, NULL, u.proplist))) {
        pacat_log("pa_context_new() failed.");
        goto quit;
    }

    pa_context_set_state_callback(u.context, context_state_callback, &u);

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

    if (u.play_stream) {
        pa_stream_unref(u.play_stream);
        u.play_stream = NULL;
    }

    if (u.rec_stream) {
        pa_stream_unref(u.rec_stream);
        u.rec_stream = NULL;
    }

    if (u.context) {
        pa_context_disconnect(u.context);
        pa_context_unref(u.context);
        u.context = NULL;
    }

    if (time_event) {
        assert(u.mainloop_api);
        u.mainloop_api->time_free(time_event);
    }

    if (u.play_ctrl_event) {
        assert(u.mainloop_api);
        u.mainloop_api->io_free(u.play_ctrl_event);
    }

    if (u.rec_ctrl_event) {
        assert(u.mainloop_api);
        u.mainloop_api->io_free(u.rec_ctrl_event);
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

    if (!u.ret)
        goto main;

    cleanup_pidfile(pidfile_path, pidfile_fd);

    return u.ret;
}
