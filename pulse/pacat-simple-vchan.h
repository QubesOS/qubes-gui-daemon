#ifndef __PACAT_SIMPLE_VCHAN_H
#define __PACAT_SIMPLE_VCHAN_H

#include <stdbool.h>
#include <pulse/pulseaudio.h>
#include <glib.h>
#include <libvchan.h>
#include <qubesdb-client.h>

#define PACAT_PIDFILE_PATH_TPL "/var/run/qubes/pacat.%d"

struct userdata {
    pa_mainloop_api *mainloop_api;
    GMainLoop *loop;
    const char *name;
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
    pa_io_event* play_ctrl_event;
    pa_io_event* rec_ctrl_event;

    GMutex prop_mutex;
    qdb_handle_t qdb;
    qdb_handle_t watch_qdb; // separate connection for watches
    char *qdb_config_path;
    char *qdb_status_path;
    char *qdb_request_path;
    int control_socket_fd;
    pa_io_event* control_socket_event;
    bool rec_allowed;
    bool rec_requested;
    bool never_block;
    bool pending_play_cork;
    bool draining;

    int domid;
    int play_watch_fd, rec_watch_fd;
};

void pacat_log(const char *fmt, ...);

#endif /* __PACAT_SIMPLE_VCHAN_H */
