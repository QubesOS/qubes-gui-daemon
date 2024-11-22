#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
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

MAKEFLAGS := -rR
VCHAN_PKG = $(if $(BACKEND_VMM),vchan-$(BACKEND_VMM),vchan)
CC=gcc
pkgs := x11 x11-xcb xcb xcb-shm xcb-aux glib-2.0 $(VCHAN_PKG) libpng libnotify libconfig
objs := xside.o png.o trayicon.o ../gui-common/double-buffer.o ../gui-common/txrx-vchan.o \
	../gui-common/error.o list.o
extra_cflags := -I../include/ -g -O2 -Wall -Wextra -Werror -pie -fPIC \
		$(shell pkg-config --cflags $(pkgs)) \
		-fvisibility=hidden \
		-fno-strict-aliasing \
		-fno-strict-overflow \
		-fno-delete-null-pointer-checks \
		-Wp,-D_GNU_SOURCE -Werror=missing-prototypes

LDLIBS := $(shell pkg-config --libs $(pkgs)) -lqubes-pure
all: qubes-guid # qubes-guid.1
vpath %.c ../common
qubes-guid: $(objs)
	$(CC) -g -pie -o qubes-guid $(objs) -Wall -lm $(LDLIBS) $(LDFLAGS) -Wl,-Bsymbolic

qubes-guid.1: qubes-guid
	LC_ALL=C help2man --version-string=`cat ../version` --no-info --name="Qubes GUI daemon" ./qubes-guid  > qubes-guid.1

clean:
	rm -f qubes-guid ./*.o ./*~

%.o: %.c Makefile
	$(CC) -MD -MP -MF $@.dep -c -o $@ $(extra_cflags) $(CFLAGS) $<
-include *.dep
