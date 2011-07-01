#
# This is the SPEC file for creating binary and source RPMs for the VMs.
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

Name:		qubes-gui-vm	
Version:	%{version}
Release:	1%{dist}
Summary:	The Qubes GUI Agent for AppVMs

Group:		Qubes
Vendor:		Invisible Things Lab
License:	GPL
URL:		http://www.qubes-os.org

Source:		.

%define pa_ver 0.9.21

BuildRequires:	gcc
BuildRequires:	kernel-devel
BuildRequires:	libX11-devel
BuildRequires:	libXcomposite-devel
BuildRequires:	libXdamage-devel
BuildRequires:	libXt-devel
BuildRequires:	libtool-ltdl-devel
BuildRequires:	libtool-ltdl-devel
BuildRequires:	pulseaudio-libs-devel
BuildRequires:	xen-devel
BuildRequires:	xorg-x11-server-devel
BuildRequires:	qubes-core-appvm-devel >= 1.6.1
BuildRequires:	qubes-core-appvm-libs
BuildRequires:	ConsoleKit-devel
Requires:	qubes-core-vm xen-qubes-vm-essentials
Requires:	pulseaudio = %{pa_ver}
AutoReq: 0

%define _builddir %(pwd)

%description
The Qubes GUI agent that needs to be installed in VM in order to provide the Qubes fancy GUI.

%prep
# we operate on the current directory, so no need to unpack anything

%build
#make clean
make appvm

%install
rm -rf $RPM_BUILD_ROOT
install -D gui-agent/qubes_gui $RPM_BUILD_ROOT/usr/bin/qubes_gui
install -D appvm_scripts/usrbin/qubes-session $RPM_BUILD_ROOT/usr/bin/qubes-session
install -D appvm_scripts/usrbin/qubes_run_xorg.sh $RPM_BUILD_ROOT/usr/bin/qubes_run_xorg.sh
install -D appvm_scripts/usrbin/qubes_xorg_wrapper.sh $RPM_BUILD_ROOT/usr/bin/qubes_xorg_wrapper.sh
install -D consolekit/ck-xinit-session-qubes $RPM_BUILD_ROOT/usr/bin/ck-xinit-session-qubes
install -D pulse/start-pulseaudio-with-vchan $RPM_BUILD_ROOT/usr/bin/start-pulseaudio-with-vchan
install -D pulse/libsetup-vchan-early.so $RPM_BUILD_ROOT/%{_libdir}/libsetup-vchan-early.so
install -D pulse/module-vchan-sink.so $RPM_BUILD_ROOT/%{_libdir}/pulse-%{pa_ver}/modules/module-vchan-sink.so
install -D xf86-input-mfndev/src/.libs/qubes_drv.so $RPM_BUILD_ROOT/%{_libdir}/xorg/modules/drivers/qubes_drv.so
install -D relaxed_xf86ValidateModes/relaxed_xf86ValidateModes.so $RPM_BUILD_ROOT/%{_libdir}/relaxed_xf86ValidateModes.so
install -D appvm_scripts/etc/X11/xorg-qubes.conf.template $RPM_BUILD_ROOT/etc/X11/xorg-qubes.conf.template
install -D appvm_scripts/etc/init.d/qubes_gui $RPM_BUILD_ROOT/etc/init.d/qubes_gui
install -D appvm_scripts/etc/profile.d/qubes_gui.sh $RPM_BUILD_ROOT/etc/profile.d/qubes_gui.sh
install -D appvm_scripts/etc/profile.d/qubes_gui.csh $RPM_BUILD_ROOT/etc/profile.d/qubes_gui.csh
install -D appvm_scripts/etc/profile.d/qubes-session.sh $RPM_BUILD_ROOT/etc/profile.d/qubes-session.sh
install -D appvm_scripts/etc/sysconfig/desktop $RPM_BUILD_ROOT/etc/sysconfig/desktop
install -D appvm_scripts/etc/X11/xinit/xinitrc.d/qubes_keymap.sh $RPM_BUILD_ROOT/etc/X11/xinit/xinitrc.d/qubes_keymap.sh
install -d $RPM_BUILD_ROOT/var/log/qubes

%post
chkconfig qubes_gui on

# use qubes-specific consolekit plugin
sed -i 's#/usr/bin/ck-xinit-session\([^-]\)#/usr/bin/ck-xinit-session-qubes\1#' /etc/X11/xinit/xinitrc-common

%preun
if [ "$1" = 0 ] ; then
	chkconfig qubes_gui off
fi

%triggerin -- xorg-x11-xinit

# use qubes-specific consolekit plugin
sed -i 's#/usr/bin/ck-xinit-session\([^-]\)#/usr/bin/ck-xinit-session-qubes\1#' /etc/X11/xinit/xinitrc-common

%clean
rm -rf $RPM_BUILD_ROOT


%files
%defattr(-,root,root,-)
/usr/bin/qubes_gui
/usr/bin/qubes-session
/usr/bin/qubes_run_xorg.sh
/usr/bin/qubes_xorg_wrapper.sh
%attr(4755,root,root)/usr/bin/ck-xinit-session-qubes
/usr/bin/start-pulseaudio-with-vchan
%{_libdir}/libsetup-vchan-early.so
%{_libdir}/pulse-%{pa_ver}/modules/module-vchan-sink.so
%{_libdir}/xorg/modules/drivers/qubes_drv.so
%attr(4755,root,root) %{_libdir}/relaxed_xf86ValidateModes.so
%attr(0644,root,root) /etc/X11/xorg-qubes.conf.template
/etc/init.d/qubes_gui
/etc/profile.d/qubes_gui.sh
/etc/profile.d/qubes_gui.csh
/etc/profile.d/qubes-session.sh
/etc/X11/xinit/xinitrc.d/qubes_keymap.sh
%config /etc/sysconfig/desktop
%dir /var/log/qubes
