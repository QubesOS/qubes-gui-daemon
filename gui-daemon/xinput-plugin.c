#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include "xside.h"
#include "xutils.h"
#include "txrx.h"
#include <X11/extensions/XInput2.h>
#include "qubes-gui-protocol.h"

static int xinput_plugin_enabled = false;

void qubes_daemon_xinput_plug__init(Ghandles * g) {
    int ev_base, err_base; /* ignore */
    if (!XQueryExtension(g->display, "XInputExtension", &g->xi_opcode, &ev_base, &err_base)) {
        fprintf(stderr, "X Input extension not available. Key press events not available. Upgrade your X11 server now.\n");
        exit(1);
    }
    xinput_plugin_enabled = true;
}

void qubes_daemon_xinput_plug__on_new_window(Ghandles * g, Window child_win) {
    if (!xinput_plugin_enabled) return;

    // select xinput events
    XIEventMask xi_mask;
    xi_mask.deviceid = XIAllMasterDevices; // https://stackoverflow.com/questions/44095001/getting-double-rawkeypress-events-using-xinput2
    xi_mask.mask_len = XIMaskLen(XI_LASTEVENT);
    if (!(xi_mask.mask = calloc(xi_mask.mask_len, sizeof(char)))) {
        fputs("Out of memory!\n", stderr);
        exit(1);
    }
    XISetMask(xi_mask.mask, XI_KeyPress);
    XISetMask(xi_mask.mask, XI_KeyRelease);
    XISetMask(xi_mask.mask, XI_FocusIn);
    XISetMask(xi_mask.mask, XI_FocusOut);

    int err = XISelectEvents(g->display, child_win, &xi_mask, 1);
    if (err) {
        fprintf(stderr, "Failed to subscribe to XI events. ErrCode: %d\n", err);
        exit(1);
    }
    free(xi_mask.mask);
    XSync(g->display, False);
}


/* check and handle guid-special keys
 * currently only for inter-vm clipboard copy
 */
static bool is_special_keypress_xinput(Ghandles * g, const XIDeviceEvent * ev, XID remote_winid)
{
    XKeyEvent xev;    
    xev.state = ev->mods.effective;
    xev.keycode = ev->detail;
    xev.type = ev->evtype;
    xev.time = ev->time;
    return is_special_keypress(g, &xev, remote_winid);
}

/* handle local XInput event: KeyPress, KeyRelease
 * send it to relevant window in VM
 *
 * Note, no raw keys are press
 */
static void process_xinput_key(Ghandles * g, const XIDeviceEvent * ev)
{
    CHECK_NONMANAGED_WINDOW(g, ev->event);
    // yes, ev->event is the window number
    update_wm_user_time(g, ev->event, ev->time);
    if (ev->flags & XIKeyRepeat)
        return; // don't send key repeat events    
    if (is_special_keypress_xinput(g, ev, vm_window->remote_winid))
        return;

    struct msg_hdr hdr;
    hdr.type = MSG_XI_KEYPRESS;
    hdr.window = vm_window->remote_winid;

    struct msg_xi_keypress k;
    // TODO:
    // k.type = ev->evtype; // ev->type is always Generic Event
    // k.x = ev->event_x;
    // k.y = ev->event_y;
    // k.state = ev->mods.effective;
    // k.keycode = ev->detail;

    write_message(g->vchan, hdr, k);
}

/* handle local XInput event: FocusIn, FocusOut
 * send to relevant window in VM */
static void process_xinput_focus(Ghandles * g, const XILeaveEvent * ev)
{
    CHECK_NONMANAGED_WINDOW(g, ev->event);
    update_wm_user_time(g, ev->event, ev->time);

    if (ev->type == XI_FocusIn) {
        send_keymap_notify(g);
    }

    struct msg_hdr hdr;
    hdr.type = MSG_FOCUS;
    hdr.window = vm_window->remote_winid;

    struct msg_focus k;
    // TODO:
    // k.type = ev->evtype;
    // k.mode = ev->mode;
    // k.detail = ev->detail;
    write_message(g->vchan, hdr, k);
}

bool qubes_daemon_xinput_plug__process_xevent__return_is_xinput_event(Ghandles * g, XEvent * xevent) {
    if (!xinput_plugin_enabled) return false;

    XGenericEventCookie *cookie =  &xevent->xcookie;
    if ( ! (XGetEventData(g->display, cookie) &&
            cookie->type == GenericEvent &&
            cookie->extension == g->xi_opcode)) return false;

    XIEvent* xi_event = cookie->data; // from test_xi2.c in xinput cli utility

    switch (xi_event->evtype) {
    // ideally raw input events are better, but I'm relying on X server's built-in event filtering and routing feature here
    case XI_KeyPress:
    case XI_KeyRelease:
        process_xinput_key(g, (XIDeviceEvent *)xi_event);
        break;
    case XI_FocusIn:
    case XI_FocusOut:
        process_xinput_focus(g, (XILeaveEvent *)xi_event);
        break;
    }
    XFreeEventData(g->display, cookie);

    return true;
}
