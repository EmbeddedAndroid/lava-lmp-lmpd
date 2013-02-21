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

#include <termios.h>
#include <linux/serial.h>

#define LOCAL_RESOURCE_PATH "/usr/share/lmpd"

static int max_poll_elements;
static struct pollfd *pollfds;
static int *fd_to_pollfd_index;
static int count_pollfds;
static char *fd_to_type;

static char force_exit = 0;
#define MAX_LMP 128
#define MAX_JSON 16384

static int issue;

enum lmpd_socket_types {
	LST_UNKNOWN,
	LST_NETLINK,
	LST_TTYACM
};

enum lmpd_protocols {
	/* always first */
	PROTOCOL_HTTP = 0,
	PROTOCOL_LMPD,
};

enum decode_hdr {
	WAIT_FOR_SOH,
	INSIDE_HDR,
	INSIDE_PAYLOAD,
};

struct lmp {
	char dev_path[64];
	char tree_path[128];
	char serial[64];
	char json_lmp[MAX_JSON];

	char fifo[MAX_JSON];
	int head;
	int tail;

	int eot;
	unsigned int seen_sot:1;

	enum decode_hdr hdr_state;
	int fd;
};

static struct lmp lmp[MAX_LMP];
static int nlmp;

static char json_fifo[65536];
static int json_fifo_head;

struct serveable {
	const char *urlpath;
	const char *mimetype;
};

