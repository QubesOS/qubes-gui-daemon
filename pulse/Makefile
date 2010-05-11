CC=gcc
VCHANLIBS=-lxenctrl -lxenstore -lvchan
VCHANCFLAGS=-I../vchan/vchan
VCHANLDFLAGS=-L../vchan/vchan
all: module-vchan-sink.so pacat-simple-vchan
module-vchan-sink.so: module-vchan-sink.o libsetup-vchan-early.so
	$(CC) -shared -o module-vchan-sink.so module-vchan-sink.o \
		-L. -lsetup-vchan-early
module-vchan-sink.o: module-vchan-sink.c
	$(CC) -Wall -c $(VCHANCFLAGS) -fPIC module-vchan-sink.c  -DHAVE_CONFIG_H -I.
libsetup-vchan-early.so: libsetup-vchan-early.o
	 $(CC) -shared -o libsetup-vchan-early.so libsetup-vchan-early.o \
	 	$(VCHANLDFLAGS) $(VCHANLIBS)
libsetup-vchan-early.o: libsetup-vchan-early.c
	 $(CC) -Wall -c $(VCHANCFLAGS) -fPIC libsetup-vchan-early.c
pacat-simple-vchan.o: pacat-simple-vchan.c
	$(CC) -Wall -c $(VCHANCFLAGS) -I. pacat-simple-vchan.c	
vchanio.o: vchanio.c
	$(CC) -Wall -c $(VCHANCFLAGS) vchanio.c	
pacat-simple-vchan: pacat-simple-vchan.o vchanio.o
	$(CC) -o pacat-simple-vchan pacat-simple-vchan.o vchanio.o \
		$(VCHANLDFLAGS) $(VCHANLIBS) -lpulse-simple
clean:
	rm -f *.so *.o pacat-simple-vchan *~
