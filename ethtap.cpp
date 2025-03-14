//
// The Simplest Ethernet VPN Evar
//
#include <stdio.h>
#include <stdlib.h>
#include <cerrno>
#include <fcntl.h>
#include <error.h>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/if_tun.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <string>
#include <cassert>

constexpr int MAX_CONNECTIONS = 64;
constexpr time_t READ_TIMEOUT = 35 * 60;

int child_pipe = -1;
std::string remoteEndpoint;
const char *dnsmasq_conf = "dnsmasq.conf";

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

void startDnsmasq(const std::string& ifname, const std::string& ipaddr)
{
	// fork twice to keep an intermediate child with su privileges,
	// that can kill dnsmasq on exit.
	int pipefd[2];
	if (pipe(pipefd)) {
		perror("pipe");
		return;
	}
	int childpid = fork();
	if (childpid < 0) {
		perror("fork");
		close(pipefd[0]);
		close(pipefd[1]);
		return;
	}
	if (childpid > 0)
	{
		// close the read end
		close(pipefd[0]);
		// save the write end
		child_pipe = pipefd[1];
		// parent is done
		return;
	}
	// close the write end
	close(pipefd[1]);
	// fork dnsmasq
	int dnsmasq_pid = fork();
	if (dnsmasq_pid < 0) {
		perror("fork(dnsmasq)");
		exit(1);
	}
	if (dnsmasq_pid == 0)
	{
		// grandchild execs dnsmasq
		char confarg[512];
		snprintf(confarg, sizeof(confarg), "--conf-file=%s", dnsmasq_conf);
		execl("/usr/sbin/dnsmasq", "dnsmasq",
				confarg,
				("--interface=" + ifname).c_str(),
				("--dhcp-range=" + ipaddr + "," + ipaddr).c_str(),
				nullptr);
		perror("execl");
		exit(1);
	}
	// child waits on the pipe then kills dnsmasq
	char c;
	ssize_t l = read(pipefd[0], &c, 1);
	(void)l;
	kill(dnsmasq_pid, SIGTERM);
	waitpid(dnsmasq_pid, nullptr, 0);
	exit(0);
}

void stopDnsmasq()
{
	if (child_pipe != 1)
		close(child_pipe);
}

