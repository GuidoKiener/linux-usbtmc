/***************************************************************************
                                 test-raw.c
                                 ----------

    Programme to test the linux usbtmc driver against
    any newer T&M instrument (Keysight, Tektronix, Rohde & Schwarz, ...
    This code also serves as an example for how to use
    the driver with raw read/write functions.

    copyright : (C) 2015 by Dave Penkler
    email     : dpenkler@gmail.com
    copyright : (C) 2018 by Guido Kiener
    email     : usbtmc@kiener-muenchen.de
 ***************************************************************************/

#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <endian.h>
#include <linux/usb/ch9.h>
//#include <linux/usb/tmc.h>
#include "tmc.h"


#define NUM_CAPS 9
typedef struct cap_entry {
	char * desc;
	unsigned int mask;
} cap_entryT;

/* Driver encoded capability masks */

const cap_entryT cap_list[NUM_CAPS] = {
	{"TRIGGER      ",  USBTMC488_CAPABILITY_TRIGGER        },
	{"REN_CONTROL  ",  USBTMC488_CAPABILITY_REN_CONTROL    },
	{"GOTO_LOCAL   ",  USBTMC488_CAPABILITY_GOTO_LOCAL     },
	{"LOCAL_LOCKOUT",  USBTMC488_CAPABILITY_LOCAL_LOCKOUT  },
	{"488_DOT_2    ",  USBTMC488_CAPABILITY_488_DOT_2      },
	{"DT1          ",  USBTMC488_CAPABILITY_DT1            },
	{"RL1          ",  USBTMC488_CAPABILITY_RL1            },
	{"SR1          ",  USBTMC488_CAPABILITY_SR1            },
	{"FULL_SCPI    ",  USBTMC488_CAPABILITY_FULL_SCPI      }
};

#define HEADER_SIZE 12
#define BULKSIZE 4096

static int fd;
static const char star[] = {' ','*'};
static __u8 s_tag = 0x01;
static __u8 s_tag_in = 0;
static __u8 s_tag_out = 0;

static void show_caps(unsigned char caps) {
	int i;
	printf("Instrument capabilities: * prefix => supported capability\n\n");
	for (i=0;i<NUM_CAPS;i++)
		printf("\t%c%s\n",star[((caps & cap_list[i].mask) != 0)],
			cap_list[i].desc);
}

#define NUM_STAT_BITS 8
typedef struct stat_entry {
	char * desc;
	unsigned int mask;
} stat_entryT;

/*
   Service request enable register (SRE) Bit definitions
 */

#define SRE_Trigger            1
#define SRE_User               2
#define SRE_Message            4
#define SRE_MessageAvailable  16
#define SRE_Event_Status      32
#define SRE_Operation_Status 128

/* need these defines to inline bit definitions in SRE command strings */
#define xstr(s) str(s)
#define str(s) #s

/*
   Status register bit definitions
 */

#define STB_TRG    1
#define STB_USR    2
#define STB_MSG    4
#define STB_MAV   16
#define STB_ESB   32
#define STB_MSS   64
#define STB_OSR  128

const stat_entryT stb_bits[NUM_STAT_BITS] = {
	{"TRG",  STB_TRG},
	{"USR",  STB_USR},
	{"MSG",  STB_MSG},
	{"__8",  8}, /* not used */
	{"MAV",  STB_MAV},
	{"ESB",  STB_ESB},
	{"MSS",  STB_MSS},
	{"OSR",  STB_OSR}
};

/* Helper routines */

static double main_ts = 0.0;
double getTS_usec() {
	struct timeval tv;
	double ts,tmp;
	gettimeofday(&tv,NULL);
	tmp = tv.tv_sec*1e6 + (double)tv.tv_usec;
	ts = tmp - main_ts;
	main_ts = tmp;
	return ts;
}

static void show_stb(unsigned char stb) {
	int i;
	printf("%10.0f STB = ",getTS_usec());
	for (i=0;i<NUM_STAT_BITS;i++)
		if (stb & stb_bits[i].mask) {
			printf("%s ",stb_bits[i].desc);
		}
	printf("\n");
}

static int flag=0; /* set to 1 if srq handler called */

void srq_handler(int sig) {
	flag = 1;
}


unsigned int get_stb() {
	unsigned char stb;
	if (0 != ioctl(fd,USBTMC488_IOCTL_READ_STB,&stb)) {
		perror("stb ioctl failed");
		exit(1);
	}
	return stb;
}