static const struct serveable whitelist[] = {
	{ "/favicon.ico", "image/x-icon" },
	{ "/lmp-logo.png", "image/png" },
	{ "/HDMI.png", "image/png" },
	{ "/USB-A.png", "image/png" },
	{ "/USB-minib.png", "image/png" },
	{ "/SD.png", "image/png" },
	{ "/uSD.png", "image/png" },
	{ "/undefined.png", "image/png" },
	{ "/jack4.png", "image/png" },
	{ "/bus8.png", "image/png" },

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

static int lmp_find_by_fd(int fd1)
{
	int n;

	for (n = 0; n < nlmp; n++)
		if (lmp[n].fd == fd1)
			return n;

	return -1;
}

static int lmpd_sort_compare(const void *a, const void *b)
{
	return strcmp(((struct lmp *)a)->serial, ((struct lmp *)b)->serial);
}

static void lmpd_sort(void)
{
	qsort(lmp, nlmp, sizeof lmp[0], lmpd_sort_compare);
}

/* this protocol server (always the first one) just knows how to do HTTP */

static int callback_http(struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason, void *user,
							   void *in, size_t len)
{
	char buf[256];
	int n = 0;
	static unsigned char buffer[MAX_JSON + 10];
	unsigned char *p = buffer;
//	struct per_session_data__http *pss =
//				(struct per_session_data__http *)user;
	int m;
	int fd = (int)(long)in;

	switch (reason) {
	case LWS_CALLBACK_HTTP:

		/* check for the "send a big file by hand" example case */

		if (!strcmp((const char *)in, "/json")) {

			p = buffer;
			p += sprintf((char *)p,
				"HTTP/1.0 200 OK\x0d\x0a"
				"Server: libwebsockets\x0d\x0a"
				"Content-Type: application-json\x0d\x0a"
					"\x0d\x0a");
			n = libwebsocket_write(wsi, buffer,
					p - buffer,
					LWS_WRITE_HTTP);
			if (n < 0)
				return -1;

			strcpy((char *)buffer, "{\"schema\":\"org.linaro.lmp.boardlist\",\"boards\":[");
			m = libwebsocket_write(wsi,
					buffer, strlen((char *)buffer),
					LWS_WRITE_HTTP);
			if (m < 0)
				return -1;

			for (n = 0; n < nlmp; n++) {
				p = buffer;
				strcpy((char *)p, lmp[n].json_lmp);
				p += strlen(lmp[n].json_lmp);
				if (n != nlmp - 1)
					*p++ = ',';
				m = libwebsocket_write(wsi,
						buffer, p - buffer,
						LWS_WRITE_HTTP);
				if (m < 0)
					return -1;
			}

			strcpy((char *)buffer, "]}");
			m = libwebsocket_write(wsi,
					buffer, strlen((char *)buffer),
					LWS_WRITE_HTTP);


			return -1;
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
		break;

	case LWS_CALLBACK_ADD_POLL_FD:

		if (count_pollfds >= max_poll_elements) {
			lwsl_err("too many sockets\n");
			return 1;
		}

		fd_to_pollfd_index[fd] = count_pollfds;
		pollfds[count_pollfds].fd = fd;
		pollfds[count_pollfds].events = (int)(long)len;
		pollfds[count_pollfds++].revents = 0;
		break;

	case LWS_CALLBACK_DEL_POLL_FD:
		m = fd_to_pollfd_index[fd];
		if (m < 0)
			break;
		if (pollfds[m].fd != fd) {
			lwsl_err("fd mismatch in delete");
			break;
		}
		count_pollfds--;
		fd_to_pollfd_index[fd] = -1;
		fd_to_type[fd] = LST_UNKNOWN;
		if (m == count_pollfds)
			break;
		/* have the last guy take up the vacant slot */
		pollfds[m] = pollfds[count_pollfds];
		fd_to_pollfd_index[pollfds[m].fd] = m;
		break;

	case LWS_CALLBACK_SET_MODE_POLL_FD:
		pollfds[fd_to_pollfd_index[fd]].events |= (int)(long)len;
		break;

	case LWS_CALLBACK_CLEAR_MODE_POLL_FD:
		pollfds[fd_to_pollfd_index[fd]].events &= ~(int)(long)len;
		break;

	default:
		break;
	}

	return 0;
}

struct per_session_data__lmpd {
	int m;
	int sent_lmp_json;
	int json_fifo_tail;

	int last_issue;
};

static int callback_lmpd(struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason, void *user,
							   void *in, size_t len)
{
	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + MAX_JSON +
						  LWS_SEND_BUFFER_POST_PADDING];
	struct per_session_data__lmpd *pss =
				(struct per_session_data__lmpd *)user;
	int n;
	int m;
	unsigned char *p = &buf[LWS_SEND_BUFFER_PRE_PADDING];

	switch (reason) {

	case LWS_CALLBACK_ESTABLISHED:
		pss->m = 0;
		lwsl_notice("established\n");
		libwebsocket_callback_on_writable(context, wsi);
		break;

	case LWS_CALLBACK_CLOSED:
		lwsl_notice("closed\n");
		break;

	case LWS_CALLBACK_RECEIVE:
		break;

	case LWS_CALLBACK_SERVER_WRITEABLE:

		if (issue != pss->last_issue) {
			pss->last_issue = issue;
			pss->sent_lmp_json = 0;
		}

		/* issue stashed lmp json if we didn't do it already */

		if (pss->sent_lmp_json < MAX_LMP) {
			if (!pss->sent_lmp_json)
				m = LWS_WRITE_TEXT;
			else
				m = LWS_WRITE_CONTINUATION;
			if (!pss->sent_lmp_json)
				p += sprintf((char *)p, "{\"schema\":\"org.linaro.lmp.boardlist\",\"boards\":[");
			if (nlmp) {
				while (pss->sent_lmp_json < nlmp && lmp[pss->sent_lmp_json].json_lmp[0] == '\0')
					pss->sent_lmp_json++;
				if (pss->sent_lmp_json < nlmp) {
					strcpy((char *)p, lmp[pss->sent_lmp_json].json_lmp);
					p += strlen((char *)lmp[pss->sent_lmp_json].json_lmp);
				}
				if (pss->sent_lmp_json >= nlmp - 1) {
					p += sprintf((char *)p, "    ]}");
					pss->sent_lmp_json = MAX_LMP;
				} else {
					*p++ = ',';
					m |= LWS_WRITE_NO_FIN;
				}
			} else {
				p += sprintf((char *)p, "]}");
				pss->sent_lmp_json = MAX_LMP;
			}

			n = libwebsocket_write(wsi,
					&buf[LWS_SEND_BUFFER_PRE_PADDING],
					p - &buf[LWS_SEND_BUFFER_PRE_PADDING],
					m);
			if (!n)
				pss->sent_lmp_json++;
			libwebsocket_callback_on_writable(context, wsi);
			break;
		}

		if (pss->json_fifo_tail == json_fifo_head)
			break;

		/* something to send */

		m = 0;
		while (pss->json_fifo_tail != json_fifo_head &&
				json_fifo[pss->json_fifo_tail] != '\x04' &&
				m < MAX_JSON) {
			*p++ = json_fifo[pss->json_fifo_tail++];
			if (pss->json_fifo_tail == sizeof(json_fifo))
				pss->json_fifo_tail = 0;
			m++;
		}
		if (json_fifo[pss->json_fifo_tail] == '\x04') {
			pss->json_fifo_tail++;
			if (pss->json_fifo_tail == sizeof(json_fifo))
				pss->json_fifo_tail = 0;
		}

		n = libwebsocket_write(wsi, &buf[LWS_SEND_BUFFER_PRE_PADDING],
				m, LWS_WRITE_TEXT);
		if (n) {
			lwsl_err("failed to send ws data %d\n");
			return -1;
		}
		break;

	default:
		break;
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
	}, {
		"wsprotocol.org.linaro.lmpd",
		callback_lmpd,
		sizeof (struct per_session_data__lmpd),
		4096,
	},
	{ NULL, NULL, 0, 0 }
};

static int
lmpd_cmd(struct lmp *plmp, const char *cmd)
{
	int n;

	n = write(plmp->fd, cmd, strlen(cmd));
	lwsl_notice("lmpd_cmd: %c %d\n", cmd[0], n);
	if (n < 0)
		return n;

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
	struct termios tty;

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
		if (i >= 0) {
			lwsl_notice("Found %s\n", devpath);
			return 0;
		}
		/* create the struct lmp */
		if (nlmp == sizeof(lmp) / sizeof(lmp[0])) {
			lwsl_err("Too many lmp\n");
			return 0;
		}

		memset(&lmp[nlmp], 0, sizeof lmp[nlmp]);
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

		/* enforce suitable tty state */

		memset (&tty, 0, sizeof tty);
		if (tcgetattr (lmp[nlmp].fd, &tty)) {
			lwsl_err("tcgetattr failed on %s\n", devname);
			close(lmp[nlmp].fd);
			return 0;
		}

		tty.c_lflag &= ~(ISIG | ICANON | IEXTEN | ECHO | XCASE |
				ECHOE | ECHOK | ECHONL | ECHOCTL | ECHOKE);
		tty.c_iflag &= ~(INLCR | IGNBRK | IGNPAR | IGNCR | ICRNL |
				   IMAXBEL | IXON | IXOFF | IXANY | IUCLC);
		tty.c_oflag &= ~(ONLCR | OPOST | OLCUC | OCRNL | ONLRET);
		tty.c_cc[VMIN]  = 1;
		tty.c_cc[VTIME] = 0;
		tty.c_cc[VEOF] = 1;
		tty.c_cflag &=  ~(CBAUD | CSIZE | CSTOPB | PARENB | CRTSCTS);
		tty.c_cflag |= (8 | CREAD | 0 | 0 | 1 | CLOCAL);

		cfsetispeed(&tty, B115200);
		cfsetospeed(&tty, B115200);
		tcsetattr(lmp[nlmp].fd, TCSANOW, &tty);

		/* add us to the poll array */
		fd_to_type[lmp[nlmp].fd] = LST_TTYACM;
		callback_http(NULL, NULL, LWS_CALLBACK_ADD_POLL_FD,
				      NULL, (void *)(long)lmp[nlmp].fd, POLLIN);
		lwsl_notice("Added %s fd %d serial %s\n",
					devname, lmp[nlmp].fd, serial);

		/* make it official */
		lmpd_cmd(&lmp[nlmp++], "j");

		lmpd_sort();

		/* inform when we got the json back */
		return 0;
	}
	if (relevant == 4 && !strcmp(act, "remove")) {
		if (i < 0)
			return 0;
		lwsl_notice("Removed #%d %s serial %s, closing fd %d\n",
			i, lmp[i].dev_path, lmp[i].serial, lmp[i].fd);
		fd_to_type[lmp[i].fd] = LST_UNKNOWN;
		callback_http(NULL, NULL, LWS_CALLBACK_DEL_POLL_FD,
			      NULL, (void *)(long)lmp[i].fd, 0);
		lwsl_err("closing fd %d\n", lmp[i].fd);
		close(lmp[i].fd);
		if (nlmp > 1)
			lmp[i] = lmp[nlmp - 1];
		nlmp--;
		lmpd_sort();
		goto inform;
	}

	return 0;

inform:
	/* inform clients something happened */
	issue++;
	libwebsocket_callback_on_writable_all_protocol(
						&protocols[PROTOCOL_LMPD]);

	return 0;
}

