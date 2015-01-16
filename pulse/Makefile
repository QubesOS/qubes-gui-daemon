CC=gcc
CFLAGS=-Wall -Wextra -Werror -g -O2
VCHANLIBS=`pkg-config --libs vchan-$(BACKEND_VMM)`
VCHANCFLAGS=`pkg-config --cflags vchan-$(BACKEND_VMM)`
GLIBCFLAGS=`pkg-config --cflags glib-2.0` `pkg-config --cflags dbus-glib-1`
GLIBLIBS=`pkg-config --libs glib-2.0` `pkg-config --libs dbus-glib-1` `pkg-config --libs gthread-2.0`
all: module-vchan-sink.so pacat-simple-vchan
pacat-simple-vchan.o: pacat-simple-vchan.c
	$(CC) $(CFLAGS) -c $(VCHANCFLAGS) -I. $(GLIBCFLAGS) pacat-simple-vchan.c
pacat-control-object.o: pacat-control-object.c pacat-control-object.h pacat-control-stub.h
	$(CC) $(CFLAGS) -c $(VCHANCFLAGS) -I. $(GLIBCFLAGS) $<
pacat-simple-vchan: pacat-simple-vchan.o pacat-control-object.o
	$(CC) -o pacat-simple-vchan $^ \
		$(VCHANLIBS) -lpulse -lpulse-mainloop-glib $(GLIBLIBS)
# Do not autogenerate because dbus-binding-tool from dbus-glib-0.86 (in chroot)
# produces output incompatible with dbus-glib-0.84 (in Qubes dom0).
#pacat-control-stub.h: pacat-control-api.xml
#	dbus-binding-tool --mode=glib-server --prefix=pacat-control $< > $@
clean:
	rm -f *.so *.o pacat-simple-vchan *~
