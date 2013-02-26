#include "lmpd.h"

int max_poll_elements;
struct pollfd *pollfds;
int *fd_to_pollfd_index;
int count_pollfds;
char *fd_to_type;

char force_exit = 0;

int issue;

struct lmp lmp[MAX_LMP];
int nlmp;

char json_fifo[JSON_FIFO_SIZE];
int json_fifo_head;

static int lmp_find_by_fd(int fd1)
{
	int n;

	for (n = 0; n < nlmp; n++)
		if (lmp[n].fd == fd1)
			return n;

	return -1;
}

struct libwebsocket_protocols protocols[] = {
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

	fprintf(stderr, "%p\n", info.ssl_cipher_list);

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
