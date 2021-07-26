VCHAN_PKG = $(if $(BACKEND_VMM),vchan-$(BACKEND_VMM),vchan)
CC ?= gcc
CFLAGS += -Wall -Wextra -Werror -g -O2 -fPIC -D_FORTIFY_SOURCE=2 -fcf-protection -fstack-clash-protection -D_GNU_SOURCE=
VCHANLIBS=`pkg-config --libs $(VCHAN_PKG)`
VCHANCFLAGS=`pkg-config --cflags $(VCHAN_PKG)`
GLIBCFLAGS=`pkg-config --cflags glib-2.0`
GLIBLIBS=`pkg-config --libs glib-2.0`
all: pacat-simple-vchan
pacat-simple-vchan.o: pacat-simple-vchan.c
	$(CC) $(CFLAGS) -c $(VCHANCFLAGS) -I. $(GLIBCFLAGS) pacat-simple-vchan.c
pacat-simple-vchan: pacat-simple-vchan.o
	$(CC) -o pacat-simple-vchan $^ \
		$(VCHANLIBS) -lpulse -lpulse-mainloop-glib -lqubesdb $(GLIBLIBS)
clean:
	rm -f ./*~ ./*.o ./*.so ./pacat-simple-vchan
.PHONY: clean all
