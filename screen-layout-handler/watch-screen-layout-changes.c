#include <sys/signalfd.h>

#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <assert.h>

int main(int argc, char **argv) {
    sigset_t sigmask;
    int sigfd;
    Display *d;
    Window root_win;
    int xrr_event_base = 0;
    int xrr_error_base = 0;
    int x11_fd;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <script> [args ...]\n", argv[0]);
        exit(2);
    }

    signal(SIGCHLD, SIG_IGN);
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &sigmask, NULL) == -1)
        err(1, "Couldn't block signals for graceful signal recovery");

    sigfd = signalfd(-1, &sigmask, SFD_CLOEXEC);
    if (sigfd == -1)
        err(1, "Couldn't create signalfd for graceful signal recovery");

    d = XOpenDisplay(NULL);
    if (!d)
        errx(1, "Failed to open display");

    root_win = DefaultRootWindow(d);

    if (!XRRQueryExtension(d, &xrr_event_base, &xrr_error_base))
        errx(1, "RandR extension missing");

    XRRSelectInput(d, root_win, RRScreenChangeNotifyMask);

    XFlush(d);
    x11_fd = ConnectionNumber(d);
    for (;;) {
        int layout_changed;
        XEvent ev;
        struct pollfd fds[2] = {
            { .fd = sigfd, .events = POLLIN | POLLHUP, .revents = 0 },
            { .fd = x11_fd, .events = POLLIN | POLLHUP, .revents = 0 },
        };

        if (poll(fds, 2, -1) == -1) {
            XCloseDisplay(d);
            err(1, "poll");
        }

        if (fds[0].revents) {
            /* This must be SIGTERM as we are not listening on anything else */
            break;
        }

        layout_changed = 0;
        while (XPending(d)) {
            XNextEvent(d, &ev);
            XRRUpdateConfiguration(&ev);

            /* This should be the only event we get, but check regardless. */
            if (ev.type == xrr_event_base + RRScreenChangeNotify)
                layout_changed = 1;
        }

        if (layout_changed) {
            fprintf(stderr, "Screen layout change event received\n");
            switch (fork()) {
                case 0:
                    close(x11_fd);
                    execvp(argv[1], &argv[1]);
                    err(1, "exec");

                case -1:
                    warn("fork");
                    break;

                default:
                    break;
            }
        }
    }
    XCloseDisplay(d);
    return 0;
}
