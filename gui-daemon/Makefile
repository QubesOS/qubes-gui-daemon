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

VCHAN_PKG = $(if $(BACKEND_VMM),vchan-$(BACKEND_VMM),vchan)
CC=gcc
CFLAGS+=-I../include/ -g -O2 -Wall -Wextra -Werror -pie -fPIC \
		`pkg-config --cflags libconfig` \
		`pkg-config --cflags libnotify` \
		`pkg-config --cflags libpng` \
		`pkg-config --cflags $(VCHAN_PKG)`
OBJS=xside.o png.o trayicon.o ../gui-common/double-buffer.o ../gui-common/txrx-vchan.o \
	../gui-common/error.o ../common/list.o

all: qubes-guid qubes-guid.1
qubes-guid: $(OBJS)
	$(CC) -g -pie -o qubes-guid $(OBJS) \
		-lX11 -lXext -Wall -lm \
		`pkg-config --libs $(VCHAN_PKG)` \
		`pkg-config --libs libpng` \
		`pkg-config --libs libconfig` \
		`pkg-config --libs libnotify`

qubes-guid.1: qubes-guid
	LC_ALL=C help2man --version-string=`cat ../version` --no-info --name="Qubes GUI daemon" ./qubes-guid  > qubes-guid.1

clean:
	rm -f qubes-guid *.o *~
