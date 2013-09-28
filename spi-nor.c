/*
 * spi-nor.c
 *
 *  Created on: Sep 27, 2013
 *      Author: agreen
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <lejp.h>
#include <poll.h>
#include <errno.h>
#include <termios.h>
#include <linux/serial.h>

enum nor_commands {
	NOR_CMD_READ	= 0x03,
	NOR_CMD_QOR	= 0x6b,
	NOR_CMD_4QOR	= 0x6c, /* 32-bit addresses */
	NOR_CMD_RDID	= 0x9f,
	NOR_CMD_WREN	= 0x06,
	NOR_CMD_SE	= 0xD8,
	NOR_CMD_PP	= 0x02,
	NOR_CMD_QPP	= 0x32,
	NOR_CMD_4QPP	= 0x34, /* 32-bit addresses */
	NOR_CMD_WRR	= 0x01,
	NOR_CMD_CLSR	= 0x30,
	NOR_CMD_RDSR	= 0x05,
};

/* overall detect / read / program state machine */

enum seq {
	SEQ_IDLE,

	SEQ_RDID,
	SEQ_WAIT_RDID,

	SEQ_COMPLETED,
	SEQ_FAILED,

	SEQ_READ,
	SEQ_WAIT_READ,

	SEQ_WRITE1,
	SEQ_WRITE2,
	SEQ_WRITE3,
	SEQ_WRITE4,

	SEQ_ERASE1,
};

enum {
	UTF8_VIOL__4BIT = 0xff,
	UTF8_VIOL__INIT = 0xfe,
	UTF8_VIOL__CS_HILO = 0xfd,
	UTF8_VIOL__WAIT_FLASH_DONE = 0xfc,
};

typedef unsigned short __le16;
#define le16_to_cpu(__x) (__x)

struct cfi_erase_block_info {
	__le16 count_blocks_minus_one __attribute__ ((packed));
	__le16 size_div_256 __attribute__ ((packed));
} __attribute__ ((packed));

struct cfi {
	char qry[3];
	__le16 cmd_set;

	__le16 ext_ads;
	__le16 alt_ads;
	__le16 alt_ext_ads;

	unsigned char bcd_min_vcc_prog;
	unsigned char bcd_max_vcc_prog;
	unsigned char bcd_min_vpp_prog;
	unsigned char bcd_max_vpp_prog;

	unsigned char timeout_us_order_typ_single;
	unsigned char timeout_us_order_typ_multi;
	unsigned char timeout_us_order_typ_single_erase;
	unsigned char timeout_us_order_typ_bulk_erase;

	unsigned char timeout_typ_order_max_single;
	unsigned char timeout_typ_order_max_multi;
	unsigned char timeout_typ_order_max_single_erase;
	unsigned char timeout_typ_order_max_bulk_erase;

	unsigned char total_size_byte_order;
	__be16 jep137_interface_code;
	__be16 page_size_bytes_order;

	unsigned char count_erase_block_regions;
	struct cfi_erase_block_info ebi[];
} __attribute__ ((packed));

static int seq = SEQ_RDID;
static int ctr;
static int budget;
static unsigned char rdid[256];
struct cfi *cfi = (struct cfi *)(rdid + 0x10);
static int fd_file = -1;
static unsigned long address = 0, length = 0;
static char no_erase = 0;
static int allow_send = 0;
static char failed_cfi = 0;

struct chips {
	unsigned int erase_sect;
	unsigned long size;
	unsigned int write_page_size;
	int ads32;
};
static struct chips chip;

static const char *manf[] = {\
	"",
	"Spansion",
	"AMI",
	"",
	"Fujitsu",
	"",
	"",
	"Hitachi",
};

/* JSON frame */

static const char *tx_cmd[] = {
	"\x02{"
		"\"schema\":\"org.linaro.lmp.lsgpio\","
		"\"spi\":{"
			"\"write\":\"",

			"\","
			"\"read\":\"",

			"\""
		"}"
	"}\x04",
};


/* parser strings */

enum {
	LEJPPM_READ = 1,
};

static const char * const paths[] = {
	"read",
};

