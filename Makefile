#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2010  Joanna Rutkowska <joanna@invisiblethingslab.com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
#

MANDIR ?= /usr/share/man
LIBDIR ?= /usr/lib64
export LIBDIR

help:
	@echo "Qubes GUI main Makefile:" ;\
	    echo "make all                  <--- build binaries";\
	    echo "make all                  <--- install files into \$$DESTDIR";\
	    echo "make clean                <--- clean all the binary files";\
	    exit 0;

all: gui-daemon/qubes-guid shmoverride/shmoverride.so shmoverride/X-wrapper-qubes \
		pulse/pacat-simple-vchan screen-layout-handler/watch-screen-layout-changes

gui-daemon/qubes-guid:
	(cd gui-daemon; $(MAKE))

shmoverride/shmoverride.so:
	(cd shmoverride; $(MAKE) shmoverride.so)

shmoverride/X-wrapper-qubes:
	(cd shmoverride; $(MAKE) X-wrapper-qubes)
	
pulse/pacat-simple-vchan:
	$(MAKE) -C pulse pacat-simple-vchan

screen-layout-handler/watch-screen-layout-changes:
	$(MAKE) -C screen-layout-handler watch-screen-layout-changes

install:
	install -D gui-daemon/qubes-guid $(DESTDIR)/usr/bin/qubes-guid
	install -m 0644 -D gui-daemon/qubes-guid.1 $(DESTDIR)$(MANDIR)/man1/qubes-guid.1
	install -D pulse/pacat-simple-vchan $(DESTDIR)/usr/bin/pacat-simple-vchan
	install -D pulse/qubes.AudioInputEnable $(DESTDIR)/etc/qubes-rpc/qubes.AudioInputEnable
	install -D pulse/qubes.AudioInputDisable $(DESTDIR)/etc/qubes-rpc/qubes.AudioInputDisable
	install -D shmoverride/X-wrapper-qubes $(DESTDIR)/usr/bin/X-wrapper-qubes
	install -D shmoverride/shmoverride.so $(DESTDIR)$(LIBDIR)/qubes-gui-daemon/shmoverride.so
	install -D -m 0644 gui-daemon/guid.conf $(DESTDIR)/etc/qubes/guid.conf
	install -D gui-daemon/qubes-localgroup.sh $(DESTDIR)/etc/X11/xinit/xinitrc.d/qubes-localgroup.sh
	install -D -m 0644 gui-daemon/qubes.ClipboardPaste.policy $(DESTDIR)/etc/qubes-rpc/policy/qubes.ClipboardPaste
	install -D screen-layout-handler/watch-screen-layout-changes $(DESTDIR)/usr/libexec/qubes/watch-screen-layout-changes
	install -D -m 0644 screen-layout-handler/qubes-screen-layout-watches.desktop $(DESTDIR)/etc/xdg/autostart/qubes-screen-layout-watches.desktop
	$(MAKE) -C window-icon-updater install

tar:
	git archive --format=tar --prefix=qubes-gui/ HEAD -o qubes-gui.tar

clean:
	(cd common; $(MAKE) clean)
	(cd gui-common; $(MAKE) clean)
	(cd gui-daemon; $(MAKE) clean)
	(cd shmoverride; $(MAKE) clean)
	$(MAKE) -C pulse clean

