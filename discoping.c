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
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netdb.h>

const uint32_t MAGIC = 0xDC15C001;
#define PING 1
#define PONG 2
#define DISCOVER 3
#define DNS_TTL 3600

uint16_t port = 7655;
const char *accessPointsFile = "/etc/dcnet/accesspoints";
int sockfd = -1;
struct stat oldstat;
time_t lastRefresh;

uint8_t accessPoints[512];
size_t apSize;

void error(const char *str)
{
  perror(str);
  exit(1);
}

void pong(struct sockaddr_in *addr, const uint8_t *data, size_t len)
{
	uint8_t resp[13];
	if (len != sizeof(resp)) {
		fprintf(stderr, "Invalid ping packet received: len %zd\n", len);
		return;
	}
	memcpy(resp, data, sizeof(resp));
	resp[4] = PONG;
	ssize_t sent = sendto(sockfd, resp, sizeof(resp), 0, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
    if (sent < 0)
      error("ERROR: sendto");
}

uint32_t resolve(const char *servname)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;		// IPv4
	hints.ai_socktype = SOCK_DGRAM;	// Datagram socket

	struct addrinfo *result;
	int rc = getaddrinfo(servname, NULL, &hints, &result);
	if (rc != 0) {
		fprintf(stderr, "%s: DNS failure: %s\n", servname, gai_strerror(rc));
		return 0;
	}
	if (result == NULL) {
		fprintf(stderr, "%s: DNS failure: no record found\n", servname);
		return 0;
	}
	char ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &((struct sockaddr_in *)result->ai_addr)->sin_addr, ip, INET_ADDRSTRLEN);
	printf("%s: found %s\n", servname, ip);
	uint32_t ipaddr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(result);

	return ipaddr;
}

void refresh()
{
	struct stat newstat;
	if (stat(accessPointsFile, &newstat)) {
		perror(accessPointsFile);
		return;
	}
	if (memcmp(&oldstat.st_mtim, &newstat.st_mtim, sizeof(oldstat.st_mtim))) {
		// Force refresh if access points file has changed
		lastRefresh = 0;
		memcpy(&oldstat.st_mtim, &newstat.st_mtim, sizeof(oldstat.st_mtim));
	}
	time_t now = time(NULL);
	if (now - lastRefresh < DNS_TTL)
		return;

	lastRefresh = now;
	FILE *f = fopen(accessPointsFile, "r");
	if (f == NULL) {
		perror(accessPointsFile);
		return;
	}
	uint8_t *apbuf = &accessPoints[0];
	// file format:
	// 1.2.3.4 DCNet Europe
	// 1.2.3.5 DCNet USA
	// or
	// dcnet.flyca.st DCNet America
	// dcnet-eu.flyca.st DCNet Europe
	// dcnet-br.flyca.st
	char line[256];
	while (fgets(line, sizeof(line), f) != NULL)
	{
		if (line[0] == '#')
			continue;
		const char *ip = strtok(line, " \t\r\n");
		if (ip == NULL)
			continue;
		uint32_t apaddr = resolve(ip);
		if (apaddr == 0)
			continue;
		memcpy(apbuf, &apaddr, 4);
		/*
		struct in_addr apaddr;
		if (inet_aton(ip, &apaddr) == 0) {
			fprintf(stderr, "Invalid IP: %s\n", ip);
			continue;
		}
		memcpy(apbuf, &apaddr.s_addr, 4);
		*/
		apbuf += 4;
		const char *name = strtok(NULL, "\r\n");
		if (name == NULL)
			name = ip;
		size_t l = strlen(name);
		if (l > 16)
			l = 16;
		*apbuf++ = (uint8_t)l;
		memcpy(apbuf, name, l);
		apbuf += l;
	}
	apSize = (size_t)(apbuf - &accessPoints[0]);
}

void disco(struct sockaddr_in *addr, const uint8_t *data, size_t len)
{
	refresh();
	uint8_t resp[512];
	memcpy(&resp[0], data, 5);
	memcpy(&resp[5], accessPoints, apSize);
	ssize_t sent = sendto(sockfd, resp, apSize + 5, 0, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
    if (sent < 0)
      error("ERROR: sendto");
}

int main(int argc, char *argv[])
{
	if (argc > 3) {
		fprintf(stderr, "usage: %s [<port> [<access points file>] ]\n", argv[0]);
		fprintf(stderr, "Default port: %d. Default access points file: %s\n", port, accessPointsFile);
		exit(1);
	}
	if (argc >= 2) {
		port = (uint16_t)atoi(argv[1]);
	    if (port < 1 || port > 65535) {
	    	fprintf(stderr, "%d is an invalid port.\n", port);
	    	exit(1);
	    }
	    if (argc > 2)
	    	accessPointsFile = argv[2];
	}
    printf("Started discoping on port %d with list %s\n", port, accessPointsFile);
    refresh();
	sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sockfd < 0)
		error("ERROR opening socket");
	int optval = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	struct sockaddr_in serveraddr;
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons(port);
	serveraddr.sin_addr.s_addr = INADDR_ANY;
	int rc = bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr));
	if (rc < 0)
		error("ERROR: bind");

	for (;;)
	{
		struct sockaddr_in srcAddr;
		socklen_t addrlen = sizeof(srcAddr);
		uint8_t data[64];
		ssize_t len = recvfrom(sockfd, data, sizeof(data), 0, (struct sockaddr *)&srcAddr, &addrlen);
		if (len < 0)
			error("ERROR: recvfrom");
		if (len < 5) {
			fprintf(stderr, "Invalid packet received: len %zd\n", len);
			continue;
		}
		if (memcmp(&MAGIC, data, sizeof(MAGIC))) {
			fprintf(stderr, "Invalid packet received: bad magic\n");
			continue;
		}
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &srcAddr.sin_addr, ip, INET_ADDRSTRLEN);
		printf("Received from %s: %d (len %d)\n", ip, data[4], (int)len);
		switch (data[4])
		{
		case PING:
			pong(&srcAddr, data, (size_t)len);
			break;
		case DISCOVER:
			disco(&srcAddr, data, (size_t)len);
			break;
		default:
			fprintf(stderr, "Invalid packet received: bad op\n");
			break;
		}
	}

	return 0;
}
