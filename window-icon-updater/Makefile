all:
	@true

install:
	install -D icon-receiver $(DESTDIR)/usr/lib/qubes/icon-receiver
	install -d $(DESTDIR)/etc/qubes-rpc
	ln -s /var/run/qubes/icon-receiver.sock $(DESTDIR)/etc/qubes-rpc/qubes.WindowIconUpdater
	install -d $(DESTDIR)/etc/qubes/rpc-config
	install -m 0664 -D qubes.WindowIconUpdater.config $(DESTDIR)/etc/qubes/rpc-config/qubes.WindowIconUpdater
	install -m 0664 -D qubes-icon-receiver.desktop $(DESTDIR)/etc/xdg/autostart/qubes-icon-receiver.desktop