void handleProlog(int sock)
{
	timeval tv {};
	tv.tv_sec = 3;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	uint16_t size;
	uint8_t buf[6];
	if (read(sock, &size, sizeof(size)) != sizeof(size)
			|| size != sizeof(buf)
			|| read(sock, buf, sizeof(buf)) != sizeof(buf)
			|| memcmp(buf, "DCNET", 5)) {
		fprintf(stderr, "Invalid prolog or timeout\n");
		exit(1);
	}
	if (buf[5] != 1) {
		fprintf(stderr, "Unknown protocol version: %d\n", buf[5]);
		exit(1);
	}
	// reset recv timeout to default
	tv.tv_sec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static const char *getDate()
{
	time_t now;
	time(&now);
	char *nowstr = ctime(&now);
	nowstr[strlen(nowstr) - 1] = '\0';
	return nowstr;
}

static void logend() {
	printf("[%s] Link to %s closed\n", getDate(), remoteEndpoint.c_str());
}

void handleConnection(int sock)
{
	handleProlog(sock);
	printf("[%s] Connection from %s\n", getDate(), remoteEndpoint.c_str());

	int tap_fd = open("/dev/net/tun", O_RDWR | O_CLOEXEC);
	if (tap_fd < 0)
		error(-1, errno, "/dev/net/tun");

	// Set tap mode
	ifreq ifr {};
	ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
	if (ioctl(tap_fd, TUNSETIFF, &ifr))
		error(-1, errno, "ioctl(TUNSETIFF)");

	// Set interface IP address
	std::string ifname(ifr.ifr_name);
	if (ifname.substr(0, 3) != "tap" || !isdigit(ifname[3])) {
		fprintf(stderr, "Unknown interface %s. Aborting\n", ifname.c_str());
		exit(1);
	}
	int ifnum = atoi(&ifname[3]);
	if (ifnum >= MAX_CONNECTIONS) {
		fprintf(stderr, "Maximum BBA connections reached: %d\n", ifnum);
		exit(1);
	}
	std::string ipaddr = "172.20.1." + std::to_string(ifnum * 2);
	printf("%s: interface %s - IP address %s\n", remoteEndpoint.c_str(), ifname.c_str(), ipaddr.c_str());
	sockaddr_in *ifaddr = (sockaddr_in *)&ifr.ifr_addr;
	ifaddr->sin_family = AF_INET;
	inet_pton(AF_INET, ipaddr.c_str(), &ifaddr->sin_addr);
	// Note that we use the client socket file descriptor, instead of the tap one, for these ioctls
	// because they must be done on a socket. The target interface name is set in ifr_name and doesn't
	// affect the socket at all.
	if (ioctl(sock, SIOCSIFADDR, &ifr))
		error(-1, errno, "ioctl(SIOCSIFADDR)");

	// Set network mask
	ifaddr = (sockaddr_in *)&ifr.ifr_netmask;
	ifaddr->sin_family = AF_INET;
	inet_pton(AF_INET, "255.255.255.254", &ifaddr->sin_addr);
	if (ioctl(sock, SIOCSIFNETMASK, &ifr))
		error(-1, errno, "ioctl(SIOCSIFNETMASK)");

	// Set interface up
	ioctl(sock, SIOCGIFFLAGS, &ifr);
	ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
	if (ioctl(sock, SIOCSIFFLAGS, &ifr))
		error(-1, errno, "ioctl(SIOCSIFFLAGS)");

	ipaddr[ipaddr.length() - 1] += 1;
	startDnsmasq(ifname, ipaddr);

	// Leave superuser mode
	uid_t uid, gid;
	passwd *user = getpwnam("nobody");
	if (user == nullptr) {
		uid = 65534;
		gid = 65434;
	}
	else {
		uid = user->pw_uid;
		gid = user->pw_gid;
	}
	if (setgid(gid))
		error(-1, errno, "setgid");
	if (setuid(uid))
		error(-1, errno, "setuid");
	atexit(logend);

	setNonBlocking(tap_fd);
	setNonBlocking(sock);

	uint8_t inbuf[1600];
	uint8_t outbuf[1600];
	unsigned inbuflen = 0;
	unsigned outbuflen = 0;
	time_t last_sock_read = time(NULL);
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
		timeval tv;
		tv.tv_sec = READ_TIMEOUT - (time(NULL) - last_sock_read);
		if (tv.tv_sec <= 0) {
			fprintf(stderr, "No data received for 35 min. Closing connection\n");
			break;
		}
		tv.tv_usec = 0;
		if (select(nfds, &readfds, &writefds, nullptr, &tv) == -1)
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
				break;
			}
			if (ret > 0)
			{
				uint8_t mac0 = outbuf[2];
				if ((mac0 & 1) && mac0 != 0xff) {
					//printf("Out frame: multicast filtered\n");
				}
				else
				{
					//printf("Out frame: %zd\n", ret);
					*(uint16_t *)&outbuf[0] = ret;
					outbuflen = ret + 2;
					FD_SET(sock, &writefds);
				}
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
				//fprintf(stderr, "socket read EOF\n");
				break;
			}
			if (ret > 0) {
				inbuflen += ret;
				FD_SET(tap_fd, &writefds);
				last_sock_read = time(NULL);
			}
		}
		if (FD_ISSET(tap_fd, &writefds))
		{
			uint16_t framelen = *(uint16_t *)&inbuf[0];
			if (inbuflen >= framelen + 2u)
			{
				//printf("In frame: %d\n", framelen);
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
				//printf("Out sent(%d) -> %zd\n", outbuflen, ret);
				outbuflen -= ret;
				if (outbuflen > 0)
					memmove(outbuf, outbuf + ret, (size_t)outbuflen);
			}
		}
		/*
	int i = read(tap_fd, buf, sizeof(buf));
	if (i < 0)
	    error(-1, errno, "read(tap)");
	if (i <= 0)
	    break;
	printf("Frame: dest %02x:%02x:%02x:%02x:%02x:%02x "
	       "src %02x:%02x:%02x:%02x:%02x:%02x, payload %d bytes, total %d bytes\n",
	    buf[0], buf[1], buf[2], buf[3], buf[4], buf[5],
	    buf[6], buf[7], buf[8], buf[9], buf[10], buf[11],
		 *(uint16_t *)&buf[12], i);
		 */
	}
	close(sock);
	close(tap_fd);
	stopDnsmasq();
	exit(0);
}

int main(int argc, char *argv[])
{
	setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
	signal(SIGCHLD, SIG_IGN);

	if (argc > 1)
		dnsmasq_conf = argv[1];
	int ssock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int option = 1;
	setsockopt(ssock, SOL_SOCKET, SO_REUSEADDR, (const char *)&option, sizeof(option));
	sockaddr_in serveraddr{};
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = INADDR_ANY;
	serveraddr.sin_port = htons(7655);

	if (::bind(ssock, (sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
		close(ssock);
		error(1, errno, "bind");
	}
	listen(ssock, 5);
	for (;;)
	{
		sockaddr_in src_addr{};
		socklen_t addr_len = sizeof(src_addr);
		int sock = accept(ssock, (sockaddr *)&src_addr, &addr_len);
		if (sock < 0) {
			perror("accept");
			break;
		}
		remoteEndpoint = inet_ntoa(src_addr.sin_addr) + std::string(":") + std::to_string(ntohs(src_addr.sin_port));
		if (fork() == 0) {
			close(ssock);
			handleConnection(sock);
		}
		close(sock);
	}
	close(ssock);
	return 0;
}