static char nyb;
static unsigned long original_length = 1;
static struct timeval tv;
static unsigned long start_time, per_sec;
static int sec = 0;

int set_tty_for_lmp(int fd)
{
	struct termios tty;

	/* enforce suitable tty state */

	memset(&tty, 0, sizeof tty);
	if (tcgetattr(fd, &tty))
		return 1;

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
	tcsetattr(fd, TCSANOW, &tty);

	return 0;
}

void track_time(void)
{
	int s_rem = 0;
	int elapsed_ms;
	int percent;
	int n;

	gettimeofday(&tv, NULL);
	if ((length && sec == tv.tv_sec) || seq <= SEQ_WAIT_RDID ||
							!original_length)
		return;

	elapsed_ms = (((tv.tv_sec * 1000000) + tv.tv_usec) - start_time) / 1000;
	if (elapsed_ms) {
		per_sec = ((original_length - length) * 1000) / elapsed_ms;
		if (per_sec)
			s_rem = length / per_sec;
	}
	percent = 100 - (((length * 100) / original_length));
	if (!length)
		percent = 100;
	fprintf(stderr, "   [");
	for (n = 0; n < 100; n += 2)
		if (n <= percent)
			fprintf(stderr, "#");
		else
			fprintf(stderr, " ");
	fprintf(stderr, "]  ");

	if (elapsed_ms > 500)
		fprintf(stderr, " %4ldKiB/sec", per_sec / 1000);

	if (s_rem && elapsed_ms > 500)
		fprintf(stderr, ", rem: %4ds", s_rem);
	else
		if (!length)
			fprintf(stderr, ", finished in %d.%1ds",
				  elapsed_ms / 1000, (elapsed_ms % 1000) / 100);
		else
			fprintf(stderr, "                      ");

	fprintf(stderr, "    \r");
	sec = tv.tv_sec;
}

static char
parse_callback(struct lejp_ctx *ctx, char reason)
{
	int n, m, k = 0;
	unsigned char buf[128];
	static unsigned char esc;

	if (reason == LEJPCB_COMPLETE) {

		/* need to figure out what to do next */

		switch (seq) {
		case SEQ_WAIT_READ:
			if (length)
				seq = SEQ_READ;
			else
				seq = SEQ_COMPLETED;
			break;

		case SEQ_WAIT_RDID:
			if (cfi->qry[0] != 'Q' || cfi->qry[1] != 'R' ||
							   cfi->qry[2] != 'Y') {
				failed_cfi++;
				if (failed_cfi < 3) {
					seq = SEQ_RDID;
					break;
				}
				fprintf(stderr, "Failed to get CFI block\n");
				seq = SEQ_FAILED;
				break;
			}

			chip.size = 1 << cfi->total_size_byte_order;
			chip.erase_sect = 0;
for (n = 0; n < cfi->count_erase_block_regions; n++)
	if (chip.erase_sect < (le16_to_cpu(cfi->ebi[n].size_div_256) << 8))
		chip.erase_sect = le16_to_cpu(cfi->ebi[n].size_div_256) << 8;
			chip.write_page_size =
				1 << le16_to_cpu(cfi->page_size_bytes_order);
			chip.ads32 =
			       le16_to_cpu(cfi->jep137_interface_code) == 0x102;
			seq = SEQ_COMPLETED;
			break;
		}

		return 0;
	}


	if (ctx->path_match != LEJPPM_READ)
		return 0;

	switch (reason) {
	case LEJPCB_VAL_STR_START:
		nyb = 0;
		break;
	case LEJPCB_VAL_STR_CHUNK:
	case LEJPCB_VAL_STR_END:
		for (n = 0; n < ctx->npos;) {

			switch ((unsigned char)ctx->buf[n] & 0xc0) {
			case 0xc0:
				esc = ctx->buf[n++];
				continue;
			case 0x80:
				nyb = (esc << 6) | (ctx->buf[n++] & 0x3f);
				break;
			default:
				nyb = ctx->buf[n++];
				break;
			}

			switch (seq) {
			case SEQ_WAIT_RDID:
				if (ctr < sizeof(rdid))
					rdid[ctr++] = nyb;
				break;
			case SEQ_WAIT_READ:
				buf[k++] = nyb;
				break;
			}
		}
		if (seq == SEQ_WAIT_READ) {
			m = write(fd_file, buf, k);
			length -= k;
			address += k;
			if (m < 0) {
				fprintf(stderr,
					"Failed to write to output file\n");
				seq = SEQ_FAILED;
			}
		}
		track_time();
		break;
	}

	return 0;
}

