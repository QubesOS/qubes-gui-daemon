#
# This is the SPEC file for creating binary and source RPMs for the Dom0.
#
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


%{!?version: %define version %(cat version)}

Name:		qubes-gui-dom0	
Version:	%{version}
Release:	1
Summary:	The Qubes GUI virtualization (Dom0 side) 

Group:		Qubes
Vendor:		Invisible Things Lab
License:	GPL
URL:		http://www.qubes-os.org

Source:		.

Requires:	qubes-core-dom0 xorg-x11-server-Xorg kdm

%define _builddir %(pwd)

%description
The Qubes GUI virtualization infrastructure that needs to be installed in Dom0.

%prep
# we operate on the current directory, so no need to unpack anything

%build
make clean
make dom0

%pre

%install
rm -rf $RPM_BUILD_ROOT
install -D gui-daemon/qubes_guid $RPM_BUILD_ROOT/usr/bin/qubes_guid
install -D shmoverride/X_wrapper_qubes $RPM_BUILD_ROOT/usr/bin/X_wrapper_qubes
install -D shmoverride/shmoverride.so $RPM_BUILD_ROOT/%{_libdir}/shmoverride.so
install -D vchan/vchan/libvchan.so $RPM_BUILD_ROOT/%{_libdir}/libvchan.so
mkdir -p $RPM_BUILD_ROOT/var/log/qubes
mkdir -p $RPM_BUILD_ROOT/var/run/qubes

%triggerin -- xorg-x11-server-Xorg
ln -sf /usr/bin/X_wrapper_qubes /usr/bin/X

%postun
if [ "$1" = 0 ] ; then
	# no more packages left
    ln -sf /usr/bin/Xorg /usr/bin/X
fi

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%attr(4750,root,qubes) /usr/bin/qubes_guid
/usr/bin/X_wrapper_qubes
%{_libdir}/shmoverride.so
%{_libdir}/libvchan.so
%attr(2770,root,qubes) %dir /var/log/qubes
%attr(770,root,qubes) %dir /var/run/qubes
