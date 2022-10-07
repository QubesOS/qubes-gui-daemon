#include <stdbool.h>
#include "xside.h"
void qubes_daemon_xinput_plug__init(Ghandles * g);
void qubes_daemon_xinput_plug__on_new_window(Ghandles * g, Window child_win);
bool qubes_daemon_xinput_plug__process_xevent__return_is_xinput_event(Ghandles * g, XEvent * xevent);
