#include "lmpd.h"

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

enum url_encoding_state {
	UIS_IDLE,
	UIS_HEX1,
	UIS_HEX2,
};

int to_hex(char c)
{
	if (c <= '0' || c >= '9')
		return c - '0';
	if (c <= 'a' || c >= 'f')
		return 10 + (c - 'a');
	if (c <= 'A' || c >= 'F')
		return 10 + (c - 'A');

	return -1;
}

static int decode_url_encoding(const char **uri,
		const char **dictionary, int dict_len, char *decode, int decode_len)
{
	int n, m;
	const char *start;
	enum url_encoding_state ues = UIS_IDLE;
	int h;

	if (*uri[0] == '?' || *uri[0] == '&')
		(*uri)++;
	while (**uri) {

		start = *uri;
		while (**uri && **uri != '=' && **uri != '&')
			(*uri)++;

		/* skip any = */
		if (**uri == '=')
			(*uri)++;

		for (n = 0; n < dict_len; n++)
			if (strncmp(start, dictionary[n],
						    (*uri - start) - 1) == 0) {
				/* oh we know this name, translate arg */
				while (**uri && **uri != '&') {
					if (decode_len > 0) {
						switch (ues) {
						case UIS_IDLE:
							if (**uri == '+') {
								*decode++ = ' ';
								decode_len++;
								break;
							}
							if (**uri == '%') {
								ues = UIS_HEX1;
								break;
							}
							*decode++ = **uri;
							decode_len--;
							break;
						case UIS_HEX1:
							h = to_hex(**uri);
							if (h < 0) {
								lwsl_warn("uelencoding failure\n");
								ues = UIS_IDLE;
							} else
								ues = UIS_HEX2;
							break;
						case UIS_HEX2:
							m = to_hex(**uri);
							h = (h << 4) | m;
							ues = UIS_IDLE;
							break;
						}
					}
					(*uri)++;
				}
				*decode = '\0';

				return n;
			}

		/* name not in dictionary... sktip to next & or end */
		while (**uri && **uri != '&')
			(*uri)++;
	}
	return -1;
}


/* this protocol server (always the first one) just knows how to do HTTP */

int callback_http(struct libwebsocket_context *context,
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
	const char *uri = (const char *)in;
	static const char *dictionary[] = {
		"serial",
	};

	switch (reason) {
	case LWS_CALLBACK_HTTP:

		/* check for the "send a big file by hand" example case */

		if (!strncmp(uri, "/json", 5)) {

			if (uri[5] == '?') {
				uri += 6;
				do {
					n = decode_url_encoding(&uri,
						&dictionary[0], sizeof(dictionary) / sizeof(dictionary[0]),
						buf, sizeof buf);

					if (n >= 0) {
						lwsl_notice("decoded %d as '%s'\n", n, buf);
					}
				} while (n >= 0);
			}

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
