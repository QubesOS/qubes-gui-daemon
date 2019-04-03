#ifndef __PACAT_SIMPLE_VCHAN_H
#define __PACAT_SIMPLE_VCHAN_H

#include <pulse/pulseaudio.h>
#include <glib.h>
#include <libvchan.h>
#include <qubesdb-client.h>

#define PACAT_PIDFILE_PATH_TPL "/var/run/qubes/pacat.%d"

struct userdata {
    pa_mainloop_api *mainloop_api;
    GMainLoop *loop;
    char *name;
    int ret;

    libvchan_t *play_ctrl;
    libvchan_t *rec_ctrl;

    pa_proplist *proplist;
    pa_context *context;
    pa_stream *play_stream;
    pa_stream *rec_stream;
    char *play_device;
    char *rec_device;

    pa_io_event* play_stdio_event;
    pa_io_event* rec_stdio_event;

    int rec_allowed;
    int rec_requested;
    GMutex prop_mutex;
    qdb_handle_t qdb;
    char *qdb_path;
    int control_socket_fd;
    pa_io_event* control_socket_event;
};

void pacat_log(const char *fmt, ...);

#endif /* __PACAT_SIMPLE_VCHAN_H */
