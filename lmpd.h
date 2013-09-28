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
#include <lejp.h>

#include <termios.h>
#include <linux/serial.h>

#define LOCAL_RESOURCE_PATH INSTALL_DATADIR"/lmpd"
#define MAX_LMP 128
#define MAX_JSON 16384
#define JSON_FIFO_SIZE 65536

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

struct per_session_data__lmpd {
	int m;
	int sent_lmp_json;
	int json_fifo_tail;

	int last_issue;
};

struct per_session_data__http {
	int fd;
};

extern int max_poll_elements;
extern struct pollfd *pollfds;
extern int *fd_to_pollfd_index;
extern int count_pollfds;
extern char *fd_to_type;

extern char force_exit;

extern int issue;

extern struct lmp lmp[MAX_LMP];
extern int nlmp;

extern char json_fifo[JSON_FIFO_SIZE];
extern int json_fifo_head;

extern int
lmpd_service_netlink(struct libwebsocket_context *context, struct pollfd *pfd);
extern int callback_http(struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason, void *user,
							  void *in, size_t len);
extern int callback_lmpd(struct libwebsocket_context *context,
		struct libwebsocket *wsi,
		enum libwebsocket_callback_reasons reason, void *user,
							  void *in, size_t len);
extern struct libwebsocket_protocols protocols[];

extern int set_tty_for_lmp(int fd);
