#
# The Qubes OS Project, http://www.qubes-os.org
#
# Copyright (C) 2010  Rafal Wojtczuk  <rafal@invisiblethingslab.com>
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

BACKEND_VMM_PKGCONFIG = $(shell pkg-config --variable=backend_vmm vchan)
BACKEND_VMM ?= $(firstword $(BACKEND_VMM_PKGCONFIG) xen)
ifneq ($(BACKEND_VMM),xen)
$(error shmoverride currently supports Xen only, '$(BACKEND_VMM)' set)
endif

LIBDIR ?= /usr/lib64
extra_cflags := -g -O2 -I../include/ -fPIC -Wall -Wextra -Werror \
		-DBACKEND_VMM_$(BACKEND_VMM) \
		-DSHMOVERRIDE_LIB_PATH=\"$(LIBDIR)/qubes-gui-daemon/shmoverride.so\" \
		-I../include
CC=gcc

all: shmoverride.so X-wrapper-qubes

shmoverride.so: shmoverride.o ./list.o
	$(CC) $(CFLAGS) $(extra_cflags) -shared -o shmoverride.so \
		shmoverride.o list.o -ldl -lxenctrl -lxengnttab

vpath %.c ../common

X-wrapper-qubes: X-wrapper-qubes.o

clean:
	rm -f ./*~ ./*.o shmoverride.so X-wrapper-qubes

%.o: %.c Makefile
	$(CC) -MD -MP -MF $@.dep -c -o $@ $(extra_cflags) $(CFLAGS) $<
-include *.dep