static unsigned long _atol(const char *s)
{
	unsigned long u = 0;
	char num[30];
	int n = 0;

	if (s[0] == '0' && s[1] == 'x') {
		sscanf(&s[2], "%lx", &u);
		return u;
	}

	while (*s >= '0' && *s <= '9')
		num[n++] = *s++;
	num[n] = '\0';

	u = atol(num);

	if (*s == 'g' || *s == 'G')
		return u * 1024 * 1024 * 1024;
	if (*s == 'm' || *s == 'M')
		return u * 1024 * 1024;
	if (*s == 'k' || *s == 'K')
		return u * 1024;

	return u;
}

static int append_utf8(char *dest, const unsigned char c)
{
	if (c < 0x80 && c >= 0x20 && c != '\"' && c != '\\') {
		*dest = c;
		return 1;
	}
	*dest++ = 0xc0 | (c >> 6);
	*dest++ = 0x80 | (c & 0x3f);

	return 2;
}

void hexdump(unsigned char *p, int len)
{
	char str[48];
	int n, y = 0, m, c;
	static const char hex[] = "0123456789ABCDEF";

	str[y++] = '\r';
	str[y++] = '\n';
	str[y++] = '\0';
	fprintf(stderr, "%s", str);
	y = 0;

	for (n = 0; n < len; n++) {
		if (!y)
			y = sprintf(str, "%04X: ", n);

		str[y++] = hex[(p[n] >> 4) & 0xf];
		str[y++] = hex[p[n] & 0xf];
		if ((n & 7) == 7 || n == len - 1) {
			m = n;
			while ((m & 7) != 7) {
				str[y++] = ' ';
				str[y++] = ' ';
				str[y++] = ' ';
				m++;
			}
			str[y++] = ' ';
			str[y++] = ' ';
			c = 8;
			m = n & ~7;
			if (m + 8 > len)
				c = len - m;
			while (c--) {
				if (p[m] < 32 || p[m] > 126)
					str[y++] = '.';
				else
					str[y++] = p[m];
				m++;
			}

			str[y++] = '\r';
			str[y++] = '\n';
			str[y++] = '\0';
			fprintf(stderr, "%s", str);
			y = 0;
		} else
			str[y++] = ' ' ;
	}
}

