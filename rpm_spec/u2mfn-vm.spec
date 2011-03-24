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


%define _builddir %(pwd)/u2mfn
# the following can be overwritten via cmdline

%{!?kernel_version:%{error: "You must define kernel_version in rpmbuild cmdline!"}}

%{!?version: %define version %(cat version_u2mfn)}

Name:		qubes-u2mfn-vm-{kernel-%{kernel_version}}
Version:	%{version}
Release:	1
Summary:	The u2mfn kernel module for the Qubes GUI Agent for AppVMs

Group:		Qubes
Vendor:		Invisible Things Lab
License:	GPL
URL:		http://www.qubes-os.org

Source:		.

BuildRequires:	kernel-devel
Requires:	qubes-core-vm
AutoReq:    0
Provides:	qubes-u2mfn-vm


%description
The u2mfn kernel module for the Qubes GUI Agent for AppVMs.

%prep
# we operate on the current directory, so no need to unpack anything

%build
make clean

KERNDIR=/usr/src/kernels/%{kernel_version}
HAVE_GET_PHYS_TO_MACHINE=1
grep -q get_phys_to_machine.*EXPORT_SYMBOL $KERNDIR/Module.symvers || HAVE_GET_PHYS_TO_MACHINE=0

make -C $KERNDIR SUBDIRS=%{_builddir} modules \
        EXTRA_CFLAGS="-DHAVE_GET_PHYS_TO_MACHINE=$HAVE_GET_PHYS_TO_MACHINE"

%install
rm -rf $RPM_BUILD_ROOT

install -D u2mfn.ko $RPM_BUILD_ROOT/lib/modules/%{kernel_version}/extra/u2mfn.ko
install -D ../appvm_scripts/etc/sysconfig/modules/qubes_u2mfn.modules $RPM_BUILD_ROOT/etc/sysconfig/modules/qubes_u2mfn.modules

%post
depmod -a %{kernel_version}

%clean
rm -rf $RPM_BUILD_ROOT


%files
%defattr(-,root,root,-)
/lib/modules/%{kernel_version}/extra/u2mfn.ko
/etc/sysconfig/modules/qubes_u2mfn.modules




