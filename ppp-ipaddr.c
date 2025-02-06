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
#include <sqlite3.h>
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
static char assignedIp[16];

static sqlite3 *openDb()
{
	sqlite3 *db;
	int rc = sqlite3_open("/usr/local/var/ppp-addr.db", &db);
	if (rc) {
		error("Can't open database: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return NULL;
	}
	sqlite3_busy_timeout(db, 1000);	// 1 sec timeout when database is locked
	return db;
}

static void releaseIp()
{
	if (assignedIp[0] == '\0')
		return;
	sqlite3 *db = openDb();
	if (db == NULL) {
		error("Can't release IP %s", assignedIp);
		return;
	}
	char stmt[256];
	sprintf(stmt, "UPDATE POOL SET ASSIGNED=0 WHERE IP='%s'", assignedIp);
	char *zErrMsg = NULL;
	int rc = sqlite3_exec(db, stmt, NULL, 0, &zErrMsg);
        if (rc != SQLITE_OK) {
                error("SQL error (%s not released): %s", assignedIp, zErrMsg);
		sqlite3_free(zErrMsg);
	}
	sqlite3_close(db);
}

static int dbCallback(void *arg, int ncol, char **coltext, char**colname)
{
	if (coltext[0] != NULL)
		strcpy(assignedIp, coltext[0]);
	return 0;
}

static char *findIp()
{
	sqlite3 *db = openDb();
	if (db == NULL)
		return NULL;
	for (int i = 0; i < 100; i++)
	{
		char *zErrMsg = NULL;
		int rc = sqlite3_exec(db, "SELECT IP FROM POOL WHERE ASSIGNED=0 LIMIT 1", dbCallback, 0, &zErrMsg);
		if (rc != SQLITE_OK) {
			error("SQL error: %s", zErrMsg);
			sqlite3_free(zErrMsg);
			sqlite3_close(db);
			return NULL;
		}
		if (assignedIp[0] == '\0') {
			error("IP pool is exhausted");
			sqlite3_close(db);
			return NULL;
		}
		char stmt[256];
		sprintf(stmt, "UPDATE POOL SET ASSIGNED=1 WHERE IP='%s' AND ASSIGNED=0", assignedIp);
		rc = sqlite3_exec(db, stmt, NULL, 0, &zErrMsg);
		if (rc != SQLITE_OK) {
			error("SQL error (update): %s", zErrMsg);
			sqlite3_free(zErrMsg);
			sqlite3_close(db);
			assignedIp[0] = '\0';
			return NULL;
		}
		if (sqlite3_changes(db) != 0) {
			sqlite3_close(db);
			return assignedIp;
		}
		assignedIp[0] = '\0';
	}
	error("Something is wrong with the IP pool database. Giving up.");
	sqlite3_close(db);
	return NULL;
}

static const char *getDate()
{
	time_t now;
	time(&now);
	char *nowstr = ctime(&now);
	nowstr[strlen(nowstr) - 1] = '\0';
	return nowstr;
}

extern char **environ;
static char peerAddr[32] = "?";
static char *peerName = "?";
static struct in_addr localAddr = { 0 };

static void assignIp(u_int32_t *addrp)
{
	// Check if we have an IP already (can happen when negotiation fails)
	if (localAddr.s_addr != 0) {
		*addrp = localAddr.s_addr;
		return;
	}
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

	char *ipstr = findIp();
	if (ipstr != NULL)
	{
		inet_aton(ipstr, &localAddr);
		*addrp = localAddr.s_addr;
		info("[%s] Connection from %s user %s assigned IP %s",
			getDate(), peerAddr, peerName, ipstr);
	}
	else {
		error("[%s] Connection from %s user %s: can't find IP to assign",
			getDate(), peerAddr, peerName);
	}
}

static void pppExit(void *arg, int i)
{
	if (localAddr.s_addr != 0)
		info("[%s] Disconnection from %s user %s releasing IP %s",
			getDate(), peerAddr, peerName, inet_ntoa(localAddr));
	releaseIp();
}

void
plugin_init(void)
{
	info("DCNet IP address plugin");
	ip_choose_hook = &assignIp;
	add_notifier(&exitnotify, pppExit, NULL);
	srand(time(NULL));
}