/* Send string to T&M instrument */
static int tmc_raw_write_common(char *msg, __u32 length, __u32 *written, int async)
{
	struct usbtmc_message data;
	__u64 total = 0;
	int retval = 0;
	__u32 addflag = (async)?(USBTMC_FLAG_ASYNC):0;
	char buf[1024];
	/* Size of first package is USB 3.0 max packet size.
	 * Use only multiple of max package size (64,512,1024) to avoid sending a short package.
	 * Only last package can be a short package.
	 */

	if (!msg)
		return -EINVAL;

	buf[0] = 1;
	buf[1] = s_tag;
	buf[2] = ~s_tag;

	s_tag_out = s_tag;

	s_tag++;
	if (s_tag == 0) s_tag++;

	buf[3] = 0; /* Reserved */
	buf[4] = length >> 0;
	buf[5] = length >> 8;
	buf[6] = length >> 16;
	buf[7] = length >> 24;
	buf[8] = 0x01; /* EOM */
	buf[9] = 0; /* Reserved */
	buf[10] = 0; /* Reserved */
	buf[11] = 0; /* Reserved */

	data.message = buf;
	data.transfer_size = length + HEADER_SIZE; /* 32 bit alignment done by driver */

	if (data.transfer_size <= sizeof(buf)) {
		data.flags = addflag; /* synchron */
		memcpy(&buf[HEADER_SIZE], msg, length);
		retval = ioctl(fd,USBTMC_IOCTL_WRITE, &data);
		goto exit;
	}

	data.transfer_size = sizeof(buf);
	data.flags = (USBTMC_FLAG_ASYNC | addflag); /* asynchron */
	memcpy(&buf[HEADER_SIZE], msg, sizeof(buf)-HEADER_SIZE);
	retval = ioctl(fd, USBTMC_IOCTL_WRITE, &data);
	if (retval < 0)
		goto exit;

	assert(data.transferred == sizeof(buf));
	data.message = msg + (sizeof(buf) - HEADER_SIZE);
	data.transfer_size = length - (sizeof(buf) - HEADER_SIZE);
	data.flags = (USBTMC_FLAG_APPEND | addflag); /* append and wait*/
	retval = ioctl(fd, USBTMC_IOCTL_WRITE, &data);
exit:
	total += data.transferred;
	if (written) {
		if (total >= HEADER_SIZE)
			total -= HEADER_SIZE;
		if (total > length) {
			assert((total - length) <= 3);
			total = length; // strip 32 bit alignment done by driver */
		}
		*written = total;
	}
	// TODO: Abort BULK OUT in case of error!
	if (retval < 0)
		retval = -errno;
	return retval;
}

static int tmc_raw_write(char *msg, __u32 length, __u32 *written)
{
	return tmc_raw_write_common(msg, length, written, 0);
}

static int tmc_raw_write_async(char *msg, __u32 length, __u32 *written)
{
	return tmc_raw_write_common(msg, length, written, 1);
}

static int tmc_raw_write_result_async(__u32 *written)
{
	__u64 transferred;
	/* Be careful: transferred size includes header and up to 3 padded bytes */
	int retval = ioctl(fd, USBTMC_IOCTL_WRITE_RESULT, &transferred);
	if (written)
		*written = transferred;
	return retval;
}

static int tmc_raw_send(char *msg)
{
	__u32 written;
	int retval = tmc_raw_write(msg, strlen(msg), &written);
	return retval;
}

static int tmc_send(char *msg)
{
	int retval = write(fd, msg, strlen(msg));
	return retval;
}

void setSRE(int val) {
	char buf[32];
	snprintf(buf,32,"*SRE %d\n",val);
	tmc_raw_send(buf);
}

static int tmc_raw_read_async_start(__u32 max_len)
{
	struct usbtmc_message data;
	char request[HEADER_SIZE];
	int retval;

	request[0] = 2;
	request[1] = s_tag;
	request[2] = ~s_tag;

	s_tag_in = s_tag;

	s_tag++;
	if (s_tag == 0) s_tag++;

	request[3] = 0; /* Reserved */
	request[4] = max_len >> 0;
	request[5] = max_len >> 8;
	request[6] = max_len >> 16;
	request[7] = max_len >> 24;
	request[8] = 0x00; /* termchar enabled */
	request[9] = 0; /* termchar */
	request[10] = 0; /* Reserved */
	request[11] = 0; /* Reserved */

	data.message = request;
	data.transfer_size = HEADER_SIZE;
	data.flags = USBTMC_FLAG_ASYNC; /* async */

	retval = ioctl(fd,USBTMC_IOCTL_WRITE, &data);
	//printf("tmc_raw_read_async_start 1: rv=%d\n", retval);
	if (retval < 0)
		return retval;

	data.message = NULL; /* just trigger asynchronous read */
	data.transfer_size = BULKSIZE;
	data.flags = USBTMC_FLAG_ASYNC; /* sync */
	retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
	//printf("tmc_raw_read_async_start 2: rv=%d\n", retval);
	if (retval < 0) {
		if (errno == EAGAIN) // expected value
			retval = 0;
	}
	return retval;
}

