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

#include <libwebsockets.h>

#define LOCAL_RESOURCE_PATH "/usr/share/lmpd"

static int max_poll_elements;
static struct pollfd *pollfds;
static int *fd_lookup;
static int count_pollfds;
static char force_exit = 0;
static char *fd_to_type;

#define MAX_LMP 128

enum lmpd_socket_types {
	LST_UNKNOWN,
	LST_NETLINK,
	LST_TTYACM
};

enum lmpd_protocols {
	/* always first */
	PROTOCOL_HTTP = 0,

	/* always last */
	DEMO_PROTOCOL_COUNT
};

struct lmp {
	char dev_path[64];
	char tree_path[128];
	char serial[64];
	int fd;
};

static struct lmp lmp[MAX_LMP];
static int nlmp;

struct serveable {
	const char *urlpath;
	const char *mimetype;
};

static const struct serveable whitelist[] = {
	{ "/favicon.ico", "image/x-icon" },
	{ "/lmp-logo.png", "image/png" },

	/* last one is the default served if no match */
	{ "/index.html", "text/html" },
};

struct per_session_data__http {
	int fd;
};

static int lmp_find_by_devpath(const char *devpath)
{
	int n;
	int len = strlen(devpath);

	/*
	 * if the devpath of what is being removed matches the lhs of the
	 * devpath, it's enough to mean the ancestor is removed so must the
	 * child be
	 */

	for (n = 0; n < nlmp; n++)
		if (strncmp(lmp[n].tree_path, devpath, len) == 0)
			return n;

	return -1;
}

/* this protocol server (always the first one) just knows how to do HTTP */

static int callback_http(struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason, void *user,
							   void *in, size_t len)
{
	char buf[256];
	int n;
	unsigned char *p;
	static unsigned char buffer[8192];
	struct stat stat_buf;
	struct per_session_data__http *pss =
				(struct per_session_data__http *)user;
	int m;
	int fd = (int)(long)in;

	switch (reason) {
	case LWS_CALLBACK_HTTP:

		/* check for the "send a big file by hand" example case */

		if (!strcmp((const char *)in, "/leaf.jpg")) {

			/* well, let's demonstrate how to send the hard way */

			p = buffer;

			pss->fd = open(LOCAL_RESOURCE_PATH"/leaf.jpg", O_RDONLY);
			if (pss->fd < 0)
				return -1;

			fstat(pss->fd, &stat_buf);

			/*
			 * we will send a big jpeg file, but it could be
			 * anything.  Set the Content-Type: appropriately
			 * so the browser knows what to do with it.
			 */

			p += sprintf((char *)p,
				"HTTP/1.0 200 OK\x0d\x0a"
				"Server: libwebsockets\x0d\x0a"
				"Content-Type: image-jpeg\x0d\x0a"
					"Content-Length: %u\x0d\x0a\x0d\x0a",
					(unsigned int)stat_buf.st_size);

			/*
			 * send the http headers...
			 * this won't block since it's the first payload sent
			 * on the connection since it was established
			 */

			n = libwebsocket_write(wsi, buffer,
				   p - buffer, LWS_WRITE_HTTP);

			if (n < 0) {
				close(pss->fd);
				return -1;
			}
			/*
			 * book us a LWS_CALLBACK_HTTP_WRITEABLE callback
			 */
			libwebsocket_callback_on_writable(context, wsi);
			break;
		}

		/* if not, send a file the easy way */

		for (n = 0; n < (sizeof(whitelist) / sizeof(whitelist[0]) - 1); n++)
			if (in && strcmp((const char *)in, whitelist[n].urlpath) == 0)
				break;

		sprintf(buf, LOCAL_RESOURCE_PATH"%s", whitelist[n].urlpath);

		if (libwebsockets_serve_http_file(context, wsi, buf, whitelist[n].mimetype))
			return -1; /* through completion or error, close the socket */

		/*
		 * notice that the sending of the file completes asynchronously,
		 * we'll get a LWS_CALLBACK_HTTP_FILE_COMPLETION callback when
		 * it's done
		 */

		break;

	case LWS_CALLBACK_HTTP_FILE_COMPLETION:
		return -1;

	case LWS_CALLBACK_HTTP_WRITEABLE:
		/*
		 * we can send more of whatever it is we were sending
		 */

		do {
			n = read(pss->fd, buffer, sizeof buffer);
			/* problem reading, close conn */
			if (n < 0)
				goto bail;
			/* sent it all, close conn */
			if (n == 0)
				goto bail;
			/*
			 * because it's HTTP and not websocket, don't need to
			 * take care about pre and postamble
			 */
			n = libwebsocket_write(wsi, buffer, n, LWS_WRITE_HTTP);
			if (n < 0)
				/* write failed, close conn */
				goto bail;

		} while (!lws_send_pipe_choked(wsi));
		libwebsocket_callback_on_writable(context, wsi);
		break;

bail:
		close(pss->fd);
		return -1;

	case LWS_CALLBACK_ADD_POLL_FD:

		if (count_pollfds >= max_poll_elements) {
			lwsl_err("too many sockets\n");
			return 1;
		}

		fd_lookup[fd] = count_pollfds;
		pollfds[count_pollfds].fd = fd;
		pollfds[count_pollfds].events = (int)(long)len;
		pollfds[count_pollfds++].revents = 0;
		break;

	case LWS_CALLBACK_DEL_POLL_FD:
		if (!--count_pollfds)
			break;
		m = fd_lookup[fd];
		fd_to_type[fd] = LST_UNKNOWN;
		/* have the last guy take up the vacant slot */
		pollfds[m] = pollfds[count_pollfds];
		fd_lookup[pollfds[count_pollfds].fd] = m;
		break;

	case LWS_CALLBACK_SET_MODE_POLL_FD:
		pollfds[fd_lookup[fd]].events |= (int)(long)len;
		break;

	case LWS_CALLBACK_CLEAR_MODE_POLL_FD:
		pollfds[fd_lookup[fd]].events &= ~(int)(long)len;
		break;

	default:
		break;
	}

	return 0;
}

