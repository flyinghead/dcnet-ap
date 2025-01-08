#
# dependencies: ppp-dev libsqlite3-dev
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
	sqlite3 /usr/local/var/ppp-addr.db < createdb.sql

