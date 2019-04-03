CC=gcc
CFLAGS=-Wall -Wextra -Werror -g -O2
VCHANLIBS=`pkg-config --libs vchan-$(BACKEND_VMM)`
VCHANCFLAGS=`pkg-config --cflags vchan-$(BACKEND_VMM)`
GLIBCFLAGS=`pkg-config --cflags glib-2.0`
GLIBLIBS=`pkg-config --libs glib-2.0`
all: module-vchan-sink.so pacat-simple-vchan
pacat-simple-vchan.o: pacat-simple-vchan.c
	$(CC) $(CFLAGS) -c $(VCHANCFLAGS) -I. $(GLIBCFLAGS) pacat-simple-vchan.c
pacat-simple-vchan: pacat-simple-vchan.o
	$(CC) -o pacat-simple-vchan $^ \
		$(VCHANLIBS) -lpulse -lpulse-mainloop-glib -lqubesdb $(GLIBLIBS)
clean:
	rm -f *.so *.o pacat-simple-vchan *~
