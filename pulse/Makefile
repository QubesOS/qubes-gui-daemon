CC=gcc
VCHANLIBS=-lxenctrl -lxenstore -lvchan
VCHANCFLAGS=-I../vchan/vchan
VCHANLDFLAGS=-L../vchan/vchan
all: module-vchan-sink.so pacat-simple-vchan
module-vchan-sink.so: module-vchan-sink.o
	$(CC) -shared -o module-vchan-sink.so module-vchan-sink.o \
		$(VCHANLDFLAGS) $(VCHANLIBS)
module-vchan-sink.o: module-vchan-sink.c
	$(CC) -Wall -c $(VCHANCFLAGS) -fPIC module-vchan-sink.c  -DHAVE_CONFIG_H -I.
pacat-simple-vchan.o: pacat-simple-vchan.c
	$(CC) -Wall -c $(VCHANCFLAGS) -I. pacat-simple-vchan.c	
vchanio.o: vchanio.c
	$(CC) -Wall -c $(VCHANCFLAGS) vchanio.c	
pacat-simple-vchan: pacat-simple-vchan.o vchanio.o
	$(CC) -o pacat-simple-vchan pacat-simple-vchan.o vchanio.o \
		$(VCHANLDFLAGS) $(VCHANLIBS) -lpulse-simple -lpulse
clean:
	rm -f *.so *.o pacat-simple-vchan *~
