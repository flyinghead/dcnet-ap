#
# dependencies: ppp ppp-dev sqlite3 libsqlite3-dev
#
INSTALL_DIR=/usr/local/
CFLAGS=-O3 -fPIC -Wall
CXXFLAGS=-O3 -Wall
DEPS=

all: ppp-ipaddr.so ethtap

ppp-ipaddr.so: ppp-ipaddr.o
	$(CC) -shared -o $@ $< -lsqlite3

ethtap: ethtap.o
	$(CXX) $(CXXFLAGS) -o $@ $<

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

%.o: %.cpp $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

install: all
	mkdir -p $(INSTALL_DIR)/lib/pppd/2.4.9
	install -m 0644 ppp-ipaddr.so $(INSTALL_DIR)/lib/pppd/2.4.9
	mkdir -p $(INSTALL_DIR)/bin
	install ethtap $(INSTALL_DIR)/bin

clean:
	rm -f *.o ppp-ipaddr.so ethtap

createdb:
	mkdir -p /usr/local/var
	sqlite3 /usr/local/var/ppp-addr.db < createdb.sql

createservice:
	mkdir -p /usr/local/lib/systemd
	cp pppd.socket pppd@.service ethtap.service /usr/local/lib/systemd
	systemctl enable /usr/local/lib/systemd/pppd.socket
	systemctl enable /usr/local/lib/systemd/pppd@.service
	systemctl start pppd.socket
	systemctl enable /usr/local/lib/systemd/ethtap.service
	systemctl start ethtap.service

archive:
	tar cvzf ppp-plugin.tar.gz Makefile createdb.sql ppp-ipaddr.c ethtap.cpp \
		pppd.socket pppd@.service ethtap.service dnsmasq-ethtap.conf