void sighandler(int sig)
{
	force_exit = 1;
}


int main(void)
{
	struct sockaddr_nl nls;
	char buf[MAX_JSON];
	int n, m, i, j, k;
	int fd;
	DIR *dir;
	struct dirent *dirent;
	const char *coldplug_dir = "/sys/class/tty/";
	int debug_level = 7;
	struct libwebsocket_context *context = NULL;
	struct lws_context_creation_info info;
	int use_ssl = 0;
	int opts = 0;
	struct lmp *plmp;
	char c;

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
	fd_to_pollfd_index = malloc(max_poll_elements * sizeof (int));
	fd_to_type = malloc(max_poll_elements * sizeof (char));
	if (pollfds == NULL || fd_to_pollfd_index == NULL || fd_to_type == NULL) {
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
		if (m < 0)
			continue;
		if (m == 0) {
			libwebsocket_service_fd(context, NULL);
			continue;
		}
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
				k = lmp_find_by_fd(pollfds[n].fd);
				if (k < 0) {
					lwsl_err("lost fd %d\n", pollfds[n].fd);
					break;
				}
				plmp = &lmp[k];
				i = read(pollfds[n].fd, buf, sizeof buf);
				if (i <= 0) {
					lwsl_err("read failed %d\n", i);
					fd_to_type[pollfds[n].fd] = LST_UNKNOWN;
					callback_http(NULL, NULL,
						LWS_CALLBACK_DEL_POLL_FD, NULL,
						(void *)(long)pollfds[n].fd, 0);
					/* inform clients something happened */
					libwebsocket_callback_on_writable_all_protocol(
						     &protocols[PROTOCOL_LMPD]);
					n = count_pollfds;
					break;
				}
				buf[i] = '\0';
//				puts(buf);

				/*
				 * When we first connect to it, it might be in
				 * the middle of spamming things.  Drain the
				 * incoming data until first SOT
				 */

				j = 0;
				while (!plmp->seen_sot && j < i) {
					if (buf[j] == '\x02') {
						plmp->seen_sot = 1;
						break;
					}
					j++;
				}

				/*
				 * dump what we have into per-lmp fifo,
				 * keeping count of any EOTs seen
				 */

				for (; j < i; j++) {
					plmp->fifo[plmp->head++] = buf[j];
					if (plmp->head == sizeof(plmp->fifo))
						plmp->head = 0;
					if (buf[j] == '\x04')
						plmp->eot++;
				}

				if (!plmp->eot)
					break;

				/* spill completed jsons into common fifo */

				j = 0;
				while (plmp->eot) {
					c = plmp->fifo[plmp->tail];
					if (c == '\x02') {
						j = 0;
						plmp->tail++;
					} else {
						buf[j++] = plmp->fifo[plmp->tail];
						json_fifo[json_fifo_head++] =
						       plmp->fifo[plmp->tail++];
					}
					if (plmp->tail == sizeof(plmp->fifo))
						plmp->tail = 0;
					if (json_fifo_head == sizeof(json_fifo))
						json_fifo_head = 0;
					/* was it an EOT we just copied in? */
					if (c == '\x04') {
						plmp->eot--;
						/*
						 * in the local buf,
						 * replace EOT with '\0'
						 */
						buf[j - 1] = '\0';
						/*
						 * take a copy of the board
						 * definition when we see that
						 * fly by (without EOT)
						 */
						if (strstr(buf, "org.linaro.lmp.board") != NULL) {
							if (plmp->json_lmp[0] == '\0')
								/* force all connections to reissue board list */
								issue++;
							puts(buf);
							memcpy(plmp->json_lmp, buf, j);
						}
					}
				}

				/* inform clients something happened */
				libwebsocket_callback_on_writable_all_protocol(
						     &protocols[PROTOCOL_LMPD]);
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

	for (n = 0; n < count_pollfds; n++) {

		switch (fd_to_type[pollfds[n].fd]) {
		case LST_NETLINK:
			close(pollfds[n].fd);
			close(pollfds[n].fd);
			break;
		default:
			break;
		}
	}

	if (context)
		libwebsocket_context_destroy(context);

	free(fd_to_type);
	free(pollfds);
	free(fd_to_pollfd_index);

	return 1;
}
