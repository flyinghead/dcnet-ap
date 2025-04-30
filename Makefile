#
# dependencies: ppp ppp-dev
#
prefix = /usr/local
exec_prefix = $(prefix)
sbindir = $(exec_prefix)/sbin
libdir = $(exec_prefix)/lib

CFLAGS=-O3 -fPIC -Wall
CXXFLAGS=-O3 -Wall
DEPS=

all: ppp-ipaddr.so ethtap discoping

ppp-ipaddr.so: ppp-ipaddr.o
	$(CC) -shared -o $@ $<

ethtap: ethtap.o
	$(CXX) $(CXXFLAGS) -o $@ $<

discoping: discoping.o
	$(CC) $(CFLAGS) -o $@ $<

dcnetbba: dcnetbba.o
	$(CXX) $(CXXFLAGS) -o $@ $<

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

%.o: %.cpp $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

install: all
	PPP_VER=`ls /usr/lib/pppd/ | head -1` ; \
	mkdir -p $(DESTDIR)/usr/lib/pppd/$$PPP_VER && \
	install -m 0644 ppp-ipaddr.so $(DESTDIR)/usr/lib/pppd/$$PPP_VER
	mkdir -p $(DESTDIR)$(sbindir)
	install ethtap $(DESTDIR)$(sbindir)
	install discoping $(DESTDIR)$(sbindir)
	install iptables-dcnet $(DESTDIR)$(sbindir)
	mkdir -p $(DESTDIR)/var/log/dcnet

clean:
	rm -f *.o ppp-ipaddr.so ethtap discoping dcnetbba

createservice:
	cp pppd.socket pppd@.service ethtap.service discoping.service iptables-dcnet.service /usr/lib/systemd/system/
	sed -i -e "s:/usr/local/sbin/:$(sbindir)/:g" /usr/lib/systemd/system/discoping.service
	sed -i -e "s:/usr/local/sbin/:$(sbindir)/:g" /usr/lib/systemd/system/ethtap.service
	sed -i -e "s:/usr/local/sbin/:$(sbindir)/:g" /usr/lib/systemd/system/iptables-dcnet.service
	systemctl enable pppd.socket
	systemctl restart pppd.socket
	systemctl enable ethtap.service
	systemctl restart ethtap.service
	systemctl enable discoping.service
	systemctl restart discoping.service
	systemctl enable iptables-dcnet.service
	systemctl restart iptables-dcnet.service

archive:
	tar cvzf dcnet-ap.tar.gz Makefile ppp-ipaddr.c ethtap.cpp discoping.c dcnetbba.cpp \
		pppd.socket pppd@.service ethtap.service dnsmasq-ethtap.conf options.dcnet discoping.service \
		accesspoints iptables-dcnet.service iptables-dcnet
