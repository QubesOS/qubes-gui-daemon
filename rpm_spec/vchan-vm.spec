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


%define _builddir %(pwd)/vchan
# the following can be overwritten via cmdline

%{!?kernel_version:%{error: "You must define kernel_version in rpmbuild cmdline!"}}

%{!?version: %define version %(cat version_vchan)}

Name:		qubes-vchan-vm-{kernel-%{kernel_version}}
Version:	%{version}
Release:	1
Summary:	The vchan library for the Qubes GUI Agent for AppVMs

Group:		Qubes
Vendor:		Invisible Things Lab
License:	GPL
URL:		http://www.qubes-os.org

Source:		.

BuildRequires:	kernel-devel
Requires:	qubes-core-vm
AutoReq:    0
Provides:	qubes-vchan-vm


%description
The vchan library for the Qubes GUI Agent to be installed in VM.

%prep
# we operate on the current directory, so no need to unpack anything

%build
make clean

#make -C /usr/src/kernels/%{kernel_version} SUBDIRS=%{_builddir}/event_channel modules \
#        EXTRA_CFLAGS=-I%{_builddir}/xenincl

make -C /usr/src/kernels/%{kernel_version} SUBDIRS=%{_builddir}/u2mfn modules \
        EXTRA_CFLAGS=-I%{_builddir}/xenincl

#cd vchan && make

%install
rm -rf $RPM_BUILD_ROOT

#install -D event_channel/evtchn.ko $RPM_BUILD_ROOT/lib/modules/%{kernel_version}/extra/evtchn.ko
install -D u2mfn/u2mfn.ko $RPM_BUILD_ROOT/lib/modules/%{kernel_version}/extra/u2mfn.ko
#install -D vchan/libvchan.so $RPM_BUILD_ROOT/%{_libdir}/libvchan.so
install -D ../appvm_scripts/etc/sysconfig/modules/qubes_vchan.modules $RPM_BUILD_ROOT/etc/sysconfig/modules/qubes_vchan.modules

%post
depmod -a %{kernel_version}
#modprobe evtchn
#modprobe u2mfn

%clean
rm -rf $RPM_BUILD_ROOT


%files
%defattr(-,root,root,-)
#/lib/modules/%{kernel_version}/extra/evtchn.ko
/lib/modules/%{kernel_version}/extra/u2mfn.ko
#/%{_libdir}/libvchan.so
/etc/sysconfig/modules/qubes_vchan.modules




