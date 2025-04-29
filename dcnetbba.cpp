/*
	DCNet access point services.
    Copyright (C) 2025 Flyinghead <flyinghead.github@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
// Work in progress bba dreampi utility for dcnet
//
// ip tuntap add mode tap user pi group pi name tap0
// (enable dhcpc for tap0)
// ip route add 172.20.0.0/16 dev tap0
//
// Change destination from local DCNet address to console address on all packets coming from tap0
// iptables -t nat -A PREROUTING -i tap0 -d 172.20.1.1 -j DNAT --to-destination 192.168.1.2
// Change source from console address to local DCNet address on all packets sent to tap0
// iptables -t nat -A POSTROUTING -o tap0 -s 192.168.1.2 -j SNAT --to 172.20.1.1
// FIXME need to know the assigned IP address. DCNETv2?
//
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <linux/if_tun.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <assert.h>

#define DCNET_HOST "dcnet.flyca.st"
#define DCNET_PORT 7655
const char *tap_interface = "tap0";

bool setNonBlocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		flags = 0;
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) != 0) {
		perror("fcntl(O_NONBLOCK)");
		return false;
	}
	return true;
}

int main(int argc, char *argv[])
{
	if (argc >= 2)
		tap_interface = argv[1];
	fprintf(stderr, "DCNet BBA starting on interface %s\n", tap_interface);

	// Resolve server name
	addrinfo hints {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags |= AI_CANONNAME;
	addrinfo *result;
	if (getaddrinfo(DCNET_HOST, nullptr, &hints, &result))
		error(-1, errno, "getaddrinfo");
	if (result == nullptr) {
		fprintf(stderr, "%s: host not found\n", DCNET_HOST);
		return -1;
	}
	char s[100];
	sockaddr_in *serverAddress = (sockaddr_in *)result->ai_addr;
	inet_ntop(result->ai_family, &serverAddress->sin_addr, s, 100);
	serverAddress->sin_port = htons(DCNET_PORT);
	printf("connecting to %s (%s)\n", s, result->ai_canonname);

	// Connect
	int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (connect(sock, (const sockaddr *)serverAddress, sizeof(sockaddr)))
		error(-1, errno, "connect");
	int optval = 1;
	setsockopt(sock, SOL_TCP, TCP_NODELAY, &optval, (socklen_t)sizeof(optval));

	// Write prolog
	uint8_t prolog[] = { 6, 0, 'D', 'C', 'N', 'E', 'T', 1 };
	if (write(sock, prolog, sizeof(prolog)) != sizeof(prolog))
		error(-1, errno, "write(prolog)");

	// Now open the tap device
	int tap_fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
	if (tap_fd < 0)
		error(-1, errno, "/dev/net/tun");

	// Set tap mode
	ifreq ifr {};
	strncpy(ifr.ifr_name, tap_interface, IFNAMSIZ - 1);
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	if (ioctl(tap_fd, TUNSETIFF, &ifr))
		error(-1, errno, "ioctl(TUNSETIFF)");

	// Set interface up
	ioctl(sock, SIOCGIFFLAGS, &ifr);
	ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
	if (ioctl(sock, SIOCSIFFLAGS, &ifr))
		error(-1, errno, "ioctl(SIOCSIFFLAGS)");

	// And start pumping data
	setNonBlocking(tap_fd);
	setNonBlocking(sock);

	uint8_t inbuf[1600];
	uint8_t outbuf[1600];
	unsigned inbuflen = 0;
	unsigned outbuflen = 0;
	for (;;)
	{
		fd_set readfds;
		FD_ZERO(&readfds);
		if (inbuflen < sizeof(inbuf))
			FD_SET(sock, &readfds);
		if (outbuflen == 0)
			// Don't read from tap until the current frame is sent out
			FD_SET(tap_fd, &readfds);

		fd_set writefds;
		FD_ZERO(&writefds);
		if (inbuflen > 2)
		{
			// Only write full frames to tap
			uint16_t framelen = *(uint16_t *)&inbuf[0];
			assert(framelen + 2u < sizeof(inbuf));
			if (inbuflen >= framelen + 2u)
				FD_SET(tap_fd, &writefds);
		}
		if (outbuflen > 0)
			FD_SET(sock, &writefds);

		int nfds = (sock > tap_fd ? sock : tap_fd) + 1;
		if (select(nfds, &readfds, &writefds, nullptr, nullptr) == -1)
		{
			if (errno == EINTR)
				continue;
			perror("select");
			break;
		}
		if (FD_ISSET(tap_fd, &readfds))
		{
			ssize_t ret = read(tap_fd, outbuf + 2, sizeof(outbuf) - 2u);
			if (ret < 0)
			{
				if (errno != EINTR && errno != EWOULDBLOCK) {
					perror("read(tap)");
					break;
				}
				ret = 0;
			}
			else if (ret == 0) {
				fprintf(stderr, "tap read EOF\n");
				break;
			}
			if (ret > 0)
			{
				printf("Out frame: %zd\n", ret);
				*(uint16_t *)&outbuf[0] = ret;
				outbuflen = ret + 2;
				FD_SET(sock, &writefds);
			}
		}
		if (FD_ISSET(sock, &readfds))
		{
			ssize_t ret = read(sock, inbuf + inbuflen, sizeof(inbuf) - (size_t)inbuflen);
			if (ret < 0)
			{
				if (errno != EINTR && errno != EWOULDBLOCK) {
					perror("read(socket)");
					break;
				}
				ret = 0;
			}
			else if (ret == 0) {
				fprintf(stderr, "socket read EOF\n");
				break;
			}
			if (ret > 0) {
				inbuflen += ret;
				FD_SET(tap_fd, &writefds);
			}
		}
		if (FD_ISSET(tap_fd, &writefds))
		{
			uint16_t framelen = *(uint16_t *)&inbuf[0];
			assert(inbuflen >= framelen + 2u);
			printf("In frame: %d\n", framelen);
			ssize_t ret = write(tap_fd, inbuf + 2, framelen);
			if (ret < 0) {
				if (errno != EINTR && errno != EWOULDBLOCK) {
					perror("write(tap)");
					break;
				}
				ret = 0;
			}
			if (ret > 0)
			{
				if (ret != framelen)
					fprintf(stderr, "WARNING: tap write truncated %d -> %zd\n", framelen, ret);
				inbuflen -= framelen + 2;
				if (inbuflen > 0)
					memmove(inbuf, inbuf + framelen + 2, (size_t)inbuflen);
			}
		}
		if (FD_ISSET(sock, &writefds))
		{
			ssize_t ret = write(sock, outbuf, (size_t)outbuflen);
			if (ret < 0) {
				if (errno == EINTR && errno != EWOULDBLOCK) {
					perror("write(socket)");
					break;
				}
				ret = 0;
			}
			if (ret > 0)
			{
				printf("Out sent(%d) -> %zd\n", outbuflen, ret);
				outbuflen -= ret;
				if (outbuflen > 0)
					memmove(outbuf, outbuf + ret, (size_t)outbuflen);
			}
		}
	}
	fprintf(stderr, "DCNet BBA stopping\n");
	close(tap_fd);
	close(sock);
	return 0;
}
