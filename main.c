#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <dirent.h>
#include <signal.h>

#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/socket.h>

#include <linux/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>


int main(void)
{
	struct sockaddr_nl nls;
	struct sockaddr_nl sa;
	struct nlmsghdr *nh;
	struct pollfd pfd[2];
	char buf[4096];
	int len;
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr msg;
	int n;
	int i;
	int fd;
	int pid;
	char path[128];
	char *data;
	int relevant = 0;
	char *devname, *action, *serial;
	DIR *dir;
	struct dirent *dirent;
	const char *coldplug_dir = "/sys/class/tty/";

	memset(&nls,0,sizeof(struct sockaddr_nl));
	nls.nl_family = AF_NETLINK;
	nls.nl_pid = getpid();
	nls.nl_groups = -1;

	pfd[0].events = POLLIN;
	pfd[0].fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (pfd[0].fd == -1) {
		printf("Not root\n");
		exit(1);
	}

	// Listen to netlink socket

	if (bind(pfd[0].fd, (void *)&nls, sizeof(nls))) {
		printf("Bind failed\n");
		exit(1);
	}

	/* provoke coldplug changes */
	dir = opendir(coldplug_dir);
	if (!dir) {
		fprintf(stderr, "Unable to open coldplug dir\n");
	} else {
		while (1) {
			dirent = readdir(dir);
			if (!dirent)
				break;
			if (!strstr(dirent->d_name, "ttyACM"))
				continue;
			sprintf(buf, "%s/%s/uevent", coldplug_dir, dirent->d_name);
			fd = open(buf, O_RDWR, 0);
			if (fd > 0) {
				// fprintf(stderr, "changing %s\n", dirent->d_name);

				if (write(fd, "change", 6) != 6) {
					fprintf(stderr, "write failed\n");
					break;
				}
				close(fd);
			} else
				fprintf(stderr, "unable to open %s: %s\n", buf, strerror(errno));
		}
	}

	/* service netlink events */

	while (poll(&pfd[0], 1, -1) >= 0) {

		if (!(pfd[0].revents & POLLIN))
			continue;

		len = recv(pfd[0].fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (len <= 0) {
			perror("recv\n");
			continue;
		}

//		fprintf(stderr, "rx %d\n", len);

		relevant = 0;
		i = 0;
		while (i < len) {
			if (!strcmp(&buf[i], "ID_MODEL=LavaLMP"))
				relevant++;
			if (!strncmp(&buf[i], "ACTION=", 7)) {
				relevant++;
				action = &buf[i + 7];
			}
			if (!strncmp(&buf[i], "DEVNAME=", 8)) {
				if (strstr(&buf[i], "ttyACM")) {
					relevant++;
					devname = &buf[i + 8];
				}
			}
			if (!strncmp(&buf[i], "ID_SERIAL_SHORT=", 16)) {
				relevant++;
				serial = &buf[i + 16];
			}

//			fprintf(stderr, "tok %s\n", buf + i);
			i += strlen(buf + i) + 1;
		}

		if (relevant == 4) {
			fprintf(stderr, "%s %s %s\n", action, devname, serial);
		}

	}

	return 1;
}
