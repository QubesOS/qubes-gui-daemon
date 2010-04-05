#!/bin/sh
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

#expects W, H, MEM and DEPTH env vars to be set by invoker
RES="$W"x"$H"
MODELINE="70 $W $(($W+1)) $(($W+2)) $(($W+3)) $H $(($H+1)) $(($H+2)) $(($H+3))"
sed -e  s/%MEM%/$MEM/ \
	    -e  s/%DEPTH%/$DEPTH/ \
	    -e  s/%MODELINE%/"$MODELINE"/ \
	    -e  s/%RES%/QB$RES/ < /etc/X11/xorg-qubes.conf.template \
	    > /etc/X11/xorg-qubes.conf
exec /usr/bin/Xorg -nolisten tcp :0 vt02 -config /etc/X11/xorg-qubes.conf
