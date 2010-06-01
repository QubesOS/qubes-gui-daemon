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

help:
	@echo "Qubes GUI main Makefile:" ;\
	    echo "make dom0             <--- make GUI code for Dom0"; \
	    echo "make appvm            <--- make GUI agent code for VM"; \
	    echo; \
	    echo "make rpms             <--- make all rpms and sign them";\
	    echo "make rpms_dom0        <--- create binary rpms for dom0"; \
	    echo "make rpms_appvm       <--- create binary rpms for appvm"; \
	    echo "make rpms_appvm_kmods <--- create kernel module rpms for appvm"; \
	    echo; \
	    echo "make update_repo      <-- copy newly generated rpms to qubes yum repo";\
	    echo "make clean            <--- clean all the binary files";\
	    exit 0;

dom0: vchan/vchan/libvchan.so gui-daemon/qubes-guid shmoverride/shmoverride.so shmoverride/X_wrapper_qubes pulse/pacat-simple-vchan

appvm: vchan/vchan/libvchan.so gui-agent/qubes-gui vchan/u2mfn/u2mfn.ko xf86-input-mfndev/src/.libs/qubes_drv.so pulse/module-vchan-sink.so

gui-daemon/qubes-guid:
	(cd gui-daemon; $(MAKE))

shmoverride/shmoverride.so:
	(cd shmoverride; $(MAKE) shmoverride.so)

shmoverride/X_wrapper_qubes:
	(cd shmoverride; $(MAKE) X_wrapper_qubes)
	

vchan/vchan/libvchan.so: vchan/u2mfn/u2mfnlib.o
	(cd vchan/vchan; $(MAKE) libvchan.so)

pulse/pacat-simple-vchan:
	$(MAKE) -C pulse pacat-simple-vchan

gui-agent/qubes-gui:
	(cd gui-agent; $(MAKE))

vchan/u2mfn/u2mfn.ko:
	(cd vchan/u2mfn; ./buildme.sh)

vchan/u2mfn/u2mfnlib.o:
	(cd vchan/u2mfn; make)

xf86-input-mfndev/src/.libs/qubes_drv.so:
	(cd xf86-input-mfndev && ./bootstrap && ./configure && make)

pulse/module-vchan-sink.so:
	$(MAKE) -C pulse module-vchan-sink.so

make rpms:
	@make rpms_dom0
	@make rpms_appvm_kmods
	@make rpms_appvm

	rpm --addsign rpm/x86_64/*.rpm
	(if [ -d rpm/i686 ] ; then rpm --addsign rpm/i686/*.rpm; fi)

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

update_repo:
	ln -f rpm/x86_64/*.rpm ../yum/rpm/
	(if [ -d rpm/i686 ] ; then ln -f rpm/i686/*.rpm ../yum/rpm/; fi)

clean:
	(cd common; $(MAKE) clean)
	(cd gui-agent; $(MAKE) clean)
	(cd gui-common; $(MAKE) clean)
	(cd gui-daemon; $(MAKE) clean)
	(cd shmoverride; $(MAKE) clean)
	$(MAKE) -C pulse clean
	(cd xf86-input-mfndev; if [ -e Makefile ] ; then $(MAKE) distclean; fi; ./bootstrap --clean || echo )
	(cd vchan; $(MAKE) clean)
	(cd vchan/event_channel; ./cleanup.sh || echo)
	(cd vchan/u2mfn; ./cleanup.sh || echo)

