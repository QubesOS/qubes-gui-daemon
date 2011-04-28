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

RPMS_DIR=rpm/
VERSION := $(shell cat version)
VERSION_U2MFN := $(shell cat version_u2mfn)

help:
	@echo "Qubes GUI main Makefile:" ;\
	    echo "make rpms                 <--- make all rpms and sign them";\
	    echo "make rpms_dom0            <--- create binary rpms for dom0"; \
	    echo "make rpms_appvm           <--- create binary rpms for appvm"; \
	    echo "make rpms_appvm_kmods     <--- create kernel module rpms for appvm"; \
	    echo; \
	    echo "make clean                <--- clean all the binary files";\
	    echo "make update-repo-current  <-- copy newly generated rpms to qubes yum repo";\
	    echo "make update-repo-current-testing <-- same, but for -current-testing repo";\
	    echo "make update-repo-unstable <-- same, but to -testing repo";\
	    echo "make update-repo-installer -- copy dom0 rpms to installer repo"
	    exit 0;

dom0: gui-daemon/qubes-guid shmoverride/shmoverride.so shmoverride/X_wrapper_qubes pulse/pacat-simple-vchan

appvm: gui-agent/qubes-gui u2mfn/u2mfn.ko xf86-input-mfndev/src/.libs/qubes_drv.so pulse/module-vchan-sink.so relaxed_xf86ValidateModes/relaxed_xf86ValidateModes.so consolekit/ck-xinit-session-qubes

gui-daemon/qubes-guid:
	(cd gui-daemon; $(MAKE))

shmoverride/shmoverride.so:
	(cd shmoverride; $(MAKE) shmoverride.so)

shmoverride/X_wrapper_qubes:
	(cd shmoverride; $(MAKE) X_wrapper_qubes)
	
pulse/pacat-simple-vchan:
	$(MAKE) -C pulse pacat-simple-vchan

relaxed_xf86ValidateModes/relaxed_xf86ValidateModes.so:
	$(MAKE) -C relaxed_xf86ValidateModes
	
gui-agent/qubes-gui:
	(cd gui-agent; $(MAKE))

u2mfn/u2mfn.ko:
	(cd u2mfn; ./buildme.sh)

xf86-input-mfndev/src/.libs/qubes_drv.so:
	(cd xf86-input-mfndev && ./bootstrap && ./configure && make LDFLAGS=-lu2mfn)

pulse/module-vchan-sink.so:
	$(MAKE) -C pulse module-vchan-sink.so

consolekit/ck-xinit-session-qubes:
	(cd consolekit; $(MAKE))

make rpms:
	@make rpms_dom0
	@make rpms_appvm_kmods
	@make rpms_appvm

	rpm --addsign rpm/x86_64/*$(VERSION)*.rpm rpm/x86_64/qubes-u2mfn*$(VERSION_U2MFN)*.rpm
	(if [ -d rpm/i686 ] ; then rpm --addsign rpm/i686/*$(VERSION)*.rpm; fi)

rpms_appvm:
	rpmbuild --define "_rpmdir rpm/" -bb rpm_spec/gui-vm.spec

rpms_appvm_kmods:
	@echo "Building gui-vm rpm..."
	@echo ===========================
	rpm_spec/build_kernel_rpms.sh

rpms_dom0:
	rpmbuild --define "_rpmdir rpm/" -bb rpm_spec/gui-dom0.spec

tar:
	git archive --format=tar --prefix=qubes-gui/ HEAD -o qubes-gui.tar

clean:
	(cd common; $(MAKE) clean)
	(cd gui-agent; $(MAKE) clean)
	(cd gui-common; $(MAKE) clean)
	(cd gui-daemon; $(MAKE) clean)
	(cd shmoverride; $(MAKE) clean)
	$(MAKE) -C pulse clean
	(cd xf86-input-mfndev; if [ -e Makefile ] ; then $(MAKE) distclean; fi; ./bootstrap --clean || echo )
	(cd u2mfn; $(MAKE) clean)
	$(MAKE) -C relaxed_xf86ValidateModes clean

update-repo-current:
	ln -f $(RPMS_DIR)/x86_64/qubes-gui-dom0-*$(VERSION)*fc13*.rpm ../yum/current-release/current/dom0/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-*-vm-*$(VERSION)*fc13*.rpm ../yum/current-release/current/vm/f13/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-*-vm-*$(VERSION)*fc14*.rpm ../yum/current-release/current/vm/f14/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-u2mfn-vm-*$(VERSION_U2MFN)*.rpm ../yum/current-release/current/vm/f13/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-u2mfn-vm-*$(VERSION_U2MFN)*.rpm ../yum/current-release/current/vm/f14/rpm/
	cd ../yum && ./update_repo.sh

update-repo-current-testing:
	ln -f $(RPMS_DIR)/x86_64/qubes-gui-dom0-*$(VERSION)*fc13*.rpm ../yum/current-release/current-testing/dom0/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-*-vm-*$(VERSION)*fc13*.rpm ../yum/current-release/current-testing/vm/f13/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-*-vm-*$(VERSION)*fc14*.rpm ../yum/current-release/current-testing/vm/f14/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-u2mfn-vm-*$(VERSION_U2MFN)*.rpm ../yum/current-release/current-testing/vm/f13/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-u2mfn-vm-*$(VERSION_U2MFN)*.rpm ../yum/current-release/current-testing/vm/f14/rpm/
	cd ../yum && ./update_repo.sh

update-repo-unstable:
	ln -f $(RPMS_DIR)/x86_64/qubes-gui-dom0-*$(VERSION)*fc13*.rpm ../yum/current-release/unstable/dom0/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-*-vm-*$(VERSION)*fc13*.rpm ../yum/current-release/unstable/vm/f13/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-*-vm-*$(VERSION)*fc14*.rpm ../yum/current-release/unstable/vm/f14/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-u2mfn-vm-*$(VERSION_U2MFN)*.rpm ../yum/current-release/unstable/vm/f13/rpm/
	ln -f $(RPMS_DIR)/x86_64/qubes-u2mfn-vm-*$(VERSION_U2MFN)*.rpm ../yum/current-release/unstable/vm/f14/rpm/
	cd ../yum && ./update_repo.sh

update-repo-installer:
	ln -f $(RPMS_DIR)/x86_64/qubes-gui-dom0-*$(VERSION)*fc13*.rpm ../installer/yum/qubes-dom0/rpm/
	cd ../installer/yum && ./update_repo.sh
