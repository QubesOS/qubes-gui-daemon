<!--
## Copyright (C) 2010 - 2019 Qubes Contributors
## See the file COPYING for copying conditions.
-->
<!--
EDITORS NOTE:
Please do not use relative links here, because this file is mirrored on github
https://github.com/qubes-gui-daemon/qubes-gui-daemon/blob/master/README.mediawiki
and relative links won't work there.
-->
= qubes-gui-daemon  =

gui-daemon is Qubes GUI Virtualisation agent/daemon, present on Dom0 side.

gui-daemon has default daemon setting. Starting the GUI will ask you whether to
start the daemon and you can then input the arguments as per what you would want
to install. The GUI domain would be the same as admin domain in all but few
scenarios. The gui-daemon is for Xorg. GUI daemon can be enabled for and run on
most OS from Linux/Xorg, Windows and MAcs.

The config is defined by guid.conf. You can find information about GUI protocol
[https://www.qubes-os.org/doc/gui/ here].

<img src="https://www.qubes-os.org/attachment/wiki/posts/qubes-components.png" alt="Qubes Components">

= Building qubes-gui-daemon =

From ''qubes-builder'' run ''make gui-daemon'' for building the gui-daemon
component. Alternatively you can ''make all BACKEND_VMM=xen'', you should be
installing dependecies manually in this case.

= What GUI Daemon has =

* screen layout handler
* icon updater

= Build Status =
<!--
EDITORS NOTE:
Please do not fix the picture links with html tags.
-->

* Status: <img src="https://travis-ci.com/QubesOS/qubes-gui-daemon.svg?branch=release3.2" alt="loading..."> | Branch: release3.2 (Qubes GUI Daemon Package 3.2)
* Status <img src="https://travis-ci.com/QubesOS/qubes-gui-daemon.svg?branch=release4.0" alt="loading..."> | Branch: release4.0 (Qubes GUI Daemon Package 4.0)
* Status: <img src="https://travis-ci.com/QubesOS/qubes-gui-daemon.svg?branch=master" alt="loading..."> | Branch: master (Qubes GUI Daemon Package)
