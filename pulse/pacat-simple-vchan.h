#ifndef __PACAT_SIMPLE_VCHAN_H
#define __PACAT_SIMPLE_VCHAN_H

#include <pulse/pulseaudio.h>
#include <glib.h>
#include <dbus/dbus-glib-bindings.h>
#include <libvchan.h>

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
	DBusGConnection *dbus;
	GObject *pacat_control;
	GMutex prop_mutex;
};

void pacat_log(const char *fmt, ...);

#endif /* __PACAT_SIMPLE_VCHAN_H */