int perform(int fd, int _job, unsigned long _address, unsigned long _length)
{
	struct pollfd pfd;
	int timeouts = 0;
	int n, m, k, ret = 0;
	unsigned char buf[4096];
	char tb[4096];
	int sync = 0;
	struct lejp_ctx ctx;
	unsigned long l;

	switch (_job) {
	case SEQ_RDID:
		goto no_print;
	case SEQ_READ:
		fprintf(stderr, "  READING ");
		break;
	case SEQ_WRITE1:
		fprintf(stderr, "  WRITING ");
		break;
	case SEQ_ERASE1:
		fprintf(stderr, "  ERASING ");
		/* correct start and length to be in integer erase blocks */
		l = _address & (chip.erase_sect - 1);
		_length += (_address - l);
		l = _length & ~(chip.erase_sect - 1);
		if (_length & (chip.erase_sect - 1))
			l += chip.erase_sect;
		_length = l;
		break;
	}

	fprintf(stderr, "Start: +0x%lx (+%ld), "
			"length: 0x%lx (%ld)\n",
				_address, _address, _length, _length);
no_print:

	address = _address;
	length = _length;
	original_length = _length;
	seq = _job;
	pfd.fd = fd;

	gettimeofday(&tv, NULL);
	start_time = ((tv.tv_sec * 1000000) + tv.tv_usec);

	while (1) {

		track_time();

		pfd.events = POLLIN | POLLERR;

		if (allow_send) {

			/* you need to book any writes here */

			switch (seq) {
			case SEQ_RDID:
			case SEQ_READ:
			case SEQ_WRITE1:
			case SEQ_WRITE2:
			case SEQ_WRITE3:
			case SEQ_WRITE4:
			case SEQ_ERASE1:
				pfd.events |= POLLOUT;
				break;
			case SEQ_COMPLETED:
				track_time();
				if (original_length)
					fprintf(stderr, "\n");
				goto bail;
			case SEQ_FAILED:
				fprintf(stderr, "failed\n");
				ret = 1;
				goto bail;
			}
		}

		n = poll(&pfd, 1, 100);
		if (n < 0)
			goto bail;
		if (n == 0) {
			/* We want to see that we have drained the rx buffer */
			if (allow_send)
				continue;
			timeouts++;
			if (timeouts < 3)
				continue;
			allow_send = 1;
			gettimeofday(&tv, NULL);
			start_time = ((tv.tv_sec * 1000000) + tv.tv_usec);
			continue;
		}
		timeouts = 0;

		if (pfd.revents & POLLIN) {

			n = read(fd, buf, sizeof(buf));
			if (n <= 0) {
				fprintf(stderr, "read error\n");
				goto bail;
			}

			if (!allow_send)
				continue;

			m = 0;
			while (m < n) {
				if (buf[m] == '\x02') {
					lejp_construct(
						&ctx, parse_callback, NULL,
						      paths, ARRAY_SIZE(paths));
					sync = 1;
					goto again;
				}
				if (buf[m] == '\x04') {
					lejp_destruct(&ctx);
					goto again;
				}
				if (!sync)
					goto again;

				lejp_parse(&ctx, &buf[m], 1);
	again:
				m++;
			}
		}

		if (!allow_send || !(pfd.revents & POLLOUT))
			continue;

		/* a write we booked earlier is now possible */

		m = 0;
		switch (seq) {

		case SEQ_RDID:
			m = sprintf(tb, "%s", tx_cmd[0]);
			tb[m++] = (char)UTF8_VIOL__INIT;
			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			m += append_utf8(&tb[m], NOR_CMD_RDID);
			m += sprintf(&tb[m], "%s""256""%s",
							tx_cmd[1], tx_cmd[2]);
			ctr = 0;
			goto do_cmd;

		case SEQ_READ:
			m = sprintf(tb, "%s", tx_cmd[0]);
			if (chip.ads32) {
				m += append_utf8(&tb[m], NOR_CMD_4QOR);
				m += append_utf8(&tb[m], address >> 24);
			} else
				m += append_utf8(&tb[m], NOR_CMD_QOR);
			m += append_utf8(&tb[m], address >> 16);
			m += append_utf8(&tb[m], address >> 8);
			m += append_utf8(&tb[m], address);
			m += append_utf8(&tb[m], 0xdd);
			n = 1024;
			if (length < n)
				n = length;
			m += sprintf(&tb[m], "%s""q,""%d%s",tx_cmd[1],
							     n, tx_cmd[2]);
			goto do_cmd;

		case SEQ_WRITE1:
			m = sprintf(tb, "%s", tx_cmd[0]);
			m += append_utf8(&tb[m], NOR_CMD_WREN);
			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			m += append_utf8(&tb[m], NOR_CMD_WRR);
			m += append_utf8(&tb[m], 0x02);
			m += append_utf8(&tb[m], 0x02);
			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			m += append_utf8(&tb[m], NOR_CMD_RDSR);
			tb[m++] = (char)UTF8_VIOL__WAIT_FLASH_DONE;
			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			m += append_utf8(&tb[m], NOR_CMD_WREN);
			tb[m++] = (char)UTF8_VIOL__CS_HILO;

			/* fallthru */
		case SEQ_WRITE2:
			m += append_utf8(&tb[m], NOR_CMD_WREN);
			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			if (chip.ads32) {
				m += append_utf8(&tb[m], NOR_CMD_4QPP);
				m += append_utf8(&tb[m], address >> 24);
			} else
				m += append_utf8(&tb[m], NOR_CMD_QPP);

			//m += append_utf8(&tb[m], NOR_CMD_PP);
			m += append_utf8(&tb[m], address >> 16);
			m += append_utf8(&tb[m], address >> 8);
			m += append_utf8(&tb[m], address);
			tb[m++] = (char)UTF8_VIOL__4BIT;

			budget = chip.write_page_size -
					(address & (chip.write_page_size - 1));
			if (budget > length)
				budget = length;
			seq = SEQ_WRITE3 - 1;
			goto do_cmd;

		case SEQ_WRITE3: /* Bulk data for <= one page */
			k = 128;
			if (length < k)
				k = length;
			if (budget < k)
				k = budget;
			if (!k) {
				seq = SEQ_WRITE4;
				break;
			}
			n = read(fd_file, buf, k);
			if (n < k) {
				fprintf(stderr, "problem reading from file "
					"(read %d vs %d) "
					"address=%lX length=%lX\n",
							n, k, address, length);
				seq = SEQ_FAILED;
				break;
			}

			/* Convert to UTF-8 with forced coding where needed */
			for (m = 0, n = 0; n < k; n++)
				m += append_utf8(&tb[m], buf[n]);

			budget -= k;
			address += k;
			length -= k;
			if (budget) {
				seq--;
				goto do_cmd;
			}

			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			m += append_utf8(&tb[m], NOR_CMD_RDSR);
			tb[m++] = (char)UTF8_VIOL__WAIT_FLASH_DONE;
			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			m += append_utf8(&tb[m], NOR_CMD_WREN);
			tb[m++] = (char)UTF8_VIOL__CS_HILO;

			seq = SEQ_WRITE2 - 1;
			goto do_cmd;

		case SEQ_WRITE4: /* end of page program */
			m = sprintf(tb, "%s""0""%s", tx_cmd[1], tx_cmd[2]);
			seq = SEQ_COMPLETED - 1;
			goto do_cmd;

		case SEQ_ERASE1:
			m = sprintf(tb, "%s", tx_cmd[0]);
			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			m += append_utf8(&tb[m], NOR_CMD_WREN);
			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			m += append_utf8(&tb[m], NOR_CMD_CLSR);
			tb[m++] = (char)UTF8_VIOL__CS_HILO;

			m += append_utf8(&tb[m], NOR_CMD_SE);
			m += append_utf8(&tb[m], address >> 16);
			m += append_utf8(&tb[m], address >> 8);
			m += append_utf8(&tb[m], address);

			tb[m++] = (char)UTF8_VIOL__CS_HILO;
			m += append_utf8(&tb[m], NOR_CMD_RDSR);
			tb[m++] = (char)UTF8_VIOL__WAIT_FLASH_DONE;
			tb[m++] = (char)UTF8_VIOL__CS_HILO;

			m += sprintf(&tb[m], "%s""0""%s",tx_cmd[1], tx_cmd[2]);

			length -= chip.erase_sect;
			address += chip.erase_sect;

			if (!length)
				seq = SEQ_COMPLETED - 1;
			else
				seq--;

			/* fallthru */
	do_cmd:
			n = write(fd, tb, m);
			if (n < m) {
				fprintf(stderr, "write failed\n");
				goto bail;
			}
			seq++;
			break;
		}
	}
bail:
	return ret;
}

