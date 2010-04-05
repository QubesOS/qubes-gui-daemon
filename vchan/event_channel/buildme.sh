make -C /lib/modules/`uname -r`/build/ SUBDIRS=`pwd` modules \
	EXTRA_CFLAGS=-I`pwd`/../xenincl