int
lmpd_service_netlink(struct libwebsocket_context *context, struct pollfd *pfd)
{
	char buf[4096];
	int len;
	int relevant = 0;
	char *devname, *act, *serial, *devpath;
	int i;

	len = recv(pfd->fd, buf, sizeof(buf), MSG_DONTWAIT);
	if (len <= 0) {
		perror("recv\n");
		return len;
	}

	devpath = NULL;
	relevant = 0;
	i = 0;
	while (i < len) {
		if (!strcmp(&buf[i], "ID_MODEL=LavaLMP"))
			relevant++;
		if (!strncmp(&buf[i], "ACTION=", 7)) {
			relevant++;
			act = &buf[i + 7];
		}
		if (!strncmp(&buf[i], "DEVPATH=", 8)) {
			relevant++;
			devpath = &buf[i + 8];
		}
		if (!strncmp(&buf[i], "DEVNAME=", 8) &&
						    strstr(&buf[i], "ttyACM")) {
			relevant++;
			devname = &buf[i + 8];
		}
		if (!strncmp(&buf[i], "ID_SERIAL_SHORT=", 16)) {
			relevant++;
			serial = &buf[i + 16];
		}

//		fprintf(stderr, "tok %s\n", buf + i);
		i += strlen(buf + i) + 1;
	}

	if (!devpath)
		return 0;

	i = lmp_find_by_devpath(devpath);
	if (relevant == 5 && (!strcmp(act, "add") || !strcmp(act, "change"))) {
		if (i >= 0)
			return 0;
		/* create the struct lmp */
		if (nlmp == sizeof(lmp) / sizeof(lmp[0])) {
			lwsl_err("Too many lmp\n");
			return 0;
		}
		strncpy(lmp[nlmp].tree_path, devpath,
					 sizeof lmp[nlmp].tree_path);
		lmp[nlmp].tree_path[sizeof(lmp[nlmp].tree_path) - 1] = '\0';
		strncpy(lmp[nlmp].dev_path, devname, sizeof lmp[nlmp].dev_path);
		lmp[nlmp].dev_path[sizeof(lmp[nlmp].dev_path) - 1] = '\0';
		strncpy(lmp[nlmp].serial, serial, sizeof lmp[nlmp].serial);
		lmp[nlmp].serial[sizeof(lmp[nlmp].serial) - 1] = '\0';

		/* open the device path */
		lmp[nlmp].fd = open(devname, O_RDWR, 0);
		if (lmp[nlmp].fd < 0) {
			lwsl_err("Unable to open %s\n", devname);
			return 0;
		}
		/* add us to the poll array */
		fd_to_type[lmp[nlmp].fd] = LST_TTYACM;
		callback_http(NULL, NULL, LWS_CALLBACK_ADD_POLL_FD,
				      NULL, (void *)(long)lmp[nlmp].fd, POLLIN);
		/* make it official */
		nlmp++;
		lwsl_notice("Added %s serial %s\n", devname, serial);
	}
	if (relevant == 4 && !strcmp(act, "remove")) {
		if (i < 0)
			return 0;
		lwsl_notice("Removed %s serial %s\n",
				lmp[i].dev_path, lmp[i].serial);
		fd_to_type[lmp[nlmp].fd] = LST_TTYACM;
		callback_http(NULL, NULL, LWS_CALLBACK_DEL_POLL_FD,
			      NULL, (void *)(long)lmp[nlmp].fd, 0);

		close(lmp[i].fd);
		if (nlmp > 1)
			lmp[i] = lmp[nlmp - 1];
		nlmp--;
	}

	return 0;
}

