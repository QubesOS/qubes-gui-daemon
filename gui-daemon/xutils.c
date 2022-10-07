#include <stdint.h>
#include <stdio.h>
#include <libvchan.h>
#include "xside.h"
#include "txrx.h" // write_message
#include "qubes-gui-protocol.h"

void send_keymap_notify(Ghandles * g)
{
    struct msg_hdr hdr;
    char keys[32];
    int err = XQueryKeymap(g->display, keys);
    if (err) {
        fprintf(stderr, "XQueryKeymap failed: %d.\n", err);
        return; // non fatal
    }
    hdr.type = MSG_KEYMAP_NOTIFY;
    hdr.window = 0;
    write_message(g->vchan, hdr, keys);
}
