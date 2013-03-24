#include <lmpd.h>

static int lmp_find_by_serial(const char *serial)
{
	int n;

	for (n = 0; n < nlmp; n++)
		if (!strcmp(lmp[n].serial, serial))
			return n;

	return -1;
}

static const char * const paths[] = {
	"schema",
	"serial",
	"identify"
};

static int lmp_index;

static char
json_cb(struct lejp_ctx *ctx, char reason)
{

	if (reason & LEJP_FLAG_CB_IS_VALUE) {
		switch (ctx->path_match) {
		case 1: /* schema */
			/* schema has to be in "org.linaro.lmp" namespace */
			if (strncmp(ctx->buf, "org.linaro.lmp.", 15))
				goto bail;
			break;
		case 2: /* serial */
			lmp_index = lmp_find_by_serial(ctx->buf);
			if (lmp_index < 0)
				goto bail;
			break;
		}
		return 0;
	}
	return 0;

bail:
	/* we do not know how to cope with this */
	return -1;
}


int callback_lmpd(struct libwebsocket_context *context,
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
	struct lejp_ctx ctx;

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
		lmp_index = -1;
		lwsl_notice("rx %s\n", (char *)in);
		lejp_construct(&ctx, json_cb, in, paths, ARRAY_SIZE(paths));
		n = lejp_parse(&ctx, in, len);
		if (n < 0) {
			lwsl_err("parse err %d\n", n);
			break;
		}
		if (lmp_index < 0) {
			lwsl_err("no serial\n");
			break;
		}
		buf[0] = '\x02';
		strcpy((char *)&buf[1], (char *)in);
		buf[len + 1] ='\x04';
		buf[len + 2] = '\0';
		n = write(lmp[lmp_index].fd, buf, len + 2);
		if (n != len + 2)
			lwsl_err("problem writing to LMP\n");
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
			if (n >= 0)
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

//		buf[LWS_SEND_BUFFER_PRE_PADDING + m] = '\0';
//		fprintf(stderr, "ws issue: %s\n", &buf[LWS_SEND_BUFFER_PRE_PADDING]);

		n = libwebsocket_write(wsi, &buf[LWS_SEND_BUFFER_PRE_PADDING],
				m, LWS_WRITE_TEXT);
		if (n < 0) {
			lwsl_err("failed to send ws data %d\n");
			return -1;
		}

		if (pss->json_fifo_tail != json_fifo_head)
			libwebsocket_callback_on_writable(context, wsi);
		break;

	default:
		break;
	}

	return 0;
}
