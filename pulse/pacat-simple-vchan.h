#ifndef __PACAT_SIMPLE_VCHAN_H
#define __PACAT_SIMPLE_VCHAN_H

#include <pulse/pulseaudio.h>
#include <glib.h>
#include <libvchan.h>

struct userdata {
	pa_mainloop_api *mainloop_api;
	GMainLoop *loop;
	char *name;

	struct libvchan *play_ctrl;
	struct libvchan *rec_ctrl;

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
};

#endif /* __PACAT_SIMPLE_VCHAN_H */