static struct libwebsocket_protocols protocols[] = {
	/* first protocol must always be HTTP handler */

	{
		"http-only",		/* name */
		callback_http,		/* callback */
		sizeof (struct per_session_data__http),	/* per_session_data */
		0,			/* max frame size / rx buffer */
	},
};

void sighandler(int sig)
{
	force_exit = 1;
}


int main(void)
{
	struct sockaddr_nl nls;
	char buf[256];
	int n, m;
	int fd;
	DIR *dir;
	struct dirent *dirent;
	const char *coldplug_dir = "/sys/class/tty/";
	int debug_level = 7;
	struct libwebsocket_context *context;
	struct lws_context_creation_info info;
	int use_ssl = 0;
	int opts = 0;

	memset(&info, 0, sizeof info);
	info.port = 7681;

	/* tell the library what debug level to emit and to send it to syslog */
	lws_set_log_level(debug_level, NULL /*lwsl_emit_syslog*/);

	lwsl_notice("lmpd - (C) Copyright 2013 "
			"Andy Green <andy.green@linaro.org> - "
						    "licensed under GPL3\n");
	signal(SIGINT, sighandler);

	max_poll_elements = getdtablesize();
	pollfds = malloc(max_poll_elements * sizeof (struct pollfd));
	fd_lookup = malloc(max_poll_elements * sizeof (int));
	fd_to_type = malloc(max_poll_elements * sizeof (char));
	if (pollfds == NULL || fd_lookup == NULL || fd_to_type == NULL) {
		lwsl_err("Out of memory pollfds=%d\n", max_poll_elements);
		return -1;
	}

	memset(fd_to_type, LST_UNKNOWN, max_poll_elements);

	info.protocols = protocols;
#ifndef LWS_NO_EXTENSIONS
	info.extensions = libwebsocket_get_internal_extensions();
#endif
	if (!use_ssl) {
		info.ssl_cert_filepath = NULL;
		info.ssl_private_key_filepath = NULL;
	} else {
		info.ssl_cert_filepath =
			LOCAL_RESOURCE_PATH"/libwebsockets-test-server.pem";
		info.ssl_private_key_filepath =
			LOCAL_RESOURCE_PATH"/libwebsockets-test-server.key.pem";
	}
	info.gid = -1;
	info.uid = -1;
	info.options = opts;

	context = libwebsocket_create_context(&info);
	if (context == NULL) {
		lwsl_err("libwebsocket init failed\n");
		return -1;
	}

	memset(&nls,0,sizeof(struct sockaddr_nl));
	nls.nl_family = AF_NETLINK;
	nls.nl_pid = getpid();
	nls.nl_groups = -1;

	fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (fd == -1) {
		fprintf(stderr, "unable to open netlink socket\n");
		return 1;
	}

	/* Listen to netlink socket */

	if (bind(fd, (void *)&nls, sizeof(nls))) {
		printf("Bind failed\n");
		exit(1);
	}

	/* add the netlink fd */

	fd_to_type[fd] = LST_NETLINK;
	callback_http(NULL, NULL, LWS_CALLBACK_ADD_POLL_FD, NULL,
						      (void *)(long)fd, POLLIN);

	/* provoke coldplug changes */
	dir = opendir(coldplug_dir);
	if (!dir) {
		fprintf(stderr, "Unable to open coldplug dir\n");
		goto post_coldplug;
	}

	/* prod all the ttyACMs so we will hear about them */

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
			fprintf(stderr, "unable to open %s: %s\n",
						  buf, strerror(errno));
	}
post_coldplug:

	/*
	 * service loop
	 */
	m = 0;
	while (m >= 0 && !force_exit) {
		m = poll(pollfds, count_pollfds, 50);
		if (m <= 0)
			continue;
		for (n = 0; n < count_pollfds; n++) {
			if (!pollfds[n].revents)
				continue;
			switch (fd_to_type[pollfds[n].fd]) {
			case LST_NETLINK:
				if (!(pollfds[n].revents & POLLIN))
					return 0;

				if (lmpd_service_netlink(context, &pollfds[n]))
					goto done;
				break;
			case LST_TTYACM:
				break;
			default:
				/*
				 * returns immediately if the fd does not
				 * match anything under libwebsockets
				 * control
				 */
				if (libwebsocket_service_fd(context,
							       &pollfds[n]) < 0)
					goto done;
				break;
			}
		}

	}
done:

	free(fd_to_type);
	free(pollfds);
	free(fd_lookup);

	return 1;
}
