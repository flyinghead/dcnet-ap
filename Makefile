#
# dependencies: ppp ppp-dev sqlite3 libsqlite3-dev
#
INSTALL_DIR=/usr/local/lib/pppd/2.4.9/
CC=gcc
CFLAGS=-O3 -fPIC -Wall
DEPS=

ppp-ipaddr.so: ppp-ipaddr.o
	$(CC) -shared -o $@ $< -lsqlite3

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

install: ppp-ipaddr.so
	mkdir -p $(INSTALL_DIR)
	install -o root -g root -m 0644 $< $(INSTALL_DIR)

clean:
	rm -f *.o ppp-ipaddr.so

createdb:
	mkdir -p /usr/local/var
	sqlite3 /usr/local/var/ppp-addr.db < createdb.sql

createservice:
	mkdir -p /usr/local/lib/systemd
	cp pppd.socket pppd@.service /usr/local/lib/systemd
	systemctl enable /usr/local/lib/systemd/pppd.socket
	systemctl enable /usr/local/lib/systemd/pppd@.service
	systemctl start pppd.socket

archive:
	tar cvzf ppp-plugin.tar.gz Makefile createdb.sql ppp-ipaddr.c