int tmc_raw_read_async_result(char *msg, __u32 max_len, __u32 *received)
{
	struct usbtmc_message data;
	__u64 total = 0;
	__u32 expected_size = 0;
	char *buf;
	int retval;

	if (!msg)
		return -EINVAL;

	buf = malloc(BULKSIZE);
	if (!buf)
		return -ENOMEM;

	data.message = buf;
	/* Attention! must be multiple of BULKSIZE otherwise urb data is truncated.
	 * Note that kernel driver only wants to copy a complete urb.
	 */
	data.transfer_size = BULKSIZE;
	data.flags = USBTMC_FLAG_ASYNC; /* async */
	retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
	if (retval < 0) {
		goto exit;
	}

	if (data.transferred < HEADER_SIZE) {
		retval = -EPROTO;
		goto exit;
	}
	data.transferred -= HEADER_SIZE;

	if (buf[0] != 2) {
		retval = -EPROTO; /* response out of order */
		goto exit;
	}

#if 0 // This sample does not check sequence number here.
	if (memcmp(request,buf,4) != 0) {
		retval = -EPROTO; /* response out of order */
		goto exit;
	}
#endif

	expected_size = le32toh(*(__u32*)&buf[4]);
	if (expected_size > max_len || data.transferred > expected_size) {
		retval = -EPROTO; /* more data than requested */
		goto exit;
	}

	memcpy(msg, (char*)data.message + HEADER_SIZE, data.transferred);
	total = data.transferred;
	expected_size -= data.transferred;

	if (retval == 0) {
		// No short packet or ZLP received => ready
		do {
			data.message = msg + total;
			data.transfer_size = expected_size;
			data.flags = USBTMC_FLAG_ASYNC|USBTMC_FLAG_IGNORE_TRAILER;
			retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
			if (retval < 0) {
				if (errno == EAGAIN) {
					struct pollfd pfd;
					pfd.fd = fd;
					pfd.events = POLLIN|POLLERR|POLLHUP;
					retval = poll(&pfd,1,100); // should not take longer!
					if (retval!=1) {
						ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
						retval = -ETIMEDOUT;
						goto exit;
					}
					retval = 0;
					continue; // try again!
				}
				retval = -errno;
				ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
				goto exit;
			}
			total += data.transferred;
			expected_size -= data.transferred;
		} while (retval == 0);
		assert(retval == 1); // must be short packet or ZLP now.
		retval = 0;
	}
exit:
	if (received)
		*received = (__u32)total;

	// TODO: In case of error abort bulk in.
	free(buf);
	return retval;
}

