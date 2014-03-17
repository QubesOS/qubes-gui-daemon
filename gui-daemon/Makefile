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

CC=gcc
CFLAGS+=-I../include/ -g -Wall -Wextra -Werror `pkg-config --cflags libconfig` \
	`pkg-config --cflags libpng` -pie -fPIC
OBJS=xside.o png.o ../gui-common/double-buffer.o ../gui-common/txrx-vchan.o \
	../gui-common/error.o ../common/list.o

all: qubes-guid
qubes-guid: $(OBJS)
	$(CC) -g -pie -o qubes-guid $(OBJS) \
		-lX11 -lXext -lvchan -lxenstore -lxenctrl -Wall \
		`pkg-config --libs libpng` \
		`pkg-config --libs libconfig`
clean:
	rm -f qubes-guid *.o *~
