#
# dependencies: ppp (runtime), ppp-dev (build)
#
prefix = /usr/local
exec_prefix = $(prefix)
sbindir = $(exec_prefix)/sbin
libdir = $(exec_prefix)/lib

CFLAGS=-O3 -g -fPIC -Wall -Wconversion
CXXFLAGS=-O3 -g -Wall
DEPS=Makefile

PPP_VER=$(shell echo '#include <pppd/pppdconf.h>' | cc -E $(CFLAGS) - >/dev/null 2>&1 && echo '2.5.2' || echo '2.4.9')
ifeq ("$(PPP_VER)", "2.4.9")
CFLAGS+=-DPPP_24
endif

all: ppp-ipaddr.so ethtap discoping

ppp-ipaddr.so: ppp-ipaddr.o $(DEPS)
	$(CC) -shared -o $@ $<

ethtap: ethtap.o $(DEPS)
	$(CXX) $(CXXFLAGS) -o $@ $<

discoping: discoping.o $(DEPS)
	$(CC) $(CFLAGS) -o $@ $<

dcnetbba: dcnetbba.o $(DEPS)
	$(CXX) $(CXXFLAGS) -o $@ $<

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

%.o: %.cpp $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

install: all
	mkdir -p $(DESTDIR)/usr/lib/pppd/$(PPP_VER) && \
	install -m 0644 ppp-ipaddr.so $(DESTDIR)/usr/lib/pppd/$(PPP_VER)
	mkdir -p $(DESTDIR)$(sbindir)
	install ethtap $(DESTDIR)$(sbindir)
	install discoping $(DESTDIR)$(sbindir)
	install iptables-dcnet $(DESTDIR)$(sbindir)
	mkdir -p $(DESTDIR)/var/log/dcnet

clean:
	rm -f *.o ppp-ipaddr.so ethtap discoping dcnetbba

createservice:
	cp pppd.socket pppd@.service ethtap.service discoping.service iptables-dcnet.service /usr/lib/systemd/system/
	cp psmash-pppd.socket psmash-pppd@.service /usr/lib/systemd/system/
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
	systemctl enable psmash-pppd.socket
	systemctl restart psmash-pppd.socket

archive:
	tar cvzf dcnet-ap.tar.gz Makefile ppp-ipaddr.c ethtap.cpp discoping.c dcnetbba.cpp \
		pppd.socket pppd@.service ethtap.service dnsmasq-ethtap.conf options.dcnet discoping.service \
		accesspoints iptables-dcnet.service iptables-dcnet psmash-pppd.socket psmash-pppd@.service options.psmash
