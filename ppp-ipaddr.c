#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>

#undef bool
#define bool pppbool
#include <pppd/pppd.h>
#include <pppd/chap-new.h>
#include <pppd/upap.h>
#include <pppd/fsm.h>
#include <pppd/ipcp.h>
#undef bool

char pppd_version[] = VERSION;
static const char *start_ip;
static option_t options[] = {
	{ "startip", o_string, &start_ip, "Starting IP address assigned to ppp0" },
	{ NULL }
};
static char peerAddr[32] = "?";
static char *peerName = "?";

static const char *getDate()
{
	time_t now;
	time(&now);
	char *nowstr = ctime(&now);
	nowstr[strlen(nowstr) - 1] = '\0';
	return nowstr;
}

static void assignIp(u_int32_t *addrp)
{
	// peer address
	char *env = getenv("REMOTE_ADDR");
	if (env != NULL)
	{
		strcpy(peerAddr, env);
		env = getenv("REMOTE_PORT");
		if (env != NULL) {
			strcat(peerAddr, ":");
			strcat(peerAddr, env);
		}
	}
	// peer user name
	if (peer_authname != NULL)
		peerName = peer_authname;

	if (strncmp(ifname, "ppp", 3) != 0) {
		error("[%s] %s user %s - ifname invalid: %s",
				getDate(), peerAddr, peerName, ifname);
		*addrp = 0;
		return;
	}
	int i = atoi(&ifname[3]);
	if (i < 0 || i > 255) {
		error("[%s] %s user %s - too many connections: %s",
				getDate(), peerAddr, peerName, ifname);
		*addrp = 0;
		return;
	}
	if (start_ip == NULL) {
		start_ip = "172.20.0.10";
		info("Option '%s' not set. Using default %s", options[0].name, start_ip);
	}
	struct in_addr inaddr;
	inet_aton(start_ip, &inaddr);
	inaddr.s_addr = htonl(ntohl(inaddr.s_addr) + i);
	*addrp = inaddr.s_addr;

	char *ipstr = inet_ntoa(inaddr);
	info("[%s] %s: Connection from %s user %s assigned IP %s",
			getDate(), ifname, peerAddr, peerName, ipstr);
}

static void pppExit(void *arg, int i)
{
	info("[%s] %s: Disconnection from %s user %s",
			getDate(), ifname, peerAddr, peerName);
}

void
plugin_init(void)
{
	info("DCNet IP address plugin");
	add_options(options);
	ip_choose_hook = &assignIp;
	add_notifier(&exitnotify, pppExit, NULL);
}

