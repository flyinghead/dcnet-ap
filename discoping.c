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
#include <ctype.h>
#include <poll.h>

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

typedef struct {
	char name[16];
	uint32_t externalIp;
	uint32_t internalIp;
	uint64_t lastPing;
	int pingCount;
	int offline;
} AccessPoint;
AccessPoint accessPoints[16];
int apCount;

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
      perror("ERROR: sendto");
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
	apCount = 0;
	memset(accessPoints, 0, sizeof(accessPoints));
	// File format:
	// <DNS name or external IP address> [<access point name> [<internal IP address>]]
	// Access point name defaults to the DNS name/external address.
	// If access point name contains space or tab characters, it must be enclosed in double quotes.
	// Internal IP address is optional and will be used to check internal connectivity with the access point.
	// Examples:
	// 1.2.3.4 "US Central" 172.21.0.2
	// 1.2.3.5 Europe
	// dcnet.flyca.st "US Central"
	// dcnet-eu.flyca.st Europe 172.21.0.2
	// dcnet-br.flyca.st
	char line[256];
	int lineNum = 0;
	while (fgets(line, sizeof(line), f) != NULL)
	{
		lineNum++;
		char *p = strtok(line, "\r\n");
		if (p == NULL)
			continue;
		while (isblank(*p))
			p++;
		if (*p == '#' || *p == '\0')
			continue;
		const char *dnsName = p;
		while (*p != '\0' && !isblank(*p))
			p++;
		while (isblank(*p))
			*p++ = '\0';
		AccessPoint *ap = &accessPoints[apCount];
		ap->externalIp = resolve(dnsName);
		if (ap->externalIp == 0)
			continue;
		if (*p == '\0') {
			// ip/dns_name only
			strncpy(ap->name, dnsName, sizeof(ap->name) - 1);
			ap->internalIp = 0;
			apCount++;
			continue;
		}
		const char *name = NULL;
		if (*p == '"') {
			name = ++p;
			while (*p != '\0' && *p != '"')
				p++;
			if (*p != '"') {
				fprintf(stderr, "%d: Invalid entry: %s \"%s\n", lineNum, dnsName, name);
				continue;
			}
			*p++ = '\0';
		}
		else {
			name = p;
			while (*p != '\0' && !isblank(*p))
				p++;
		}
		while (isblank(*p))
			*p++ = '\0';
		strncpy(ap->name, name, sizeof(ap->name) - 1);
		if (*p == '\0') {
			ap->internalIp = 0;
		}
		else {
			struct in_addr apaddr;
			if (inet_aton(p, &apaddr) == 0) {
				fprintf(stderr, "%d: Invalid internal IP address: %s\n", lineNum, p);
				continue;
			}
			ap->internalIp = apaddr.s_addr;
		}
		apCount++;
	}
}

void disco(struct sockaddr_in *addr, const uint8_t *data, size_t len)
{
	refresh();
	uint8_t resp[512];
	memcpy(&resp[0], data, 5);
	uint8_t *p = &resp[5];
	for (int i = 0; i < apCount; i++)
	{
		AccessPoint *ap = &accessPoints[i];
		if (ap->offline)
			continue;
		memcpy(p, &ap->externalIp, sizeof(uint32_t));
		p += sizeof(uint32_t);
		size_t l = strlen(ap->name);
		*p++ = (uint8_t)l;
		memcpy(p, ap->name, l);
		p += l;
	}
	ssize_t sent = sendto(sockfd, resp, (size_t)(p - resp), 0, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
    if (sent < 0)
      perror("ERROR: disco sendto");
}

int pingAccessPoints(int sockfd, int force)
{
	int pingSent = 0;
	struct timespec tsnow;
	clock_gettime(CLOCK_REALTIME, &tsnow);
	uint64_t now = (uint64_t)tsnow.tv_sec * 1000 + (uint64_t)tsnow.tv_nsec / 1000000;
	for (int i = 0; i < apCount; i++)
	{
		AccessPoint *ap = &accessPoints[i];
		if (ap->internalIp == 0)
			continue;
		if (force) {
			ap->pingCount = 0;
		}
		else
		{
			if (ap->lastPing == 0 || ap->lastPing + 5000 > now)
				continue;
			if (ap->pingCount == 5)
			{
				if (ap->offline == 0)
					fprintf(stderr, "Access point \"%s\" is offline\n", ap->name);
				ap->offline = 1;
				continue;
			}
			ap->pingCount++;
		}
		uint8_t payload[sizeof(MAGIC) + 1 + sizeof(uint64_t)];
		memcpy(payload, &MAGIC, sizeof(MAGIC));
		payload[4] = PING;
		ap->lastPing = now;
		memcpy(payload + 5, &ap->lastPing, sizeof(uint64_t));

		pingSent++;
		struct sockaddr_in apAddr;
		apAddr.sin_family = AF_INET;
		apAddr.sin_port = htons(port);
		apAddr.sin_addr.s_addr = ap->internalIp;
		ssize_t sent = sendto(sockfd, payload, sizeof(payload), 0, (struct sockaddr *)&apAddr, sizeof(apAddr));
	    if (sent < 0)
	      perror("ERROR: ping sendto");
	}
	return pingSent;
}

void apPong(struct sockaddr_in *addr, const uint8_t *data, size_t len)
{
	if (len != 13) {
		fprintf(stderr, "Invalid pong packet received: len %zd\n", len);
		return;
	}
	for (int i = 0; i < apCount; i++)
	{
		AccessPoint *ap = &accessPoints[i];
		if (ap->internalIp != addr->sin_addr.s_addr)
			continue;
		ap->lastPing = 0;
		ap->pingCount = 0;
		if (ap->offline == 1)
			fprintf(stderr, "Access point \"%s\" is back online\n", ap->name);
		ap->offline = 0;
		return;
	}
	char ip[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &addr->sin_addr, ip, INET_ADDRSTRLEN);
	fprintf(stderr, "Pong message from unexpected address: %s\n", ip);
}

int main(int argc, char *argv[])
{
	setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

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

	int pinging = pingAccessPoints(sockfd, 1);
	time_t nextPing = time(NULL) + (pinging ? 5 : 60);
	struct pollfd pfd = { sockfd, POLLIN };
	for (;;)
	{
		pfd.revents = 0;
		int timeout = (int)((nextPing - time(NULL)) * 1000);
		if (timeout < 0)
			timeout = 0;
		int rc = poll(&pfd, 1, timeout);
		if (rc < 0)
			error("ERROR: poll");
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "ERROR: poll event 0x%x\n", pfd.revents);
			break;
		}
		if (rc > 0)
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
			switch (data[4])
			{
			case PING:
				pong(&srcAddr, data, (size_t)len);
				break;
			case DISCOVER:
				disco(&srcAddr, data, (size_t)len);
				break;
			case PONG:
				apPong(&srcAddr, data, (size_t)len);
				break;
			default:
				fprintf(stderr, "Invalid packet received: bad op\n");
				break;
			}
		}
		if (time(NULL) >= nextPing) {
			pinging = pingAccessPoints(sockfd, pinging == 0);
			nextPing = time(NULL) + (pinging ? 5 : 60);
		}
	}
	close(sockfd);

	return 0;
}
