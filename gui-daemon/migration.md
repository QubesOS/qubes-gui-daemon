## C<->Zig interop tips

translate command:

```
zig translate-c -I/usr/include  -I../include/ -I/usr/include/vchan-xen -I/usr/include/libpng16 -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/glib-2.0 -I/usr/lib/glib-2.0/include -I/usr/include/sysprof-4 -I/usr/include/libmount -I/usr/include/blkid xside.c
```
