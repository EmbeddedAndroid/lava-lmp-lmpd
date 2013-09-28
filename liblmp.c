/*
 * liblmp.c
 *
 *  Created on: Sep 27, 2013
 *      Author: agreen
 */

#include <lmpd.h>

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
