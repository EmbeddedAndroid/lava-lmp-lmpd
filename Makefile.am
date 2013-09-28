bin_PROGRAMS=lmpd spinor
lmpd_SOURCES=main.c http.c netlink.c websocket.c liblmp.c
lmpd_CFLAGS=-fPIC -Wall -Werror -D_FORTIFY_SOURCE=2 -fstack-protector -std=gnu99 -pedantic -DINSTALL_DATADIR=\"${datarootdir}\"
lmpd_LDFLAGS=-fPIC
lmpd_LDADD=-lwebsockets -llejp

spinor_SOURCES=spi-nor.c
spinor_CFLAGS=-fPIC -Wall -Werror -D_FORTIFY_SOURCE=2 -fstack-protector -std=gnu99 -pedantic -DINSTALL_DATADIR=\"${datarootdir}\"
spinor_LDFLAGS=-fPIC
spinor_LDADD=-llejp


#
# cook a random test cert and key
# notice your real cert and key will want to be 0600 permissions
#
lmpd.pem mp.key.pem:
	printf "GB\nErewhon\nAll around\naepd-test-cert\n\nlocalhost\nnone@invalid.org\n" | \
	openssl req -new -newkey rsa:1024 -days 10000 -nodes -x509 -keyout \
	./lmpd.key.pem -out ./lmpd.pem >/dev/null 2>&1  && \
	chmod 644 	./lmpd.key.pem \
			./lmpd.pem

clean-local:
	rm -f ./lmpd.key.pem ./lmpd.pem

EXTRA_INSTALL=share/index.html share/favicon.ico \
	share/lmp-logo.png \
	share/HDMI.png \
	share/USB-minib.png \
	share/USB-A.png \
	share/SD.png \
	share/uSD.png \
	share/undefined.png \
	share/jack4.png \
	share/bus8.png \
	share/RJ45.png \
	share/SATA.png

install-data-local: lmpd.pem lmpd.key.pem
	mkdir -p $(DESTDIR)$(datadir)/lmpd
	cp $(EXTRA_INSTALL) \
	   lmpd.pem \
	   lmpd.key.pem \
		$(DESTDIR)$(datadir)/lmpd