#if 1
static int tmc_raw_read(char *msg, __u32 max_len, __u32 *received)
{
	struct pollfd pfd;
	int timeout = 2000;
	int err;
	__u32 transferred = 0;

	int rv = tmc_raw_read_async_start(max_len);
	if (rv < 0) {
		printf("tmc_raw_read failed to start: errno=%d\n", errno);
		ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
		goto exit;
	}

	pfd.fd = fd;
	pfd.events = POLLOUT|POLLIN|POLLERR|POLLHUP;
	err = poll(&pfd,1,timeout);
	if (err!=1) {
		ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
		rv = -ETIMEDOUT;
		goto exit;
	}

	/* Note that POLLOUT is set when all anchor "submitted" is empty */
	if (pfd.revents & (POLLERR|POLLOUT)) {
		__u32 written;
		rv = tmc_raw_write_result_async(&written);
		if (rv < 0 || written != HEADER_SIZE) {
			printf("tmc_raw_read failed to write header: rv=%d written=%u\n", rv, written);
			ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
			goto exit;
		}
	}
	if (pfd.revents & (POLLERR|POLLIN)) {
		rv = tmc_raw_read_async_result( msg, max_len, &transferred);
		if (rv < 0) {
			printf("tmc_raw_read failed: rv=%d received=%u\n", rv, transferred);
		}
	}
	else {
		/* must not happen */
		assert(0);
		rv = -1; // TODO:
	}
exit:
	if (received)
		*received = transferred;

	return rv;


}
#else
static int tmc_raw_read(char *msg, __u32 max_len, __u32 *received)
{
	struct usbtmc_message data;
	__u64 total = 0;
	__u32 expected_size = 0;
	char request[HEADER_SIZE];
	char *buf;
	int retval;

	if (!msg)
		return -EINVAL;

	buf = malloc(BULKSIZE);
	if (!buf)
		return -ENOMEM;

	request[0] = 2;
	request[1] = s_tag;
	request[2] = ~s_tag;

	s_tag_in = s_tag;

	s_tag++;
	if (s_tag == 0) s_tag++;

	request[3] = 0; /* Reserved */
	request[4] = max_len >> 0;
	request[5] = max_len >> 8;
	request[6] = max_len >> 16;
	request[7] = max_len >> 24;
	request[8] = 0x00; /* termchar enabled */
	request[9] = 0; /* termchar */
	request[10] = 0; /* Reserved */
	request[11] = 0; /* Reserved */

	data.message = request;
	data.transfer_size = HEADER_SIZE;
	data.flags = USBTMC_FLAG_ASYNC;

	retval = ioctl(fd,USBTMC_IOCTL_WRITE, &data);
	if (retval < 0) {
		retval = -errno;
		goto exit;
	}

	data.message = buf;
	data.transfer_size = BULKSIZE;
	data.flags = 0; /* sync */
	retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
	if (retval < 0) {
		retval = -errno;
		goto exit;
	}

	if (data.transferred < HEADER_SIZE) {
		retval = -EPROTO;
		goto exit;
	}
	data.transferred -= HEADER_SIZE;

	if (memcmp(request,buf,4) != 0) {
		retval = -EPROTO; /* response out of order */
		goto exit;
	}

	expected_size = le32toh(*(__u32*)&buf[4]);
	if (expected_size > max_len || data.transferred > expected_size) {
		retval = -EPROTO; /* more data than requested */
		goto exit;
	}

	memcpy(msg, (char*)data.message + HEADER_SIZE, data.transferred);
	total = data.transferred;
	expected_size -= data.transferred;

	if (retval == 0) {
		// No short packet or ZLP received => ready
		data.message = msg + total;
		data.transfer_size = expected_size;
		data.flags = USBTMC_FLAG_IGNORE_TRAILER; /* sync */
		retval = ioctl(fd,USBTMC_IOCTL_READ, &data);
		total += data.transferred;
		if (retval < 0) {
			retval = -errno;
		}
		assert(retval == 1); // must be short packet or ZLP now.
		retval = 0;
	}
exit:
	if (received)
		*received = (__u32)total;

	// TODO: In case of error abort bulk in.
	free(buf);
	return retval;
}
#endif

static int tmc_read(char *msg, __u32 max_len, __u32 *received)
{
	int len = read(fd,msg,max_len);
	if (received)
		*received = (len>0)? len : 0;
	return len;
}

/* Wait for SRQ using poll() */
static int poll_for_srq(int timeout) {
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLPRI;
	return poll(&pfd,1,timeout);
}

/* Wait for SRQ using ioctl() */
static int wait_for_srq(unsigned int timeout) {
	return ioctl(fd, USBTMC488_IOCTL_WAIT_SRQ, &timeout);
}

/* return true if we can write */
static int wait_for_write(int timeout)
{
	struct pollfd pfd;
	int err;
	pfd.fd = fd;
	pfd.events = POLLOUT|POLLERR|POLLHUP;
	err = poll(&pfd,1,timeout);
	return ((err == 1) && (pfd.events & POLLOUT));
}

/* return true if we can read */
static int wait_for_read(int timeout)
{
	struct pollfd pfd;
	int err;
	pfd.fd = fd;
	pfd.events = POLLIN|POLLERR|POLLHUP;
	err = poll(&pfd,1,timeout);
	return ((err == 1) && (pfd.events & POLLIN));
}

static void wait_for_user()
{
	char buf[8];
	read(0,buf,1);
}

static int enable_eom(unsigned char enabled)
{
	int rv = ioctl(fd, USBTMC_IOCTL_EOM_ENABLE, &enabled);
	assert(rv == 0);
}

static int set_timeout(unsigned int tmo)
{
	int rv = ioctl(fd, USBTMC_IOCTL_SET_TIMEOUT, &tmo);
	assert(rv == 0);
}

static int show_api_version()
{
	unsigned int api_version;
	int rv = ioctl(fd, USBTMC_IOCTL_API_VERSION, &api_version);
	assert(rv == 0);
	printf("USBTMC_API_VERSION = %u\n", api_version);
}

const size_t MAX_BL = 1024;

static void any_system_error()
{
  char buf[MAX_BL];
  int rv, res;
  __u32 received = 0;

  tmc_raw_send("system:error?\n");
  rv = tmc_raw_read(buf,MAX_BL, &received);
  if (rv < 0 || received == 0) {
	printf("read failed: rv=%d errno=%d recv=%u\n", rv, errno, received);
	exit(1);
  }
  rv = sscanf(buf, "%d,", &res);
  if (res != 0 || rv < 1) {
	printf("syst:err? = %.*s", received, buf);
	exit(1);
  }
}

