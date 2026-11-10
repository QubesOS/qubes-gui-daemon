# Qubes GUI daemon

The host side of GUI virtualization in [Qubes OS](https://www.qubes-os.org). The
GUI daemon (`qubes-guid`) runs in dom0 or a GuiVM and talks to the GUI agent in
each VM over the Qubes GUI protocol, carried on a Xen vchan. It places each VM's
windows on the host display and tags them with the VM's colour label, which the
window manager uses to draw the familiar per-VM coloured border. It also passes
clipboard, window and tray icons, and other GUI events between the VM and the
host.

Because the VM and the host do not share an X server, window contents are moved
with Xen shared memory: `shmoverride` is preloaded into the host Xorg server so
that composition buffers from a foreign domain can be mapped in place of ordinary
shared memory. The repository also holds the matching PulseAudio-based audio
daemon and dom0 policy and extension files.

See the GUI protocol documentation at
[qubes-os.org/doc/gui](https://www.qubes-os.org/doc/gui/) for how the protocol
works and how it fits into the Qubes architecture.
