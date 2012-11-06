CC=gcc
VCHANLIBS=-lxenctrl -lxenstore -lvchan
VCHANCFLAGS=-I../vchan/vchan
VCHANLDFLAGS=-L../vchan/vchan
GLIBCFLAGS=`pkg-config --cflags glib-2.0` `pkg-config --cflags dbus-glib-1`
GLIBLIBS=`pkg-config --libs glib-2.0` `pkg-config --libs dbus-glib-1`
all: module-vchan-sink.so pacat-simple-vchan
module-vchan-sink.so: module-vchan-sink.o
	$(CC) -shared -o module-vchan-sink.so module-vchan-sink.o \
		$(VCHANLDFLAGS) $(VCHANLIBS)
module-vchan-sink.o: module-vchan-sink.c
	$(CC) -Wall -c $(VCHANCFLAGS) -fPIC module-vchan-sink.c  -DHAVE_CONFIG_H -I.
pacat-simple-vchan.o: pacat-simple-vchan.c
	$(CC) -Wall -c $(VCHANCFLAGS) -I. $(GLIBCFLAGS) pacat-simple-vchan.c
pacat-control-object.o: pacat-control-object.c pacat-control-object.h pacat-control-stub.h
	$(CC) -Wall -c $(VCHANCFLAGS) -I. $(GLIBCFLAGS) $<
vchanio.o: vchanio.c
	$(CC) -Wall -c $(VCHANCFLAGS) vchanio.c	
pacat-simple-vchan: pacat-simple-vchan.o vchanio.o pacat-control-object.o
	$(CC) -o pacat-simple-vchan $^ \
		$(VCHANLDFLAGS) $(VCHANLIBS) -lpulse -lpulse-mainloop-glib $(GLIBLIBS)
# Do not autogenerate because dbus-binding-tool from dbus-glib-0.86 (in chroot)
# produces output incompatible with dbus-glib-0.84 (in Qubes dom0).
#pacat-control-stub.h: pacat-control-api.xml
#	dbus-binding-tool --mode=glib-server --prefix=pacat_control $< > $@
clean:
	rm -f *.so *.o pacat-simple-vchan *~
