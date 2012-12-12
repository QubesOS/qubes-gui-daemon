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
Release:	1%{dist}
Summary:	The Qubes GUI virtualization (Dom0 side) 

Group:		Qubes
Vendor:		Invisible Things Lab
License:	GPL
URL:		http://www.qubes-os.org

Source:		.

Requires:	qubes-core-dom0 >= 1.3.14
Requires:	xorg-x11-server-Xorg kdm pulseaudio-libs
Requires:	/usr/bin/kdialog
Requires:	pulseaudio
Requires:	libconfig
BuildRequires:  pulseaudio-libs-devel
BuildRequires:  xen-devel
BuildRequires:  libXt-devel
BuildRequires:  libXext-devel
BuildRequires:	libconfig-devel
BuildRequires:	gcc
BuildRequires:	qubes-core-libs-devel >= 1.6.1
BuildRequires:	qubes-core-libs

%define _builddir %(pwd)

%description
The Qubes GUI virtualization infrastructure that needs to be installed in Dom0.

%prep
# we operate on the current directory, so no need to unpack anything
# symlink is to generate useful debuginfo packages
rm -f %{name}-%{version}
ln -sf . %{name}-%{version}
%setup -T -D

%build
make clean
make dom0

%pre

%install
rm -rf $RPM_BUILD_ROOT
install -D gui-daemon/qubes_guid $RPM_BUILD_ROOT/usr/bin/qubes_guid
install -D pulse/pacat-simple-vchan $RPM_BUILD_ROOT/usr/bin/pacat-simple-vchan
install -D pulse/pacat-control-api.xml $RPM_BUILD_ROOT/usr/share/dbus-1/interfaces/org.QubesOS.Audio.xml
install -D shmoverride/X_wrapper_qubes $RPM_BUILD_ROOT/usr/bin/X_wrapper_qubes
install -D shmoverride/shmoverride.so $RPM_BUILD_ROOT/%{_libdir}/shmoverride.so
install -D gui-daemon/guid.conf $RPM_BUILD_ROOT/%{_sysconfdir}/qubes/guid.conf
install -D gui-daemon/qubes-localgroup.sh $RPM_BUILD_ROOT/etc/X11/xinit/xinitrc.d/qubes-localgroup.sh
install -D gui-daemon/qubes.ClipboardPaste.policy $RPM_BUILD_ROOT%{_sysconfdir}/qubes_rpc/policy/qubes.ClipboardPaste

%triggerin -- xorg-x11-server-Xorg
ln -sf /usr/bin/X_wrapper_qubes /usr/bin/X

%postun
if [ "$1" = 0 ] ; then
	# no more packages left
    ln -sf /usr/bin/Xorg /usr/bin/X
fi

%clean
rm -rf $RPM_BUILD_ROOT
rm -f %{name}-%{version}

%files
%defattr(-,root,root,-)
%attr(4750,root,qubes) /usr/bin/qubes_guid
/usr/bin/pacat-simple-vchan
/usr/bin/X_wrapper_qubes
%{_libdir}/shmoverride.so
%config(noreplace) %{_sysconfdir}/qubes/guid.conf
%config(noreplace) %{_sysconfdir}/qubes_rpc/policy/qubes.ClipboardPaste
/etc/X11/xinit/xinitrc.d/qubes-localgroup.sh
/usr/share/dbus-1/interfaces/org.QubesOS.Audio.xml