int main () {
  int rv;
  __u32 bigsize = 4000000;
  char *sBigSend = malloc(bigsize + MAX_BL); /* freed when program exits */
  char *sBigReceive = malloc(bigsize + MAX_BL); /* freed when program exits */

  unsigned int tmp,tmp1,caps;
  __u32 len,n, digits;
  __u8 stb;
  __u32 received, sent;
  char buf[MAX_BL];
  int oflags;
  //fd_set fdsel[3];
  double time, time2;
  int i;
  struct usbtmc_ctrlrequest req;

  /* Open file */
  if (0 > (fd = open("/dev/usbtmc0",O_RDWR))) {
	perror("failed to open device");
	exit(1);
  }

  /***********************************************************
   * 1. Prepare interface
   ***********************************************************/
  setSRE(0x00); /* disable SRQ */
  set_timeout(2000);
  enable_eom(1);
  /* Send device clear */
  rv = ioctl(fd,USBTMC_IOCTL_CLEAR);
  assert(rv == 0);
  tmc_send("*CLS\n");
  getTS_usec(); /* initialize time stamp */
  show_stb(get_stb());

  if (!wait_for_write(0)) {
	// poll function failed
  	perror("cannot write");
	exit(1);
  }

  /* Send identity query */
  tmc_raw_send("*IDN?\n");
  /* Read and print returned identity string */
  tmc_raw_read(buf,MAX_BL, &received);
  puts("*******************************************************************");
  printf("Testing device: *IDN? = %.*s", received, buf);
  show_api_version();
  puts("*******************************************************************");
  puts("1. Performance test with read/write");
  getTS_usec(); /* initialize time stamp */
  for (i = 0; i < 10; i++) {
	write(fd, "*OPC?\n",6);
	read(fd, sBigReceive, 10);
  }
  time = getTS_usec();
  printf("*OPC? Latency = %.0f us per call with read/write functions\n", time/10.0);
  any_system_error();

  puts("*******************************************************************");
  puts("2. Performance test with raw read/write");
  getTS_usec(); /* initialize time stamp */
  for (i = 0; i < 10; i++) {
	tmc_raw_write("*OPC?\n",6,NULL);
	tmc_raw_read(sBigReceive, 10, NULL);
  }
  time = getTS_usec();
  printf("*OPC? Latency = %.0f us per call with raw read/write functions\n", time/10.0);

  puts("*******************************************************************");
  puts("3. Test split write with USBTMC_IOCTL_EOM_ENABLE");
  enable_eom(0);
  tmc_send("system:");
  enable_eom(1);
  tmc_send("error?");
  rv = tmc_read(buf,MAX_BL, &received);
  assert(rv >= 0 && received > 0);
  any_system_error();

  puts("*******************************************************************");
  puts("4a. Test SRQ with poll mode");
  show_stb(get_stb());
  setSRE(0x10); /* Do SRQ when MAV set (message available) */
  getTS_usec(); /* initialise time stamp */
  tmc_raw_send("*TST?");
  rv = poll_for_srq(1000);
  assert(rv == 1);
  stb = get_stb();
  show_stb(stb);
  assert((stb & (STB_MSS|STB_MAV)) == (STB_MSS|STB_MAV));
  stb = get_stb();
  show_stb(stb);
  assert((stb & (STB_MSS|STB_MAV)) == (STB_MAV));
  tmc_raw_read(buf,MAX_BL, &received);
  setSRE(0x00);
  any_system_error();

  puts("*******************************************************************");
  puts("4b. Test SRQ with USBTMC488_IOCTL_WAIT_SRQ");
  setSRE(0x10); /* Do SRQ when MAV set (message available) */
  getTS_usec(); /* initialise time stamp */
  tmc_raw_send("*TST?");
  rv = wait_for_srq(1000);
  assert(rv == 0);
  stb = get_stb();
  show_stb(stb);
  assert((stb & (STB_MSS|STB_MAV)) == (STB_MSS|STB_MAV));
  stb = get_stb();
  show_stb(stb);
  assert((stb & (STB_MSS|STB_MAV)) == (STB_MAV));
  tmc_raw_read(buf,MAX_BL, &received);
  setSRE(0x00);
  any_system_error();

  getTS_usec(); /* initialise time stamp */
  rv = wait_for_srq(200);
  time = getTS_usec() / 1000.0;
  assert(rv == -1 && errno == ETIMEDOUT);
  printf("Is time = %f near to 200 ms?\n", time);
  assert( time >= 180.0 && time <= 1000.0 );

  any_system_error();

  puts("*******************************************************************");
  puts("5a. Send and receive big data and verify content with write/read");
  /* prepare big send data */
  digits = sprintf( buf, "%u", bigsize );
  n = sprintf( sBigSend,":MMEM:DATA 'test.txt',#%u%s", digits, buf );
  for (i = 0; i < bigsize; i++)
	sBigSend[n+i] = (char)i;

  getTS_usec(); /* initialize time stamp */
  rv = write(fd, sBigSend, n+bigsize);
  time = getTS_usec();
  assert(rv > 0);
  assert(rv == n+bigsize);
  any_system_error(); /* wait until file is written */

  rv = tmc_send("mmem:data? 'test.txt'");
  assert(rv > 0);
  getTS_usec(); /* initialize time stamp */
  tmc_read(sBigReceive, bigsize + MAX_BL, &received);
  time2 = getTS_usec();
  if ( memcmp(&sBigSend[n], &sBigReceive[2+digits], bigsize) != 0) {
  	perror("data mismatch");
	exit(1);
  }
  printf("Standard I/O: send rate=%.3f MB/s, read rate %.3f MB/s\n", 
	bigsize * (1.0e6/(1024*1024)) / time, bigsize * (1.0e6/(1024*1024)) / time2);
  any_system_error();

  puts("*******************************************************************");
  puts("5b. Send and receive big data and verify content with raw read/write");
  /* prepare big send data */
  digits = sprintf( buf, "%u", bigsize );
  n = sprintf( sBigSend,":MMEM:DATA 'test.txt',#%u%s", digits, buf );
  for (i = 0; i < bigsize; i++)
	sBigSend[n+i] = (char)i+10;

  getTS_usec(); /* initialize time stamp */
  tmc_raw_write(sBigSend, n+bigsize, &sent);
  time = getTS_usec();
  assert(sent == (n+bigsize));
  any_system_error(); /* wait until file is written */

  tmc_raw_send("mmem:data? 'test.txt'");
  getTS_usec(); /* initialize time stamp */
  tmc_raw_read(sBigReceive, bigsize + MAX_BL, &received);
  time2 = getTS_usec();
  if ( memcmp(&sBigSend[n], &sBigReceive[2+digits], bigsize) != 0) {
	perror("data mismatch");
	exit(1);
  }
  printf("Raw I/O: send rate=%.3f MB/s, read rate %.3f MB/s\n",
	bigsize * (1.0e6/(1024*1024)) / time, bigsize * (1.0e6/(1024*1024)) / time2);
  any_system_error();

  puts("*******************************************************************");
  puts("5c. Simulate with copy: Send and receive big data with raw read/write");
  /* prepare big send data */
  digits = sprintf( buf, "%u", bigsize );
  n = sprintf( sBigSend,":MMEM:DATA 'test.txt',#%u%s", digits, buf );
  for (i = 0; i < bigsize; i++)
	sBigSend[n+i] = (char)i+10;

  getTS_usec(); /* initialize time stamp */
  {
	char *_localbuf = malloc(bigsize + MAX_BL); /* freed when program exits */
	memcpy(_localbuf, sBigSend, n+bigsize);
        tmc_raw_write(_localbuf, n+bigsize, &sent);
	free(_localbuf);
  }
  time = getTS_usec();
  assert(sent == (n+bigsize));
  any_system_error(); /* wait until file is written */

  tmc_raw_send("mmem:data? 'test.txt'");
  getTS_usec(); /* initialize time stamp */
  tmc_raw_read(sBigReceive, bigsize + MAX_BL, &received);
  time2 = getTS_usec();
  if ( memcmp(&sBigSend[n], &sBigReceive[2+digits], bigsize) != 0) {
	perror("data mismatch");
	exit(1);
  }
  printf("Raw I/O: send rate=%.3f MB/s, read rate %.3f MB/s\n",
	bigsize * (1.0e6/(1024*1024)) / time, bigsize * (1.0e6/(1024*1024)) / time2);
  any_system_error();

  puts("*******************************************************************");
  puts("5d. Send and receive big data and verify content with async raw read/write");
  /***********************************************************
   * 5d. asynchronous raw read/write
   *     Note that async write does not send more then 16 * 4k
   ***********************************************************/
  /* test asynchronous write and reduce bigsize for simple testing */
  if (bigsize > (15 * 4096))
	  bigsize = 15 * 4096;

  digits = sprintf( buf, "%u", bigsize );
  n = sprintf( sBigSend,":MMEM:DATA 'test.txt',#%u%s", digits, buf );
  for (i = 0; i < bigsize; i++) 
	sBigSend[n+i] = (char)i+5;

  getTS_usec(); /* initialize time stamp */
  rv = tmc_raw_write_async(sBigSend,n+bigsize, &sent);
  if (rv < 0 || !wait_for_write(500)) {
	// poll function failed
  	perror("cannot write asynchron");
	exit(1);
  }
  time = getTS_usec();
  assert(sent <= (n+bigsize));
  assert(rv == 0);
  printf("Async write: rv=%d sent=%u send rate=%.3f MB/s\n", rv, sent, bigsize * (1.0e6/(1024*1024)) / time);
  rv = tmc_raw_write_result_async(&sent);
  printf("Async result: rv=%d transferred=%u expected=%u\n", rv, sent, ((n+bigsize+HEADER_SIZE+3)& ~3));
  assert(sent == ((n+bigsize+HEADER_SIZE+3)& ~3));
  assert(rv == 0);

  /* test asynchronous read */
  tmc_raw_send("mmem:data? 'test.txt'");
  getTS_usec();
  rv = tmc_raw_read_async_start(bigsize + MAX_BL);
  time = getTS_usec();
  if (rv < 0 || !wait_for_read(500)) {
	// poll function failed
  	perror("cannot start asynchron read");
	exit(1);
  }
  time2 = getTS_usec();
  printf("Async read: rv=%d read rate=%.3f MB/s\n", rv, bigsize * (1.0e6/(1024*1024)) / time2);

  getTS_usec();
  rv = tmc_raw_read_async_result(sBigReceive, bigsize + MAX_BL, &received);
  time = getTS_usec();
  if (rv < 0) {
  	perror("cannot read asynchronous result");
	exit(1);
  }
  printf("Async read result: rv=%d received=%u expected=%u\n", rv, received, bigsize + 3 + digits);
  assert(received == (bigsize + 3 + digits));

  if ( memcmp(&sBigSend[n], &sBigReceive[2+digits], bigsize) != 0) {
  	perror("data mismatch");
	exit(1);
  }

  puts("*******************************************************************");
  puts("6a.  Test canceling asynchronous write");
  rv = tmc_raw_write_async(sBigSend,n+bigsize, &sent);
  assert(rv == 0);
  usleep(100);
  rv = ioctl(fd, USBTMC_IOCTL_CANCEL_IO);
  assert(rv == 0);
  rv = wait_for_write(10000);
  assert(rv != 0); // Should be true and no timeout!
  rv = tmc_raw_write_result_async(&sent);
  assert(rv == -1);
  printf("Async write successful canceled: errno = %d\n", errno);
  assert(errno == ECANCELED);

  rv = ioctl(fd, USBTMC_IOCTL_CLEANUP_IO);
  assert(rv == 0);

  rv = ioctl(fd, USBTMC_IOCTL_ABORT_BULK_OUT_TAG, &s_tag_out);
  assert(rv == 0); // If you hang here, then reduce or increase usleep above!

  /* Send device clear. Normally not needed */
  rv = ioctl(fd,USBTMC_IOCTL_CLEAR);
  assert(rv == 0);

  any_system_error();

  puts("*******************************************************************");
  puts("6b.  Test canceling asynchronous read");
  tmc_raw_send("mmem:data? 'test.txt'");

  rv = tmc_raw_read_async_start(bigsize + MAX_BL);
  assert(rv == 0);
  usleep(100000); // give instrument time to generate response.
  rv = ioctl(fd, USBTMC_IOCTL_CANCEL_IO);
  assert(rv == 0);
  rv = wait_for_read(10000);
  assert(rv != 0); // Should be true with no timeout!
  rv = tmc_raw_read_async_result(sBigReceive, bigsize + MAX_BL, &received);
  assert(rv == -1);
  printf("Async read successful canceled: errno = %d\n", errno);
  assert(errno == ECANCELED);

  rv = ioctl(fd, USBTMC_IOCTL_CLEANUP_IO);
  assert(rv == 0);

  rv = ioctl(fd, USBTMC_IOCTL_ABORT_BULK_IN_TAG, &s_tag_in);
  assert(rv == 0);

  any_system_error();

  if (0)
  {
	// Special test "test:scpi:internal:sync" == "sleep"
	__u8 tag;
	rv = tmc_raw_send("test:scpi:internal:sync 8000");
	assert(rv == 0);

	rv = tmc_raw_send("test:scpi:internal:sync 100");
	assert(rv == 0);
	tag = s_tag_out;
	rv = tmc_raw_send("test:scpi:internal:sync 200");
	assert(rv != 0 );
	rv = ioctl(fd, USBTMC_IOCTL_ABORT_BULK_OUT_TAG, &tag);
	assert(rv == 0);
	rv = ioctl(fd, USBTMC_IOCTL_ABORT_BULK_OUT_TAG, &s_tag_out);
	assert(rv == 0);
  }

  puts("*******************************************************************");
  puts("7a.  Test error handling for OUT PIPE");

  /* test error handling for standard write */
  ioctl(fd,USBTMC_IOCTL_SET_OUT_HALT); // lock bulk out
  rv = tmc_send("system:error?\n");
  printf("standard write must fail: rv=%d errno=%d\n", rv, errno);
  assert(errno == EPIPE);
  if (rv >= 0) {
	exit(1);
  }

  /* test error handling for raw write */
  ioctl(fd,USBTMC_IOCTL_SET_OUT_HALT); // lock bulk out
  rv = tmc_raw_send("system:error?\n");
  printf("send should fail: rv=%d errno=%d\n", rv, errno);
  assert(errno == EPIPE);
  if (rv >= 0) {
	exit(1);
  }

  ioctl(fd,USBTMC_IOCTL_SET_OUT_HALT); // lock bulk out
  rv = tmc_raw_write(sBigSend,n+bigsize, &sent);
  printf("big write should fail: rv=%d errno=%d sent=%u\n", rv, errno, sent);
  assert(errno == EPIPE);
  if (rv >= 0) {
	exit(1);
  }

  /* test error handling for async raw write */
  errno = 0;
  rv = tmc_raw_write_async("123",3, &sent);
  printf("async write shall fail: rv=%d errno=%d sent=%u\n", rv, errno, sent);
  assert(rv == 0 && sent == 3);

  getTS_usec();
  rv = wait_for_write(1000);
  time = getTS_usec();
  printf("wait for write must return immediately: rv=%d time=%f msec\n", rv, time/1000.0);
  assert(rv == 1);
  assert(time < 400 * 1000);

  rv = tmc_raw_write_result_async(&sent);
  printf("async result: rv=%d transferred=%u\n", rv, sent);
  assert(sent == 0);
  assert(rv < 0);
  if (rv >= 0 || sent > 0) {
  	perror("async send result should fail: ");
	exit(1);
  }
  assert(errno == EPIPE);

  /* test error handling for async read */
  rv = tmc_raw_read(buf,MAX_BL, &received);
  printf("read should fail: rv=%d errno=%d recv=%u\n", rv, errno, received);
  if (rv >= 0 || errno != EPIPE) {
  	perror("synchron read should fail: ");
	exit(1);
  }

  rv = ioctl(fd,USBTMC_IOCTL_CLEANUP_IO);
  assert(rv == 0);

  rv = ioctl(fd,USBTMC_IOCTL_CLEAR);
  assert(rv == 0);

  ioctl(fd,USBTMC_IOCTL_CLEAR_IN_HALT); // reset bulk in
  ioctl(fd,USBTMC_IOCTL_CLEAR_OUT_HALT); // reset bulk out

  any_system_error();

  puts("*******************************************************************");
  puts("7b.  Test error handling for IN PIPE");

  ioctl(fd,USBTMC_IOCTL_SET_IN_HALT); // lock bulk in
  tmc_raw_send("*idn?");
  rv = tmc_raw_read(buf,MAX_BL, &received);
  printf("read should fail: rv=%d errno=%d recv=%u\n", rv, errno, received);
  if (rv >= 0 || received > 0) {
  	perror("async send result should fail: ");
	exit(1);
  }

  rv = ioctl(fd,USBTMC_IOCTL_CLEAR);
  assert(rv == 0);
  rv = ioctl(fd,USBTMC_IOCTL_CLEAR_IN_HALT); // clear feature halt bulk in
  assert(rv == 0);

  any_system_error();

  /* just try to read data. We should run into timeout */
  rv = tmc_raw_read(buf,MAX_BL, &received);
  printf("read should fail with timeout: rv=%d errno=%d recv=%u\n", rv, errno, received);
  assert(rv == -ETIMEDOUT);
  assert(received == 0);

  any_system_error();

  puts("*******************************************************************");
  puts("8.  Test USBTMC_IOCTL_CTRL_REQUEST");

  /* Read manufacturer string */
  req.req.bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
  req.req.bRequest = USB_REQ_GET_DESCRIPTOR;
  req.req.wValue = USB_DT_STRING << 8 | 0x01; // Index of string
  req.req.wIndex = 0;
  req.req.wLength = MAX_BL;
  req.data = buf;
  
  memset(buf,0, MAX_BL);
  rv = ioctl(fd, USBTMC_IOCTL_CTRL_REQUEST, &req);
  if (rv < 0) {
	printf("request failed: rv=%d errno=%d\n", rv, errno);
  } else {
	/* Sorry. There is a better way to print wchar_t */
	for (i=2; i < rv; i+=2)
		printf("%c", buf[i]);
	printf("\n");
  }
	
  printf("done\n");
  close(fd);
  exit(0);
}