static struct option options[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "no-erase",	no_argument,		NULL, 'n' },
	{ "debug",	no_argument,		NULL, 'd' },
	{ NULL, 0, 0, 0 }
};

int main(int argc, char *argv[])
{
	int n, ret = 0;
	int fd;
	struct stat stat;
	int _job = SEQ_COMPLETED;
	unsigned long _address = 0, _length = 0;
	int verbose = 0;
	int verify = 0;

	fprintf(stderr, "LSGPIO LMP SPI NOR Programmer (C)2013 Linaro, Ltd\n");

	while (n >= 0) {
		n = getopt_long(argc, argv, "hndv", options, NULL);
		if (n < 0)
			continue;
		switch (n) {
		case 'n':
			no_erase = 1;
			break;
		case 'd':
			verbose = 1;
			break;
		case 'v':
			verify = 1;
			break;
		case 'h':
usage:
			fprintf(stderr, "Usage: %s <LMP path> "
				"[erase <start ofs> <len>]\n"
				"[write <write file> <start ofs> [<len>]]\n"
				"[read <read file> <start ofs> <len>]\n"
				"\n"
				"For offset and length, you can give decimal,\n"
				"hex like 0x100, or SI units like 4M/ 4G/ 4K\n"
				"\n", argv[0]);
			return 1;
		}
	}

	if (optind >= argc)
		goto usage;

	fd = open(argv[optind], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Unable to open %s: %s\n",
					argv[optind], strerror(errno));
		return 1;
	}
	if (set_tty_for_lmp(fd)) {
		fprintf(stderr, "Unable to set TTY state on %s\n",
								argv[optind]);
		return 1;
	}
	optind++;
	if (optind < argc) {
		if (!strcmp(argv[optind], "read"))
			_job = SEQ_READ;
		if (!strcmp(argv[optind], "write"))
			_job = SEQ_WRITE1;
		if (!strcmp(argv[optind], "erase"))
			_job = SEQ_ERASE1;

		if (_job == SEQ_COMPLETED) {
			close(fd);
			goto usage;
		}
	}

	if (_job == SEQ_READ || _job == SEQ_WRITE1) {
		optind++;
		if (optind + 1 >= argc) {
			close(fd);
			goto usage;
		}
		if (_job == SEQ_READ)
			fd_file = open(argv[optind], O_WRONLY | O_CREAT, 0660);
		else
			fd_file = open(argv[optind], O_RDONLY);
		if (fd_file < 0) {
			fprintf(stderr, "unable to open %s\n", argv[optind]);
			goto bail;
		}
	}

	if (_job != SEQ_COMPLETED) {
		optind++;
		if (optind >= argc) {
			close(fd);
			close(fd_file);
			goto usage;
		}
		_address = _atol(argv[optind]);
		optind++;
		if (optind >= argc) {
			if (_job != SEQ_WRITE1) {
				close(fd);
				close(fd_file);
				goto usage;
			}

			if (fstat(fd_file, &stat)) {
				fprintf(stderr, "unable to stat write file\n");
				goto bail;
			}

			_length = stat.st_size;
		} else
			_length = _atol(argv[optind]);
	}

	/* find out what we have first */
	ret = perform(fd, SEQ_RDID, 0, 0);
	if (ret)
		goto bail;

	if (rdid[0] < sizeof(manf) / sizeof(manf[0]))
		fprintf(stderr, "%s %02X%02X, ", manf[rdid[0]],
			rdid[1], rdid[2]);
	else
		fprintf(stderr, "Unknown manf 0x%02x %02X%02X, ", rdid[0],
				rdid[1], rdid[2]);

	fprintf(stderr, "%ldMB, write page: %d, Erase: %dK, Ads: ",
			   chip.size >> 20, chip.write_page_size,
			   chip.erase_sect >> 10);
	if (chip.ads32)
		fprintf(stderr, "32-bit\n");
	else
		fprintf(stderr, "24-bit\n");

	if (verbose)
		for (n = 0; n < cfi->count_erase_block_regions; n++) {
			fprintf(stderr, "     erase region %d: %d x %d\n",
				n + 1,
				le16_to_cpu(cfi->ebi[n].count_blocks_minus_one),
				le16_to_cpu(cfi->ebi[n].size_div_256) << 8);
		}

	/* in the case of write, we usually want to auto-erase before */
	if (_job == SEQ_WRITE1 && !no_erase) {
		ret = perform(fd, SEQ_ERASE1, _address, _length);
		if (ret)
			goto bail;
	}
	/* do the main task */
	if (_job != SEQ_COMPLETED)
		ret = perform(fd, _job, _address, _length);

	if (_job == SEQ_WRITE1 && verify) {

	}

bail:
	close(fd);
	if (fd_file >= 0)
		close(fd_file);

	return ret;
}
